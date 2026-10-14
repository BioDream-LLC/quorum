// Does AA-CBR, decided by quorum's grounded solver, learn anything? Measured on real data against honest baselines.
//
//   eval_learn voting    <house-votes-84.data>
//   eval_learn tictactoe <tic-tac-toe.data>
//
// ONE program for every dataset, deliberately. The protocol -- folds, seed, nested selection, baselines, assertions --
// is the part that must not differ between datasets, and a copy per dataset would guarantee that the next fix to it
// lands in only one copy (PROC-D16, which has already happened twice in this repository). Only the loaders differ.
//
// Datasets (both CC BY 4.0, see learn/data/SOURCE.txt):
//   voting     UCI Congressional Voting Records. 435 rows, 16 votes, party. Each vote is two features, "voted y on i"
//              and "voted n on i", so a '?' is the absence of both and asserts nothing. 32 features.
//   tictactoe  UCI Tic-Tac-Toe Endgame. 958 boards, 9 cells each x/o/blank, "does x have three in a row". Each cell is
//              three features, so 27 -- inside the 30 observations a device casebook can carry. Chosen because no
//              single feature settles it: the concept is a disjunction of eight lines, where the voting data is
//              dominated by one vote and every method collapses onto the same one-feature rule.
//
// Protocol: 10-fold cross-validation over a FIXED shuffle, so the number is reproducible bit-for-bit. Every method sees
// the same folds. Nothing is tuned on the test folds -- there is nothing to tune: AA-CBR's only choice is the default
// outcome, and that is set to the training fold's majority class, not chosen by looking at results.
//
// Baselines, chosen to be the ones a sceptic would reach for:
//   majority        predicts the training fold's commonest class. Anything that cannot beat this learned nothing.
//   1-NN Hamming    nearest training case by feature disagreement. The obvious case-based method WITHOUT
//                   argumentation, so it isolates what the argumentation adds.
//   naive Bayes     Laplace-smoothed, per-feature. The standard cheap probabilistic learner.
//
// The ctest assertions are the claims that must hold for the result to mean anything: every AA-CBR labelling total,
// every solve inside its proven bound, and AA-CBR strictly better than majority. Accuracy against the other baselines
// is REPORTED, not asserted -- asserting it would turn a measurement into a target.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/learn/aacbr.hpp"
#include "quorum/learn/baselines.hpp"
#include "quorum/learn/features.hpp"

using quorum::learn::AaCbr;
using quorum::learn::Case;
using quorum::learn::Coherence;
using quorum::learn::Features;
using quorum::learn::NaiveBayes;
using quorum::learn::nearest_neighbour;
using quorum::learn::project;
using quorum::learn::top_k_features;

namespace {

constexpr int kFolds = 10;
constexpr std::uint32_t kSeed = 20260929U;
// Set by the loader. Every loop over features goes to this, never to a constant.
int g_features = 0;

std::vector<Case> load_voting(const char* path, int* rows_seen) {
    g_features = 32;
    std::ifstream in(path);
    std::vector<Case> out;
    std::string line;
    *rows_seen = 0;
    while (std::getline(in, line)) {
        if (line.empty()) { continue; }
        ++*rows_seen;
        std::stringstream ss(line);
        std::string cell;
        std::getline(ss, cell, ',');
        const bool republican = cell == "republican";
        Features x = 0;
        for (int i = 0; i < 16 && std::getline(ss, cell, ','); ++i) {
            if (cell == "y") { x |= 1U << i; }
            if (cell == "n") { x |= 1U << (16 + i); }
        }
        out.push_back({x, republican});
    }
    return out;
}

std::vector<Case> load_tictactoe(const char* path, int* rows_seen) {
    g_features = 27;
    std::ifstream in(path);
    std::vector<Case> out;
    std::string line;
    *rows_seen = 0;
    while (std::getline(in, line)) {
        if (line.empty()) { continue; }
        ++*rows_seen;
        std::stringstream ss(line);
        std::string cell;
        Features x = 0;
        for (int i = 0; i < 9 && std::getline(ss, cell, ','); ++i) {
            if (cell == "x") { x |= 1U << i; }
            if (cell == "o") { x |= 1U << (9 + i); }
            if (cell == "b") { x |= 1U << (18 + i); }
        }
        std::getline(ss, cell, ',');
        out.push_back({x, cell == "positive"});
    }
    return out;
}

struct Tally {
    int correct = 0;
    int total = 0;
    std::vector<double> per_fold;
    void add_fold(int c, int t) {
        correct += c;
        total += t;
        per_fold.push_back(t == 0 ? 0.0 : static_cast<double>(c) / t);
    }
    double mean() const { return total == 0 ? 0.0 : static_cast<double>(correct) / total; }
    double sd() const {
        double m = 0.0;
        for (double v : per_fold) { m += v; }
        m /= static_cast<double>(per_fold.size());
        double s = 0.0;
        for (double v : per_fold) { s += (v - m) * (v - m); }
        return std::sqrt(s / static_cast<double>(per_fold.size() - 1));
    }
};


enum class Method { Majority, NearestNeighbour, Bayes, AaCbrVanilla, AaCbr };

const char* method_name(Method m) {
    switch (m) {
        case Method::Majority: return "majority class";
        case Method::NearestNeighbour: return "1-NN, Hamming";
        case Method::Bayes: return "naive Bayes";
        case Method::AaCbrVanilla: return "AA-CBR, vanilla";
        case Method::AaCbr: return "AA-CBR, majority";
    }
    return "?";
}

struct AaStats {
    int not_total = 0;
    int over_bound = 0;
    std::int32_t max_steps = 0;
    std::int32_t max_bound = 0;
    std::int32_t max_args = 0;
    std::int32_t max_attacks = 0;
    int queries = 0;
};

/// Correct predictions on `test` after learning from `train`, with features restricted to the `k` highest-gain ones
/// measured on `train` ALONE. k = 32 means no selection.
int score(Method m, const std::vector<Case>& train_raw, const std::vector<Case>& test_raw, int k, AaStats* st) {
    const Features mask = k >= g_features ? ~Features{0} : top_k_features(train_raw, k, g_features);
    const std::vector<Case> train = project(train_raw, mask);
    const std::vector<Case> test = project(test_raw, mask);
    const bool delta = quorum::learn::majority_outcome(train);
    int correct = 0;
    switch (m) {
        case Method::Majority:
            for (const Case& q : test) { correct += delta == q.outcome ? 1 : 0; }
            break;
        case Method::NearestNeighbour:
            for (const Case& q : test) { correct += nearest_neighbour(train, q.x, delta) == q.outcome ? 1 : 0; }
            break;
        case Method::Bayes: {
            NaiveBayes nb;
            nb.fit(train, g_features);
            for (const Case& q : test) { correct += nb.predict(q.x) == q.outcome ? 1 : 0; }
            break;
        }
        case Method::AaCbrVanilla:
        case Method::AaCbr: {
            AaCbr aa(delta, m == Method::AaCbr ? Coherence::MajorityPerPattern : Coherence::KeepAll);
            aa.fit(train);
            for (const Case& q : test) {
                const auto p = aa.predict(q.x);
                correct += p.outcome == q.outcome ? 1 : 0;
                if (st != nullptr) {
                    ++st->queries;
                    st->not_total += p.total ? 0 : 1;
                    st->over_bound += p.steps > p.bound ? 1 : 0;
                    st->max_steps = std::max(st->max_steps, p.steps);
                    st->max_bound = std::max(st->max_bound, p.bound);
                    st->max_args = std::max(st->max_args, p.arguments);
                    st->max_attacks = std::max(st->max_attacks, p.attacks);
                }
            }
            break;
        }
    }
    return correct;
}

void split(const std::vector<Case>& data, const std::vector<std::size_t>& order, int folds, int fold,
           std::vector<Case>* train, std::vector<Case>* test) {
    train->clear();
    test->clear();
    for (std::size_t i = 0; i < order.size(); ++i) {
        (static_cast<int>(i % static_cast<std::size_t>(folds)) == fold ? test : train)->push_back(data[order[i]]);
    }
}

// Candidate k values. 99 stands for "every feature the dataset has"; values above g_features are skipped.
const int kCandidateK[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 99};

int effective_k(int k) { return k >= g_features ? g_features : k; }
bool candidate_applies(int k) { return k == 99 || k < g_features; }

/// Choose k for `m` by 5-fold cross-validation INSIDE `train`. The outer test fold is never seen.
int choose_k(Method m, const std::vector<Case>& train) {
    std::vector<std::size_t> order(train.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::mt19937 rng(kSeed + 1U);
    std::shuffle(order.begin(), order.end(), rng);
    int best_k = g_features;
    int best = -1;
    for (int kc : kCandidateK) {
        if (!candidate_applies(kc)) { continue; }
        const int k = effective_k(kc);
        int total = 0;
        for (int f = 0; f < 5; ++f) {
            std::vector<Case> tr, va;
            split(train, order, 5, f, &tr, &va);
            total += score(m, tr, va, k, nullptr);
        }
        if (total > best) { best = total; best_k = k; }  // strict: ties keep the smaller k
    }
    return best_k;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: eval_learn voting|tictactoe <data file>\n");
        return 2;
    }
    const std::string which = argv[1];
    const char* path = argv[2];
    int rows = 0;
    std::vector<Case> data;
    const char* title = "";
    if (which == "voting") {
        data = load_voting(path, &rows);
        title = "UCI Congressional Voting Records";
    } else if (which == "tictactoe") {
        data = load_tictactoe(path, &rows);
        title = "UCI Tic-Tac-Toe Endgame";
    } else {
        std::printf("FAIL  unknown dataset '%s'\n", which.c_str());
        return 2;
    }
    if (data.empty()) {
        std::printf("FAIL  could not read %s\n", path);
        return 1;
    }

    std::vector<std::size_t> order(data.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::mt19937 rng(kSeed);
    std::shuffle(order.begin(), order.end(), rng);

    const Method methods[] = {Method::Majority, Method::NearestNeighbour, Method::Bayes, Method::AaCbrVanilla,
                              Method::AaCbr};
    constexpr int kMethods = 5;

    std::printf("AA-CBR on quorum's grounded solver -- %s\n", title);
    std::printf("  rows read %d, cases %zu, %d-fold CV, fixed shuffle seed %u\n", rows, data.size(), kFolds, kSeed);

    // ── 1. no feature selection: every method sees all 32 features ─────────────────────────────────────────────────
    std::printf("\n1. ALL %d FEATURES\n  %-22s %8s %10s   %s\n", g_features, "method", "accuracy", "fold sd", "correct / total");
    Tally all32[kMethods];
    AaStats st_all;
    AaStats st_vanilla;
    for (int fold = 0; fold < kFolds; ++fold) {
        std::vector<Case> train, test;
        split(data, order, kFolds, fold, &train, &test);
        for (int mi = 0; mi < kMethods; ++mi) {
            const Method m = methods[mi];
            all32[mi].add_fold(score(m, train, test, g_features, m == Method::AaCbr ? &st_all : m == Method::AaCbrVanilla ? &st_vanilla : nullptr),
                               static_cast<int>(test.size()));
        }
    }
    auto row = [](const char* name, const Tally& t) {
        std::printf("  %-22s %7.2f%% %9.2f%%   %d / %d\n", name, 100.0 * t.mean(), 100.0 * t.sd(), t.correct,
                    t.total);
    };
    for (int mi = 0; mi < kMethods; ++mi) { row(method_name(methods[mi]), all32[mi]); }

    // ── 2. the honest number: k chosen by nested CV inside each training fold ──────────────────────────────────────
    std::printf("\n2. FEATURES SELECTED BY INFORMATION GAIN, k CHOSEN BY NESTED 5-FOLD CV ON TRAINING DATA ONLY\n");
    std::printf("   (the outer test fold is never used to choose anything -- this is the result)\n");
    std::printf("  %-22s %8s %10s   %-15s  %s\n", "method", "accuracy", "fold sd", "correct / total", "k per fold");
    Tally nested[kMethods];
    AaStats st_nested;
    std::string ks[kMethods];
    for (int fold = 0; fold < kFolds; ++fold) {
        std::vector<Case> train, test;
        split(data, order, kFolds, fold, &train, &test);
        for (int mi = 0; mi < kMethods; ++mi) {
            const Method m = methods[mi];
            const int k = choose_k(m, train);
            ks[mi] += (fold == 0 ? "" : ",") + std::to_string(k);
            nested[mi].add_fold(score(m, train, test, k, m == Method::AaCbr ? &st_nested : nullptr),
                                static_cast<int>(test.size()));
        }
    }
    for (int mi = 0; mi < kMethods; ++mi) {
        std::printf("  %-22s %7.2f%% %9.2f%%   %3d / %-9d  %s\n", method_name(methods[mi]), 100.0 * nested[mi].mean(),
                    100.0 * nested[mi].sd(), nested[mi].correct, nested[mi].total, ks[mi].c_str());
    }

    // ── 3. the whole curve, for understanding only ─────────────────────────────────────────────────────────────────
    std::printf("\n3. ACCURACY BY k -- DESCRIPTIVE ONLY. Reading the best k off this table would be tuning on test data.\n");
    std::printf("  %4s", "k");
    for (int mi = 1; mi < kMethods; ++mi) { std::printf("  %18s", method_name(methods[mi])); }
    std::printf("\n");
    for (int kc : kCandidateK) {
        if (!candidate_applies(kc)) { continue; }
        const int k = effective_k(kc);
        std::printf("  %4d", k);
        for (int mi = 1; mi < kMethods; ++mi) {
            int c = 0, t = 0;
            for (int fold = 0; fold < kFolds; ++fold) {
                std::vector<Case> train, test;
                split(data, order, kFolds, fold, &train, &test);
                c += score(methods[mi], train, test, k, nullptr);
                t += static_cast<int>(test.size());
            }
            std::printf("  %17.2f%%", 100.0 * c / t);
        }
        std::printf("\n");
    }

    auto stats = [](const char* name, const AaStats& s) {
        std::printf("  %s: largest framework %d arguments, %d attacks; max steps %d against proven bound %d\n", name,
                    s.max_args, s.max_attacks, s.max_steps, s.max_bound);
        std::printf("  %*s  labelling not total on %d of %d queries, over bound on %d\n", static_cast<int>(std::strlen(name)),
                    "", s.not_total, s.queries, s.over_bound);
    };
    std::printf("\nAA-CBR frameworks\n");
    stats("vanilla, all features ", st_vanilla);
    stats("majority, all features", st_all);
    stats("nested selection", st_nested);

    int failures = 0;
    for (const AaStats* s : {&st_vanilla, &st_all, &st_nested}) {
        if (s->not_total != 0) { std::printf("FAIL  a labelling was not total\n"); ++failures; }
        if (s->over_bound != 0) { std::printf("FAIL  a solve exceeded its proven bound\n"); ++failures; }
    }
    if (nested[4].correct <= nested[0].correct) {
        std::printf("FAIL  AA-CBR did not beat the majority class\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
