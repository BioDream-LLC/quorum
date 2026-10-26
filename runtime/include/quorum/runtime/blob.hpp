#ifndef QUORUM_RUNTIME_BLOB_HPP
#define QUORUM_RUNTIME_BLOB_HPP

// The runtime side of `cbc --blob`: load a validated casebook from bytes in flash, with no allocation.
//
// One prebuilt firmware per board, any validated casebook. The loader is a bounds-checked cursor over the format
// documented in casebook/compiler/src/blob.cpp. It refuses, rather than clamps, anything out of range: a blob it
// cannot represent exactly is a blob it will not run.
//
// What the loader proves, in order:
//   1. transport  -- magic, format version, declared length, CRC-32. Catches a truncated or damaged flash write.
//   2. structure  -- every index in range, every count within the validated capacity and within this runtime's.
//   3. identity   -- the structural fingerprint, recomputed from the tables actually loaded, equals the one cbc
//                    wrote. The same arithmetic as the header path's computed_fingerprint().
//
// What it does NOT prove: that the blob was validated. cbc refuses to write a blob without validation, and the
// fingerprint ties the tables to that cbc run, but neither is a signature. Like the header fingerprint, this detects
// accidents, not forgery.
//
// Device code: no heap, no exceptions, nothing from the hosted standard library.

#include <cstddef>
#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/binding/binding.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

namespace quorum::runtime {

// The largest casebook this runtime accepts. Fixed at build time, so RAM use is known before deployment.
// Framework<128, 512> is about 7 KB; everything here fits the Uno R4's 32 KB of RAM with room for the application.
constexpr std::int32_t kMaxArgs = 128;
constexpr std::int32_t kMaxAttacks = 512;
constexpr std::int32_t kMaxActions = 16;
constexpr std::int32_t kMaxClaims = kMaxArgs;
constexpr std::int32_t kMaxGroups = 16;
constexpr std::int32_t kMaxGroupMembers = 64;  ///< across all groups
constexpr std::int32_t kMaxChannels = 16;
constexpr std::int32_t kMaxThresholds = 32;
constexpr std::int32_t kMaxObservations = 32;  ///< one bit each in the presence mask
constexpr std::int32_t kMaxTimers = 8;         ///< timed readings (`reading ... when ...`)

constexpr std::uint32_t kBlobMagic = 0x31424351U;  // "QCB1"
constexpr std::uint32_t kBlobFormat = 1U;
constexpr std::uint32_t kBlobFormatHeld = 3U;  ///< format 1 plus a hold_ms word per threshold
constexpr std::uint32_t kBlobFormatTimed = 4U; ///< format 3 plus the timed-reading table

enum class BlobStatus : std::uint8_t {
    Ok = 0,
    TooShort,             ///< fewer bytes than the header, or than the declared length
    BadMagic,             ///< not a quorum casebook image, or the region is erased
    BadFormat,            ///< a format version this runtime does not read
    BadCrc,               ///< damaged in transit or in flash
    Malformed,            ///< an index or count out of range; the blob does not describe a casebook
    TooLarge,             ///< a valid casebook, bigger than this runtime was built for
    FingerprintMismatch,  ///< the tables are not the ones cbc fingerprinted
};

[[nodiscard]] constexpr const char* blob_status_name(BlobStatus s) noexcept {
    switch (s) {
        case BlobStatus::Ok: return "ok";
        case BlobStatus::TooShort: return "too short";
        case BlobStatus::BadMagic: return "no casebook image (bad magic)";
        case BlobStatus::BadFormat: return "unsupported image format";
        case BlobStatus::BadCrc: return "image damaged (CRC)";
        case BlobStatus::Malformed: return "image malformed";
        case BlobStatus::TooLarge: return "casebook larger than this runtime";
        case BlobStatus::FingerprintMismatch: return "fingerprint mismatch";
    }
    return "unknown";
}

struct Edge {
    std::int32_t from = 0;
    std::int32_t to = 0;
};

/// A loaded casebook. Tables are copied out of flash; strings point into the blob, which must outlive this.
struct Casebook {
    std::uint32_t fingerprint = 0;
    std::int32_t argument_count = 0;
    std::int32_t observation_count = 0;
    std::int32_t max_args = 0;     ///< capacity cbc validated; part of the fingerprint
    std::int32_t max_attacks = 0;
    std::int32_t closed_world = -1;
    std::int32_t idle_action = -1;
    std::int32_t safe_action = -1;
    std::uint32_t worst_case_mask = 0;
    std::int32_t worst_case_steps = 0;
    std::int32_t step_bound = 0;
    std::int32_t version = 0;

    std::int32_t edge_count = 0;
    Edge edges[kMaxAttacks]{};
    std::int32_t action_count = 0;
    std::int32_t actions[kMaxActions]{};
    std::int32_t claim_count = 0;
    std::int32_t claims[kMaxClaims]{};
    std::int32_t group_count = 0;
    std::int32_t group_offset[kMaxGroups + 1]{};
    std::int32_t group_members[kMaxGroupMembers]{};

    std::int32_t channel_count = 0;
    binding::ChannelSpec channels[kMaxChannels]{};
    std::int32_t threshold_count = 0;
    binding::ThresholdSpec thresholds[kMaxThresholds]{};
    std::int32_t timer_count = 0;
    binding::TimerSpec timers[kMaxTimers]{};

    const char* name = "";
    const char* source_document = "";
    const char* source_date = "";
    const char* source_author = "";
    const char* strings = nullptr;  ///< argument names then descriptions, NUL-separated
    std::int32_t strings_size = 0;
};

using RtFramework = solver::Framework<kMaxArgs, kMaxAttacks>;
using RtSolver = solver::Grounded<kMaxArgs>;
using RtPolicy = arbiter::Policy<kMaxActions>;
using RtBinding = binding::Binding<kMaxChannels, kMaxThresholds>;
using RtTimers = binding::Timers<kMaxTimers>;

namespace detail {

class Cursor {
public:
    constexpr Cursor(const std::uint8_t* p, std::size_t n) noexcept : p_(p), n_(n) {}

    [[nodiscard]] bool u32(std::uint32_t& v) noexcept {
        if (at_ > n_ || n_ - at_ < 4U) { return false; }
        v = static_cast<std::uint32_t>(p_[at_]) | (static_cast<std::uint32_t>(p_[at_ + 1U]) << 8) |
            (static_cast<std::uint32_t>(p_[at_ + 2U]) << 16) | (static_cast<std::uint32_t>(p_[at_ + 3U]) << 24);
        at_ += 4U;
        return true;
    }
    [[nodiscard]] bool i32(std::int32_t& v) noexcept {
        std::uint32_t u = 0;
        if (!u32(u)) { return false; }
        v = static_cast<std::int32_t>(u);
        return true;
    }
    /// A count: non-negative and at most `cap`.
    [[nodiscard]] bool count(std::int32_t& v, std::int32_t cap, bool& too_large) noexcept {
        if (!i32(v) || v < 0) { return false; }
        if (v > cap) {
            too_large = true;
            return false;
        }
        return true;
    }
    [[nodiscard]] const std::uint8_t* here() const noexcept { return p_ + at_; }
    [[nodiscard]] bool skip(std::size_t k) noexcept {
        if (at_ > n_ || n_ - at_ < k) { return false; }
        at_ += k;
        return true;
    }
    [[nodiscard]] std::size_t offset() const noexcept { return at_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t at_ = 0;
};

[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) { c = (c & 1U) != 0U ? (c >> 1) ^ 0xEDB88320U : c >> 1; }
    }
    return c ^ 0xFFFFFFFFU;
}

}  // namespace detail

/// The same arithmetic as the generated header's computed_fingerprint(), over the tables actually loaded.
[[nodiscard]] inline std::uint32_t computed_fingerprint(const Casebook& cb) noexcept {
    std::uint32_t h = 2166136261U;
    const auto mix = [&h](std::int32_t v) noexcept {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        for (std::uint32_t byte = 0; byte < 4U; ++byte) {
            h = static_cast<std::uint32_t>(h ^ ((u >> (byte * 8U)) & 0xFFU));
            h = static_cast<std::uint32_t>(h * 16777619U);
        }
    };
    mix(cb.argument_count);
    mix(cb.observation_count);
    mix(cb.max_args);
    mix(cb.max_attacks);
    mix(cb.edge_count);
    for (std::int32_t i = 0; i < cb.edge_count; ++i) {
        mix(cb.edges[i].from);
        mix(cb.edges[i].to);
    }
    mix(cb.action_count);
    for (std::int32_t i = 0; i < cb.action_count; ++i) { mix(cb.actions[i]); }
    // Roles and binding, since 2026-10-11 (see cbc's fingerprint()): a changed fallback or setpoint is a different
    // casebook, and an image carrying one is refused even with a correct CRC.
    mix(cb.closed_world);
    mix(cb.idle_action);
    mix(cb.safe_action);
    mix(cb.channel_count);
    for (std::int32_t i = 0; i < cb.channel_count; ++i) {
        const binding::ChannelSpec& c = cb.channels[i];
        mix(c.valid_lo);
        mix(c.valid_hi);
        mix(static_cast<std::int32_t>(c.stale_after_ms));
        mix(static_cast<std::int32_t>(c.flatline_after_ms));
        mix(c.flatline_epsilon);
        mix(c.unavailable_obs);
        mix(c.flatline_obs);
    }
    mix(cb.threshold_count);
    for (std::int32_t i = 0; i < cb.threshold_count; ++i) {
        const binding::ThresholdSpec& t = cb.thresholds[i];
        mix(t.channel);
        mix(t.observation);
        mix(t.sense == binding::Sense::Above ? 1 : 0);
        mix(t.on);
        mix(t.off);
        mix(static_cast<std::int32_t>(t.hold_ms));
    }
    if (cb.timer_count > 0) {
        mix(cb.timer_count);
        for (std::int32_t i = 0; i < cb.timer_count; ++i) {
            const binding::TimerSpec& t = cb.timers[i];
            mix(static_cast<std::int32_t>(t.kind));
            mix(t.observation);
            mix(t.source);
            mix(t.after);
            mix(static_cast<std::int32_t>(t.window_ms));
            mix(t.times);
        }
    }
    return h;
}

/// Load and check a blob. On anything but Ok, `out` must not be used.
[[nodiscard]] inline BlobStatus load(const std::uint8_t* data, std::size_t size, Casebook& out) noexcept {
    if (data == nullptr || size < 16U) { return BlobStatus::TooShort; }
    detail::Cursor c(data, size);
    std::uint32_t magic = 0, format = 0, total = 0;
    if (!c.u32(magic) || !c.u32(format) || !c.u32(total) || !c.u32(out.fingerprint)) { return BlobStatus::TooShort; }
    if (magic != kBlobMagic) { return BlobStatus::BadMagic; }
    if (format != kBlobFormat && format != kBlobFormatHeld && format != kBlobFormatTimed) { return BlobStatus::BadFormat; }
    const bool timed = format == kBlobFormatTimed;
    const bool held = format == kBlobFormatHeld || timed;
    if (total < 20U || total > size || total % 4U != 0U) { return BlobStatus::TooShort; }
    {
        detail::Cursor tail(data + total - 4U, 4U);
        std::uint32_t crc = 0;
        if (!tail.u32(crc) || crc != detail::crc32(data, total - 4U)) { return BlobStatus::BadCrc; }
    }
    // From here the cursor may only see the bytes the CRC covered.
    c = detail::Cursor(data, total - 4U);
    if (!c.skip(16U)) { return BlobStatus::TooShort; }

    bool big = false;
    const auto fail = [&big]() noexcept { return big ? BlobStatus::TooLarge : BlobStatus::Malformed; };
    std::uint32_t wc_mask = 0;
    if (!c.count(out.argument_count, kMaxArgs, big) || !c.count(out.observation_count, kMaxObservations, big) ||
        !c.i32(out.max_args) || !c.i32(out.max_attacks) || !c.i32(out.closed_world) || !c.i32(out.idle_action) ||
        !c.i32(out.safe_action) || !c.u32(wc_mask) || !c.i32(out.worst_case_steps) || !c.i32(out.step_bound) ||
        !c.i32(out.version)) {
        return fail();
    }
    out.worst_case_mask = wc_mask;
    const std::int32_t n = out.argument_count;
    const auto arg = [n](std::int32_t a) noexcept { return a >= 0 && a < n; };
    if (out.max_args < n || out.max_args > kMaxArgs) { return out.max_args > kMaxArgs ? BlobStatus::TooLarge : fail(); }
    if (out.max_attacks > kMaxAttacks) { return BlobStatus::TooLarge; }
    if (out.observation_count > n || !arg(out.closed_world)) { return BlobStatus::Malformed; }

    if (!c.count(out.edge_count, kMaxAttacks, big)) { return fail(); }
    if (out.edge_count + out.observation_count > out.max_attacks) { return BlobStatus::Malformed; }
    for (std::int32_t i = 0; i < out.edge_count; ++i) {
        Edge& e = out.edges[i];
        if (!c.i32(e.from) || !c.i32(e.to) || !arg(e.from) || !arg(e.to)) { return BlobStatus::Malformed; }
    }
    if (!c.count(out.action_count, kMaxActions, big) || out.action_count == 0) { return fail(); }
    bool idle_found = false, safe_found = false;
    for (std::int32_t i = 0; i < out.action_count; ++i) {
        if (!c.i32(out.actions[i]) || !arg(out.actions[i])) { return BlobStatus::Malformed; }
        idle_found = idle_found || out.actions[i] == out.idle_action;
        safe_found = safe_found || out.actions[i] == out.safe_action;
    }
    if (!idle_found || !safe_found) { return BlobStatus::Malformed; }
    if (!c.count(out.claim_count, kMaxClaims, big)) { return fail(); }
    for (std::int32_t i = 0; i < out.claim_count; ++i) {
        if (!c.i32(out.claims[i]) || !arg(out.claims[i])) { return BlobStatus::Malformed; }
    }
    if (!c.count(out.group_count, kMaxGroups, big)) { return fail(); }
    out.group_offset[0] = 0;
    for (std::int32_t g = 0; g < out.group_count; ++g) {
        std::int32_t k = 0;
        if (!c.count(k, kMaxGroupMembers - out.group_offset[g], big)) { return fail(); }
        for (std::int32_t j = 0; j < k; ++j) {
            std::int32_t& m = out.group_members[out.group_offset[g] + j];
            if (!c.i32(m) || !arg(m)) { return BlobStatus::Malformed; }
        }
        out.group_offset[g + 1] = out.group_offset[g] + k;
    }

    const auto obs_or_none = [&out](std::int32_t o) noexcept { return o == -1 || (o >= 0 && o < out.observation_count); };
    if (!c.count(out.channel_count, kMaxChannels, big)) { return fail(); }
    for (std::int32_t i = 0; i < out.channel_count; ++i) {
        binding::ChannelSpec& s = out.channels[i];
        if (!c.i32(s.valid_lo) || !c.i32(s.valid_hi) || !c.u32(s.stale_after_ms) || !c.u32(s.flatline_after_ms) ||
            !c.i32(s.flatline_epsilon) || !c.i32(s.unavailable_obs) || !c.i32(s.flatline_obs) ||
            !obs_or_none(s.unavailable_obs) || !obs_or_none(s.flatline_obs)) {
            return BlobStatus::Malformed;
        }
    }
    if (!c.count(out.threshold_count, kMaxThresholds, big)) { return fail(); }
    for (std::int32_t i = 0; i < out.threshold_count; ++i) {
        binding::ThresholdSpec& t = out.thresholds[i];
        std::uint32_t sense = 0;
        t.hold_ms = 0U;
        if (!c.i32(t.channel) || !c.i32(t.observation) || !c.u32(sense) || !c.i32(t.on) || !c.i32(t.off) ||
            (held && !c.u32(t.hold_ms)) || t.channel < 0 || t.channel >= out.channel_count || t.observation < 0 ||
            t.observation >= out.observation_count || sense > 1U) {
            return BlobStatus::Malformed;
        }
        t.sense = sense == 0U ? binding::Sense::Above : binding::Sense::Below;
    }
    out.timer_count = 0;
    if (timed) {
        if (!c.count(out.timer_count, kMaxTimers, big) || out.timer_count == 0) { return fail(); }
        for (std::int32_t i = 0; i < out.timer_count; ++i) {
            binding::TimerSpec& t = out.timers[i];
            std::uint32_t kind = 0;
            if (!c.u32(kind) || !c.i32(t.observation) || !c.i32(t.source) || !c.i32(t.after) || !c.u32(t.window_ms) ||
                !c.i32(t.times) || kind > 2U) {
                return BlobStatus::Malformed;
            }
            t.kind = static_cast<binding::TimerKind>(kind);
        }
        // The same rules the device will apply at configure time, checked now so a bad table is a load failure.
        if (RtTimers::validate(out.timers, out.timer_count, out.observation_count) != binding::Status::Ok) {
            return BlobStatus::Malformed;
        }
    }

    // Strings: 4 provenance strings, then n names, then n descriptions, each NUL-terminated.
    std::int32_t sbytes = 0;
    // The string section runs exactly to the CRC, and its last byte is a terminator, so no scan can run off the end.
    if (!c.i32(sbytes) || sbytes <= 0 || c.offset() + static_cast<std::size_t>(sbytes) != total - 4U ||
        c.here()[sbytes - 1] != 0U) {
        return BlobStatus::Malformed;
    }
    const char* s = reinterpret_cast<const char*>(c.here());
    const std::int32_t need = 4 + 2 * n;
    std::int32_t seen = 0;
    for (std::int32_t i = 0; i < sbytes && seen < need; ++i) {
        if (s[i] == '\0') { ++seen; }
    }
    if (seen < need) { return BlobStatus::Malformed; }
    const char* p = s;
    const auto next = [&p]() noexcept {
        const char* here = p;
        while (*p != '\0') { ++p; }
        ++p;
        return here;
    };
    out.name = next();
    out.source_document = next();
    out.source_date = next();
    out.source_author = next();
    out.strings = p;
    out.strings_size = sbytes - static_cast<std::int32_t>(p - s);

    if (computed_fingerprint(out) != out.fingerprint) { return BlobStatus::FingerprintMismatch; }
    return BlobStatus::Ok;
}

namespace detail {
/// The k-th NUL-terminated string after `p`. load() proved enough terminators exist.
[[nodiscard]] inline const char* nth(const char* p, std::int32_t k) noexcept {
    for (std::int32_t i = 0; i < k; ++i) {
        while (*p != '\0') { ++p; }
        ++p;
    }
    return p;
}
}  // namespace detail

/// Argument name, or "" if out of range. Linear in the index: for reports, not for the control loop.
[[nodiscard]] inline const char* name_of(const Casebook& cb, std::int32_t a) noexcept {
    if (a < 0 || a >= cb.argument_count || cb.strings == nullptr) { return ""; }
    return detail::nth(cb.strings, a);
}

/// Argument description, or "" if out of range.
[[nodiscard]] inline const char* description_of(const Casebook& cb, std::int32_t a) noexcept {
    if (a < 0 || a >= cb.argument_count || cb.strings == nullptr) { return ""; }
    return detail::nth(cb.strings, cb.argument_count + a);
}

/// As the header's make_policy(): declare every action in order, the safe action as fallback.
[[nodiscard]] inline solver::Status make_policy(RtPolicy& p, const Casebook& cb) noexcept {
    for (std::int32_t i = 0; i < cb.action_count; ++i) {
        const solver::Status s = p.declare_action(solver::Arg{cb.actions[i]});
        if (s != solver::Status::Ok) { return s; }
    }
    return p.set_fallback(solver::Arg{cb.safe_action});
}

/// As the header's instantiate(): the fixed edges in order, then a closed-world attack on every absent observation.
[[nodiscard]] inline solver::Status instantiate(RtFramework& fw, const Casebook& cb, std::uint32_t present) noexcept {
    fw.reset();
    solver::Status s = fw.declare(cb.argument_count);
    if (s != solver::Status::Ok) { return s; }
    for (std::int32_t i = 0; i < cb.edge_count; ++i) {
        s = fw.add_attack(solver::Arg{cb.edges[i].from}, solver::Arg{cb.edges[i].to});
        if (s != solver::Status::Ok) { return s; }
    }
    for (std::int32_t a = 0; a < cb.observation_count; ++a) {
        if ((present & (1U << static_cast<std::uint32_t>(a))) == 0U) {
            s = fw.add_attack(solver::Arg{cb.closed_world}, solver::Arg{a});
            if (s != solver::Status::Ok) { return s; }
        }
    }
    return fw.finalize();
}

/// As the header's make_timers(). Only meaningful when the casebook declares timed readings.
[[nodiscard]] inline binding::Status make_timers(RtTimers& t, const Casebook& cb) noexcept {
    return t.configure(cb.timers, cb.timer_count, cb.observation_count);
}

/// As the header's make_binding(). Only meaningful when the casebook declares channels.
[[nodiscard]] inline binding::Status make_binding(RtBinding& b, const Casebook& cb) noexcept {
    return b.configure(cb.channels, cb.channel_count, cb.thresholds, cb.threshold_count);
}

// ── format 2: a palette ───────────────────────────────────────────────────────────────────────────────────────────
//
// Several casebooks on one device: one combined framework, solved once per cycle, and one policy per member applied
// to that labelling (cbc's PaletteMember; see emit_palette_blob for the layout). Kept apart from the single-casebook
// loader above so that path is unchanged. No binding layer: a palette takes the presence mask directly.

constexpr std::int32_t kMaxMembers = 4;

struct Member {
    std::int32_t observation_offset = 0;
    std::int32_t observation_count = 0;
    std::int32_t closed_world = -1;
    std::int32_t idle_action = -1;
    std::int32_t safe_action = -1;
    std::int32_t action_count = 0;
    std::int32_t actions[kMaxActions]{};
    const char* name = "";
};

struct Palette {
    std::uint32_t fingerprint = 0;
    std::int32_t argument_count = 0;
    std::int32_t observation_count = 0;
    std::int32_t max_args = 0;
    std::int32_t max_attacks = 0;
    std::int32_t step_bound = 0;
    std::int32_t edge_count = 0;
    Edge edges[kMaxAttacks]{};
    std::int32_t member_count = 0;
    Member members[kMaxMembers]{};
    const char* strings = nullptr;  ///< argument names then descriptions
};

/// The format of the image at `data`, or 0 if it is not a quorum image. Lets firmware choose a loader.
[[nodiscard]] inline std::uint32_t blob_format(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 8U) { return 0U; }
    detail::Cursor c(data, size);
    std::uint32_t magic = 0, format = 0;
    if (!c.u32(magic) || !c.u32(format) || magic != kBlobMagic) { return 0U; }
    return format;
}

/// The same arithmetic as cbc's palette_fingerprint(), over the tables actually loaded.
[[nodiscard]] inline std::uint32_t computed_fingerprint(const Palette& p) noexcept {
    std::uint32_t h = 2166136261U;
    const auto mix = [&h](std::int32_t v) noexcept {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        for (std::uint32_t byte = 0; byte < 4U; ++byte) {
            h = static_cast<std::uint32_t>(h ^ ((u >> (byte * 8U)) & 0xFFU));
            h = static_cast<std::uint32_t>(h * 16777619U);
        }
    };
    mix(p.argument_count);
    mix(p.observation_count);
    mix(p.max_args);
    mix(p.max_attacks);
    mix(p.edge_count);
    for (std::int32_t i = 0; i < p.edge_count; ++i) {
        mix(p.edges[i].from);
        mix(p.edges[i].to);
    }
    mix(p.member_count);
    for (std::int32_t m = 0; m < p.member_count; ++m) {
        const Member& x = p.members[m];
        mix(x.observation_offset);
        mix(x.observation_count);
        mix(x.closed_world);
        mix(x.idle_action);
        mix(x.safe_action);
        mix(x.action_count);
        for (std::int32_t i = 0; i < x.action_count; ++i) { mix(x.actions[i]); }
    }
    return h;
}

/// Load and check a palette image: the same three checks as load(), plus that the members' observation slices are
/// disjoint, in order and cover the combined observations exactly.
[[nodiscard]] inline BlobStatus load_palette(const std::uint8_t* data, std::size_t size, Palette& out) noexcept {
    if (data == nullptr || size < 16U) { return BlobStatus::TooShort; }
    detail::Cursor c(data, size);
    std::uint32_t magic = 0, format = 0, total = 0;
    if (!c.u32(magic) || !c.u32(format) || !c.u32(total) || !c.u32(out.fingerprint)) { return BlobStatus::TooShort; }
    if (magic != kBlobMagic) { return BlobStatus::BadMagic; }
    if (format != 2U) { return BlobStatus::BadFormat; }
    if (total < 20U || total > size || total % 4U != 0U) { return BlobStatus::TooShort; }
    {
        detail::Cursor tail(data + total - 4U, 4U);
        std::uint32_t crc = 0;
        if (!tail.u32(crc) || crc != detail::crc32(data, total - 4U)) { return BlobStatus::BadCrc; }
    }
    c = detail::Cursor(data, total - 4U);
    if (!c.skip(16U)) { return BlobStatus::TooShort; }
    bool big = false;
    const auto fail = [&big]() noexcept { return big ? BlobStatus::TooLarge : BlobStatus::Malformed; };
    if (!c.count(out.argument_count, kMaxArgs, big) || !c.count(out.observation_count, kMaxObservations, big) ||
        !c.i32(out.max_args) || !c.i32(out.max_attacks) || !c.i32(out.step_bound) ||
        !c.count(out.member_count, kMaxMembers, big) || out.member_count == 0) {
        return fail();
    }
    const std::int32_t n = out.argument_count;
    const auto arg = [n](std::int32_t a) noexcept { return a >= 0 && a < n; };
    if (out.max_args < n || out.max_args > kMaxArgs) { return out.max_args > kMaxArgs ? BlobStatus::TooLarge : fail(); }
    if (out.max_attacks > kMaxAttacks) { return BlobStatus::TooLarge; }
    if (out.observation_count > n) { return BlobStatus::Malformed; }
    if (!c.count(out.edge_count, kMaxAttacks, big)) { return fail(); }
    if (out.edge_count + out.observation_count > out.max_attacks) { return BlobStatus::Malformed; }
    for (std::int32_t i = 0; i < out.edge_count; ++i) {
        Edge& e = out.edges[i];
        if (!c.i32(e.from) || !c.i32(e.to) || !arg(e.from) || !arg(e.to)) { return BlobStatus::Malformed; }
    }
    std::int32_t next_obs = 0;
    for (std::int32_t m = 0; m < out.member_count; ++m) {
        Member& x = out.members[m];
        if (!c.i32(x.observation_offset) || !c.i32(x.observation_count) || !c.i32(x.closed_world) ||
            !c.i32(x.idle_action) || !c.i32(x.safe_action) || !c.count(x.action_count, kMaxActions, big)) {
            return fail();
        }
        if (x.observation_offset != next_obs || x.observation_count < 0 ||
            x.observation_offset + x.observation_count > out.observation_count || !arg(x.closed_world) ||
            x.action_count == 0) {
            return BlobStatus::Malformed;
        }
        next_obs = x.observation_offset + x.observation_count;
        bool idle = false, safe = false;
        for (std::int32_t i = 0; i < x.action_count; ++i) {
            if (!c.i32(x.actions[i]) || !arg(x.actions[i])) { return BlobStatus::Malformed; }
            idle = idle || x.actions[i] == x.idle_action;
            safe = safe || x.actions[i] == x.safe_action;
        }
        if (!idle || !safe) { return BlobStatus::Malformed; }
    }
    if (next_obs != out.observation_count) { return BlobStatus::Malformed; }
    std::int32_t sbytes = 0;
    if (!c.i32(sbytes) || sbytes <= 0 || c.offset() + static_cast<std::size_t>(sbytes) != total - 4U ||
        c.here()[sbytes - 1] != 0U) {
        return BlobStatus::Malformed;
    }
    const char* s = reinterpret_cast<const char*>(c.here());
    const std::int32_t need = out.member_count + 2 * n;
    std::int32_t seen = 0;
    for (std::int32_t i = 0; i < sbytes && seen < need; ++i) {
        if (s[i] == '\0') { ++seen; }
    }
    if (seen < need) { return BlobStatus::Malformed; }
    for (std::int32_t m = 0; m < out.member_count; ++m) { out.members[m].name = detail::nth(s, m); }
    out.strings = detail::nth(s, out.member_count);
    if (computed_fingerprint(out) != out.fingerprint) { return BlobStatus::FingerprintMismatch; }
    return BlobStatus::Ok;
}

/// As the palette header's instantiate(): fixed edges, then each member's closed world over its own slice.
[[nodiscard]] inline solver::Status instantiate(RtFramework& fw, const Palette& p, std::uint32_t present) noexcept {
    fw.reset();
    solver::Status s = fw.declare(p.argument_count);
    if (s != solver::Status::Ok) { return s; }
    for (std::int32_t i = 0; i < p.edge_count; ++i) {
        s = fw.add_attack(solver::Arg{p.edges[i].from}, solver::Arg{p.edges[i].to});
        if (s != solver::Status::Ok) { return s; }
    }
    for (std::int32_t m = 0; m < p.member_count; ++m) {
        const Member& x = p.members[m];
        for (std::int32_t a = 0; a < x.observation_count; ++a) {
            const std::int32_t o = x.observation_offset + a;
            if ((present & (1U << static_cast<std::uint32_t>(o))) == 0U) {
                s = fw.add_attack(solver::Arg{x.closed_world}, solver::Arg{o});
                if (s != solver::Status::Ok) { return s; }
            }
        }
    }
    return fw.finalize();
}

/// As the palette header's per-member make_policy().
[[nodiscard]] inline solver::Status make_policy(RtPolicy& pol, const Palette& p, std::int32_t member) noexcept {
    if (member < 0 || member >= p.member_count) { return solver::Status::BadArgument; }
    const Member& x = p.members[member];
    for (std::int32_t i = 0; i < x.action_count; ++i) {
        const solver::Status s = pol.declare_action(solver::Arg{x.actions[i]});
        if (s != solver::Status::Ok) { return s; }
    }
    return pol.set_fallback(solver::Arg{x.safe_action});
}

}  // namespace quorum::runtime

#endif  // QUORUM_RUNTIME_BLOB_HPP
