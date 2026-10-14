#pragma once

#include <cstddef>
#include <cstdint>

/// \file
/// Identifiers that cannot be confused with each other. Freestanding.
///
/// Salvaged from the ICCMA engine's `core/ids.hpp`, which explains the reasoning better than this
/// header will: the `.af` format numbers arguments from one, every array counts from zero, and mixing the two
/// "produces answers about the neighbouring argument -- plausible, internally consistent, and wrong on every
/// instance, which is the failure mode least likely to be noticed and most likely to be published."
///
/// That argument is the reason this line is C++ and not C. In C these are all `int` and the protection is gone.
///
/// Differences from the competition original, all forced by the target:
///
///  * No `<ostream>`, `<string>`, `<stdexcept>`, `<functional>`. Only `<cstdint>`. A freestanding target has
///    no iostreams and no allocator, and `std::hash` is not needed where nothing is a hash key.
///  * `to_internal_checked` returns a status instead of throwing. Built with `-fno-exceptions`.
///  * `Var` and `Lit` are **deleted**. There is no SAT solver in this line -- see README for why that is a
///    certification requirement rather than a compromise.

namespace quorum::solver {

/// Outcome of any operation that can fail. There are no exceptions on this target, and refusal is a
/// first-class answer -- the competition engine's `Verdict::Unknown` discipline, carried over.
enum class Status : std::uint8_t {
    Ok = 0,
    CapacityExceeded,   ///< the compile-time bound was too small for this framework
    BadArgument,        ///< an argument index outside 1..n (or 0..n-1 internally)
    NotFinalized,       ///< solve attempted before `finalize()`
    AlreadyFinalized,   ///< mutation attempted after `finalize()`
};

// ─────────────────────────────────────────────────────────────────────────────
// A strongly typed integer index.
//
// CRTP so each identifier is a distinct type at no runtime cost: sizeof(Arg) == sizeof(std::int32_t) and every
// operation inlines to the operation on the underlying integer. The tag exists only to make the types different.
// ─────────────────────────────────────────────────────────────────────────────
template <typename Tag, typename Underlying = std::int32_t>
class Index {
public:
    using value_type = Underlying;

    /// Explicit, always. An implicit constructor would defeat the entire purpose of the class.
    constexpr explicit Index(Underlying v) noexcept : value_(v) {}

    constexpr Index() noexcept = default;

    [[nodiscard]] constexpr Underlying get() const noexcept { return value_; }

    /// Is this a usable index at all?
    ///
    /// Exists so that "no index" can be represented and, more importantly, DETECTED. A function that has nothing
    /// meaningful to return must be able to say so in a way a caller trips over rather than reads past. Returning 0
    /// is the trap: 0 is a perfectly valid argument, so a caller who skips the error check gets the first
    /// observation in the casebook -- a sensor reading -- and may execute it as though it were an action.
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ >= 0; }

    /// The explicit "no index" value.
    [[nodiscard]] static constexpr Index none() noexcept { return Index{static_cast<Underlying>(-1)}; }

    // Comparison is meaningful; arithmetic mostly is not. An argument plus an argument is not an argument, so
    // `+` is deliberately absent. `++` is present because iterating over arguments means something.
    friend constexpr bool operator==(Index a, Index b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Index a, Index b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Index a, Index b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator<=(Index a, Index b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>(Index a, Index b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator>=(Index a, Index b) noexcept { return a.value_ >= b.value_; }

    constexpr Index& operator++() noexcept { ++value_; return *this; }
    constexpr Index operator++(int) noexcept { Index t = *this; ++value_; return t; }

private:
    Underlying value_{};
};

struct ArgTag {};
struct FileArgTag {};

/// A zero-based argument index. Indexes every array here.
using Arg = Index<ArgTag>;

/// A one-based argument as it appears in a casebook or a `.af` file. Never indexes anything.
using FileArg = Index<FileArgTag>;

/// The only conversion from external numbering to internal numbering.
[[nodiscard]] inline constexpr Arg to_internal(FileArg f) noexcept {
    return Arg{f.get() - 1};
}

/// The only conversion from internal numbering back to external, used when emitting a witness.
[[nodiscard]] inline constexpr FileArg to_file(Arg a) noexcept {
    return FileArg{a.get() + 1};
}

/// Validate a one-based argument against a framework's size, then convert.
///
/// The competition original throws here, reasoning that "silently mapping argument 0 to index -1 turns a bad
/// input into memory corruption." That reasoning is sound and the throw is not available, so the status is an
/// out-parameter and the caller is obliged to check it. `[[nodiscard]]` on the status enforces that.
[[nodiscard]] inline constexpr Status to_internal_checked(FileArg f, std::int32_t arg_count, Arg& out) noexcept {
    if (f.get() < 1 || f.get() > arg_count) {
        out = Arg{0};
        return Status::BadArgument;
    }
    out = to_internal(f);
    return Status::Ok;
}

/// Signed-to-unsigned subscript conversion, in exactly one named place.
///
/// Array subscripts are `std::size_t`; every identifier in this library is `std::int32_t`. The options were to
/// scatter `static_cast` across every subscript, to make the capacities unsigned and fight the conversion at the
/// other end, or to switch off `-Wsign-conversion` and lose a genuinely useful warning. This is the fourth
/// option: one function, one documented precondition, zero runtime cost.
///
/// \pre `i >= 0`. Every caller has already range-checked, either against `argument_count()` at insertion time or
///      against a loop bound derived from it. This function deliberately does not re-check -- a branch in every
///      subscript would be paid on a device that is counting cycles, for a condition the type system and the
///      insertion-time validation already establish.
[[nodiscard]] inline constexpr std::size_t ix(std::int32_t i) noexcept {
    return static_cast<std::size_t>(i);
}

}  // namespace quorum::solver
