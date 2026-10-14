// v4 of the pre-registered hybrid test: 200 freshly generated problems. See learn/PREREGISTRATION.md.
//
//   eval_synth
//
// Every generator choice below is the registered one: 6 attributes of 2-4 values, a 3-term DNF of 1-3 literals per
// term, positive rate in [0.2, 0.8] or redraw, a casebook knowing 1 or 2 terms, n in {50, 200}, label noise in
// {0, 0.05}, 50 problems per cell, test on the whole instance space with true labels, seed 20261001. The methods come
// from quorum/learn/hybrid.hpp unchanged; "v3" and "v4" differ only in the two Knowledge switches.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
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
using quorum::learn::Knowledge;
using quorum::learn::Method;
using quorum::learn::run_method;

namespace {

constexpr std::uint32_t kSeed = 20261001U;
constexpr int kAttrs = 6;
constexpr int kTerms = 3;
constexpr int kPerCell = 50;

struct Literal {
    int attr;
    int value;
};
using Term = std::vector<Literal>;

struct Problem {
    int values[kAttrs]{};
    int base[kAttrs]{};
    int feature_count = 0;
    Term terms[kTerms];
    int rejections = 0;

    Features literal_bit(const Literal& l) const { return Features{1} << (base[l.attr] + l.value); }
    Features term_features(const Term& t) const {
        Features x = 0;
        for (const Literal& l : t) { x |= literal_bit(l); }
        return x;
    }
    // Instance index -> one-hot features, and its true label.
    Features encode(int index, int* digits) const {
        Features x = 0;
        for (int a = 0; a < kAttrs; ++a) {
            digits[a] = index % values[a];
            index /= values[a];
            x |= Features{1} << (base[a] + digits[a]);
        }
        return x;
    }
    bool label(Features x) const {
        for (const Term& t : terms) {
            const Features f = term_features(t);
            if ((x & f) == f) { return true; }
        }
        return false;
    }
    int space() const {
        int n = 1;
        for (int v : values) { n *= v; }
        return n;
    }
};

Problem draw_problem(std::mt19937& rng) {
    Problem p;
    auto uniform = [&rng](int lo, int hi) { return quorum::learn::rnd::uniform_int(rng, lo, hi); };
    int off = 0;
    for (int a = 0; a < kAttrs; ++a) {
        p.values[a] = uniform(2, 4);
        p.base[a] = off;
        off += p.values[a];
    }
    p.feature_count = off;
    for (;;) {
        for (Term& t : p.terms) {
            t.clear();
            int attrs[kAttrs] = {0, 1, 2, 3, 4, 5};
            quorum::learn::rnd::shuffle(attrs, attrs + kAttrs, rng);
            const int len = uniform(1, 3);
            for (int i = 0; i < len; ++i) { t.push_back({attrs[i], uniform(0, p.values[attrs[i]] - 1)}); }
        }
        int pos = 0;
        int digits[kAttrs];
        for (int i = 0; i < p.space(); ++i) { pos += p.label(p.encode(i, digits)) ? 1 : 0; }
        const double rate = static_cast<double>(pos) / p.space();
        if (rate >= 0.2 && rate <= 0.8) { return p; }
        ++p.rejections;
    }
}

Knowledge knowledge_of(const Problem& p, const std::vector<int>& known, bool v4) {
    Knowledge kb;
    for (int t : known) {
        const Features f = p.term_features(p.terms[t]);
        kb.priors.push_back({f, true});
        kb.features |= f;
    }
    kb.asserts = true;
    kb.min_exception_support = v4 ? 2 : 1;
    kb.may_decline = v4;
    return kb;
}

struct Score {
    double ba = 0.0;
};

double evaluate(Method m, const Knowledge& kb, const std::vector<Case>& train, const std::vector<Case>& test,
                const Folds& inner, int feature_count, Invariants* inv) {
    const int k = m == Method::Casebook ? 0 : choose_k(m, kb, inner, feature_count);
    return 100.0 * run_method(m, kb, train, test, k, feature_count, inv).ba();
}

}  // namespace

int main() {
    std::mt19937 rng(kSeed);
    std::printf("casebook + learning, v4 -- %d freshly generated problems, seed %u\n", 4 * kPerCell, kSeed);

    struct Row {
        int n;
        double eta;
        double casebook, learned, v3, v4;
        double full_casebook, full_v4, full_v3;
    };
    std::vector<Row> rows;
    Invariants inv;
    int rejections = 0;

    for (int n : {50, 200}) {
        for (double eta : {0.0, 0.05}) {
            for (int i = 0; i < kPerCell; ++i) {
                const Problem p = draw_problem(rng);
                rejections += p.rejections;

                // The casebook's partial knowledge: 1 or 2 of the 3 terms.
                int order[kTerms] = {0, 1, 2};
                quorum::learn::rnd::shuffle(order, order + kTerms, rng);
                const int known_count = quorum::learn::rnd::uniform_int(rng, 1, 2);
                const std::vector<int> known(order, order + known_count);
                const std::vector<int> all{0, 1, 2};

                // Training records, uniform over the space, labels flipped with probability eta.
                std::vector<Case> train;
                int digits[kAttrs];
                for (int r = 0; r < n; ++r) {
                    const Features x = p.encode(quorum::learn::rnd::uniform_int(rng, 0, p.space() - 1), digits);
                    train.push_back({x, p.label(x) != quorum::learn::rnd::bernoulli(rng, eta)});
                }
                std::vector<Case> test;
                for (int s = 0; s < p.space(); ++s) {
                    const Features x = p.encode(s, digits);
                    test.push_back({x, p.label(x)});
                }
                std::vector<std::size_t> idx(train.size());
                for (std::size_t j = 0; j < idx.size(); ++j) { idx[j] = j; }
                quorum::learn::rnd::shuffle(idx.begin(), idx.end(), rng);
                Folds inner(5);
                for (std::size_t j = 0; j < idx.size(); ++j) {
                    for (std::size_t f = 0; f < 5U; ++f) {
                        (j % 5U == f ? inner[f].second : inner[f].first).push_back(train[idx[j]]);
                    }
                }

                const int fc = p.feature_count;
                const Knowledge part3 = knowledge_of(p, known, false), part4 = knowledge_of(p, known, true);
                const Knowledge full3 = knowledge_of(p, all, false), full4 = knowledge_of(p, all, true);
                Row r{n, eta, 0, 0, 0, 0, 0, 0, 0};
                r.casebook = evaluate(Method::Casebook, part4, train, test, inner, fc, &inv);
                r.learned = evaluate(Method::Learned, Knowledge{}, train, test, inner, fc, &inv);
                r.v3 = evaluate(Method::Hybrid, part3, train, test, inner, fc, &inv);
                r.v4 = evaluate(Method::Hybrid, part4, train, test, inner, fc, &inv);
                r.full_casebook = evaluate(Method::Casebook, full4, train, test, inner, fc, &inv);
                r.full_v3 = evaluate(Method::Hybrid, full3, train, test, inner, fc, &inv);
                r.full_v4 = evaluate(Method::Hybrid, full4, train, test, inner, fc, &inv);
                rows.push_back(r);
            }
        }
    }

    std::printf("  concepts redrawn for a positive rate outside [0.2, 0.8]: %d\n\n", rejections);
    std::printf("MEAN BALANCED ACCURACY, %% (50 problems per cell; incomplete casebook unless marked)\n");
    std::printf("  %5s %5s %10s %10s %10s %10s   %14s %14s %14s\n", "n", "noise", "casebook", "learned", "hybrid v3",
                "hybrid v4", "full casebook", "full hyb v3", "full hyb v4");
    for (int n : {50, 200}) {
        for (double eta : {0.0, 0.05}) {
            double c = 0, l = 0, h3 = 0, h4 = 0, fc = 0, f3 = 0, f4 = 0;
            int cnt = 0;
            for (const Row& r : rows) {
                if (r.n != n || r.eta != eta) { continue; }
                c += r.casebook; l += r.learned; h3 += r.v3; h4 += r.v4;
                fc += r.full_casebook; f3 += r.full_v3; f4 += r.full_v4;
                ++cnt;
            }
            std::printf("  %5d %5.2f %10.2f %10.2f %10.2f %10.2f   %14.2f %14.2f %14.2f\n", n, eta, c / cnt, l / cnt,
                        h3 / cnt, h4 / cnt, fc / cnt, f3 / cnt, f4 / cnt);
        }
    }

    int w1 = 0, w2 = 0, ok3 = 0, w1v3 = 0, w2v3 = 0, ok3v3 = 0;
    double g1 = 0, g2 = 0, g1v3 = 0, g2v3 = 0;
    for (const Row& r : rows) {
        w1 += r.v4 > r.casebook ? 1 : 0;
        w2 += r.v4 > r.learned ? 1 : 0;
        ok3 += r.full_casebook - r.full_v4 <= 1.0 ? 1 : 0;
        g1 += (r.v4 - r.casebook) / static_cast<double>(rows.size());
        g2 += (r.v4 - r.learned) / static_cast<double>(rows.size());
        w1v3 += r.v3 > r.casebook ? 1 : 0;
        w2v3 += r.v3 > r.learned ? 1 : 0;
        ok3v3 += r.full_casebook - r.full_v3 <= 1.0 ? 1 : 0;
        g1v3 += (r.v3 - r.casebook) / static_cast<double>(rows.size());
        g2v3 += (r.v3 - r.learned) / static_cast<double>(rows.size());
    }
    const int total = static_cast<int>(rows.size());
    const bool c1 = w1 * 100 >= 80 * total && g1 >= 10.0;
    const bool c2 = w2 * 100 >= 80 * total && g2 >= 5.0;
    const bool c3 = ok3 * 100 >= 95 * total;
    const bool c4 = inv.not_total == 0 && inv.over_bound == 0;

    std::printf("\nPRE-REGISTERED CRITERIA, v4 (learn/PREREGISTRATION.md)\n");
    std::printf("  1. v4 beats the incomplete casebook:        %3d/%d problems, mean gain %+.2f pts   %s\n", w1, total, g1,
                c1 ? "PASS" : "FAIL");
    std::printf("  2. v4 beats learning alone:                 %3d/%d problems, mean gain %+.2f pts   %s\n", w2, total, g2,
                c2 ? "PASS" : "FAIL");
    std::printf("  3. v4 within 1 pt of the complete casebook: %3d/%d problems                     %s\n", ok3, total,
                c3 ? "PASS" : "FAIL");
    std::printf("  4. invariants: %ld AA-CBR queries, not total %ld, over bound %ld; largest %d args, %d attacks   %s\n",
                inv.queries, inv.not_total, inv.over_bound, inv.max_args, inv.max_attacks, c4 ? "PASS" : "FAIL");
    std::printf("\n  for comparison, the unrepaired v3 method on the same problems (judges nothing):\n");
    std::printf("     beats casebook %d/%d (%+.2f), beats learned %d/%d (%+.2f), within 1 pt of complete casebook %d/%d\n",
                w1v3, total, g1v3, w2v3, total, g2v3, ok3v3, total);
    std::printf("\n  VERDICT: learning makes an impactful difference to a casebook: %s\n", c1 && c2 && c4 ? "YES" : "NOT SHOWN");
    if (!c3) { std::printf("  DEFECT: learning still harms the complete casebook too often\n"); }
    return c4 ? 0 : 1;
}
