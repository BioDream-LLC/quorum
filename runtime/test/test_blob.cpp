// casebook_blob_equivalence -- the runtime blob path decides exactly as the compiled-header path, in every state.
//
// Built once per reference casebook, with QUORUM_HEADER / QUORUM_NS / QUORUM_HAS_CHANNELS set by CMake, and given
// the blob cbc wrote for the same source. Three things are checked:
//
//   1. tables  -- every scalar, edge, action, claim, exclusive group, channel and threshold in the blob equals the
//                 header's, every name and description matches, and the fingerprint is the header's.
//   2. states  -- for every presence mask, decide() through the runtime's fixed-capacity framework gives the same
//                 outcome, the same action and the same step count as through the header's exactly-sized one.
//   3. refusal -- damaged blobs are refused for the right reason: a flipped byte (CRC), an edited edge with the CRC
//                 repaired (fingerprint), truncation, erased flash, and a wrong format version.
//
// Step counts equal is the claim the WCET argument needs: the step bound is a property of the casebook, not of the
// container. Cycle counts are NOT claimed equal -- reset() is linear in capacity -- and must be measured on silicon.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/runtime/blob.hpp"

#include QUORUM_HEADER

namespace cb = QUORUM_NS;
namespace rt = quorum::runtime;
using quorum::solver::Status;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++failures;
    }
}

[[nodiscard]] std::vector<std::uint8_t> read_all(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void put_crc(std::vector<std::uint8_t>& b) {
    const std::uint32_t c = rt::detail::crc32(b.data(), b.size() - 4U);
    for (std::size_t k = 0; k < 4U; ++k) { b[b.size() - 4U + k] = static_cast<std::uint8_t>((c >> (8U * k)) & 0xFFU); }
}

void check_tables(const rt::Casebook& r) {
    expect(r.fingerprint == cb::kCasebookFingerprint, "fingerprint equals the header's");
    expect(rt::computed_fingerprint(r) == cb::computed_fingerprint(), "recomputed fingerprints agree");
    expect(r.argument_count == cb::argument_count, "argument count");
    expect(r.observation_count == cb::kObservationCount, "observation count");
    expect(r.max_args == cb::kMaxArgs && r.max_attacks == cb::kMaxAttacks, "capacities");
    expect(r.closed_world == cb::kClosedWorld, "closed-world argument");
    expect(r.idle_action == cb::kIdleAction && r.safe_action == cb::kSafeAction, "idle and safe actions");
    expect(r.worst_case_mask == cb::kWorstCaseMask && r.worst_case_steps == cb::kWorstCaseSteps, "worst case");
    expect(r.step_bound == cb::kStepBound, "step bound");
    expect(r.version == cb::kCasebookVersion, "casebook version");
    expect(std::strcmp(r.name, cb::kCasebookName) == 0, "casebook name");
    expect(std::strcmp(r.source_document, cb::kSourceDocument) == 0, "source document");
    expect(std::strcmp(r.source_date, cb::kSourceDate) == 0, "source date");
    expect(std::strcmp(r.source_author, cb::kSourceAuthor) == 0, "source author");

    bool edges = r.edge_count == cb::kCasebookEdges;
    for (std::int32_t i = 0; edges && i < r.edge_count; ++i) {
        edges = r.edges[i].from == cb::kCasebook[i].from && r.edges[i].to == cb::kCasebook[i].to;
    }
    expect(edges, "every edge, in order");
    bool actions = r.action_count == cb::kActionCount;
    for (std::int32_t i = 0; actions && i < r.action_count; ++i) { actions = r.actions[i] == cb::kActions[i]; }
    expect(actions, "actions, in order");
    bool claims = r.claim_count == cb::kClaimCount;
    for (std::int32_t i = 0; claims && i < r.claim_count; ++i) { claims = r.claims[i] == cb::kClaims[i]; }
    expect(claims, "claims");
    bool groups = r.group_count == cb::kExclusiveGroupCount;
    for (std::int32_t g = 0; groups && g <= r.group_count; ++g) { groups = r.group_offset[g] == cb::kExclusiveOffset[g]; }
    for (std::int32_t i = 0; groups && i < r.group_offset[r.group_count]; ++i) {
        groups = r.group_members[i] == cb::kExclusiveFlat[i];
    }
    expect(groups, "exclusive groups");

    bool names = true;
    for (std::int32_t a = 0; names && a < r.argument_count; ++a) {
        names = std::strcmp(rt::name_of(r, a), cb::name_of(a)) == 0 &&
                std::strcmp(rt::description_of(r, a), cb::description_of(a)) == 0;
        if (!names) { std::printf("      argument %d: '%s' vs '%s'\n", a, rt::name_of(r, a), cb::name_of(a)); }
    }
    expect(names, "every argument name and description");

    expect(r.channel_count == cb::kChannelCount && r.threshold_count == cb::kThresholdCount, "binding counts");
#if QUORUM_HAS_CHANNELS
    bool specs = true;
    for (std::int32_t i = 0; specs && i < r.channel_count; ++i) {
        const auto& a = r.channels[i];
        const auto& b = cb::kChannels[i];
        specs = a.valid_lo == b.valid_lo && a.valid_hi == b.valid_hi && a.stale_after_ms == b.stale_after_ms &&
                a.flatline_after_ms == b.flatline_after_ms && a.flatline_epsilon == b.flatline_epsilon &&
                a.unavailable_obs == b.unavailable_obs && a.flatline_obs == b.flatline_obs;
    }
    for (std::int32_t i = 0; specs && i < r.threshold_count; ++i) {
        const auto& a = r.thresholds[i];
        const auto& b = cb::kThresholds[i];
        specs = a.channel == b.channel && a.observation == b.observation && a.sense == b.sense && a.on == b.on &&
                a.off == b.off && a.hold_ms == b.hold_ms;
    }
    expect(specs, "every channel and threshold spec");
    static rt::RtBinding binding;
    expect(rt::make_binding(binding, r) == quorum::binding::Status::Ok, "the runtime binding configures");
#endif
#if QUORUM_HAS_TIMERS
    bool timers = r.timer_count == cb::kTimerCount;
    for (std::int32_t i = 0; timers && i < r.timer_count; ++i) {
        const auto& a = r.timers[i];
        const auto& b = cb::kTimers[i];
        timers = a.kind == b.kind && a.observation == b.observation && a.source == b.source && a.after == b.after &&
                 a.window_ms == b.window_ms && a.times == b.times;
    }
    expect(timers, "every timed reading");
    static rt::RtTimers rtimers;
    expect(rt::make_timers(rtimers, r) == quorum::binding::Status::Ok, "the runtime timers configure");
    static cb::CaseTimers htimers;
    expect(cb::make_timers(htimers) == quorum::binding::Status::Ok, "the header's timers configure");
#else
    expect(r.timer_count == 0, "no timed readings in an image without them");
#endif
}

[[nodiscard]] std::uint32_t check_states(const rt::Casebook& r) {
    static cb::CaseFramework hfw;
    static cb::CaseSolver hsolver;
    static cb::CasePolicy hpolicy;
    static rt::RtFramework rfw;
    static rt::RtSolver rsolver;
    static rt::RtPolicy rpolicy;
    expect(cb::make_policy(hpolicy) == Status::Ok, "header policy");
    expect(rt::make_policy(rpolicy, r) == Status::Ok, "runtime policy");

    const std::uint32_t states = 1U << static_cast<std::uint32_t>(cb::kObservationCount);
    std::uint32_t differ = 0;
    for (std::uint32_t m = 0; m < states; ++m) {
        if (cb::instantiate(hfw, m) != Status::Ok || rt::instantiate(rfw, r, m) != Status::Ok) {
            std::printf("FAIL  instantiate at state 0x%x\n", m);
            ++failures;
            return states;
        }
        const auto h = quorum::arbiter::decide(hfw, hsolver, hpolicy);
        const auto v = quorum::arbiter::decide(rfw, rsolver, rpolicy);
        const bool same = h.outcome == v.outcome && h.steps == v.steps && h.action.valid() == v.action.valid() &&
                          (!h.action.valid() || h.action.get() == v.action.get());
        if (!same) {
            if (differ < 5U) { std::printf("      state 0x%x differs (steps %d vs %d)\n", m, h.steps, v.steps); }
            ++differ;
        }
    }
    expect(differ == 0U, "every state decides identically, with identical step counts");
    return states;
}

void check_refusals(const std::vector<std::uint8_t>& good) {
    static rt::Casebook r;
    std::vector<std::uint8_t> b = good;

    b[good.size() / 2U] ^= 0x01U;
    expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::BadCrc, "a flipped byte is refused as damaged");

    // Edit the first edge's target, then repair the CRC: transport is fine, identity is not.
    b = good;
    constexpr std::size_t kFirstEdgeTo = 4U * 16U + 4U;  // 15 header words, the edge count, then from, to
    b[kFirstEdgeTo] = static_cast<std::uint8_t>(b[kFirstEdgeTo] ^ 0x01U);
    put_crc(b);
    const rt::BlobStatus edited = rt::load(b.data(), b.size(), r);
    expect(edited == rt::BlobStatus::FingerprintMismatch || edited == rt::BlobStatus::Malformed,
           "an edited edge with a repaired CRC is refused");

    // The fallback, and (if the casebook has a binding) a setpoint, each edited with the CRC repaired: until
    // 2026-10-11 both of these loaded, because the fingerprint covered only the framework and the action list.
    b = good;
    constexpr std::size_t kSafeAction = 40U;  // 16-byte preamble, then args, obs, max_args, max_attacks, cw, idle, safe
    static rt::Casebook orig;
    expect(rt::load(good.data(), good.size(), orig) == rt::BlobStatus::Ok, "the good image loads");
    std::int32_t other = -1;
    for (std::int32_t i = 0; i < orig.action_count; ++i) {
        if (orig.actions[i] != orig.safe_action) { other = orig.actions[i]; }
    }
    if (other >= 0) {
        for (std::size_t k = 0; k < 4U; ++k) {
            b[kSafeAction + k] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(other) >> (8U * k)) & 0xFFU);
        }
        put_crc(b);
        expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::FingerprintMismatch,
               "a changed fallback with a repaired CRC is refused");
    }
    if (orig.threshold_count > 0) {
        // Find the first threshold record (channel, observation, sense, on, off) by its contents, then edit `on`.
        const quorum::binding::ThresholdSpec& th = orig.thresholds[0];
        const std::uint32_t rec[5] = {static_cast<std::uint32_t>(th.channel), static_cast<std::uint32_t>(th.observation),
                                      th.sense == quorum::binding::Sense::Above ? 0U : 1U,
                                      static_cast<std::uint32_t>(th.on), static_cast<std::uint32_t>(th.off)};
        const auto word = [&good](std::size_t k) {
            return static_cast<std::uint32_t>(good[k]) | (static_cast<std::uint32_t>(good[k + 1]) << 8U) |
                   (static_cast<std::uint32_t>(good[k + 2]) << 16U) | (static_cast<std::uint32_t>(good[k + 3]) << 24U);
        };
        std::size_t at = 0, hits = 0;
        for (std::size_t k = 0; k + 20U <= good.size() - 4U; k += 4U) {
            bool same = true;
            for (std::size_t w = 0; w < 5U && same; ++w) { same = word(k + 4U * w) == rec[w]; }
            if (same) { at = k + 12U; ++hits; }
        }
        expect(hits == 1U, "the first threshold record occurs once in the image, so it can be edited");
        if (hits == 1U) {
            b = good;
            b[at] = static_cast<std::uint8_t>(b[at] ^ 0x10U);  // 90.000 -> 90.016, say: small, plausible, wrong
            put_crc(b);
            expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::FingerprintMismatch,
                   "a changed setpoint with a repaired CRC is refused");
        }
    }

    if (orig.timer_count > 0) {
        // The first timed reading's record (kind, observation, source, after, window, times): lengthen its window.
        const quorum::binding::TimerSpec& tm = orig.timers[0];
        const std::uint32_t rec[6] = {static_cast<std::uint32_t>(tm.kind), static_cast<std::uint32_t>(tm.observation),
                                      static_cast<std::uint32_t>(tm.source), static_cast<std::uint32_t>(tm.after),
                                      tm.window_ms, static_cast<std::uint32_t>(tm.times)};
        const auto word = [&good](std::size_t k) {
            return static_cast<std::uint32_t>(good[k]) | (static_cast<std::uint32_t>(good[k + 1]) << 8U) |
                   (static_cast<std::uint32_t>(good[k + 2]) << 16U) | (static_cast<std::uint32_t>(good[k + 3]) << 24U);
        };
        std::size_t at = 0, hits = 0;
        for (std::size_t k = 0; k + 24U <= good.size() - 4U; k += 4U) {
            bool same = true;
            for (std::size_t w = 0; w < 6U && same; ++w) { same = word(k + 4U * w) == rec[w]; }
            if (same) { at = k + 16U; ++hits; }
        }
        expect(hits == 1U, "the first timed-reading record occurs once in the image, so it can be edited");
        if (hits == 1U) {
            b = good;
            b[at + 1U] = static_cast<std::uint8_t>(b[at + 1U] ^ 0x01U);  // the window, longer by 256 ms
            put_crc(b);
            expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::FingerprintMismatch,
                   "a changed timer window with a repaired CRC is refused");
            b = good;
            b[at - 16U] = 2U;  // After becomes Count: a count of 0 cannot configure
            put_crc(b);
            expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::Malformed,
                   "a timed-reading table that cannot configure is refused at load");
        }
        b = good;
        b[4] = 3U;
        put_crc(b);
        expect(rt::load(b.data(), b.size(), r) != rt::BlobStatus::Ok,
               "a timed image relabelled as format 3 does not load with its timers silently dropped");
    }

    expect(rt::load(good.data(), good.size() - 8U, r) == rt::BlobStatus::TooShort, "a truncated image is refused");

    const std::vector<std::uint8_t> erased(good.size(), 0xFFU);
    expect(rt::load(erased.data(), erased.size(), r) == rt::BlobStatus::BadMagic, "erased flash is refused");

    b = good;
    b[4] = 2U;
    put_crc(b);
    expect(rt::load(b.data(), b.size(), r) == rt::BlobStatus::BadFormat, "an unknown format version is refused");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: test_blob <casebook.qcb>\n");
        return 2;
    }
    const std::vector<std::uint8_t> blob = read_all(argv[1]);
    static rt::Casebook r;
    const rt::BlobStatus st = rt::load(blob.data(), blob.size(), r);
    if (st != rt::BlobStatus::Ok) {
        std::printf("FAIL  load: %s\n", rt::blob_status_name(st));
        return 1;
    }
    check_tables(r);
    const std::uint32_t states = check_states(r);
    check_refusals(blob);
    if (failures != 0) {
        std::printf("blob_equivalence %s: %d failure(s)\n", cb::kCasebookName, failures);
        return 1;
    }
    std::printf("blob_equivalence %s: %u bytes, %u states, identical to the compiled header\n", cb::kCasebookName,
                static_cast<unsigned>(blob.size()), states);
    return 0;
}
