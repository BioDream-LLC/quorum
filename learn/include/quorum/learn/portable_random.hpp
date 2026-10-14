#ifndef QUORUM_LEARN_PORTABLE_RANDOM_HPP
#define QUORUM_LEARN_PORTABLE_RANDOM_HPP

// Random choices that come out the same on every compiler and every CPU.
//
// std::mt19937's output sequence is fixed by the C++ standard. std::shuffle, std::uniform_int_distribution and
// std::bernoulli_distribution are not: the standard fixes only their distribution, so each library may turn the same
// generator output into different choices. libstdc++ did change between GCC 9 and GCC 14, and the pre-registered
// experiments recorded with GCC 9 on 2026-09-30 drew different train/test splits under GCC 14 (on x86 and on a
// Raspberry Pi 5 alike), so their recorded numbers could not be reproduced there.
//
// These functions use only mt19937 output and integer arithmetic, so a given seed gives the same choices everywhere.
//
// QUORUM_LEARN_STD_RANDOM=1 selects the standard-library versions instead. That exists for one reason: to keep the
// ORIGINAL 2026-09-30 records reproducible on the compiler they were made with. New work uses the portable versions.

#include <cstdint>
#include <iterator>
#include <random>
#include <utility>

#ifndef QUORUM_LEARN_STD_RANDOM
#define QUORUM_LEARN_STD_RANDOM 0
#endif

namespace quorum::learn::rnd {

#if QUORUM_LEARN_STD_RANDOM

template <typename It>
void shuffle(It first, It last, std::mt19937& g) {
    std::shuffle(first, last, g);
}
inline int uniform_int(std::mt19937& g, int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); }
inline bool bernoulli(std::mt19937& g, double p) { return std::bernoulli_distribution(p)(g); }

#else

/// Uniform in [0, n), n >= 1, by rejection: drop the 2^32 mod n lowest outputs, then reduce. No modulo bias.
inline std::uint32_t below(std::mt19937& g, std::uint32_t n) {
    const std::uint32_t limit = static_cast<std::uint32_t>(0U - n) % n;  // 2^32 mod n
    for (;;) {
        const auto x = static_cast<std::uint32_t>(g());
        if (x >= limit) { return x % n; }
    }
}

/// Fisher-Yates, last element first.
template <typename It>
void shuffle(It first, It last, std::mt19937& g) {
    const auto n = static_cast<std::uint32_t>(std::distance(first, last));
    for (std::uint32_t i = n; i > 1U; --i) {
        const std::uint32_t j = below(g, i);
        using std::swap;
        swap(first[static_cast<std::ptrdiff_t>(i - 1U)], first[static_cast<std::ptrdiff_t>(j)]);
    }
}

/// Uniform in [lo, hi].
inline int uniform_int(std::mt19937& g, int lo, int hi) {
    const auto span = static_cast<std::uint32_t>(static_cast<std::int64_t>(hi) - lo) + 1U;
    return static_cast<int>(static_cast<std::int64_t>(lo) + below(g, span));
}

/// True with probability p: a 53-bit fraction from two outputs (27 + 26 bits), exact in a double.
inline bool bernoulli(std::mt19937& g, double p) {
    const auto hi = static_cast<std::uint64_t>(static_cast<std::uint32_t>(g()) >> 5U);
    const auto lo = static_cast<std::uint64_t>(static_cast<std::uint32_t>(g()) >> 6U);
    const double u = static_cast<double>((hi << 26U) | lo) / 9007199254740992.0;  // 2^53
    return u < p;
}

#endif

}  // namespace quorum::learn::rnd

#endif  // QUORUM_LEARN_PORTABLE_RANDOM_HPP
