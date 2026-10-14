#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

/// \file
/// Abstract Argumentation for Case-Based Reasoning (AA-CBR), learned from data and decided by quorum's own grounded
/// solver.
///
/// HOST ONLY. This allocates and has no WCET. It is the LEARNING half of a two-stage design: a casebook is learned here,
/// offline, and only then validated and compiled for a device. Nothing in this file may ever run on a device, and a
/// learned casebook is never modified on one -- a framework that changes itself in the field has no proven step bound,
/// no exhaustive validation and no measured timing, which are the three things quorum exists to provide.
///
/// ## The method, in the form implemented
///
/// Following Cyras, Satoh & Toni (2016) as simplified by Cocarascu, Stylianou, Cyras & Toni (2020). A case is a set of
/// features with a binary outcome. For a query, the framework is:
///
///   arguments   every past case, one DEFAULT argument (no features, outcome delta), and the NEW case
///   attacks     past case A attacks B, or the default, when
///                 (1) their outcomes differ,
///                 (2) A's features are a STRICT superset of B's -- the more specific case wins, and
///                 (3) no past case C with A's outcome lies strictly between them (concision: only the nearest
///                     more-specific disagreement attacks, so an argument is not attacked twice for one reason)
///               the new case attacks every past case that has a feature the new case lacks -- an irrelevant case
///               cannot speak about this one
///
/// Prediction is delta if the default is in the grounded extension, and the other outcome if not.
///
/// ## Why this fits quorum
///
/// Past-case attacks run strictly from more features to fewer, so the case-to-case graph is acyclic; the new case is
/// unattacked. The framework is therefore acyclic and its grounded labelling is total -- nothing is ever Undecided.
/// `Prediction::total` records that rather than assuming it, because an Undecided default would mean this file does
/// not implement what it says it does.
///
/// It also avoids a failure seen in an earlier argumentation design, where symmetric conflict could not produce a
/// winner: ten adjudications, ten INDETERMINATE. AA-CBR never has symmetric conflict. Strength is SPECIFICITY, an ordering fixed by
/// the data, not by the confidence of whatever produced the argument.
///
/// ## Incoherence: the one place counting enters
///
/// Vanilla AA-CBR assumes a COHERENT casebase -- no two cases with the same features and different outcomes. Real data
/// is not coherent, and the vanilla method fails on it in a specific, measured way: it counts ARGUMENTS, not evidence.
/// On the UCI voting data restricted to one vote, 245 democrats and 2 republicans voted the same way; after duplicates
/// collapse those are one argument each, the republican one attacks the default, nothing more specific defends it, and
/// every one of the 245 is classified republican. Accuracy fell to 39.8%, below always guessing the majority (61.4%).
///
/// `Coherence::MajorityPerPattern` repairs this before any argument exists: each distinct feature pattern keeps ONE case,
/// with the outcome most training cases sharing that pattern had. An exact tie keeps no case -- the data does not favour
/// either outcome for that pattern, so it contributes no argument rather than an arbitrary one. The semantics is then
/// unchanged: attacks, concision, the new-case rule and the grounded decision are all exactly as above.
///
/// `Coherence::KeepAll` is the vanilla behaviour, kept so the failure stays reproducible. `incoherent_pairs()` reports
/// how far the training data departed from coherence under either policy, measured before any repair.

namespace quorum::learn {

/// A set of up to 32 binary features, one per bit.
using Features = std::uint32_t;

struct Case {
    Features x;
    bool outcome;
};

enum class Coherence : std::uint8_t {
    KeepAll,             ///< vanilla AA-CBR: identical features with different outcomes are both kept
    MajorityPerPattern,  ///< one case per distinct pattern, majority outcome; a tie contributes no case
};

[[nodiscard]] inline constexpr bool subset_of(Features a, Features b) noexcept { return (a & ~b) == 0U; }
[[nodiscard]] inline constexpr bool strict_subset_of(Features a, Features b) noexcept {
    return a != b && subset_of(a, b);
}

class AaCbr {
public:
    // Host capacities. The framework is ~12 bytes per attack, so this is about 12 MB, heap-allocated once.
    static constexpr std::int32_t kMaxArgs = 4096;
    static constexpr std::int32_t kMaxAttacks = 1 << 20;

    using Fw = quorum::solver::Framework<kMaxArgs, kMaxAttacks>;
    using Solver = quorum::solver::Grounded<kMaxArgs>;

    /// Argument 0 is the default. Past cases are 1..n. The new case is n+1.
    static constexpr std::int32_t kDefault = 0;

    struct Prediction {
        bool outcome;
        bool default_in;     ///< the default argument is IN the grounded extension
        bool total;          ///< no argument was left Undecided -- must always hold, see the file comment
        std::int32_t steps;  ///< solver work actually done
        std::int32_t bound;  ///< the solver's proven bound for this framework, 2n + 2m
        std::int32_t arguments;
        std::int32_t attacks;
    };

    explicit AaCbr(bool default_outcome, Coherence coherence = Coherence::MajorityPerPattern)
        : delta_(default_outcome),
          coherence_(coherence),
          fw_(std::make_unique<Fw>()),
          solver_(std::make_unique<Solver>()) {}

    /// Learn from `cases`. Identical (features, outcome) pairs always collapse, since a repeated case adds no argument;
    /// what happens to identical features with DIFFERENT outcomes is the `Coherence` policy.
    /// `priors` are cases that did not come from data: rules from a reviewed casebook, written as (features, outcome).
    /// A rule "these observations together mean failure" IS an AA-CBR case, so knowledge and data share one framework
    /// and one semantics, and the specificity ordering alone decides between them.
    ///
    /// One policy decision, the theory-revision principle: data may REFINE a prior, never overrule it at the prior's
    /// own level of specificity. A learned pattern with exactly a prior's features is dropped, so a contradicting record
    /// cannot silently delete reviewed knowledge; to defeat a prior the data must show a strictly MORE specific
    /// situation with the other outcome, which becomes an exception the reviewer can read. Priors are kept even when
    /// they duplicate each other's features with different outcomes -- that is a contradiction in the reviewed
    /// casebook, counted by `incoherent_priors()`, and left for the reviewer rather than resolved by the learner.
    ///
    /// The exception guard (`set_min_exception_support`), added after the MONK-3 replication (PREREGISTRATION.md, v3):
    /// with 5% wrong labels, single records became exceptions that defeated correct rules, and a complete, correct
    /// casebook got worse by being shown data. With a minimum support of m, a learned case may attack a reviewed rule
    /// only if at least m records back it. The default of 1 keeps plain AA-CBR, so every earlier result reproduces.
    void fit(const std::vector<Case>& cases, const std::vector<Case>& priors = {}) {
        // Tally every distinct pattern first. Both policies start here, and incoherence is measured here, before any
        // repair, so the number reported is a property of the data rather than of the policy.
        struct Tally {
            Features x;
            std::int32_t pos;
            std::int32_t neg;
        };
        std::vector<Tally> patterns;
        for (const Case& c : cases) {
            bool found = false;
            for (Tally& t : patterns) {
                if (t.x == c.x) {
                    (c.outcome ? t.pos : t.neg) += 1;
                    found = true;
                    break;
                }
            }
            if (!found) { patterns.push_back({c.x, c.outcome ? 1 : 0, c.outcome ? 0 : 1}); }
        }

        incoherent_ = 0;
        incoherent_priors_ = 0;
        overruled_ = 0;
        unsupported_exceptions_ = 0;
        cases_.clear();
        prior_.clear();
        support_.clear();
        for (const Case& p : priors) {
            bool dup = false;
            for (const Case& k : cases_) {
                if (k.x == p.x && k.outcome == p.outcome) { dup = true; }
                if (k.x == p.x && k.outcome != p.outcome) { ++incoherent_priors_; }
            }
            if (!dup) {
                cases_.push_back(p);
                prior_.push_back(true);
                support_.push_back(kPriorSupport);
            }
        }
        const std::size_t n_priors = cases_.size();
        auto covered_by_prior = [&](Features x) {
            for (std::size_t i = 0; i < n_priors; ++i) {
                if (cases_[i].x == x) { return true; }
            }
            return false;
        };
        for (const Tally& t : patterns) {
            if (t.pos > 0 && t.neg > 0) { ++incoherent_; }
            if (covered_by_prior(t.x)) {
                ++overruled_;
                continue;
            }
            if (coherence_ == Coherence::KeepAll) {
                if (t.pos > 0) { cases_.push_back({t.x, true}); support_.push_back(t.pos); }
                if (t.neg > 0) { cases_.push_back({t.x, false}); support_.push_back(t.neg); }
            } else if (t.pos != t.neg) {
                cases_.push_back({t.x, t.pos > t.neg});
                support_.push_back(t.pos > t.neg ? t.pos : t.neg);
            }
        }
        prior_.resize(cases_.size(), false);

        if (static_cast<std::int64_t>(cases_.size()) + 2 > kMaxArgs) {
            throw std::length_error("AA-CBR: casebase exceeds the host framework's argument capacity");
        }

        build_attacks_();

        // The exception guard. A learned case that directly attacks a reviewed rule must be backed by at least
        // `min_exception_support_` records. Removing one can change concision -- a case it shadowed may now attack
        // the rule directly -- so repeat until nothing changes. Each round removes at least one case, so it ends.
        for (bool changed = min_exception_support_ > 1; changed;) {
            changed = false;
            std::vector<bool> drop(cases_.size(), false);
            for (const auto& e : base_) {
                if (e.second == kDefault || !is_prior(e.second) || is_prior(e.first)) { continue; }
                if (support_[static_cast<std::size_t>(e.first - 1)] < min_exception_support_) {
                    drop[static_cast<std::size_t>(e.first - 1)] = true;
                }
            }
            std::vector<Case> kept_cases;
            std::vector<bool> kept_prior;
            std::vector<std::int32_t> kept_support;
            for (std::size_t i = 0; i < cases_.size(); ++i) {
                if (drop[i]) {
                    ++unsupported_exceptions_;
                    changed = true;
                    continue;
                }
                kept_cases.push_back(cases_[i]);
                kept_prior.push_back(prior_[i]);
                kept_support.push_back(support_[i]);
            }
            if (changed) {
                cases_ = std::move(kept_cases);
                prior_ = std::move(kept_prior);
                support_ = std::move(kept_support);
                build_attacks_();
            }
        }
    }

    /// Records needed before a learned case may attack a reviewed rule. 1 (the default) is plain AA-CBR: one record
    /// is enough. See `fit`.
    void set_min_exception_support(std::int32_t m) noexcept { min_exception_support_ = m < 1 ? 1 : m; }
    /// Learned exceptions to reviewed rules dropped by the guard, over all rounds.
    [[nodiscard]] std::int32_t unsupported_exceptions() const noexcept { return unsupported_exceptions_; }

private:
    static constexpr std::int32_t kPriorSupport = 1 << 30;

    // The query-independent attacks: past case -> past case, and past case -> default.
    void build_attacks_() {
        base_.clear();
        const std::int32_t n = static_cast<std::int32_t>(cases_.size());
        for (std::int32_t a = 1; a <= n; ++a) {
            const Case& ca = case_at(a);
            for (std::int32_t b = 0; b <= n; ++b) {
                if (b == a) { continue; }
                const Features xb = b == kDefault ? Features{0} : case_at(b).x;
                const bool ob = b == kDefault ? delta_ : case_at(b).outcome;
                if (ca.outcome == ob) { continue; }                 // (1) outcomes differ
                if (!strict_subset_of(xb, ca.x)) { continue; }      // (2) attacker strictly more specific
                if (!concise_(a, xb)) { continue; }                  // (3) nothing of A's outcome in between
                base_.emplace_back(a, b);
            }
        }
    }

public:
    /// Decide `query` against the learned casebase.
    [[nodiscard]] Prediction predict(Features query) {
        const std::int32_t n = static_cast<std::int32_t>(cases_.size());
        const std::int32_t newcase = n + 1;

        // declare() refuses on a finalized framework -- deliberately, so a device cannot rebuild a casebook by accident.
        // A host re-deciding per query has to say so.
        fw_->reset();
        check_(fw_->declare(n + 2));
        for (const auto& e : base_) { check_(fw_->add_attack(Arg{e.first}, Arg{e.second})); }
        for (std::int32_t i = 1; i <= n; ++i) {
            if (!subset_of(case_at(i).x, query)) { check_(fw_->add_attack(Arg{newcase}, Arg{i})); }
        }
        check_(fw_->finalize());
        check_(solver_->solve(*fw_));

        bool total = true;
        for (std::int32_t i = 0; i <= newcase; ++i) {
            if (solver_->label(Arg{i}) == quorum::solver::Label::Undecided) { total = false; break; }
        }

        const bool in = solver_->accepted(Arg{kDefault});
        return Prediction{in ? delta_ : !delta_,
                          in,
                          total,
                          solver_->steps(),
                          Solver::step_bound(fw_->argument_count(), fw_->attack_count()),
                          fw_->argument_count(),
                          fw_->attack_count()};
    }

    [[nodiscard]] bool default_outcome() const noexcept { return delta_; }
    [[nodiscard]] const std::vector<Case>& cases() const noexcept { return cases_; }
    [[nodiscard]] const std::vector<std::pair<std::int32_t, std::int32_t>>& base_attacks() const noexcept {
        return base_;
    }
    /// Distinct feature patterns that occurred with both outcomes in the training data, before any repair.
    [[nodiscard]] std::int32_t incoherent_pairs() const noexcept { return incoherent_; }
    [[nodiscard]] Coherence coherence() const noexcept { return coherence_; }
    /// Whether past case i (1-based, as in the framework) came from `priors` rather than from data.
    [[nodiscard]] bool is_prior(std::int32_t i) const { return prior_.at(static_cast<std::size_t>(i - 1)); }
    /// Prior pairs with identical features and different outcomes: a contradiction in the reviewed casebook.
    [[nodiscard]] std::int32_t incoherent_priors() const noexcept { return incoherent_priors_; }
    /// Learned patterns dropped because a prior has exactly their features.
    [[nodiscard]] std::int32_t overruled_patterns() const noexcept { return overruled_; }

private:
    using Arg = quorum::solver::Arg;

    [[nodiscard]] const Case& case_at(std::int32_t a) const { return cases_[static_cast<std::size_t>(a - 1)]; }

    /// Concision: A may attack a target with features `xb` only if no past case C, with A's outcome, satisfies
    /// xb < C.x < A.x strictly.
    [[nodiscard]] bool concise_(std::int32_t a, Features xb) const {
        const Case& ca = case_at(a);
        for (const Case& c : cases_) {
            if (c.outcome != ca.outcome) { continue; }
            if (strict_subset_of(xb, c.x) && strict_subset_of(c.x, ca.x)) { return false; }
        }
        return true;
    }

    static void check_(quorum::solver::Status s) {
        if (s != quorum::solver::Status::Ok) { throw std::runtime_error("AA-CBR: framework operation failed"); }
    }

    bool delta_;
    Coherence coherence_;
    std::vector<Case> cases_;
    std::vector<bool> prior_;
    std::vector<std::int32_t> support_;  // records behind each case; priors count as unlimited
    std::int32_t min_exception_support_ = 1;
    std::int32_t unsupported_exceptions_ = 0;
    std::int32_t incoherent_priors_ = 0;
    std::int32_t overruled_ = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> base_;
    std::int32_t incoherent_ = 0;
    std::unique_ptr<Fw> fw_;
    std::unique_ptr<Solver> solver_;
};

}  // namespace quorum::learn
