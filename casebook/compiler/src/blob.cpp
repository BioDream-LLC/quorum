// emit_blob -- the validated casebook as a data image, for a prebuilt runtime firmware.
//
// WHY THIS EXISTS
//
// The header path compiles the casebook INTO the firmware, so every casebook needs a cross-compiler. A blob lets one
// prebuilt runtime per board load any validated casebook from a reserved flash region, which is what makes a desktop
// tool possible: validate, then write the image, with no toolchain on the user's machine.
//
// The blob carries exactly the tables the header carries -- the same edges in the same order, the same actions, the
// same policy, the same binding specs -- and the same structural fingerprint, computed by the same function. The
// runtime recomputes that fingerprint from the tables it actually loaded and refuses to run on a mismatch, exactly as
// header firmware does. The CRC beneath it is only transport integrity; the fingerprint is the identity.
//
// Equivalence with the header path is not argued, it is tested: casebook_blob_equivalence decides every state of
// every reference casebook through both paths and requires identical outcomes, actions and step counts.
//
// FORMAT (version 1). Little-endian 32-bit words. The layout is fixed, so the reader in
// runtime/include/quorum/runtime/blob.hpp is a bounds-checked cursor with no allocation.
//
//   magic 'QCB1', format version, total length in bytes, fingerprint
//   argument count, observation count, max args, max attacks, closed-world argument,
//   idle action, safe action, worst-case mask, worst-case steps, step bound, casebook version
//   edge count,      then (from, to) per edge
//   action count,    then the actions
//   claim count,     then the claims
//   group count,     then per group: member count, members
//   channel count,   then 7 words per channel (valid_lo, valid_hi, stale_after_ms, flatline_after_ms,
//                    flatline_epsilon, unavailable_obs, flatline_obs)
//   threshold count, then 5 words per threshold (channel, observation, sense 0=above 1=below, on, off)
//                    -- 6 in FORMAT 3, the sixth being hold_ms (`asserts x above 90 release 85 for 5s`)
//   string bytes,    then NUL-terminated strings padded to 4: casebook name, source document, source date,
//                    source author, then every argument name, then every argument description
//   CRC-32 (IEEE) of every preceding byte
//
// FORMAT 3 is format 1 with that sixth threshold word, and nothing else. cbc writes it only when some threshold has a
// hold, so every casebook without one still produces byte-identical format-1 images -- every image measured on a board
// so far stays valid, and older firmware refuses a held image (BadFormat) rather than silently dropping the hold.
//
// FORMAT 2: a PALETTE, several casebooks on one device (cbc --palette --blob). One combined framework, one policy
// per member, exactly as emit_palette_header. No binding layer: like the palette header, a palette image takes the
// presence mask directly, so it cannot tell a dead sensor from a quiet one -- the members' channels do not survive
// the merge. Layout:
//
//   magic 'QCB1', format 2, total length in bytes, fingerprint
//   argument count, observation count, max args, max attacks, step bound, member count
//   edge count,   then (from, to) per edge
//   per member:   observation offset, observation count, closed-world argument, idle action, safe action,
//                 action count, then the actions          (all indices into the COMBINED framework)
//   string bytes, then NUL-terminated strings padded to 4: one name per member, then every argument name, then
//                 every argument description
//   CRC-32
//
// The fingerprint covers counts, capacities, every edge in order, and each member's slice, closed world and actions,
// in that order (palette_fingerprint below; the loader recomputes the same).

#include <cstdint>
#include <string>
#include <vector>

#include "quorum/cbc/cbc.hpp"

namespace quorum::cbc {

namespace {

constexpr std::uint32_t kBlobMagic = 0x31424351U;  // "QCB1" as bytes in memory
constexpr std::uint32_t kBlobFormat = 1U;
constexpr std::uint32_t kBlobFormatHeld = 3U;

void put(std::string& o, std::uint32_t v) {
    for (int b = 0; b < 4; ++b) { o.push_back(static_cast<char>((v >> (8 * b)) & 0xFFU)); }
}
void put(std::string& o, std::int32_t v) { put(o, static_cast<std::uint32_t>(v)); }
void put(std::string& o, std::size_t v) { put(o, static_cast<std::uint32_t>(v)); }

[[nodiscard]] std::uint32_t crc32(const std::string& s) {
    std::uint32_t c = 0xFFFFFFFFU;
    for (const char ch : s) {
        c ^= static_cast<std::uint8_t>(ch);
        for (int k = 0; k < 8; ++k) { c = (c & 1U) != 0U ? (c >> 1) ^ 0xEDB88320U : c >> 1; }
    }
    return c ^ 0xFFFFFFFFU;
}

}  // namespace

std::string emit_blob(const Lowered& lo) {
    bool held = false;
    for (const LoweredThreshold& t : lo.thresholds) { held = held || t.hold_ms != 0U; }
    std::string o;
    put(o, kBlobMagic);
    put(o, held ? kBlobFormatHeld : kBlobFormat);
    put(o, std::uint32_t{0});  // total length, patched below
    put(o, fingerprint(lo));

    put(o, lo.count());
    put(o, lo.observation_count);
    put(o, lo.max_args);
    put(o, lo.max_attacks);
    put(o, lo.closed_world);
    put(o, lo.idle_action);
    put(o, lo.safe_action);
    put(o, lo.worst_case_mask);
    put(o, lo.worst_case_steps);
    put(o, 2 * lo.count() + 2 * lo.max_attacks);  // the proven bound, as kStepBound
    put(o, lo.version);

    put(o, lo.edges.size());
    for (const LoweredEdge& e : lo.edges) {
        put(o, e.from);
        put(o, e.to);
    }
    put(o, lo.actions.size());
    for (const std::int32_t a : lo.actions) { put(o, a); }

    std::vector<std::int32_t> claims;
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        if (lo.arguments[static_cast<std::size_t>(i)].role == Role::Claim) { claims.push_back(i); }
    }
    put(o, claims.size());
    for (const std::int32_t c : claims) { put(o, c); }

    put(o, lo.exclusive_groups.size());
    for (const auto& g : lo.exclusive_groups) {
        put(o, g.size());
        for (const std::int32_t m : g) { put(o, m); }
    }

    put(o, lo.channels.size());
    for (const LoweredChannel& c : lo.channels) {
        put(o, c.valid_lo);
        put(o, c.valid_hi);
        put(o, c.stale_after_ms);
        put(o, c.flatline_after_ms);
        put(o, c.flatline_epsilon);
        put(o, c.unavailable_obs);
        put(o, c.flatline_obs);
    }
    put(o, lo.thresholds.size());
    for (const LoweredThreshold& t : lo.thresholds) {
        put(o, t.channel);
        put(o, t.observation);
        put(o, t.above ? std::uint32_t{0} : std::uint32_t{1});
        put(o, t.on);
        put(o, t.off);
        if (held) { put(o, t.hold_ms); }
    }

    std::string strings;
    const auto add = [&strings](const std::string& s) {
        strings += s;
        strings.push_back('\0');
    };
    add(lo.casebook);
    add(lo.provenance.document);
    add(lo.provenance.date);
    add(lo.provenance.author);
    for (const LoweredArgument& a : lo.arguments) { add(a.name); }
    for (const LoweredArgument& a : lo.arguments) { add(a.description); }
    while (strings.size() % 4U != 0U) { strings.push_back('\0'); }
    put(o, strings.size());
    o += strings;

    const std::uint32_t total = static_cast<std::uint32_t>(o.size() + 4U);
    for (int b = 0; b < 4; ++b) { o[static_cast<std::size_t>(8 + b)] = static_cast<char>((total >> (8 * b)) & 0xFFU); }
    put(o, crc32(o));
    return o;
}

std::uint32_t palette_fingerprint(const Palette& p) {
    std::uint32_t h = 2166136261U;
    const auto mix = [&h](std::int64_t v64) {
        const std::uint32_t u = static_cast<std::uint32_t>(v64);
        for (int b = 0; b < 4; ++b) {
            h ^= (u >> (8 * b)) & 0xFFU;
            h *= 16777619U;
        }
    };
    const Lowered& a = p.all;
    mix(a.count());
    mix(a.observation_count);
    mix(a.max_args);
    mix(a.max_attacks);
    mix(static_cast<std::int64_t>(a.edges.size()));
    for (const LoweredEdge& e : a.edges) {
        mix(e.from);
        mix(e.to);
    }
    mix(static_cast<std::int64_t>(p.members.size()));
    for (const PaletteMember& m : p.members) {
        mix(m.observation_offset);
        mix(m.observation_count);
        mix(m.closed_world);
        mix(m.idle_action);  // roles, since 2026-10-11
        mix(m.safe_action);
        mix(static_cast<std::int64_t>(m.actions.size()));
        for (const std::int32_t x : m.actions) { mix(x); }
    }
    return h;
}

std::string emit_palette_blob(const Palette& p) {
    const Lowered& a = p.all;
    std::string o;
    put(o, kBlobMagic);
    put(o, std::uint32_t{2});
    put(o, std::uint32_t{0});
    put(o, palette_fingerprint(p));
    put(o, a.count());
    put(o, a.observation_count);
    put(o, a.max_args);
    put(o, a.max_attacks);
    put(o, 2 * a.count() + 2 * a.max_attacks);
    put(o, p.members.size());
    put(o, a.edges.size());
    for (const LoweredEdge& e : a.edges) {
        put(o, e.from);
        put(o, e.to);
    }
    for (const PaletteMember& m : p.members) {
        put(o, m.observation_offset);
        put(o, m.observation_count);
        put(o, m.closed_world);
        put(o, m.idle_action);
        put(o, m.safe_action);
        put(o, m.actions.size());
        for (const std::int32_t x : m.actions) { put(o, x); }
    }
    std::string strings;
    const auto add = [&strings](const std::string& t) {
        strings += t;
        strings.push_back('\0');
    };
    for (const PaletteMember& m : p.members) { add(m.casebook); }
    for (const LoweredArgument& x : a.arguments) { add(x.name); }
    for (const LoweredArgument& x : a.arguments) { add(x.description); }
    while (strings.size() % 4U != 0U) { strings.push_back('\0'); }
    put(o, strings.size());
    o += strings;
    const std::uint32_t total = static_cast<std::uint32_t>(o.size() + 4U);
    for (int b = 0; b < 4; ++b) { o[static_cast<std::size_t>(8 + b)] = static_cast<char>((total >> (8 * b)) & 0xFFU); }
    put(o, crc32(o));
    return o;
}

}  // namespace quorum::cbc
