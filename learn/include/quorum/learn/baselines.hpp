// The baselines every learning claim is measured against. HOST ONLY.
//
// One copy, shared by every evaluation, so that "1-NN" means the same procedure in every number this repository
// reports. These are deliberately the plain textbook forms: a sceptic should recognise them, and a stronger baseline
// is always fairer than a weaker one, so nothing here is tuned to lose.
#ifndef QUORUM_LEARN_BASELINES_HPP
#define QUORUM_LEARN_BASELINES_HPP

#include <cmath>
#include <cstddef>
#include <vector>

#include "quorum/learn/aacbr.hpp"

namespace quorum {
namespace learn {

/// The commoner outcome in `train`; ties go to false.
inline bool majority_outcome(const std::vector<Case>& train) {
    std::size_t pos = 0;
    for (const Case& c : train) { pos += c.outcome ? 1U : 0U; }
    return 2U * pos > train.size();
}

inline int popcount(Features v) {
    int n = 0;
    while (v != 0U) {
        v &= v - 1U;
        ++n;
    }
    return n;
}

/// 1-nearest-neighbour by Hamming distance. Equally near neighbours vote; a tied vote goes to `fallback`.
inline bool nearest_neighbour(const std::vector<Case>& train, Features q, bool fallback) {
    int best = 33;
    int votes_pos = 0;
    int votes_neg = 0;
    for (const Case& c : train) {
        const int d = popcount(c.x ^ q);
        if (d < best) {
            best = d;
            votes_pos = 0;
            votes_neg = 0;
        }
        if (d == best) { (c.outcome ? votes_pos : votes_neg) += 1; }
    }
    if (votes_pos == votes_neg) { return fallback; }
    return votes_pos > votes_neg;
}

/// Naive Bayes over binary features with Laplace smoothing.
struct NaiveBayes {
    int features = 32;
    double prior[2]{};
    double p[2][32]{};

    void fit(const std::vector<Case>& train, int feature_count) {
        features = feature_count;
        double n[2] = {0.0, 0.0};
        double on[2][32]{};
        for (const Case& c : train) {
            const int k = c.outcome ? 1 : 0;
            n[k] += 1.0;
            for (int f = 0; f < features; ++f) {
                if (((c.x >> f) & 1U) != 0U) { on[k][f] += 1.0; }
            }
        }
        for (int k = 0; k < 2; ++k) {
            prior[k] = (n[k] + 1.0) / (n[0] + n[1] + 2.0);
            for (int f = 0; f < features; ++f) { p[k][f] = (on[k][f] + 1.0) / (n[k] + 2.0); }
        }
    }

    [[nodiscard]] bool predict(Features q) const {
        double s[2];
        for (int k = 0; k < 2; ++k) {
            s[k] = std::log(prior[k]);
            for (int f = 0; f < features; ++f) {
                s[k] += std::log(((q >> f) & 1U) != 0U ? p[k][f] : 1.0 - p[k][f]);
            }
        }
        return s[1] > s[0];
    }
};

}  // namespace learn
}  // namespace quorum

#endif  // QUORUM_LEARN_BASELINES_HPP
