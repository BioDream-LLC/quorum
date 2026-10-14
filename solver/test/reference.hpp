#pragma once

#include <cstdint>
#include <utility>
#include <vector>

/// \file
/// An independent, deliberately naive grounded-extension oracle, plus direct checks against Dung's definition.
///
/// Host-only. Uses `std::vector` and is allowed to be slow -- that is the point. The competition engine's
/// discipline applies: "Verification is against the definition, not the encoding. `Stable::verify` checks Dung's
/// definition directly and consults neither the solver nor the model. This discipline has caught real bugs that
/// every encoding-based check passed."
///
/// This file therefore shares no code with `grounded.hpp`. It computes the least fixpoint of the characteristic
/// function by repeated application from the empty set, which is the textbook definition and obviously correct at
/// the cost of being O(n^2 * m).

namespace quorum::reference {

/// An attack, in internal zero-based numbering.
struct Attack {
    std::int32_t from;
    std::int32_t to;
};

/// `a` is acceptable with respect to `s` iff every attacker of `a` is attacked by something in `s`.
[[nodiscard]] inline bool acceptable(std::int32_t a,
                                     const std::vector<bool>& s,
                                     std::int32_t n,
                                     const std::vector<Attack>& attacks) {
    (void)n;
    for (const Attack& incoming : attacks) {
        if (incoming.to != a) { continue; }
        const std::int32_t attacker = incoming.from;

        bool defended = false;
        for (const Attack& defence : attacks) {
            if (defence.to == attacker && s[static_cast<std::size_t>(defence.from)]) {
                defended = true;
                break;
            }
        }
        if (!defended) { return false; }
    }
    return true;
}

/// The characteristic function F(S) = { a : a is acceptable wrt S }.
[[nodiscard]] inline std::vector<bool> characteristic(const std::vector<bool>& s,
                                                      std::int32_t n,
                                                      const std::vector<Attack>& attacks) {
    std::vector<bool> out(static_cast<std::size_t>(n), false);
    for (std::int32_t a = 0; a < n; ++a) {
        out[static_cast<std::size_t>(a)] = acceptable(a, s, n, attacks);
    }
    return out;
}

/// The grounded extension: the least fixpoint of F, reached by iterating from the empty set.
[[nodiscard]] inline std::vector<bool> grounded(std::int32_t n, const std::vector<Attack>& attacks) {
    std::vector<bool> s(static_cast<std::size_t>(n), false);
    for (std::int32_t iteration = 0; iteration <= n; ++iteration) {
        std::vector<bool> next = characteristic(s, n, attacks);
        if (next == s) { return s; }
        // Move, not copy: GCC 14's -Warray-bounds misfires on std::vector<bool> copy-assignment (a known false
        // positive inside libstdc++'s memmove), which -Werror turns into a build failure on the Pi's compiler.
        s = std::move(next);
    }
    return s;  // monotone in a finite lattice, so unreachable
}

/// No two members of `s` attack each other.
[[nodiscard]] inline bool conflict_free(const std::vector<bool>& s, const std::vector<Attack>& attacks) {
    for (const Attack& at : attacks) {
        if (s[static_cast<std::size_t>(at.from)] && s[static_cast<std::size_t>(at.to)]) { return false; }
    }
    return true;
}

/// Conflict-free, and every member defended by the set.
[[nodiscard]] inline bool admissible(const std::vector<bool>& s,
                                     std::int32_t n,
                                     const std::vector<Attack>& attacks) {
    if (!conflict_free(s, attacks)) { return false; }
    for (std::int32_t a = 0; a < n; ++a) {
        if (s[static_cast<std::size_t>(a)] && !acceptable(a, s, n, attacks)) { return false; }
    }
    return true;
}

}  // namespace quorum::reference
