#pragma once

// Search-based argumentation semantics: complete, preferred and stable extensions.
//
// This is the ADVISORY tier, and it is host-only by design. It may allocate, it may recurse, it has no WCET, and it
// must never run inside a safety function. That is the whole reason the split exists: preferred and stable semantics
// are NP-hard, so no bound can be stated for them, and an unbounded computation cannot be a safety function.
//
// ── Why the safety tier can rely on this without trusting it ────────────────
//
// The grounded labelling is the LEAST complete labelling. Two consequences carry the architecture:
//
//   1. Every argument grounded labels In is In in every complete extension, and every argument it labels Out is Out in
//      every one. So the microcontroller's verdict can never CONTRADICT this tier -- only be less committal.
//   2. Therefore every complete extension E satisfies  grounded_in  ⊆  E  ⊆  grounded_in ∪ grounded_undecided.
//
// The second is not just reassurance, it is the algorithm. Search is confined to the grounded-undecided arguments,
// which across every state of every casebook here is at most 7 (see test_undecided_profile). NP-hardness is a
// statement about worst cases over all graphs, not about these graphs.
//
// That matters because an advisory tier is only worth having if it is EXACT. A tier that might have missed an
// extension cannot support any claim about the safety tier, and "probably no other reading of the plant exists" is not
// something to tell a control room. So this enumerates exhaustively and refuses to run when it cannot.

#include <cstdint>
#include <vector>

// A build-time interlock, not a comment.
//
// This header compiles perfectly well for cortex-m33 -- newlib provides <vector>, so nothing stops someone including
// it in firmware and getting a heap-allocating, unbounded NP-hard search inside a safety function. That is the single
// worst mistake available in this codebase, and it would link cleanly.
//
// The whole product argument is that the device runs only what has a proven bound. So refuse the build on a bare-metal
// ARM target. Anyone who genuinely wants this on an MCU has to define the macro and thereby state that they know the
// safety function no longer has a WCET.
#if defined(__ARM_ARCH) && !defined(__linux__) && !defined(QUORUM_ADVISORY_I_ACCEPT_NO_WCET)
#error "quorum/advisory is host-only: preferred and stable semantics are NP-hard and allocate, so they have no WCET \
and cannot be part of a safety function. Run this tier on Linux. To override deliberately, define \
QUORUM_ADVISORY_I_ACCEPT_NO_WCET."
#endif

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

namespace quorum::advisory {

using solver::Arg;
using solver::Label;

/// Why an enumeration did not produce an answer. Never a silent partial result.
enum class Status : std::uint8_t {
    Ok = 0,
    /// The grounded solve failed, so the search space is unknown.
    GroundedFailed,
    /// The undecided set exceeds `kMaxUndecided`. Reported rather than truncated: a partial enumeration presented as
    /// complete is the one failure mode that would make this tier actively harmful.
    TooManyUndecided,
};

[[nodiscard]] inline const char* status_name(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::GroundedFailed: return "GroundedFailed";
        case Status::TooManyUndecided: return "TooManyUndecided";
    }
    return "Unknown";
}

/// Subset enumeration is 2^k, so k has a hard ceiling. 24 is ~16.7M subsets, a few seconds; well past the measured
/// maximum of 7 and still far short of anything that would hang.
constexpr std::int32_t kMaxUndecided = 24;

/// One extension: the set of accepted arguments.
struct Extension {
    std::vector<bool> accepted;
    /// True when the extension leaves nothing undecided, i.e. it is also stable.
    bool stable = false;

    [[nodiscard]] std::int32_t size() const noexcept {
        std::int32_t n = 0;
        for (const bool a : accepted) {
            if (a) { ++n; }
        }
        return n;
    }

    [[nodiscard]] bool contains(Arg a) const noexcept {
        const std::size_t i = static_cast<std::size_t>(a.get());
        return i < accepted.size() && accepted[i];
    }
};

/// The result of asking the advisory tier about one state of one casebook.
struct Analysis {
    Status status = Status::Ok;

    /// Every preferred extension: the maximal complete ones. Never empty on success -- every framework has at least
    /// one, and the grounded extension is contained in all of them.
    std::vector<Extension> preferred;

    /// Every stable extension. May legitimately be empty: some frameworks have none, which is exactly why stable
    /// semantics alone is unsafe as a decision procedure and grounded is not.
    std::vector<Extension> stable;

    std::int32_t undecided_count = 0;
    std::int32_t complete_count = 0;
    /// Subsets actually examined. Reported so the cost is visible rather than assumed.
    std::int64_t subsets_examined = 0;

    /// True when exactly one preferred extension exists, so the advisory tier has a single answer to give.
    [[nodiscard]] bool unique() const noexcept { return preferred.size() == 1; }
};

namespace detail {

/// Is `set` conflict-free, i.e. does no member attack another?
template <std::int32_t MA, std::int32_t MT>
[[nodiscard]] inline bool conflict_free(const solver::Framework<MA, MT>& fw, const std::vector<bool>& set) noexcept {
    const std::int32_t n = fw.argument_count();
    for (std::int32_t a = 0; a < n; ++a) {
        if (!set[static_cast<std::size_t>(a)]) { continue; }
        const std::int32_t end = fw.out_end(Arg{a});
        for (std::int32_t k = fw.out_begin(Arg{a}); k < end; ++k) {
            if (set[static_cast<std::size_t>(fw.target(k).get())]) { return false; }
        }
    }
    return true;
}

/// Does `set` attack `a`?
template <std::int32_t MA, std::int32_t MT>
[[nodiscard]] inline bool attacks(const solver::Framework<MA, MT>& fw, const std::vector<bool>& set,
                                  std::int32_t a) noexcept {
    const std::int32_t n = fw.argument_count();
    for (std::int32_t b = 0; b < n; ++b) {
        if (!set[static_cast<std::size_t>(b)]) { continue; }
        const std::int32_t end = fw.out_end(Arg{b});
        for (std::int32_t k = fw.out_begin(Arg{b}); k < end; ++k) {
            if (fw.target(k).get() == a) { return true; }
        }
    }
    return false;
}

/// Does `set` defend `a`, i.e. attack every attacker of `a`?
template <std::int32_t MA, std::int32_t MT>
[[nodiscard]] inline bool defends(const solver::Framework<MA, MT>& fw, const std::vector<bool>& set,
                                  std::int32_t a) noexcept {
    const std::int32_t n = fw.argument_count();
    for (std::int32_t b = 0; b < n; ++b) {
        const std::int32_t end = fw.out_end(Arg{b});
        for (std::int32_t k = fw.out_begin(Arg{b}); k < end; ++k) {
            if (fw.target(k).get() != a) { continue; }
            // b attacks a; the set must attack b back.
            if (!attacks(fw, set, b)) { return false; }
        }
    }
    return true;
}

/// Is `set` a complete extension: conflict-free, admissible, and containing everything it defends?
template <std::int32_t MA, std::int32_t MT>
[[nodiscard]] inline bool is_complete(const solver::Framework<MA, MT>& fw, const std::vector<bool>& set) noexcept {
    if (!conflict_free(fw, set)) { return false; }
    const std::int32_t n = fw.argument_count();
    for (std::int32_t a = 0; a < n; ++a) {
        const bool in = set[static_cast<std::size_t>(a)];
        const bool defended = defends(fw, set, a);
        // Admissible: every member is defended. Complete: every defended argument is a member.
        if (in && !defended) { return false; }
        if (!in && defended) { return false; }
    }
    return true;
}

/// Is `set` stable: conflict-free and attacking everything outside it?
template <std::int32_t MA, std::int32_t MT>
[[nodiscard]] inline bool is_stable(const solver::Framework<MA, MT>& fw, const std::vector<bool>& set) noexcept {
    if (!conflict_free(fw, set)) { return false; }
    const std::int32_t n = fw.argument_count();
    for (std::int32_t a = 0; a < n; ++a) {
        if (set[static_cast<std::size_t>(a)]) { continue; }
        if (!attacks(fw, set, a)) { return false; }
    }
    return true;
}

}  // namespace detail

/// Enumerate every preferred and stable extension of `fw`.
///
/// Exhaustive within the grounded-undecided set, which is sound because every complete extension lies between the
/// grounded In set and the grounded In set plus its undecided arguments.
template <std::int32_t MA, std::int32_t MT, std::int32_t SA>
[[nodiscard]] inline Analysis analyse(const solver::Framework<MA, MT>& fw, solver::Grounded<SA>& grounded) {
    Analysis out;

    if (grounded.solve(fw) != solver::Status::Ok) {
        out.status = Status::GroundedFailed;
        return out;
    }

    const std::int32_t n = fw.argument_count();
    const std::size_t un = static_cast<std::size_t>(n);

    std::vector<bool> forced_in(un, false);
    std::vector<std::int32_t> undecided;
    for (std::int32_t a = 0; a < n; ++a) {
        switch (grounded.label(Arg{a})) {
            case Label::In: forced_in[static_cast<std::size_t>(a)] = true; break;
            case Label::Undecided: undecided.push_back(a); break;
            case Label::Out: break;
        }
    }
    out.undecided_count = static_cast<std::int32_t>(undecided.size());

    if (out.undecided_count > kMaxUndecided) {
        out.status = Status::TooManyUndecided;
        return out;
    }

    // Enumerate every subset of the undecided arguments. The grounded extension itself is the empty subset, so it is
    // always among the candidates -- which makes the containment guarantee checkable rather than merely asserted.
    std::vector<Extension> complete;
    const std::int64_t subsets = static_cast<std::int64_t>(1) << out.undecided_count;
    for (std::int64_t mask = 0; mask < subsets; ++mask) {
        ++out.subsets_examined;

        std::vector<bool> candidate = forced_in;
        for (std::int32_t i = 0; i < out.undecided_count; ++i) {
            if ((mask >> i) & 1) { candidate[static_cast<std::size_t>(undecided[static_cast<std::size_t>(i)])] = true; }
        }

        if (!detail::is_complete(fw, candidate)) { continue; }

        Extension e;
        e.stable = detail::is_stable(fw, candidate);
        e.accepted = std::move(candidate);
        complete.push_back(std::move(e));
    }
    out.complete_count = static_cast<std::int32_t>(complete.size());

    // Preferred extensions are exactly the maximal complete ones: every preferred extension is complete, and every
    // complete extension is admissible so is contained in some preferred one.
    for (std::size_t i = 0; i < complete.size(); ++i) {
        bool maximal = true;
        for (std::size_t j = 0; j < complete.size() && maximal; ++j) {
            if (i == j) { continue; }
            // Is complete[i] a strict subset of complete[j]?
            bool subset = true;
            bool strict = false;
            for (std::size_t a = 0; a < un && subset; ++a) {
                if (complete[i].accepted[a] && !complete[j].accepted[a]) { subset = false; }
                if (!complete[i].accepted[a] && complete[j].accepted[a]) { strict = true; }
            }
            if (subset && strict) { maximal = false; }
        }
        if (maximal) { out.preferred.push_back(complete[i]); }
    }

    for (const Extension& e : complete) {
        if (e.stable) { out.stable.push_back(e); }
    }

    return out;
}

}  // namespace quorum::advisory
