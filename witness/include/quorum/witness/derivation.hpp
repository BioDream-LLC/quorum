#pragma once

#include <cstddef>
#include <cstdint>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"

/// \file
/// Why an argument got the label it got.
///
/// ## The problem
///
/// `Grounded` produces a three-valued labelling, and a labelling is not an explanation. Nobody can read
/// `dx_bearing_wear=In` and know why. The worklist that computed it discards its reasoning as it goes, so the
/// derivation has to be reconstructed.
///
/// ## Reconstructed, not recorded
///
/// The derivation is rebuilt on demand rather than accumulated during solving. Two reasons. The tick loop would
/// otherwise pay memory and time for an explanation nobody reads in the overwhelming majority of cycles, and the
/// hot path's WCET bound would stop being the clean `2n + 2m` it is now. Grounded labelling is deterministic, so
/// nothing is lost: the labelling plus the casebook fully determines the derivation.
///
/// This follows the distinction the Kotlin line already draws between `Proof` and `ProofForest` --
/// *"That type is the interface a caller reasons about, and a tree of objects with a `children` list is the right
/// shape for reading one derivation. This is the store for MANY, where sharing and pruning matter."* A witness
/// reads one. So this is a tree, and there is no forest on the device.
///
/// ## The assumption problem, which is the reason this file is careful
///
/// From `Solver.kt`, on why a derivation marks assumed premises in capitals:
///
/// > a derivation that cannot distinguish an assumption from a fact is the single most dangerous object this
/// > library can hand anybody: an engine that proves things from premises cannot tell whether a premise is true,
/// > so one assumed premise yields a conclusion that is false and carries a complete, correct-looking derivation.
/// > That is more persuasive than a guess and therefore worse than one.
///
/// That applies here directly. An absent observation is Out because the **closed-world argument** attacks it, and
/// closed-world reasoning is an assumption, not a fact: *the sensor did not report vibration* is not *there is no
/// vibration*. The sensor could be dead. So every step that rests on the closed-world argument is flagged, and
/// `rests_on_assumption()` reports whether the verdict as a whole does. A "no fault found" conclusion that is
/// silent about this is exactly the object the passage warns against.
///
/// ## Everything here is freestanding
///
/// No heap, no exceptions, no recursion -- the walk uses an explicit stack, because unbounded stack depth is not
/// acceptable on a device and MISRA has opinions about recursion. Prose rendering lives off-device; this produces
/// structure.

namespace quorum::witness {

using solver::Arg;
using solver::FileArg;
using solver::Framework;
using solver::Grounded;
using solver::ix;
using solver::Label;
using solver::Status;
using solver::to_file;

/// Why an argument holds the label it holds.
enum class Ground : std::uint8_t {
    /// In, and nothing attacks it. The base case of the grounded construction.
    Unattacked = 0,

    /// In because every one of its attackers is Out. Children are those attackers.
    AllAttackersOut,

    /// Out because a specific attacker is In. The child is that attacker.
    ///
    /// An argument may have several In attackers; one is named. Grounded labelling does not distinguish them, and
    /// listing all of them makes a witness longer without making it more true.
    DefeatedBy,

    /// Undecided: its attackers are themselves Undecided, so nothing settles. This is what an odd cycle or a
    /// mutual defeat looks like from inside.
    Contested,

    /// Already expanded earlier in this derivation. Emitted instead of repeating a subtree.
    ///
    /// The Kotlin `ProofForest` makes the same choice for the same reason -- identical subproofs are one object
    /// reached twice, and in real rule bases that sharing is most of the structure.
    AlreadyShown,
};

[[nodiscard]] inline constexpr const char* ground_name(Ground g) noexcept {
    switch (g) {
        case Ground::Unattacked:      return "unattacked";
        case Ground::AllAttackersOut: return "all attackers defeated";
        case Ground::DefeatedBy:      return "defeated by";
        case Ground::Contested:       return "contested, nothing settles it";
        case Ground::AlreadyShown:    return "shown above";
    }
    return "?";
}

/// One line of a derivation.
struct Step {
    Arg subject{0};
    Label label = Label::Undecided;
    Ground ground = Ground::Contested;

    /// For `DefeatedBy`, the attacker that did it. Otherwise equal to `subject`.
    Arg because{0};

    /// True when this step rests on the closed-world assumption -- i.e. `because` is the closed-world argument,
    /// so `subject` is Out only because no sensor asserted it. Absence of evidence, not evidence of absence.
    bool assumed = false;

    /// Indentation level, for rendering. 0 is the queried argument.
    std::int32_t depth = 0;
};

/// A reconstructed derivation for one argument.
///
/// `MaxSteps` bounds the output. An argument is expanded at most once, so `MaxSteps >= MaxArgs` can never
/// truncate; smaller values trade completeness for memory and report it honestly via `truncated()`.
template <std::int32_t MaxSteps, std::int32_t MaxArgs>
class Derivation {
public:
    static_assert(MaxSteps > 0, "MaxSteps must be positive");
    static_assert(MaxArgs > 0, "MaxArgs must be positive");

    constexpr Derivation() noexcept = default;

    /// Rebuild the derivation of `query`.
    ///
    /// \param closed_world the casebook's closed-world argument, or a negative `Arg` if it has none. Supplying it
    ///        is what makes assumption flagging possible, so a casebook that uses closed-world reasoning and does
    ///        not declare it will produce a witness that silently overstates its confidence.
    template <std::int32_t MA, std::int32_t MT, std::int32_t MG>
    [[nodiscard]] Status build(const Framework<MA, MT>& fw,
                               const Grounded<MG>& solver,
                               Arg query,
                               Arg closed_world) noexcept {
        static_assert(MA <= MaxArgs, "Derivation capacity is smaller than the Framework");

        if (!fw.finalized()) { return Status::NotFinalized; }
        const std::int32_t n = fw.argument_count();
        if (query.get() < 0 || query.get() >= n) { return Status::BadArgument; }

        step_count_ = 0;
        truncated_ = false;
        assumed_steps_ = 0;
        max_depth_ = 0;
        for (std::int32_t i = 0; i < n; ++i) { seen_[ix(i)] = false; }

        // Explicit stack; no recursion. Depth is bounded by n because nothing is expanded twice.
        std::int32_t sp = 0;
        stack_arg_[0] = query.get();
        stack_depth_[0] = 0;
        sp = 1;

        while (sp > 0) {
            --sp;
            const std::int32_t a = stack_arg_[ix(sp)];
            const std::int32_t d = stack_depth_[ix(sp)];

            if (d > max_depth_) { max_depth_ = d; }

            if (seen_[ix(a)]) {
                emit(Arg{a}, solver.label(Arg{a}), Ground::AlreadyShown, Arg{a}, false, d);
                continue;
            }
            seen_[ix(a)] = true;

            const Label lab = solver.label(Arg{a});

            if (lab == Label::In) {
                if (fw.in_degree(Arg{a}) == 0) {
                    emit(Arg{a}, lab, Ground::Unattacked, Arg{a}, false, d);
                } else {
                    emit(Arg{a}, lab, Ground::AllAttackersOut, Arg{a}, false, d);
                    push_attackers(fw, solver, a, d, Label::Out, sp);
                }
            } else if (lab == Label::Out) {
                const std::int32_t killer = find_attacker(fw, solver, a, Label::In);
                const bool by_cw = (closed_world.get() >= 0) && (killer == closed_world.get());
                emit(Arg{a}, lab, Ground::DefeatedBy, Arg{killer < 0 ? a : killer}, by_cw, d);
                // Do not expand the closed-world argument: it is unattacked by construction and expanding it adds
                // a line that says nothing. The assumption is already recorded on this step.
                if (killer >= 0 && !by_cw) {
                    push(killer, d + 1, sp);
                }
            } else {
                emit(Arg{a}, lab, Ground::Contested, Arg{a}, false, d);
                push_attackers(fw, solver, a, d, Label::Undecided, sp);
            }
        }

        return Status::Ok;
    }

    [[nodiscard]] constexpr std::int32_t step_count() const noexcept { return step_count_; }
    [[nodiscard]] constexpr const Step& step(std::int32_t i) const noexcept { return steps_[ix(i)]; }
    [[nodiscard]] constexpr bool truncated() const noexcept { return truncated_; }

    /// Longest chain of reasoning. The Kotlin `Proof` calls this "the honest measure of how multi-step an answer
    /// really was".
    [[nodiscard]] constexpr std::int32_t depth() const noexcept { return max_depth_; }

    /// How many steps rest on the closed-world assumption.
    [[nodiscard]] constexpr std::int32_t assumed_steps() const noexcept { return assumed_steps_; }

    /// True when some step depends on absence of evidence rather than on evidence.
    [[nodiscard]] constexpr bool rests_on_assumption() const noexcept { return assumed_steps_ > 0; }

    /// True when nothing was assumed, so the verdict stands on asserted readings alone. The Kotlin `Proof` calls
    /// the equivalent `isUnconditional`.
    [[nodiscard]] constexpr bool unconditional() const noexcept { return assumed_steps_ == 0; }

private:
    void emit(Arg subject, Label lab, Ground g, Arg because, bool assumed, std::int32_t depth) noexcept {
        if (step_count_ >= MaxSteps) {
            truncated_ = true;
            return;
        }
        Step& s = steps_[ix(step_count_)];
        s.subject = subject;
        s.label = lab;
        s.ground = g;
        s.because = because;
        s.assumed = assumed;
        s.depth = depth;
        ++step_count_;
        if (assumed) { ++assumed_steps_; }
    }

    void push(std::int32_t a, std::int32_t d, std::int32_t& sp) noexcept {
        if (sp >= MaxArgs) {
            truncated_ = true;
            return;
        }
        stack_arg_[ix(sp)] = a;
        stack_depth_[ix(sp)] = d;
        ++sp;
    }

    /// `Framework` keeps only forward adjacency plus in-degree, because `Grounded` never needs an attacker list.
    /// So finding attackers means scanning. That is O(n + m) per step, which is irrelevant here: a witness is
    /// produced on demand, not every tick, and paying a reverse-CSR's memory on every device for it would be the
    /// wrong trade.
    template <std::int32_t MA, std::int32_t MT, std::int32_t MG>
    [[nodiscard]] std::int32_t find_attacker(const Framework<MA, MT>& fw,
                                             const Grounded<MG>& solver,
                                             std::int32_t target,
                                             Label want) const noexcept {
        for (std::int32_t u = 0; u < fw.argument_count(); ++u) {
            if (solver.label(Arg{u}) != want) { continue; }
            for (std::int32_t k = fw.out_begin(Arg{u}); k < fw.out_end(Arg{u}); ++k) {
                if (fw.target(k).get() == target) { return u; }
            }
        }
        return -1;
    }

    template <std::int32_t MA, std::int32_t MT, std::int32_t MG>
    void push_attackers(const Framework<MA, MT>& fw,
                        const Grounded<MG>& solver,
                        std::int32_t target,
                        std::int32_t d,
                        Label want,
                        std::int32_t& sp) noexcept {
        for (std::int32_t u = fw.argument_count() - 1; u >= 0; --u) {
            if (solver.label(Arg{u}) != want) { continue; }
            bool attacks = false;
            for (std::int32_t k = fw.out_begin(Arg{u}); k < fw.out_end(Arg{u}); ++k) {
                if (fw.target(k).get() == target) { attacks = true; break; }
            }
            if (attacks) { push(u, d + 1, sp); }
        }
    }

    static constexpr std::size_t kStepSlots = ix(MaxSteps);
    static constexpr std::size_t kArgSlots = ix(MaxArgs);

    Step steps_[kStepSlots]{};
    bool seen_[kArgSlots]{};
    std::int32_t stack_arg_[kArgSlots]{};
    std::int32_t stack_depth_[kArgSlots]{};

    std::int32_t step_count_ = 0;
    std::int32_t assumed_steps_ = 0;
    std::int32_t max_depth_ = 0;
    bool truncated_ = false;
};

}  // namespace quorum::witness
