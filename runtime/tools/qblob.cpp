// qblob -- host companion to the runtime firmware.
//
//   qblob info  <casebook.qcb>   load the image exactly as the device will, and describe it
//   qblob sweep <casebook.qcb>   decide every state through the runtime path and print the DEVICE line the board
//                                prints, so a board can be checked against the host byte for byte
//
// It uses the device loader itself (quorum/runtime/blob.hpp), not a host re-implementation: if qblob can load an
// image, the board's loader will reach the same verdict on the same bytes.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/guard.hpp"
#include "quorum/runtime/layout.hpp"

namespace rt = quorum::runtime;
using quorum::arbiter::Outcome;
using quorum::solver::Status;

namespace {

// A palette: one DEVICE line per member, each sweeping that member's own states with every other member quiet. By
// the palette's non-interference argument, each line must equal the line of the member's casebook on its own.
int palette(const char* cmd, const std::vector<std::uint8_t>& blob) {
    static rt::Palette p;
    const rt::BlobStatus st = rt::load_palette(blob.data(), blob.size(), p);
    if (st != rt::BlobStatus::Ok) {
        std::printf("REFUSED  %s\n", rt::blob_status_name(st));
        return 1;
    }
    if (std::strcmp(cmd, "info") == 0) {
        std::printf("palette       %d casebooks on one device\narguments     %d\nattacks       %d\nobservations  %d\n"
                    "fingerprint   0x%08x\nstep bound    %d\nimage bytes   %zu\n",
                    static_cast<int>(p.member_count), static_cast<int>(p.argument_count),
                    static_cast<int>(p.edge_count), static_cast<int>(p.observation_count),
                    static_cast<unsigned>(p.fingerprint), static_cast<int>(p.step_bound), blob.size());
        for (std::int32_t m = 0; m < p.member_count; ++m) {
            const rt::Member& x = p.members[m];
            std::printf("member        %s: observations %d-%d, safe action %s\n", x.name,
                        static_cast<int>(x.observation_offset),
                        static_cast<int>(x.observation_offset + x.observation_count - 1),
                        rt::detail::nth(p.strings, x.safe_action));
        }
        std::printf("binding       none: a palette takes the presence mask directly\n");
        return 0;
    }
    static rt::RtFramework fw;
    static rt::RtSolver solver;
    int rc = 0;
    std::int32_t max_steps = 0;
    for (std::int32_t m = 0; m < p.member_count; ++m) {
        const rt::Member& x = p.members[m];
        if (x.observation_count > rt::kSweepLimit) {
            std::printf("DEVICE %s worst-case state only (%d observations)\n", x.name, static_cast<int>(x.observation_count));
            continue;
        }
        static rt::RtPolicy policy;
        policy = rt::RtPolicy{};
        if (rt::make_policy(policy, p, m) != Status::Ok) {
            std::printf("REFUSED  policy for %s\n", x.name);
            return 1;
        }
        const std::uint32_t states = 1U << static_cast<std::uint32_t>(x.observation_count);
        std::uint32_t fnv = 2166136261U, asserts = 0;
        for (std::uint32_t local = 0; local < states; ++local) {
            if (rt::instantiate(fw, p, local << static_cast<std::uint32_t>(x.observation_offset)) != Status::Ok) {
                std::printf("REFUSED  instantiate\n");
                return 1;
            }
            const auto v = quorum::arbiter::decide(fw, solver, policy);
            const bool asserted = v.outcome == Outcome::Decided && v.action.valid() && v.action.get() == x.safe_action;
            fnv = (fnv ^ (v.outcome != Outcome::Decided ? 2U : (asserted ? 1U : 0U))) * 16777619U;
            asserts += asserted ? 1U : 0U;
            if (v.steps > max_steps) { max_steps = v.steps; }
        }
        std::printf("DEVICE %s states %u asserts %u fnv1a 0x%08x\n", x.name, states, asserts, fnv);
        const rt::GuardSweep g = rt::guard_sweep(p, m, fw, solver, policy);
        std::printf("GUARD %s pairs %u accepted %u replaced %u weaker %u fnv1a 0x%08x\n", x.name, g.pairs, g.accepted,
                    g.replaced, g.weaker, g.fnv);
        if (g.weaker != 0U || g.faults != 0U) { rc = 1; }
    }
    std::printf("steps  max %d, proven bound %d\n", static_cast<int>(max_steps), static_cast<int>(p.step_bound));
    if (max_steps > p.step_bound) { rc = 1; }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::strcmp(argv[1], "info") != 0 && std::strcmp(argv[1], "sweep") != 0)) {
        std::fprintf(stderr, "usage: qblob info|sweep <casebook.qcb>\n");
        return 2;
    }
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read '%s'\n", argv[2]);
        return 2;
    }
    const std::vector<std::uint8_t> blob(std::istreambuf_iterator<char>(in), {});
    if (blob.size() > rt::kBlobRegionBytes) {
        std::fprintf(stderr, "REFUSED  image is %zu bytes; the device region holds %u\n", blob.size(),
                     static_cast<unsigned>(rt::kBlobRegionBytes));
        return 1;
    }
    if (rt::blob_format(blob.data(), blob.size()) == 2U) { return palette(argv[1], blob); }
    static rt::Casebook cb;
    const rt::BlobStatus st = rt::load(blob.data(), blob.size(), cb);
    if (st != rt::BlobStatus::Ok) {
        std::printf("REFUSED  %s\n", rt::blob_status_name(st));
        return 1;
    }
    if (std::strcmp(argv[1], "info") == 0) {
        std::printf("casebook      %s v%d\nsource        %s\ndate          %s\nwritten by    %s\n", cb.name,
                    static_cast<int>(cb.version), cb.source_document, cb.source_date, cb.source_author);
        std::printf("arguments     %d\nattacks       %d\nobservations  %d\nactions       %d\nchannels      %d\n",
                    static_cast<int>(cb.argument_count), static_cast<int>(cb.edge_count),
                    static_cast<int>(cb.observation_count), static_cast<int>(cb.action_count),
                    static_cast<int>(cb.channel_count));
        std::printf("fingerprint   0x%08x\nstep bound    %d (worst case %d at state 0x%X)\nimage bytes   %zu\n",
                    static_cast<unsigned>(cb.fingerprint), static_cast<int>(cb.step_bound),
                    static_cast<int>(cb.worst_case_steps), static_cast<unsigned>(cb.worst_case_mask), blob.size());
        std::printf("safe action   %s\nidle action   %s\n", rt::name_of(cb, cb.safe_action),
                    rt::name_of(cb, cb.idle_action));
        return 0;
    }
    if (cb.observation_count > rt::kSweepLimit) {
        std::printf("DEVICE %s worst-case state only (%d observations)\n", cb.name,
                    static_cast<int>(cb.observation_count));
        return 0;
    }
    static rt::RtFramework fw;
    static rt::RtSolver solver;
    static rt::RtPolicy policy;
    if (rt::make_policy(policy, cb) != Status::Ok) {
        std::printf("REFUSED  policy\n");
        return 1;
    }
    const std::uint32_t states = 1U << static_cast<std::uint32_t>(cb.observation_count);
    std::uint32_t fnv = 2166136261U, asserts = 0;
    std::int32_t max_steps = 0;
    for (std::uint32_t m = 0; m < states; ++m) {
        if (rt::instantiate(fw, cb, m) != Status::Ok) {
            std::printf("REFUSED  instantiate at state 0x%x\n", m);
            return 1;
        }
        const auto v = quorum::arbiter::decide(fw, solver, policy);
        const bool asserted = v.outcome == Outcome::Decided && v.action.valid() && v.action.get() == cb.safe_action;
        fnv = (fnv ^ (v.outcome != Outcome::Decided ? 2U : (asserted ? 1U : 0U))) * 16777619U;
        asserts += asserted ? 1U : 0U;
        if (v.steps > max_steps) { max_steps = v.steps; }
    }
    std::printf("steps  max %d, proven bound %d\n", static_cast<int>(max_steps), static_cast<int>(cb.step_bound));
    std::printf("DEVICE %s states %u asserts %u fnv1a 0x%08x\n", cb.name, states, asserts, fnv);
    const rt::GuardSweep g = rt::guard_sweep(cb, fw, solver, policy);
    std::printf("GUARD %s pairs %u accepted %u replaced %u weaker %u fnv1a 0x%08x\n", cb.name, g.pairs, g.accepted,
                g.replaced, g.weaker, g.fnv);
    return max_steps <= cb.step_bound && g.weaker == 0U && g.faults == 0U ? 0 : 1;
}
