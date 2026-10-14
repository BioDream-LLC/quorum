// Threshold learning: the cases with a known right answer, plus the one that must find nothing.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "quorum/learn/discretize.hpp"

using quorum::learn::mdlp_cuts;

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++failures;
    }
}

void separable_classes_get_one_cut_between_them() {
    std::vector<double> v;
    std::vector<bool> y;
    for (int i = 0; i < 50; ++i) { v.push_back(i); y.push_back(false); }
    for (int i = 0; i < 50; ++i) { v.push_back(100 + i); y.push_back(true); }
    const auto c = mdlp_cuts(v, y, 2);
    expect(c.size() == 1U, "separable: exactly one cut (a second has nothing left to explain)");
    expect(!c.empty() && c[0] > 49.0 && c[0] < 100.0, "separable: the cut lies between the classes");
    expect(!c.empty() && c[0] == 74.5, "separable: the cut is the midpoint of the two values it separates");
}

void one_class_gets_no_cut() {
    std::vector<double> v;
    std::vector<bool> y;
    for (int i = 0; i < 100; ++i) { v.push_back(i); y.push_back(false); }
    expect(mdlp_cuts(v, y, 2).empty(), "pure: a signal with one class present has no cut");
}

void noise_gets_no_cut() {
    std::mt19937 rng(7U);
    std::vector<double> v;
    std::vector<bool> y;
    for (int i = 0; i < 400; ++i) { v.push_back(i); y.push_back((rng() & 1U) != 0U); }
    expect(mdlp_cuts(v, y, 2).empty(), "noise: MDL rejects a cut on labels independent of the signal");
}

// The shape of a power failure: rare failures at BOTH extremes. Both boundaries must be found.
void rare_failures_at_both_extremes_get_both_cuts() {
    std::mt19937 rng(11U);
    std::uniform_real_distribution<double> u(0.0, 10000.0);
    std::vector<double> v;
    std::vector<bool> y;
    for (int i = 0; i < 9000; ++i) {
        const double x = u(rng);
        v.push_back(x);
        y.push_back(x < 500.0 || x > 9500.0);
    }
    const auto c = mdlp_cuts(v, y, 2);
    expect(c.size() == 2U, "two-sided: both boundaries found");
    if (c.size() == 2U) {
        expect(std::fabs(c[0] - 500.0) < 5.0, "two-sided: low cut within 5 of the true 500");
        expect(std::fabs(c[1] - 9500.0) < 5.0, "two-sided: high cut within 5 of the true 9500");
    }
}

void the_cap_is_respected() {
    std::vector<double> v;
    std::vector<bool> y;
    for (int i = 0; i < 400; ++i) { v.push_back(i); y.push_back((i / 50) % 2 == 1); }  // 7 true boundaries
    expect(mdlp_cuts(v, y, 2).size() == 2U, "cap: at most max_cuts cuts even when more are justified");
    expect(mdlp_cuts(v, y, 0).empty(), "cap: max_cuts 0 means none");
    expect(mdlp_cuts(v, y, 16).size() == 7U, "cap: uncapped, every true boundary is found and no more");
}

// Six points, labels 0 0 0 1 0 1. The best cut (after the third) gains 0.459 bits per point. That clears the
// log2(N-1)/N term alone (0.387) but not the full MDL threshold with Delta (0.855), so the paper's test rejects it.
// Worked by hand; it is here because without it the Delta term could be deleted and nothing would notice.
void the_delta_term_decides_a_marginal_cut() {
    const std::vector<double> v{0, 1, 2, 3, 4, 5};
    const std::vector<bool> y{false, false, false, true, false, true};
    expect(mdlp_cuts(v, y, 2).empty(), "delta: a cut paying for log2(N-1)/N but not for Delta is rejected");
}

}  // namespace

int main() {
    separable_classes_get_one_cut_between_them();
    one_class_gets_no_cut();
    noise_gets_no_cut();
    rare_failures_at_both_extremes_get_both_cuts();
    the_cap_is_respected();
    the_delta_term_decides_a_marginal_cut();
    if (failures != 0) {
        std::printf("test_discretize: %d failed\n", failures);
        return 1;
    }
    std::printf("test_discretize: all passed\n");
    return 0;
}
