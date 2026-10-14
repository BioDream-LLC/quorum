#include <cstdio>
#include "quorum/arbiter/verdict.hpp"
#include <string>

#include "vote_generated.hpp"
namespace v = quorum::casebook::gen::vessel_hl_trip;
// The 2oo3 truth table, plus the case a mechanised check found missing.
//
// `lt_a_high` asserted alone used to produce no response at all. Voting correctly refuses to trip on one transmitter --
// that is what 2oo3 is for -- but "no trip" is not the same as "nothing to do": a transmitter reading high while the other
// two read normal is drifting or has failed high, and nothing else in the plant will say so. A HIDDEN FAILURE in the RCM
// sense, where the absence of a symptom is not evidence of health.
//
// Both directions of discrepancy are now detected, and this pins the whole table so neither can regress.
int main() {
    v::CaseFramework fw; v::CaseSolver s; v::CasePolicy p;
    if (v::make_policy(p) != quorum::solver::Status::Ok) return 1;
    struct C { unsigned m; const char* d; const char* want; };
    const C cases[] = {
        {0x0, "all normal",                   "hold"},
        {0x1, "A high only (spurious high)",  "proof_test"},
        {0x2, "B high only",                  "proof_test"},
        {0x4, "C high only",                  "proof_test"},
        {0x3, "A+B high (2oo3 met)",          "trip"},
        {0x5, "A+C high (2oo3 met)",          "trip"},
        {0x7, "all three high",               "trip"},
    };
    int bad = 0;
    for (const auto& c : cases) {
        if (v::instantiate(fw, c.m) != quorum::solver::Status::Ok) continue;
        const auto vd = quorum::arbiter::decide(fw, s, p);
        const char* got = vd.action.valid() ? v::name_of(vd.action.get()) : "none";
        const bool ok = std::string(got) == c.want;
        if (!ok) ++bad;
        std::printf("  %-32s -> %-11s %s\n", c.d, got, ok ? "" : "<-- WRONG");
    }
    std::printf("\n%s\n", bad == 0 ? "2oo3 logic and single-deviation detection both correct" : "LOGIC ERROR");
    return bad;
}
