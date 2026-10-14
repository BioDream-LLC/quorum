#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "quorum/learn/aacbr.hpp"

/// \file
/// Choosing which features a learned casebook sees. HOST ONLY.
///
/// ## Why AA-CBR needs this, measured rather than assumed
///
/// A past case speaks about a query only if the query has EVERY one of the case's features. On the UCI voting data a
/// case carries about 15 of 32 features, so that almost never happens: 229 of 435 queries had two or fewer relevant
/// past cases, and AA-CBR scored 81.4% against 93.1% for plain nearest-neighbour. Vanilla AA-CBR over raw features is
/// close to memorisation. Fewer, more informative features make cases generalise.
///
/// It is also required regardless: a quorum casebook carries at most 30 observations, because the presence mask is a
/// 32-bit word. Selection is how a learned casebook fits a device at all.
///
/// ## The criterion
///
/// Information gain of each binary feature about the outcome, computed on TRAINING data only. It is the standard
/// choice, it has no parameters, and it ranks each feature independently -- which is its known weakness: two redundant
/// features both rank highly. That is recorded here rather than fixed, because fixing it (e.g. by greedy conditional
/// selection) is a second design decision that should be measured on its own.

namespace quorum::learn {

[[nodiscard]] inline double entropy2(double pos, double total) {
    if (total <= 0.0) { return 0.0; }
    const double p = pos / total;
    if (p <= 0.0 || p >= 1.0) { return 0.0; }
    return -(p * std::log2(p) + (1.0 - p) * std::log2(1.0 - p));
}

/// Information gain, in bits, of feature `f` about the outcome over `cases`.
[[nodiscard]] inline double information_gain(const std::vector<Case>& cases, int f) {
    double n = 0.0, pos = 0.0, on = 0.0, on_pos = 0.0;
    for (const Case& c : cases) {
        n += 1.0;
        pos += c.outcome ? 1.0 : 0.0;
        if (((c.x >> f) & 1U) != 0U) {
            on += 1.0;
            on_pos += c.outcome ? 1.0 : 0.0;
        }
    }
    const double off = n - on;
    const double off_pos = pos - on_pos;
    return entropy2(pos, n) - (on / n) * entropy2(on_pos, on) - (off / n) * entropy2(off_pos, off);
}

/// The `k` highest-gain features over `cases`, as a mask. Ties break toward the lower feature index, so the result is
/// deterministic.
[[nodiscard]] inline Features top_k_features(const std::vector<Case>& cases, int k, int feature_count = 32) {
    std::vector<std::pair<double, int>> ranked;
    for (int f = 0; f < feature_count; ++f) { ranked.emplace_back(information_gain(cases, f), f); }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    Features mask = 0;
    for (int i = 0; i < k && i < static_cast<int>(ranked.size()); ++i) { mask |= 1U << ranked[static_cast<std::size_t>(i)].second; }
    return mask;
}

/// `cases` with every feature outside `mask` removed.
[[nodiscard]] inline std::vector<Case> project(const std::vector<Case>& cases, Features mask) {
    std::vector<Case> out;
    out.reserve(cases.size());
    for (const Case& c : cases) { out.push_back({c.x & mask, c.outcome}); }
    return out;
}

}  // namespace quorum::learn
