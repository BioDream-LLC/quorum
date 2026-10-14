#pragma once

#include <cstdint>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"

/// \file
/// The arbiter's decision contract.
///
/// ## Why this exists
///
/// `Grounded` produces a three-valued labelling. A labelling is not an instruction. In the hand-written pump
/// casebook, one scenario settled every label correctly and still selected **no action at all** -- cavitation and
/// impeller wear each explained the evidence, neither could defeat the other, and `act_continue` was dragged
/// Undecided along with them. That is the honest answer to the question asked. It is not something a device can
/// execute.
///
/// So the arbiter's contract is: **every tick yields an action.** When the framework declines to choose one, the
/// action comes from the casebook's declared fallback and the outcome records why. Nothing silently returns
/// nothing.
///
/// ## Why the outcomes are separated the way they are
///
/// It is tempting to collapse every no-answer case into one "unknown" value. That would be a safety defect,
/// because the required response differs:
///
///  * `Indeterminate` -- the reasoner **worked** and is reporting that the evidence cannot separate two
///    hypotheses. Escalate to a human. The machine is healthy; the world is unclear.
///  * `Faulted` -- the reasoner **did not complete**. Surrender to the hardwired interlock. This is the
///    conventional fail-safe reaction: detect the error, switch the function off, hand control back.
///  * `Conflicted` / `Unsatisfied` -- the casebook is **defective**, in two opposite directions. Neither is a
///    property of the plant and both should have been caught before installation.
///
/// Reporting a broken solver and an ambiguous pump with the same value would make those indistinguishable in the
/// witness, which is the one place they must not be.
///
/// ## Why the fallback is per casebook
///
/// The safe state is asset-specific. A pump's is stopped. A furnace wants a controlled sequence, not an instant
/// cut. A drone has no stopped state at all. The solver cannot know which, so the casebook declares it and this
/// header refuses to run without one.

namespace quorum::arbiter {

using solver::Arg;
using solver::Framework;
using solver::Grounded;
using solver::Label;
using solver::Status;
using solver::ix;

/// How an action was arrived at. Only `Decided` means the framework chose it.
enum class Outcome : std::uint8_t {
    /// Exactly one action argument is In. The framework decided.
    Decided = 0,

    /// No action is In, and at least one argument is Undecided. The reasoning completed and the evidence is
    /// irreducibly ambiguous. Escalate; do not treat as a fault.
    Indeterminate,

    /// No action is In and nothing is Undecided: the casebook decisively rejects every action it declared. A
    /// defect, and a different one from ambiguity.
    Unsatisfied,

    /// More than one action is In. The casebook failed to make its actions mutually exclusive. A defect, and the
    /// dangerous direction -- "shut down" and "continue" both surviving.
    Conflicted,

    /// The computation did not complete. The solver, not the plant, is the problem.
    Faulted,
};

[[nodiscard]] inline constexpr const char* outcome_name(Outcome o) noexcept {
    switch (o) {
        case Outcome::Decided:       return "Decided";
        case Outcome::Indeterminate: return "Indeterminate";
        case Outcome::Unsatisfied:   return "Unsatisfied";
        case Outcome::Conflicted:    return "Conflicted";
        case Outcome::Faulted:       return "Faulted";
    }
    return "?";
}

/// The result of one control cycle. `action` is always valid.
struct Verdict {
    Outcome outcome = Outcome::Faulted;
    /// The chosen action, or the casebook's declared fallback.
    ///
    /// INVALID (see `Arg::valid()`) only when `outcome == Faulted` and the policy itself was unusable, because then
    /// the casebook never declared a safe state and the arbiter refuses to invent one. Check `valid()` before acting.
    Arg action{0};
    bool is_fallback = true;            ///< false only when `outcome == Decided`
    Status status = Status::NotFinalized;  ///< why, when `outcome == Faulted`
    std::int32_t actions_in = 0;        ///< how many declared actions were In
    /// How many arguments in the WHOLE framework were Undecided. Diagnostic only.
    ///
    /// Deliberately not used to classify the outcome. On a device carrying several casebooks this counter sees all
    /// of them, so basing `Indeterminate` on it would let one casebook's unresolved cycle change another's verdict.
    /// Use `actions_undecided` for anything load-bearing.
    std::int32_t undecided = 0;
    /// How many of THIS policy's declared actions were Undecided. This is what distinguishes an ambiguous plant
    /// from a broken casebook.
    std::int32_t actions_undecided = 0;    std::int32_t steps = 0;             ///< work performed, for the timing budget

    /// The one question a caller must be able to ask without interpreting an enum.
    [[nodiscard]] constexpr bool decided() const noexcept { return outcome == Outcome::Decided; }

    /// True when the casebook itself is at fault rather than the plant or the evidence.
    [[nodiscard]] constexpr bool casebook_defect() const noexcept {
        return outcome == Outcome::Conflicted || outcome == Outcome::Unsatisfied;
    }
};

/// Which arguments are actions, and which one to fall back to.
///
/// Held as an explicit list rather than a contiguous range. The pump casebook happens to lay its actions out
/// contiguously, and relying on that would break the first time somebody inserts an argument in the middle --
/// the same class of latent error `ids.hpp` exists to prevent.
template <std::int32_t MaxActions>
class Policy {
public:
    static_assert(MaxActions > 0, "MaxActions must be positive");

    constexpr Policy() noexcept = default;

    [[nodiscard]] Status declare_action(Arg a) noexcept {
        if (count_ >= MaxActions) { return Status::CapacityExceeded; }
        if (a.get() < 0) { return Status::BadArgument; }
        for (std::int32_t i = 0; i < count_; ++i) {
            if (actions_[ix(i)] == a) { return Status::BadArgument; }  // duplicate
        }
        actions_[ix(count_)] = a;
        ++count_;
        return Status::Ok;
    }

    /// The action taken whenever the framework does not decide. Must already be a declared action -- a fallback
    /// outside the action set would be an instruction the casebook never reasoned about.
    [[nodiscard]] Status set_fallback(Arg a) noexcept {
        for (std::int32_t i = 0; i < count_; ++i) {
            if (actions_[ix(i)] == a) {
                fallback_ = a;
                has_fallback_ = true;
                return Status::Ok;
            }
        }
        return Status::BadArgument;
    }

    [[nodiscard]] constexpr std::int32_t action_count() const noexcept { return count_; }
    [[nodiscard]] constexpr Arg action(std::int32_t i) const noexcept { return actions_[ix(i)]; }
    [[nodiscard]] constexpr bool has_fallback() const noexcept { return has_fallback_; }
    [[nodiscard]] constexpr Arg fallback() const noexcept { return fallback_; }

    /// Usable only if it declares at least one action and a fallback among them.
    [[nodiscard]] constexpr Status validate() const noexcept {
        if (count_ == 0) { return Status::BadArgument; }
        if (!has_fallback_) { return Status::BadArgument; }
        return Status::Ok;
    }

private:
    static constexpr std::size_t kSlots = ix(MaxActions);
    Arg actions_[kSlots]{};
    Arg fallback_{0};
    std::int32_t count_ = 0;
    bool has_fallback_ = false;
};

/// Run one control cycle and return an executable instruction.
///
/// Solves, classifies, and always produces an action. The solver is taken by reference and reused across ticks so
/// nothing is constructed per cycle.
template <std::int32_t MA, std::int32_t MT, std::int32_t MG, std::int32_t MP>
[[nodiscard]] Verdict decide(const Framework<MA, MT>& fw,
                             Grounded<MG>& solver,
                             const Policy<MP>& policy) noexcept {
    Verdict v;

    const Status policy_ok = policy.validate();
    if (policy_ok != Status::Ok) {
        v.outcome = Outcome::Faulted;
        v.status = policy_ok;
        // There is no fallback to fall back to, so there is no action to name. `action` is left INVALID rather than
        // left at zero.
        //
        // Zero would be the trap. It is a perfectly good argument index -- in every real casebook it is the first
        // observation, a sensor reading -- so a caller that skipped the outcome check would receive something that
        // looks entirely plausible and could act on it. An invalid index makes that mistake fail loudly at the
        // integration boundary instead of quietly at the plant.
        v.action = Arg::none();
        return v;
    }

    v.action = policy.fallback();

    const Status solved = solver.solve(fw);
    if (solved != Status::Ok) {
        v.outcome = Outcome::Faulted;
        v.status = solved;
        return v;
    }

    v.status = Status::Ok;
    v.steps = solver.steps();

    for (std::int32_t i = 0; i < fw.argument_count(); ++i) {
        if (solver.label(Arg{i}) == Label::Undecided) { ++v.undecided; }
    }

    Arg chosen{0};
    for (std::int32_t i = 0; i < policy.action_count(); ++i) {
        const Arg a = policy.action(i);
        const Label l = solver.label(a);
        if (l == Label::In) {
            ++v.actions_in;
            chosen = a;
        } else if (l == Label::Undecided) {
            ++v.actions_undecided;
        }
    }

    if (v.actions_in == 1) {
        v.outcome = Outcome::Decided;
        v.action = chosen;
        v.is_fallback = false;
    } else if (v.actions_in > 1) {
        v.outcome = Outcome::Conflicted;
    } else if (v.actions_undecided > 0) {
        // Ambiguous: some action's support is unresolved, so the framework cannot commit.
        //
        // This tests the ACTIONS, not the whole framework, and that distinction is a correctness fix rather than an
        // optimisation. Counting Undecided across the framework couples every casebook on the device to every other
        // one: a palette-mate stuck in an odd cycle contributes Undecided arguments, and this casebook's honest
        // `Unsatisfied` silently becomes `Indeterminate`. That converts "the casebook is broken, fix the FMEA" into
        // "the plant is ambiguous, escalate to a human" -- a misdiagnosis caused entirely by an unrelated casebook
        // sharing the device.
        //
        // Found by the palette test, and only because a purpose-built victim casebook was added: the pump reaches
        // Unsatisfied in 0 of its 16,384 states and so could never have exposed it.
        //
        // Scoping to actions is also strictly more precise than scoping to this casebook's arguments. If every
        // declared action is definitively Out, the casebook has decisively rejected all of them, and that is a
        // defect worth reporting as such even when something elsewhere in the same casebook is unresolved.
        v.outcome = Outcome::Indeterminate;
    } else {
        v.outcome = Outcome::Unsatisfied;
    }

    return v;
}

}  // namespace quorum::arbiter
