#pragma once

#include <cstddef>
#include <cstdint>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/ids.hpp"

/// \file
/// The grounded extension: Dung's iterative construction, in bounded time and bounded memory.
///
/// ## Why this is the only semantics on this target
///
/// The competition engine answers stable, complete, preferred, semi-stable and ideal, all of them by search on
/// top of CaDiCaL. Every one is NP-hard or worse and has **no worst-case execution time**. A safety function
/// with no WCET cannot be certified at any integrity level, so none of them belongs on a device claiming to be
/// mission critical. The competition engine's `Terminator` bounds the search, but a timeout is a refusal, not an
/// answer -- adequate for a benchmark, not a basis for an interlock.
///
/// The grounded extension is different in kind. It is the *least* complete extension, it is unique, and it is
/// computable without search. This is a worklist algorithm that labels each argument at most once and traverses
/// each edge at most twice, so:
///
///     steps <= 2*n + 2*m
///
/// That is a real bound, checkable against `steps()`, and it is what makes the MCU build a certifiable safety
/// function rather than a compromise forced by RAM.
///
/// Dung's guarantee that makes this useful rather than merely cheap: the grounded extension is contained in
/// **every** complete extension. Anything it accepts is accepted under every other semantics too. So a verdict
/// from this solver is never contradicted by the richer semantics running on a Pi -- only ever less committal.
/// An arbiter can act on it and an advisory layer can refine it, without the two ever disagreeing.
///
/// ## Salvage note
///
/// The algorithm is lifted from `Semantics::grounded` in the ICCMA engine's `src/af/semantics_reduct.cpp`. Changes: the
/// forward adjacency it rebuilds per call now arrives precomputed from `Framework::finalize`; the three
/// `std::vector`s become inline arrays; and the `std::deque` worklist becomes a plain array, which is sound
/// because each argument is pushed at most once -- see `push_`.

namespace quorum::solver {

/// Dung's three-valued labelling.
///
/// Namespace scope, deliberately. This was briefly nested inside `Grounded<MaxArgs>`, which made
/// `Grounded<32>::Label` and `Grounded<256>::Label` distinct types that would not compare. A label is a semantic
/// concept and has nothing to do with how much RAM the solver was given.
///
/// `Undecided` is the honest answer for an argument in an odd cycle. It is neither accepted nor rejected, and
/// reporting it as either would be a lie the witness could not support -- the same discipline as the competition
/// engine's `Verdict::Unknown`, which "is not `Unsat`".
enum class Label : std::uint8_t {
    Undecided = 0,
    In = 1,
    Out = 2,
};

template <std::int32_t MaxArgs>
class Grounded {
public:
    static_assert(MaxArgs > 0, "MaxArgs must be positive");

    /// Convenience alias, so `Grounded<N>::Label` still reads naturally at call sites.
    using Label = quorum::solver::Label;

    constexpr Grounded() noexcept = default;

    /// Compute the grounded extension of `fw`.
    ///
    /// \return `Status::NotFinalized` if `fw.finalize()` was not called, otherwise `Status::Ok`. There is no
    ///         other failure mode: the grounded extension always exists, so unlike the search-based semantics
    ///         this cannot refuse for want of budget.
    template <std::int32_t MA, std::int32_t MT>
    [[nodiscard]] Status solve(const Framework<MA, MT>& fw) noexcept {
        static_assert(MA <= MaxArgs, "Grounded capacity is smaller than the Framework it is being given");

        if (!fw.finalized()) { return Status::NotFinalized; }

        const std::int32_t n = fw.argument_count();
        n_ = n;
        in_count_ = 0;
        head_ = 0;
        tail_ = 0;

        // Accumulated in a local and stored once, not incremented in place.
        //
        // `steps_` is a member, so the compiler must assume it is observable and reloads it, increments, and stores it
        // on every iteration of all four counting loops: three instructions of bookkeeping per step. At the pump's
        // worst case that is 3 x 140 = 420 of 2851 instructions, about 15% of the safety function's entire worst-case
        // execution time spent maintaining a diagnostic counter. Nobody reads it until solve() returns.
        //
        // Found by disassembling for Cortex-M33 to build the static WCET bound, not by any host profiling -- on x86-64
        // the store forwards and costs nothing measurable.
        std::int32_t steps = 0;

        // Unattacked arguments are in; whatever an in-argument attacks is out; an out-argument no longer blocks
        // what it attacks. Repeat to fixpoint.
        for (std::int32_t i = 0; i < n; ++i) {
            label_[ix(i)] = Label::Undecided;
            undecided_[ix(i)] = fw.in_degree(Arg{i});
            ++steps;
        }

        for (std::int32_t i = 0; i < n; ++i) {
            if (undecided_[ix(i)] == 0) {
                label_[ix(i)] = Label::In;
                ++in_count_;
                push_(i);
            }
        }

        while (head_ < tail_) {
            const std::int32_t a = queue_[ix(head_)];
            ++head_;
            ++steps;

            const std::int32_t a_end = fw.out_end(Arg{a});
            for (std::int32_t k = fw.out_begin(Arg{a}); k < a_end; ++k) {
                ++steps;
                const std::int32_t b = fw.target(k).get();
                if (label_[ix(b)] != Label::Undecided) { continue; }
                label_[ix(b)] = Label::Out;

                // b is out, so it no longer defends anything against the things b attacks.
                const std::int32_t b_end = fw.out_end(Arg{b});
                for (std::int32_t j = fw.out_begin(Arg{b}); j < b_end; ++j) {
                    ++steps;
                    const std::int32_t c = fw.target(j).get();
                    if (undecided_[ix(c)] > 0) { --undecided_[ix(c)]; }
                    if (label_[ix(c)] == Label::Undecided && undecided_[ix(c)] == 0) {
                        label_[ix(c)] = Label::In;
                        ++in_count_;
                        push_(c);
                    }
                }
            }
        }

        steps_ = steps;
        return Status::Ok;
    }

    [[nodiscard]] constexpr Label label(Arg a) const noexcept { return label_[ix(a.get())]; }

    /// True only for `In`. `Undecided` is not acceptance -- see the note on `Label`.
    [[nodiscard]] constexpr bool accepted(Arg a) const noexcept { return label_[ix(a.get())] == Label::In; }

    /// Size of the grounded extension.
    [[nodiscard]] constexpr std::int32_t in_count() const noexcept { return in_count_; }

    [[nodiscard]] constexpr std::int32_t argument_count() const noexcept { return n_; }

    /// Work actually performed, for validating the WCET bound against real casebooks.
    [[nodiscard]] constexpr std::int32_t steps() const noexcept { return steps_; }

    /// The bound `steps()` is guaranteed not to exceed, for a framework of this size.
    [[nodiscard]] static constexpr std::int32_t step_bound(std::int32_t n, std::int32_t m) noexcept {
        return 2 * n + 2 * m;
    }

private:
    /// Each argument is pushed only on an `Undecided -> In` transition, and that transition happens at most once
    /// per argument because every write of `Label::In` is guarded by the label being `Undecided`. Therefore at
    /// most `n` pushes occur and `tail_` never reaches `MaxArgs`. That is why this is a plain array rather than a
    /// ring buffer, and why it needs no overflow branch.
    void push_(std::int32_t a) noexcept {
        queue_[ix(tail_)] = a;
        ++tail_;
    }

    static constexpr std::size_t kArgSlots = ix(MaxArgs);

    Label label_[kArgSlots]{};
    std::int32_t undecided_[kArgSlots]{};
    std::int32_t queue_[kArgSlots]{};

    std::int32_t n_ = 0;
    std::int32_t head_ = 0;
    std::int32_t tail_ = 0;
    std::int32_t in_count_ = 0;
    std::int32_t steps_ = 0;
};

}  // namespace quorum::solver
