/// \file
/// Learning the binding layer: where to put the thresholds. HOST ONLY.
///
/// ## Why this exists, measured
///
/// The first pre-registered hybrid test (learn/PREREGISTRATION.md, v1) failed its first criterion. The cause was not
/// the learner but its inputs: a mode the casebook did not know could only be seen through generic alarm limits at
/// the 10th and 90th percentile, and a power failure below 3,500 W or above 9,000 W is invisible through limits at
/// 4,915 W and 7,641 W. No learner can recover a boundary its observations do not draw.
///
/// A casebook's thresholds are part of its knowledge (casebook/LANGUAGE.md §7, "thresholds live outside"). This file
/// learns them from data, so a learned casebook can carry observations nobody wrote.
///
/// ## The method
///
/// Fayyad & Irani (1993), "Multi-interval discretization of continuous-valued attributes for classification
/// learning", IJCAI-93, pp. 1022-1027: choose the cut that minimises class entropy, and accept it only if the
/// Minimum Description Length test says it pays for itself:
///
///     Gain(A, T; S) > log2(N - 1) / N + Delta(A, T; S) / N
///     Delta = log2(3^k - 2) - [k Ent(S) - k1 Ent(S1) - k2 Ent(S2)]
///
/// where k, k1, k2 count the classes present in S and its two halves. It has no parameters to tune, which is why it
/// was chosen: a threshold method with a knob would be a knob tuned after seeing v1.
///
/// One departure from the paper, stated: the number of cuts per signal is capped (`max_cuts`), and under the cap the
/// cuts are taken best-first across all current intervals -- ranked by total entropy removed, gain times interval
/// size, so a large interval is not outranked by a tiny one -- rather than by full recursion. The cap exists only
/// because a device casebook has at most 30 observations.
#ifndef QUORUM_LEARN_DISCRETIZE_HPP
#define QUORUM_LEARN_DISCRETIZE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace quorum {
namespace learn {

namespace detail {

[[nodiscard]] inline double ent_bits(double pos, double n) {
    if (n <= 0.0 || pos <= 0.0 || pos >= n) { return 0.0; }
    const double p = pos / n;
    return -(p * std::log2(p) + (1.0 - p) * std::log2(1.0 - p));
}

[[nodiscard]] inline double classes_present(double pos, double n) {
    return (pos > 0.0 ? 1.0 : 0.0) + (n - pos > 0.0 ? 1.0 : 0.0);
}

struct Split {
    bool ok = false;
    std::size_t at = 0;  // first index of the right half, in sorted order
    double gain = 0.0;
    double bits = 0.0;  // gain x interval size: total entropy removed, comparable across intervals
};

// Best MDL-acceptable split of sorted[lo, hi), or !ok. `pre[i]` = positives among sorted[0, i).
inline Split best_split(const std::vector<std::pair<double, bool>>& sorted, const std::vector<double>& pre,
                        std::size_t lo, std::size_t hi) {
    Split best;
    const double n = static_cast<double>(hi - lo);
    if (hi - lo < 2U) { return best; }
    const double pos = pre[hi] - pre[lo];
    const double h = ent_bits(pos, n);
    if (h == 0.0) { return best; }  // a pure interval has nothing left to explain
    double best_gain = -1.0;
    std::size_t best_at = 0;
    for (std::size_t i = lo + 1; i < hi; ++i) {
        if (sorted[i].first == sorted[i - 1].first) { continue; }  // a cut must separate distinct values
        const double n1 = static_cast<double>(i - lo);
        const double p1 = pre[i] - pre[lo];
        const double gain = h - (n1 / n) * ent_bits(p1, n1) - ((n - n1) / n) * ent_bits(pos - p1, n - n1);
        if (gain > best_gain) {
            best_gain = gain;
            best_at = i;
        }
    }
    if (best_gain < 0.0) { return best; }
    const double n1 = static_cast<double>(best_at - lo);
    const double p1 = pre[best_at] - pre[lo];
    const double k = classes_present(pos, n);
    const double k1 = classes_present(p1, n1);
    const double k2 = classes_present(pos - p1, n - n1);
    const double delta =
        std::log2(std::pow(3.0, k) - 2.0) - (k * h - k1 * ent_bits(p1, n1) - k2 * ent_bits(pos - p1, n - n1));
    const double threshold = std::log2(n - 1.0) / n + delta / n;
    if (best_gain > threshold) {
        best.ok = true;
        best.at = best_at;
        best.gain = best_gain;
        best.bits = best_gain * n;
    }
    return best;
}

}  // namespace detail

/// Cut points for one continuous signal against a binary outcome, ascending. Each cut is the midpoint between the two
/// distinct values it separates. At most `max_cuts`; possibly none, which is the honest answer for a signal that says
/// nothing about the outcome.
[[nodiscard]] inline std::vector<double> mdlp_cuts(const std::vector<double>& values, const std::vector<bool>& outcome,
                                                   int max_cuts) {
    std::vector<std::pair<double, bool>> sorted;
    sorted.reserve(values.size());
    for (std::size_t i = 0; i < values.size() && i < outcome.size(); ++i) { sorted.emplace_back(values[i], outcome[i]); }
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> pre(sorted.size() + 1U, 0.0);
    for (std::size_t i = 0; i < sorted.size(); ++i) { pre[i + 1U] = pre[i] + (sorted[i].second ? 1.0 : 0.0); }

    std::vector<std::pair<std::size_t, std::size_t>> intervals{{0U, sorted.size()}};
    std::vector<double> cuts;
    while (static_cast<int>(cuts.size()) < max_cuts) {
        detail::Split best;
        std::size_t which = 0;
        for (std::size_t j = 0; j < intervals.size(); ++j) {
            const detail::Split s = detail::best_split(sorted, pre, intervals[j].first, intervals[j].second);
            if (s.ok && (!best.ok || s.bits > best.bits)) {
                best = s;
                which = j;
            }
        }
        if (!best.ok) { break; }
        cuts.push_back(0.5 * (sorted[best.at - 1U].first + sorted[best.at].first));
        const auto iv = intervals[which];
        intervals[which] = {iv.first, best.at};
        intervals.emplace_back(best.at, iv.second);
    }
    std::sort(cuts.begin(), cuts.end());
    return cuts;
}

}  // namespace learn
}  // namespace quorum

#endif  // QUORUM_LEARN_DISCRETIZE_HPP
