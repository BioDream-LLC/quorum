// Tests for the AA-CBR learner.
//
// Two kinds. Hand-worked cases, each small enough to check on paper, pin down the three attack conditions and the
// new-case rule one at a time. Then a differential test against an oracle that shares NO code with the implementation:
// it derives the attacks from the definition by brute force over explicit sets, and labels by recursion over "IN iff
// every attacker is OUT", which is only valid because the framework is acyclic -- and the recursion would not terminate
// if it were not, which makes the oracle a second check on that claim.

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "quorum/learn/aacbr.hpp"

using quorum::learn::AaCbr;
using quorum::learn::Case;
using quorum::learn::Coherence;
using quorum::learn::Features;

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_failures;
    }
}

constexpr Features A = 1U << 0;
constexpr Features B = 1U << 1;
constexpr Features C = 1U << 2;

// ── the oracle ─────────────────────────────────────────────────────────────────────────────────────────────────────
//
// Arguments: index 0 default, 1..n cases, n+1 new case -- the same numbering, since the prediction is what is compared,
// not the internals. Everything else is re-derived.

struct OracleArg {
    Features x;
    bool outcome;
    bool is_new;
};

bool oracle_attacks(const std::vector<OracleArg>& args, std::size_t a, std::size_t b) {
    const OracleArg& p = args[a];
    const OracleArg& q = args[b];
    if (a == b) { return false; }
    if (q.is_new) { return false; }                     // nothing attacks the new case
    if (p.is_new) {                                     // new case attacks cases with a feature it lacks
        if (b == 0) { return false; }
        for (int f = 0; f < 32; ++f) {
            const bool in_q = ((q.x >> f) & 1U) != 0U;
            const bool in_p = ((p.x >> f) & 1U) != 0U;
            if (in_q && !in_p) { return true; }
        }
        return false;
    }
    if (a == 0) { return false; }                       // the default attacks nothing
    if (p.outcome == q.outcome) { return false; }
    // strictly more specific, feature by feature
    bool strictly_more = false;
    for (int f = 0; f < 32; ++f) {
        const bool in_q = ((q.x >> f) & 1U) != 0U;
        const bool in_p = ((p.x >> f) & 1U) != 0U;
        if (in_q && !in_p) { return false; }
        if (in_p && !in_q) { strictly_more = true; }
    }
    if (!strictly_more) { return false; }
    // concision
    for (std::size_t c = 1; c + 1 < args.size(); ++c) {
        const OracleArg& r = args[c];
        if (r.outcome != p.outcome) { continue; }
        const bool q_in_r = (q.x & ~r.x) == 0U && q.x != r.x;
        const bool r_in_p = (r.x & ~p.x) == 0U && r.x != p.x;
        if (q_in_r && r_in_p) { return false; }
    }
    return true;
}

bool oracle_in(const std::vector<OracleArg>& args, std::size_t a, int depth) {
    if (depth > static_cast<int>(args.size()) + 1) {
        std::printf("FAIL  oracle recursion exceeded n: the framework is NOT acyclic\n");
        ++g_failures;
        return false;
    }
    for (std::size_t b = 0; b < args.size(); ++b) {
        if (oracle_attacks(args, b, a) && oracle_in(args, b, depth + 1)) { return false; }
    }
    return true;
}

bool oracle_predict(const std::vector<Case>& cases, bool delta, Features query, Coherence policy,
                    const std::vector<Case>& priors = {}, int min_exception_support = 1) {
    // Priors first, exact duplicates collapsed; then the data, skipping any pattern a prior already has.
    std::vector<Case> uniq;
    std::vector<int> support;  // records behind each; a prior is unlimited
    std::vector<bool> is_prior;
    for (const Case& p : priors) {
        bool dup = false;
        for (const Case& u : uniq) { dup = dup || (u.x == p.x && u.outcome == p.outcome); }
        if (!dup) {
            uniq.push_back(p);
            support.push_back(1 << 30);
            is_prior.push_back(true);
        }
    }
    for (std::size_t i = 0; i < cases.size(); ++i) {
        bool prior_has_it = false;
        for (const Case& p : priors) { prior_has_it = prior_has_it || p.x == cases[i].x; }
        if (prior_has_it) { continue; }
        bool first_of_pattern = true;
        for (std::size_t j = 0; j < i; ++j) { first_of_pattern = first_of_pattern && cases[j].x != cases[i].x; }
        if (!first_of_pattern) { continue; }
        int pos = 0, neg = 0;
        for (const Case& c : cases) {
            if (c.x == cases[i].x) { (c.outcome ? pos : neg) += 1; }
        }
        if (policy == Coherence::KeepAll) {
            if (pos > 0) { uniq.push_back({cases[i].x, true}); support.push_back(pos); is_prior.push_back(false); }
            if (neg > 0) { uniq.push_back({cases[i].x, false}); support.push_back(neg); is_prior.push_back(false); }
        } else if (pos != neg) {
            uniq.push_back({cases[i].x, pos > neg});
            support.push_back(pos > neg ? pos : neg);
            is_prior.push_back(false);
        }
    }
    auto frame = [&](Features q) {
        std::vector<OracleArg> args;
        args.push_back({0U, delta, false});
        for (const Case& c : uniq) { args.push_back({c.x, c.outcome, false}); }
        args.push_back({q, false, true});
        return args;
    };
    // The guard, from its definition: drop every learned case with too little support that attacks a prior, and
    // repeat, since a drop can expose another.
    for (bool changed = true; changed;) {
        changed = false;
        const std::vector<OracleArg> args = frame(0U);
        std::vector<bool> drop(uniq.size(), false);
        for (std::size_t a = 0; a < uniq.size(); ++a) {
            if (is_prior[a] || support[a] >= min_exception_support) { continue; }
            for (std::size_t b = 0; b < uniq.size(); ++b) {
                if (is_prior[b] && oracle_attacks(args, a + 1, b + 1)) { drop[a] = true; }
            }
        }
        for (std::size_t a = uniq.size(); a-- > 0;) {
            if (!drop[a]) { continue; }
            uniq.erase(uniq.begin() + static_cast<std::ptrdiff_t>(a));
            support.erase(support.begin() + static_cast<std::ptrdiff_t>(a));
            is_prior.erase(is_prior.begin() + static_cast<std::ptrdiff_t>(a));
            changed = true;
        }
    }
    const std::vector<OracleArg> args = frame(query);
    return oracle_in(args, 0, 0) ? delta : !delta;
}

// ── hand-worked cases ──────────────────────────────────────────────────────────────────────────────────────────────

void one_case_flips_the_default() {
    AaCbr m(false);
    m.fit({{A, true}});
    expect(m.predict(A).outcome == true, "({a},+) against default -: query {a} is +");
    expect(m.predict(0U).outcome == false, "({a},+) against default -: query {} is - (case irrelevant, attacked)");
    expect(m.predict(B).outcome == false, "({a},+) against default -: query {b} is - (case irrelevant)");
}

void a_more_specific_case_reinstates_the_default() {
    AaCbr m(false);
    m.fit({{A, true}, {A | B, false}});
    expect(m.predict(A | B).outcome == false, "reinstatement: {a,b} is - because ({a,b},-) defeats ({a},+)");
    expect(m.predict(A).outcome == true, "reinstatement: {a} is + because ({a,b},-) is irrelevant");
    expect(m.predict(A | B | C).outcome == false, "reinstatement: extra features do not change it");
}

void concision_removes_the_redundant_attack() {
    AaCbr m(false);
    m.fit({{A, true}, {A | B, true}});
    // ({a,b},+) would attack the default, but ({a},+) lies strictly between, so only ({a},+) does.
    expect(m.base_attacks().size() == 1U, "concision: only the nearest same-outcome case attacks the default");
    expect(m.predict(A | B).outcome == true, "concision: prediction unaffected");
    expect(m.predict(B).outcome == false, "concision: {b} alone is -, since ({a},+) and ({a,b},+) are irrelevant");
}

void same_outcome_never_attacks() {
    AaCbr m(true);
    m.fit({{A, true}, {A | B, true}});
    expect(m.base_attacks().empty(), "cases agreeing with the default attack nothing");
    expect(m.predict(A | B).outcome == true, "... and the default stands");
}

void keep_all_is_vanilla() {
    AaCbr m(false, Coherence::KeepAll);
    m.fit({{A, true}, {A, true}, {B, true}, {B, false}});
    expect(m.cases().size() == 3U, "KeepAll: an exact duplicate adds no argument, a conflicting one does");
    expect(m.incoherent_pairs() == 1, "KeepAll: ({b},+) and ({b},-) are counted as incoherent");
    expect(m.predict(B).total, "KeepAll: an incoherent pair still leaves a total labelling");
}

// The measured failure, in miniature. 1 positive and 9 negatives share pattern {b}; the default is negative.
// Vanilla AA-CBR keeps ({b},+) as an argument, it attacks the default, and every {b} query is called positive --
// wrong for 9 of the 10 training cases that had it.
void one_outlier_overrules_the_majority_under_keep_all() {
    std::vector<Case> cs{{B, true}};
    for (int i = 0; i < 9; ++i) { cs.push_back({B, false}); }

    AaCbr vanilla(false, Coherence::KeepAll);
    vanilla.fit(cs);
    expect(vanilla.predict(B).outcome == true, "KeepAll: the lone positive overrules nine negatives (the defect)");

    AaCbr repaired(false, Coherence::MajorityPerPattern);
    repaired.fit(cs);
    expect(repaired.cases().size() == 1U, "Majority: one case per pattern");
    expect(repaired.predict(B).outcome == false, "Majority: nine negatives outweigh one positive");
    expect(repaired.incoherent_pairs() == 1, "Majority: incoherence is still reported, measured before repair");
}

void a_tie_contributes_no_argument() {
    AaCbr m(false, Coherence::MajorityPerPattern);
    m.fit({{B, true}, {B, false}, {A, true}});
    expect(m.cases().size() == 1U, "Majority: a 1-1 tie on {b} keeps no case");
    expect(m.predict(B).outcome == false, "Majority: with no {b} argument, the default stands");
    expect(m.predict(A).outcome == true, "Majority: the untied pattern still decides");
}

// ── properties over random casebases ───────────────────────────────────────────────────────────────────────────────

void random_casebases_agree_with_the_oracle() {
    std::mt19937 rng(20260929U);
    int queries = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const int nfeat = 1 + static_cast<int>(rng() % 6U);
        const int ncases = static_cast<int>(rng() % 14U);
        const Features mask = (1U << nfeat) - 1U;
        std::vector<Case> cases;
        for (int i = 0; i < ncases; ++i) { cases.push_back({static_cast<Features>(rng()) & mask, (rng() & 1U) != 0U}); }
        const bool delta = (rng() & 1U) != 0U;
        const Coherence policy = (trial & 1) == 0 ? Coherence::KeepAll : Coherence::MajorityPerPattern;

        AaCbr m(delta, policy);
        m.fit(cases);
        // Every query over the feature space -- exhaustive, not sampled, for spaces this small.
        for (Features q = 0; q <= mask; ++q) {
            const auto p = m.predict(q);
            ++queries;
            if (!p.total) {
                expect(false, "labelling was not total -- the framework had a cycle");
                return;
            }
            if (p.steps > p.bound) {
                expect(false, "solver exceeded its proven step bound");
                return;
            }
            if (p.outcome != oracle_predict(cases, delta, q, policy)) {
                std::printf("      trial %d query 0x%X: learner %d, oracle %d\n", trial, q, p.outcome ? 1 : 0,
                            oracle_predict(cases, delta, q, policy) ? 1 : 0);
                expect(false, "learner disagrees with the independent oracle");
                return;
            }
        }
    }
    std::printf("  random: %d queries over 400 casebases, both policies, all total, within bound, agree with oracle\n",
                queries);
}

// ── priors: reviewed casebook rules inserted alongside the data ────────────────────────────────────────────────────

// The rule "a means failure" with no data at all is exactly the casebook alone.
void a_prior_alone_is_the_casebook() {
    AaCbr m(false);
    m.fit({}, {{A, true}});
    expect(m.predict(A).outcome == true, "prior: rule fires when its observation is present");
    expect(m.predict(B).outcome == false, "prior: rule is silent when its observation is absent");
    expect(m.is_prior(1), "prior: provenance is recorded");
    AaCbr d(false);
    d.fit({}, {{A, true}, {A, true}});
    expect(d.cases().size() == 1U, "prior: a repeated rule adds no argument (it would only cost device capacity)");
}

// Data with exactly the rule's features and the other outcome must not delete the rule.
void data_cannot_overrule_a_prior_at_its_own_specificity() {
    AaCbr m(false);
    m.fit({{A, false}, {A, false}, {A, false}}, {{A, true}});
    expect(m.overruled_patterns() == 1, "prior: the contradicting pattern is dropped and counted");
    expect(m.predict(A).outcome == true, "prior: three contradicting records at the same specificity do not delete it");
}

// ...but a strictly more specific record with the other outcome is an exception, and it wins where it applies.
void data_refines_a_prior_with_a_more_specific_exception() {
    AaCbr m(false);
    m.fit({{A | B, false}}, {{A, true}});
    expect(m.predict(A).outcome == true, "prior: rule still fires without the exception's extra observation");
    expect(m.predict(A | B).outcome == false, "prior: the learned exception defeats the rule where it applies");
    expect(!m.is_prior(2), "prior: the exception is recorded as learned");
}

// Data adds a failure mode the casebook does not know.
void data_adds_what_the_casebook_lacks() {
    AaCbr m(false);
    m.fit({{B, true}, {0U, false}}, {{A, true}});
    expect(m.predict(A).outcome && m.predict(B).outcome, "prior: known mode and learned mode both fire");
    expect(!m.predict(0U).outcome, "prior: neither present, the default stands");
}

// ── the exception guard ────────────────────────────────────────────────────────────────────────────────────────────

void one_record_cannot_defeat_a_rule_when_guarded() {
    AaCbr m(false);
    m.set_min_exception_support(2);
    m.fit({{A | B, false}}, {{A, true}});
    expect(m.predict(A | B).outcome == true, "guard: a single contradicting record does not defeat the rule");
    expect(m.unsupported_exceptions() == 1, "guard: the dropped exception is counted");
}

void two_records_can_defeat_a_rule_when_guarded() {
    AaCbr m(false);
    m.set_min_exception_support(2);
    m.fit({{A | B, false}, {A | B, false}}, {{A, true}});
    expect(m.predict(A | B).outcome == false, "guard: two agreeing records do defeat the rule");
    expect(m.predict(A).outcome == true, "guard: the rule still holds where the exception does not apply");
}

// Dropping the weak exception {a,b} exposes {a,b,c} as a direct attacker of the rule; it has support 3, so it stays.
void dropping_an_exception_can_expose_another() {
    AaCbr m(false);
    m.set_min_exception_support(2);
    m.fit({{A | B, false}, {A | B | C, false}, {A | B | C, false}, {A | B | C, false}}, {{A, true}});
    expect(m.predict(A | B).outcome == true, "guard: the weak exception is gone");
    expect(m.predict(A | B | C).outcome == false, "guard: the exposed, well-supported exception now attacks the rule");
}

void the_guard_ignores_cases_that_attack_no_rule() {
    AaCbr m(false);
    m.set_min_exception_support(5);
    m.fit({{B, true}});
    expect(m.predict(B).outcome == true, "guard: with no rule to protect, one record still teaches");
}

void random_guarded_priors_agree_with_the_oracle() {
    std::mt19937 rng(20261001U);
    int queries = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const int nfeat = 1 + static_cast<int>(rng() % 6U);
        const Features mask = (1U << nfeat) - 1U;
        std::vector<Case> cases, priors;
        const int ncases = static_cast<int>(rng() % 20U);
        const int npriors = 1 + static_cast<int>(rng() % 4U);
        for (int i = 0; i < ncases; ++i) { cases.push_back({static_cast<Features>(rng()) & mask, (rng() & 1U) != 0U}); }
        for (int i = 0; i < npriors; ++i) { priors.push_back({static_cast<Features>(rng()) & mask, (rng() & 1U) != 0U}); }
        const bool delta = (rng() & 1U) != 0U;
        const Coherence policy = (trial & 1) == 0 ? Coherence::KeepAll : Coherence::MajorityPerPattern;
        const int msup = 2 + static_cast<int>(rng() % 3U);
        AaCbr m(delta, policy);
        m.set_min_exception_support(msup);
        m.fit(cases, priors);
        for (Features q = 0; q <= mask; ++q) {
            const auto p = m.predict(q);
            ++queries;
            if (!p.total || p.steps > p.bound) {
                expect(false, "guard: labelling not total, or over bound");
                return;
            }
            if (p.outcome != oracle_predict(cases, delta, q, policy, priors, msup)) {
                std::printf("      trial %d query 0x%X disagrees\n", trial, q);
                expect(false, "guard: learner disagrees with the independent oracle");
                return;
            }
        }
    }
    std::printf("  guard: %d queries over 400 guarded casebases, all total, within bound, agree with oracle\n", queries);
}

void random_priors_agree_with_the_oracle() {
    std::mt19937 rng(20260930U);
    int queries = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const int nfeat = 1 + static_cast<int>(rng() % 6U);
        const Features mask = (1U << nfeat) - 1U;
        std::vector<Case> cases, priors;
        const int ncases = static_cast<int>(rng() % 14U);
        const int npriors = static_cast<int>(rng() % 5U);
        for (int i = 0; i < ncases; ++i) { cases.push_back({static_cast<Features>(rng()) & mask, (rng() & 1U) != 0U}); }
        for (int i = 0; i < npriors; ++i) { priors.push_back({static_cast<Features>(rng()) & mask, (rng() & 1U) != 0U}); }
        const bool delta = (rng() & 1U) != 0U;
        const Coherence policy = (trial & 1) == 0 ? Coherence::KeepAll : Coherence::MajorityPerPattern;
        AaCbr m(delta, policy);
        m.fit(cases, priors);
        for (Features q = 0; q <= mask; ++q) {
            const auto p = m.predict(q);
            ++queries;
            if (!p.total || p.steps > p.bound) {
                expect(false, "priors: labelling not total, or over bound");
                return;
            }
            if (p.outcome != oracle_predict(cases, delta, q, policy, priors)) {
                std::printf("      trial %d query 0x%X disagrees\n", trial, q);
                expect(false, "priors: learner disagrees with the independent oracle");
                return;
            }
        }
    }
    std::printf("  priors: %d queries over 400 casebases with rules, all total, within bound, agree with oracle\n",
                queries);
}

}  // namespace

int main() {
    one_case_flips_the_default();
    a_more_specific_case_reinstates_the_default();
    concision_removes_the_redundant_attack();
    same_outcome_never_attacks();
    keep_all_is_vanilla();
    one_outlier_overrules_the_majority_under_keep_all();
    a_tie_contributes_no_argument();
    random_casebases_agree_with_the_oracle();
    a_prior_alone_is_the_casebook();
    data_cannot_overrule_a_prior_at_its_own_specificity();
    data_refines_a_prior_with_a_more_specific_exception();
    data_adds_what_the_casebook_lacks();
    random_priors_agree_with_the_oracle();
    one_record_cannot_defeat_a_rule_when_guarded();
    two_records_can_defeat_a_rule_when_guarded();
    dropping_an_exception_can_expose_another();
    the_guard_ignores_cases_that_attack_no_rule();
    random_guarded_priors_agree_with_the_oracle();
    if (g_failures == 0) { std::printf("test_aacbr: all passed\n"); }
    return g_failures == 0 ? 0 : 1;
}
