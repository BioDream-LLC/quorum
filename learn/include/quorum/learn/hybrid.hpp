// The methods of the pre-registered hybrid tests, in one place. HOST ONLY.
//
// Every dataset in learn/PREREGISTRATION.md is judged by exactly this code, so "casebook + learned" cannot quietly
// mean one thing on AI4I and another on MONK's (PROC-D16: a second copy is a fix that lands in only one place).
#ifndef QUORUM_LEARN_HYBRID_HPP
#define QUORUM_LEARN_HYBRID_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "quorum/learn/aacbr.hpp"
#include "quorum/learn/baselines.hpp"
#include "quorum/learn/features.hpp"

namespace quorum {
namespace learn {

/// Confusion counts and the registered primary metric.
struct Confusion {
    int tp = 0, fn = 0, tn = 0, fp = 0;
    void add(bool predicted, bool actual) {
        if (actual) {
            (predicted ? tp : fn) += 1;
        } else {
            (predicted ? fp : tn) += 1;
        }
    }
    void add(const Confusion& o) {
        tp += o.tp;
        fn += o.fn;
        tn += o.tn;
        fp += o.fp;
    }
    /// (positives caught / positives + negatives passed / negatives) / 2.
    [[nodiscard]] double ba() const {
        const double tpr = tp + fn == 0 ? 0.0 : static_cast<double>(tp) / (tp + fn);
        const double tnr = tn + fp == 0 ? 0.0 : static_cast<double>(tn) / (tn + fp);
        return 0.5 * (tpr + tnr);
    }
};

/// What must hold of every AA-CBR solve for any result to mean anything.
struct Invariants {
    long queries = 0;
    long not_total = 0;
    long over_bound = 0;
    int max_args = 0;
    int max_attacks = 0;
    void add(const Invariants& o) {
        queries += o.queries;
        not_total += o.not_total;
        over_bound += o.over_bound;
        max_args = std::max(max_args, o.max_args);
        max_attacks = std::max(max_attacks, o.max_attacks);
    }
};

enum class Method { Casebook, Learned, Hybrid, CasebookOrNn, CasebookOrNb, Nn, Nb };
constexpr int kMethods = 7;
constexpr Method kAllMethods[kMethods] = {Method::Casebook,     Method::Learned, Method::Hybrid, Method::CasebookOrNn,
                                          Method::CasebookOrNb, Method::Nn,      Method::Nb};

inline const char* method_label(Method m) {
    switch (m) {
        case Method::Casebook: return "casebook";
        case Method::Learned: return "learned";
        case Method::Hybrid: return "casebook+learned";
        case Method::CasebookOrNn: return "casebook OR 1-NN";
        case Method::CasebookOrNb: return "casebook OR NB";
        case Method::Nn: return "1-NN";
        case Method::Nb: return "NB";
    }
    return "?";
}
inline bool uses_k(Method m) { return m != Method::Casebook; }

/// A reviewed casebook, as AA-CBR cases. `asserts` is the outcome its rules conclude ("failure", "positive"); the
/// default is the other one, because a casebook says nothing until a rule fires. That is a property of the casebook,
/// not of the data, so it does not move with the training majority.
struct Knowledge {
    std::vector<Case> priors;
    Features features = 0;
    bool asserts = true;
    /// v4 (PREREGISTRATION.md). Records needed before a learned case may attack a rule; 1 is the v1-v3 behaviour.
    std::int32_t min_exception_support = 1;
    /// v4. Let the hybrid's nested CV choose k = 0: no learned cases, the casebook alone. Off in v1-v3.
    bool may_decline = false;
};

/// The AA-CBR model a method builds -- `Learned` without the casebook, `Hybrid` with it -- and the observation mask it
/// was fitted over. One function, used both to score a method and to emit the casebook that ships, so the casebook a
/// device runs is the model the experiment measured and not a re-derivation of it.
inline std::unique_ptr<AaCbr> fit_aacbr(Method m, const Knowledge& kb, const std::vector<Case>& train, int k,
                                        int feature_count, Features* mask_out) {
    const bool hybrid = m == Method::Hybrid && !kb.priors.empty();
    Features mask = k > 0 ? top_k_features(train, k, feature_count) : Features{0};
    if (m == Method::Hybrid) { mask |= kb.features; }
    auto aa = std::make_unique<AaCbr>(hybrid ? !kb.asserts : majority_outcome(train), Coherence::MajorityPerPattern);
    if (hybrid) { aa->set_min_exception_support(kb.min_exception_support); }
    // k = 0 is "learn nothing": the casebook's rules and no records at all.
    aa->fit(k > 0 ? project(train, mask) : std::vector<Case>{}, hybrid ? kb.priors : std::vector<Case>{});
    if (mask_out != nullptr) { *mask_out = mask; }
    return aa;
}

/// Train `m` on `train`, score it on `test`. Features are ranked over `feature_count` bits; `k` is ignored by the
/// casebook alone. When `inv` is non-null every AA-CBR solve is checked against the invariants.
inline Confusion run_method(Method m, const Knowledge& kb, const std::vector<Case>& train,
                            const std::vector<Case>& test, int k, int feature_count, Invariants* inv) {
    Confusion out;
    const bool majority = majority_outcome(train);
    const bool has_kb = !kb.priors.empty();
    const bool casebook_default = !kb.asserts;

    AaCbr casebook(casebook_default);
    const bool needs_casebook = m == Method::Casebook || m == Method::CasebookOrNn || m == Method::CasebookOrNb;
    if (needs_casebook && has_kb) { casebook.fit({}, kb.priors); }
    // With no casebook there is nothing to assert, and "the casebook alone" is the majority guess.
    auto casebook_says = [&](Features q) { return has_kb ? casebook.predict(q).outcome : majority; };
    auto casebook_fires = [&](Features q) { return has_kb && casebook.predict(q).outcome == kb.asserts; };

    if (m == Method::Casebook) {
        for (const Case& q : test) { out.add(casebook_says(q.x), q.outcome); }
        return out;
    }

    if (m == Method::Learned || m == Method::Hybrid) {
        Features mask = 0;
        const std::unique_ptr<AaCbr> aa = fit_aacbr(m, kb, train, k, feature_count, &mask);
        for (const Case& q : test) {
            const auto p = aa->predict(q.x & mask);
            out.add(p.outcome, q.outcome);
            if (inv != nullptr) {
                ++inv->queries;
                inv->not_total += p.total ? 0 : 1;
                inv->over_bound += p.steps > p.bound ? 1 : 0;
                inv->max_args = std::max(inv->max_args, static_cast<int>(p.arguments));
                inv->max_attacks = std::max(inv->max_attacks, static_cast<int>(p.attacks));
            }
        }
        return out;
    }
    const Features mask = top_k_features(train, k, feature_count);
    const std::vector<Case> tr = project(train, mask);

    // The OR hybrids: the casebook's assertion wins whenever it fires, otherwise the learner decides.
    if (m == Method::Nn || m == Method::CasebookOrNn) {
        for (const Case& q : test) {
            const bool nn = nearest_neighbour(tr, q.x & mask, majority);
            const bool says = m == Method::Nn ? nn : (casebook_fires(q.x) ? kb.asserts : nn);
            out.add(says, q.outcome);
        }
        return out;
    }
    NaiveBayes nb;
    nb.fit(tr, feature_count);
    for (const Case& q : test) {
        const bool b = nb.predict(q.x & mask);
        const bool says = m == Method::Nb ? b : (casebook_fires(q.x) ? kb.asserts : b);
        out.add(says, q.outcome);
    }
    return out;
}

constexpr int kCandidateK[] = {2, 3, 4, 5, 6, 8, 10, 12};

using Folds = std::vector<std::pair<std::vector<Case>, std::vector<Case>>>;

/// k by nested CV inside the training set alone, by balanced accuracy; ties keep the smaller k. The inner folds arrive
/// already encoded, so a learned binding layer can be fitted inside each one.
inline int choose_k(Method m, const Knowledge& kb, const Folds& inner, int feature_count) {
    std::vector<int> candidates;
    if (m == Method::Hybrid && kb.may_decline && !kb.priors.empty()) { candidates.push_back(0); }
    for (int k : kCandidateK) { candidates.push_back(k); }
    int best_k = candidates.front();
    double best = -1.0;
    for (int k : candidates) {
        Confusion c;
        for (const auto& f : inner) { c.add(run_method(m, kb, f.first, f.second, k, feature_count, nullptr)); }
        if (c.ba() > best + 1e-12) {
            best = c.ba();
            best_k = k;
        }
    }
    return best_k;
}

}  // namespace learn
}  // namespace quorum

#endif  // QUORUM_LEARN_HYBRID_HPP
