// v3 of the pre-registered hybrid test: replication on the MONK's problems. See learn/PREREGISTRATION.md.
//
//   eval_monks <dir containing monks-1.train monks-1.test monks-3.train monks-3.test>
//
// Same methods, same k candidates, same metric as v1/v2 -- all from quorum/learn/hybrid.hpp, nothing re-implemented.
// What differs is only what the registration says differs: the data, the official fixed train/test split, and the
// knowledge, which is the two published disjuncts of each problem's target concept.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/learn/hybrid.hpp"
#include "quorum/learn/portable_random.hpp"

using quorum::learn::Case;
using quorum::learn::choose_k;
using quorum::learn::Confusion;
using quorum::learn::Features;
using quorum::learn::Folds;
using quorum::learn::Invariants;
using quorum::learn::kAllMethods;
using quorum::learn::Knowledge;
using quorum::learn::kMethods;
using quorum::learn::Method;
using quorum::learn::method_label;
using quorum::learn::run_method;
using quorum::learn::uses_k;

namespace {

constexpr std::uint32_t kSeed = 20260929U;
constexpr int kFeatureCount = 17;

// One-hot layout: attribute a (1-based) value v (1-based) is bit kBase[a] + v - 1.
constexpr int kValues[7] = {0, 3, 3, 2, 3, 4, 2};
constexpr int kBase[7] = {0, 0, 3, 6, 8, 11, 15};
constexpr Features attr(int a, int v) { return Features{1} << (kBase[a] + v - 1); }

std::vector<Case> load(const std::string& path) {
    std::ifstream in(path);
    std::vector<Case> out;
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        int cls = 0;
        int v[7]{};
        if (!(ss >> cls)) { continue; }
        Features x = 0;
        for (int a = 1; a <= 6; ++a) {
            ss >> v[a];
            if (v[a] < 1 || v[a] > kValues[a]) { return {}; }
            x |= attr(a, v[a]);
        }
        out.push_back({x, cls == 1});
    }
    return out;
}

struct Term {
    const char* text;
    std::vector<Case> cases;
};

std::vector<Term> monk1_terms() {
    return {{"a1 = a2", {{attr(1, 1) | attr(2, 1), true}, {attr(1, 2) | attr(2, 2), true}, {attr(1, 3) | attr(2, 3), true}}},
            {"a5 = 1", {{attr(5, 1), true}}}};
}

std::vector<Term> monk3_terms() {
    Term t2{"a5 != 4 and a2 != 3", {}};
    for (int a5 = 1; a5 <= 3; ++a5) {
        for (int a2 = 1; a2 <= 2; ++a2) { t2.cases.push_back({attr(5, a5) | attr(2, a2), true}); }
    }
    return {{"a5 = 3 and a4 = 1", {{attr(5, 3) | attr(4, 1), true}}}, t2};
}

// The documented concepts, evaluated directly. Used only to confirm the files are the registered ones.
bool monk1_concept(Features x) {
    for (int v = 1; v <= 3; ++v) {
        if ((x & attr(1, v)) != 0U && (x & attr(2, v)) != 0U) { return true; }
    }
    return (x & attr(5, 1)) != 0U;
}
bool monk3_concept(Features x) {
    const bool t1 = (x & attr(5, 3)) != 0U && (x & attr(4, 1)) != 0U;
    const bool t2 = (x & attr(5, 4)) == 0U && (x & attr(2, 3)) == 0U;
    return t1 || t2;
}

struct Result {
    std::string label;
    Confusion conf[kMethods];
    int k[kMethods]{};
};

std::vector<Result> run_problem(const std::vector<Case>& train, const std::vector<Case>& test,
                                const std::vector<Term>& terms, Invariants* inv) {
    // Inner folds for the nested k choice, over a fixed shuffle of the training set.
    std::vector<std::size_t> order(train.size());
    for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::mt19937 rng(kSeed);
    quorum::learn::rnd::shuffle(order.begin(), order.end(), rng);
    Folds inner(5);
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (std::size_t f = 0; f < 5U; ++f) {
            (i % 5U == f ? inner[f].second : inner[f].first).push_back(train[order[i]]);
        }
    }

    std::vector<Result> out;
    for (unsigned s = 0; s < 4U; ++s) {
        Knowledge kb;
        Result r;
        for (unsigned t = 0; t < 2U; ++t) {
            if (((s >> t) & 1U) == 0U) { continue; }
            for (const Case& c : terms[t].cases) {
                kb.priors.push_back(c);
                kb.features |= c.x;
            }
            r.label += r.label.empty() ? "" : " + ";
            r.label += terms[t].text;
        }
        if (r.label.empty()) { r.label = "(none)"; }
        for (int mi = 0; mi < kMethods; ++mi) {
            const Method m = kAllMethods[mi];
            r.k[mi] = uses_k(m) ? choose_k(m, kb, inner, kFeatureCount) : 0;
            r.conf[mi] = run_method(m, kb, train, test, r.k[mi], kFeatureCount, inv);
        }
        out.push_back(r);
    }
    return out;
}

double ba(const Result& r, Method m) { return 100.0 * r.conf[static_cast<int>(m)].ba(); }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: eval_monks <directory of the MONK's files>\n");
        return 2;
    }
    const std::string dir = argv[1];
    struct Problem {
        const char* name;
        std::vector<Case> train, test;
        std::vector<Term> terms;
        bool (*concept_fn)(Features);
    };
    std::vector<Problem> problems = {
        {"MONK-1", load(dir + "/monks-1.train"), load(dir + "/monks-1.test"), monk1_terms(), monk1_concept},
        {"MONK-3", load(dir + "/monks-3.train"), load(dir + "/monks-3.test"), monk3_terms(), monk3_concept},
    };

    std::printf("casebook + learning, replication -- the MONK's problems, official train/test split\n");
    for (const Problem& p : problems) {
        int train_noise = 0, test_wrong = 0;
        for (const Case& c : p.train) { train_noise += c.outcome != p.concept_fn(c.x) ? 1 : 0; }
        for (const Case& c : p.test) { test_wrong += c.outcome != p.concept_fn(c.x) ? 1 : 0; }
        std::printf("  %s: %zu training rows (%d disagree with the documented concept), %zu test rows (%d disagree)\n",
                    p.name, p.train.size(), train_noise, p.test.size(), test_wrong);
        if (p.train.empty() || p.test.size() != 432U || test_wrong != 0) {
            std::printf("FAIL  the data files are not the registered ones\n");
            return 1;
        }
    }

    Invariants inv;
    double r1_gain = 0.0, r2_gain = 0.0;
    int r1_wins = 0, r2_wins = 0;
    bool r3 = true;
    for (const Problem& p : problems) {
        const std::vector<Result> res = run_problem(p.train, p.test, p.terms, &inv);
        std::printf("\n%s -- BALANCED ACCURACY, %% on all 432\n  %-30s", p.name, "casebook knows");
        for (Method m : kAllMethods) { std::printf(" %17s", method_label(m)); }
        std::printf("\n");
        for (const Result& r : res) {
            std::printf("  %-30s", r.label.c_str());
            for (Method m : kAllMethods) { std::printf(" %17.2f", ba(r, m)); }
            std::printf("\n");
        }
        std::printf("  wrong answers (missed / false):\n");
        for (const Result& r : res) {
            std::printf("  %-30s", r.label.c_str());
            for (Method m : {Method::Casebook, Method::Learned, Method::Hybrid}) {
                const Confusion& c = r.conf[static_cast<int>(m)];
                std::printf("   %s %3d / %3d", method_label(m), c.fn, c.fp);
            }
            std::printf("   k=%d\n", r.k[static_cast<int>(Method::Hybrid)]);
        }
        for (std::size_t s = 1; s <= 2U; ++s) {  // the two incomplete casebooks
            const double g1 = ba(res[s], Method::Hybrid) - ba(res[s], Method::Casebook);
            const double g2 = ba(res[s], Method::Hybrid) - ba(res[s], Method::Learned);
            r1_wins += g1 > 0.0 ? 1 : 0;
            r2_wins += g2 > 0.0 ? 1 : 0;
            r1_gain += g1 / 4.0;
            r2_gain += g2 / 4.0;
        }
        const double loss = ba(res[3], Method::Casebook) - ba(res[3], Method::Hybrid);
        std::printf("  complete casebook: casebook %.2f, casebook+learned %.2f\n", ba(res[3], Method::Casebook),
                    ba(res[3], Method::Hybrid));
        r3 = r3 && loss <= 1.0;
    }

    const bool c1 = r1_wins == 4 && r1_gain >= 10.0;
    const bool c2 = r2_wins == 4 && r2_gain >= 5.0;
    const bool c4 = inv.not_total == 0 && inv.over_bound == 0;
    std::printf("\nPRE-REGISTERED CRITERIA, v3 (learn/PREREGISTRATION.md)\n");
    std::printf("  1. learning improves the incomplete casebook: wins %d/4, mean gain %+.2f pts   %s\n", r1_wins, r1_gain,
                c1 ? "PASS" : "FAIL");
    std::printf("  2. the casebook improves learning:            wins %d/4, mean gain %+.2f pts   %s\n", r2_wins, r2_gain,
                c2 ? "PASS" : "FAIL");
    std::printf("  3. no harm with the complete casebook (both problems, limit 1 pt)              %s\n",
                r3 ? "PASS" : "FAIL");
    std::printf("  4. invariants: %ld AA-CBR queries, not total %ld, over bound %ld; largest %d args, %d attacks   %s\n",
                inv.queries, inv.not_total, inv.over_bound, inv.max_args, inv.max_attacks, c4 ? "PASS" : "FAIL");
    std::printf("\n  VERDICT: learning makes an impactful difference to a casebook: %s\n",
                c1 && c2 && c4 ? "YES" : "NOT SHOWN");
    if (!r3) { std::printf("  DEFECT: learning harms the complete casebook\n"); }
    return c4 ? 0 : 1;
}
