#ifndef QUORUM_RUNTIME_GUARD_HPP
#define QUORUM_RUNTIME_GUARD_HPP

// The AI guard: quorum as a runtime shield for another controller.
//
// A learned controller -- a TinyML model on the same chip, a language model on a gateway, a vendor's black box -- PROPOSES
// an action. quorum decides from the approved rulebook as usual, and then the guard compares the two:
//
//   * the proposal is the rulebook's own decision                      -> accepted (Agrees)
//   * the proposal OUTRANKS the rulebook's decision in the rulebook's   -> accepted (MoreCautious)
//     own precedence (`prefer shutdown over maintain over continue`)
//   * anything else                                                     -> REPLACED by the rulebook's decision, and the
//                                                                          reason says which of the cases below it was
//
// So the output is never less cautious than what the approved rulebook would have done by itself, whatever the other
// controller proposes, and in every state. That property holds by construction; `cbc` and the firmware sweep both
// check it over every state and every possible proposal anyway, and report how permissive the guard is.
//
// "Outranks" is read straight from the framework: in every casebook cbc lowers, an attack from one action to another
// exists exactly when the first is preferred over the second (`prefer` chains expand to every pair) or the second is
// the idle action. No new data is needed in the image, so every image ever built already supports the guard.
//
// The guard does not make the other controller safe. It makes the COMBINATION no less cautious than the rulebook, and
// the rulebook is the thing that was checked. A proposal the rulebook outranks is not acted on, however confident the
// model was -- which is the point.
//
// Freestanding: no heap, no exceptions, <cstdint> only.

#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/runtime/blob.hpp"

namespace quorum::runtime {

enum class GuardReason : std::uint8_t {
    Agrees = 0,          ///< the proposal is the rulebook's decision
    MoreCautious = 1,    ///< the proposal outranks the rulebook's decision, so acting on it is no less safe
    LessCautious = 2,    ///< the rulebook's decision outranks the proposal: REPLACED
    Incomparable = 3,    ///< the rulebook ranks neither above the other: REPLACED, because nothing shows it is safe
    NotAnAction = 4,     ///< the proposal is not one of this rulebook's actions: REPLACED
    NoDecision = 5,      ///< the rulebook did not decide; its fallback stands unless the proposal outranks it
};

[[nodiscard]] constexpr const char* guard_reason_name(GuardReason r) noexcept {
    switch (r) {
        case GuardReason::Agrees: return "agrees with the rulebook";
        case GuardReason::MoreCautious: return "more cautious than the rulebook";
        case GuardReason::LessCautious: return "LESS cautious than the rulebook -- replaced";
        case GuardReason::Incomparable: return "not ranked against the rulebook's choice -- replaced";
        case GuardReason::NotAnAction: return "not an action of this rulebook -- replaced";
        case GuardReason::NoDecision: return "rulebook undecided -- its fallback stands";
    }
    return "?";
}

struct GuardResult {
    std::int32_t proposed = -1;  ///< what the other controller asked for
    std::int32_t rulebook = -1;  ///< what the rulebook decided (or its fallback)
    std::int32_t output = -1;    ///< what to act on
    bool accepted = false;       ///< output == proposed
    GuardReason reason = GuardReason::NotAnAction;
};

/// Does action `a` outrank action `b`? True when the framework has an attack a -> b and both are actions.
[[nodiscard]] inline bool outranks(const Edge* edges, std::int32_t edge_count, std::int32_t a, std::int32_t b) noexcept {
    for (std::int32_t i = 0; i < edge_count; ++i) {
        if (edges[i].from == a && edges[i].to == b) { return true; }
    }
    return false;
}

/// Guard one proposal against one verdict. `actions` are the rulebook's (or palette member's) declared actions.
[[nodiscard]] inline GuardResult guard(const Edge* edges, std::int32_t edge_count, const std::int32_t* actions,
                                       std::int32_t action_count, const arbiter::Verdict& verdict,
                                       std::int32_t proposed) noexcept {
    GuardResult g;
    g.proposed = proposed;
    g.rulebook = verdict.action.valid() ? verdict.action.get() : -1;
    g.output = g.rulebook;

    bool known = false;
    for (std::int32_t i = 0; i < action_count; ++i) { known = known || actions[i] == proposed; }
    if (!known || g.rulebook < 0) {
        g.reason = GuardReason::NotAnAction;
    } else if (proposed == g.rulebook) {
        g.reason = verdict.decided() ? GuardReason::Agrees : GuardReason::NoDecision;
        g.output = proposed;
    } else if (outranks(edges, edge_count, proposed, g.rulebook)) {
        g.reason = GuardReason::MoreCautious;
        g.output = proposed;
    } else if (!verdict.decided()) {
        g.reason = GuardReason::NoDecision;
    } else if (outranks(edges, edge_count, g.rulebook, proposed)) {
        g.reason = GuardReason::LessCautious;
    } else {
        g.reason = GuardReason::Incomparable;
    }
    g.accepted = g.output == proposed;
    return g;
}

[[nodiscard]] inline GuardResult guard(const Casebook& cb, const arbiter::Verdict& verdict,
                                       std::int32_t proposed) noexcept {
    return guard(cb.edges, cb.edge_count, cb.actions, cb.action_count, verdict, proposed);
}

[[nodiscard]] inline GuardResult guard(const Palette& p, std::int32_t member, const arbiter::Verdict& verdict,
                                       std::int32_t proposed) noexcept {
    const Member& m = p.members[member];
    return guard(p.edges, p.edge_count, m.actions, m.action_count, verdict, proposed);
}

/// Every state, every possible proposal. `weaker` counts outputs the rulebook's decision outranks -- the property says
/// it is always zero. The fingerprint covers each pair's output and reason, so a board and the host can be compared
/// with one line, exactly like the DEVICE line.
struct GuardSweep {
    std::uint32_t pairs = 0;
    std::uint32_t accepted = 0;
    std::uint32_t replaced = 0;
    std::uint32_t weaker = 0;
    std::uint32_t faults = 0;
    std::uint32_t fnv = 2166136261U;
};

template <typename Framework, typename Solver, typename Policy, typename Instantiate>
[[nodiscard]] GuardSweep guard_sweep(const Edge* edges, std::int32_t edge_count, const std::int32_t* actions,
                                     std::int32_t action_count, std::int32_t observations, std::int32_t shift,
                                     Framework& fw, Solver& solver, const Policy& policy, Instantiate inst) noexcept {
    GuardSweep r;
    const std::uint32_t states = 1U << static_cast<std::uint32_t>(observations);
    for (std::uint32_t local = 0; local < states; ++local) {
        if (inst(local << static_cast<std::uint32_t>(shift)) != solver::Status::Ok) {
            ++r.faults;
            continue;
        }
        const arbiter::Verdict v = arbiter::decide(fw, solver, policy);
        for (std::int32_t k = 0; k < action_count; ++k) {
            const GuardResult g = guard(edges, edge_count, actions, action_count, v, actions[k]);
            ++r.pairs;
            if (g.accepted) { ++r.accepted; } else { ++r.replaced; }
            if (g.output != g.rulebook && !outranks(edges, edge_count, g.output, g.rulebook)) { ++r.weaker; }
            r.fnv = (r.fnv ^ static_cast<std::uint32_t>(g.output)) * 16777619U;
            r.fnv = (r.fnv ^ static_cast<std::uint32_t>(g.reason)) * 16777619U;
        }
    }
    return r;
}

template <typename Framework, typename Solver, typename Policy>
[[nodiscard]] GuardSweep guard_sweep(const Casebook& cb, Framework& fw, Solver& solver, const Policy& policy) noexcept {
    return guard_sweep(cb.edges, cb.edge_count, cb.actions, cb.action_count, cb.observation_count, 0, fw, solver,
                       policy, [&](std::uint32_t m) { return instantiate(fw, cb, m); });
}

template <typename Framework, typename Solver, typename Policy>
[[nodiscard]] GuardSweep guard_sweep(const Palette& p, std::int32_t member, Framework& fw, Solver& solver,
                                     const Policy& policy) noexcept {
    const Member& x = p.members[member];
    return guard_sweep(p.edges, p.edge_count, x.actions, x.action_count, x.observation_count, x.observation_offset,
                       fw, solver, policy, [&](std::uint32_t m) { return instantiate(fw, p, m); });
}

}  // namespace quorum::runtime

#endif  // QUORUM_RUNTIME_GUARD_HPP
