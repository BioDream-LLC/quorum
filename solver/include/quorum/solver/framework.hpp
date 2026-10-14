#pragma once

#include <cstddef>
#include <cstdint>

#include "quorum/solver/ids.hpp"

/// \file
/// A fixed-capacity argumentation framework with adjacency built once, up front.
///
/// ## What changed from the competition engine, and why
///
/// The competition `Framework` holds `std::vector<std::int32_t>` and builds attacker adjacency **on demand**,
/// reasoning that "the stable encoding needs it, a bare attack walk does not, and on 21.8 million attacks the
/// two extra passes are seconds." Both decisions are correct there and wrong here.
///
///  * **No heap.** Capacity is a template parameter, storage is inline, and the whole object can live in
///    `.bss`. Nothing allocates, ever, so there is no allocation failure to handle and no fragmentation to
///    reason about.
///  * **Adjacency is built once, in `finalize()`, not per solve.** The competition solver runs once per process
///    against an adversarial instance. An arbiter re-solves every control cycle against the *same* structure.
///    Rebuilding CSR each tick would dominate the cost and put an O(m) term in the wrong place. Build it when
///    the casebook is installed; after that, solving touches no allocation and no rebuild.
///  * **In-degree is cached.** `Grounded` needs an attacker *count* per argument, never the attacker list, so
///    the reverse CSR the competition engine builds is replaced by one integer per argument.
///
/// ## Sizing
///
/// Storage is roughly `12*MaxAttacks + 8*MaxArgs` bytes. For `MaxArgs=256, MaxAttacks=2048` that is about
/// 27 KB -- comfortable on an RP2350's 520 KB, and plausible on much smaller parts.

namespace quorum::solver {

template <std::int32_t MaxArgs, std::int32_t MaxAttacks>
class Framework {
public:
    static_assert(MaxArgs > 0, "MaxArgs must be positive");
    static_assert(MaxAttacks > 0, "MaxAttacks must be positive");

    static constexpr std::int32_t max_args = MaxArgs;
    static constexpr std::int32_t max_attacks = MaxAttacks;

    constexpr Framework() noexcept = default;

    /// Discard everything and start over. Touches only the counters and the in-degree table.
    void reset() noexcept {
        argument_count_ = 0;
        attack_count_ = 0;
        finalized_ = false;
        rejected_ = false;
        for (std::int32_t i = 0; i < MaxArgs; ++i) { in_degree_[ix(i)] = 0; }
    }

    /// Declare how many arguments the framework has. Must precede `add_attack`.
    [[nodiscard]] Status declare(std::int32_t argument_count) noexcept {
        if (finalized_) { return Status::AlreadyFinalized; }
        if (argument_count < 0) { return Status::BadArgument; }
        if (argument_count > MaxArgs) { return Status::CapacityExceeded; }
        reset();
        argument_count_ = argument_count;
        return Status::Ok;
    }

    /// Record that `from` attacks `to`.
    ///
    /// Self-attack is legal and meaningful: a self-attacker is never acceptable, which is exactly how Dung's
    /// definition handles a claim that undermines its own premise.
    [[nodiscard]] Status add_attack(Arg from, Arg to) noexcept {
        if (finalized_) { return Status::AlreadyFinalized; }
        if (!in_range(from) || !in_range(to)) {
            rejected_ = true;
            return Status::BadArgument;
        }
        if (attack_count_ >= MaxAttacks) {
            rejected_ = true;
            return Status::CapacityExceeded;
        }

        attacks_[ix(2 * attack_count_)] = from.get();
        attacks_[ix(2 * attack_count_ + 1)] = to.get();
        ++attack_count_;
        ++in_degree_[ix(to.get())];
        return Status::Ok;
    }

    /// Build forward adjacency in compressed sparse row form. Call once, after all attacks are added.
    ///
    /// Two counting passes and one scatter. O(n + m), no allocation, no recursion.
    ///
    /// Refuses to finalize if ANY earlier `add_attack` was rejected. That check exists because of the failure it
    /// prevents, which is the worst one available here: a caller that ignores a `CapacityExceeded` return and
    /// finalizes anyway gets a framework that solves perfectly cleanly and returns a confident verdict computed from
    /// a casebook with one of its rules missing. Nothing downstream can detect that -- the labelling is internally
    /// consistent and completely wrong, which is the same bug class the `Arg`/`FileArg` split was introduced to kill.
    /// Making it a sticky error turns a silent wrong answer into a `Faulted` verdict and a trip to the interlock.
    [[nodiscard]] Status finalize() noexcept {
        if (finalized_) { return Status::AlreadyFinalized; }
        if (rejected_) { return Status::CapacityExceeded; }

        const std::int32_t n = argument_count_;
        const std::int32_t m = attack_count_;

        for (std::int32_t i = 0; i <= n; ++i) { offset_[ix(i)] = 0; }
        for (std::int32_t i = 0; i < m; ++i) { ++offset_[ix(attacks_[ix(2 * i)] + 1)]; }
        for (std::int32_t i = 0; i < n; ++i) { offset_[ix(i + 1)] += offset_[ix(i)]; }

        // Scatter using `offset_` as a moving cursor, then restore it by shifting right. This avoids a second
        // cursor array, which on this target is memory we would rather not spend. After the scatter,
        // `offset_[i]` holds the END of row i, which is the START of row i+1 -- hence the shift.
        for (std::int32_t i = 0; i < m; ++i) {
            const std::int32_t src = attacks_[ix(2 * i)];
            target_[ix(offset_[ix(src)])] = attacks_[ix(2 * i + 1)];
            ++offset_[ix(src)];
        }
        for (std::int32_t i = n; i > 0; --i) { offset_[ix(i)] = offset_[ix(i - 1)]; }
        offset_[0] = 0;

        finalized_ = true;
        return Status::Ok;
    }

    [[nodiscard]] constexpr bool finalized() const noexcept { return finalized_; }
    [[nodiscard]] constexpr std::int32_t argument_count() const noexcept { return argument_count_; }
    [[nodiscard]] constexpr std::int32_t attack_count() const noexcept { return attack_count_; }

    /// How many arguments attack `a`. Cached at `add_attack` time.
    [[nodiscard]] constexpr std::int32_t in_degree(Arg a) const noexcept { return in_degree_[ix(a.get())]; }

    // Forward adjacency: the arguments `a` attacks are `target(k)` for k in [out_begin(a), out_end(a)).
    [[nodiscard]] constexpr std::int32_t out_begin(Arg a) const noexcept { return offset_[ix(a.get())]; }
    [[nodiscard]] constexpr std::int32_t out_end(Arg a) const noexcept { return offset_[ix(a.get() + 1)]; }
    [[nodiscard]] constexpr Arg target(std::int32_t k) const noexcept { return Arg{target_[ix(k)]}; }

    /// Attacks as a fraction of the possible n^2, in parts per thousand.
    ///
    /// The competition engine notes density "predicts difficulty better than size does: a 246-argument
    /// framework at 93% density beat this engine for a week while a 14,812-argument one passed." Kept because a
    /// casebook compiler should refuse to ship a dense framework to a device, and this is the number it checks.
    [[nodiscard]] constexpr std::int32_t density_permille() const noexcept {
        if (argument_count_ <= 0) { return 0; }
        const std::int64_t n = argument_count_;
        return static_cast<std::int32_t>((static_cast<std::int64_t>(attack_count_) * 1000) / (n * n));
    }

private:
    [[nodiscard]] constexpr bool in_range(Arg a) const noexcept {
        return a.get() >= 0 && a.get() < argument_count_;
    }

    static constexpr std::size_t kArgSlots = ix(MaxArgs);
    static constexpr std::size_t kAttackSlots = ix(MaxAttacks);

    std::int32_t attacks_[2U * kAttackSlots]{};   ///< flat attacker/attacked pairs, as the original stores them
    std::int32_t offset_[kArgSlots + 1U]{};       ///< CSR row offsets into `target_`
    std::int32_t target_[kAttackSlots]{};         ///< CSR column indices
    std::int32_t in_degree_[kArgSlots]{};         ///< attacker count per argument

    std::int32_t argument_count_ = 0;
    std::int32_t attack_count_ = 0;
    bool finalized_ = false;
    /// Sticky: set by any rejected `add_attack`, cleared only by `reset()`. See `finalize()` for why.
    bool rejected_ = false;
};

}  // namespace quorum::solver
