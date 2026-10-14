// How big is the advisory tier's search space, really?
//
// Asked before designing it, because the answer determines whether an exact algorithm is possible at all. Preferred and
// stable semantics are NP-hard in general, which is the reason they stay off the microcontroller. But "NP-hard in
// general" is a statement about worst cases over all graphs, not about these graphs.
//
// The structural fact that matters: the grounded labelling is the LEAST complete labelling, so every argument grounded
// labels In is In in every complete extension, and every argument it labels Out is Out in every one. Only the
// grounded-UNDECIDED arguments can vary. If that set is small, the advisory tier can enumerate exactly rather than
// approximate -- and an exact answer is worth far more here, because an advisory tier that might have missed an
// extension cannot be used to argue anything about the safety tier.
//
// This measures the undecided set across every state of every casebook, so the design rests on a distribution rather
// than on an assumption.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "quorum/solver/grounded.hpp"

#include "permit_generated.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"

using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

struct Profile {
    std::string name;
    std::int32_t arguments = 0;
    std::int32_t states = 0;
    std::int32_t states_with_undecided = 0;
    std::int32_t max_undecided = 0;
    std::int64_t total_undecided = 0;
    std::map<std::int32_t, std::int32_t> histogram;  // undecided count -> states
};

template <typename Fw, typename Solver, typename Instantiate>
Profile profile_casebook(const std::string& name, std::int32_t arguments, std::int32_t observations,
                         Instantiate instantiate) {
    Profile p;
    p.name = name;
    p.arguments = arguments;

    Fw fw;
    Solver solver;

    const std::int32_t states = 1 << observations;
    p.states = states;

    for (std::int32_t s = 0; s < states; ++s) {
        if (instantiate(fw, static_cast<std::uint32_t>(s)) != Status::Ok) { continue; }
        if (solver.solve(fw) != Status::Ok) { continue; }

        std::int32_t undecided = 0;
        for (std::int32_t a = 0; a < fw.argument_count(); ++a) {
            if (solver.label(Arg{a}) == Label::Undecided) { ++undecided; }
        }

        p.total_undecided += undecided;
        p.max_undecided = std::max(p.max_undecided, undecided);
        if (undecided > 0) { ++p.states_with_undecided; }
        ++p.histogram[undecided];
    }
    return p;
}

void report(const Profile& p) {
    std::printf("%-14s %5d args  %7d states   %6d with undecided (%.2f%%)   max undecided %3d\n", p.name.c_str(),
                p.arguments, p.states, p.states_with_undecided,
                100.0 * static_cast<double>(p.states_with_undecided) / static_cast<double>(p.states), p.max_undecided);

    if (p.max_undecided == 0) {
        std::printf("               every state fully decided by grounded -- nothing for an advisory tier to add\n");
        return;
    }

    std::printf("               distribution:");
    for (const auto& [count, states] : p.histogram) {
        if (count == 0) { continue; }
        std::printf("  %d undecided x%d", count, states);
    }
    std::printf("\n");

    // The number that decides the algorithm. 2^k subsets of the undecided set is an upper bound on the search, and a
    // backtracking search that maintains conflict-freeness will visit far fewer.
    const double worst = static_cast<double>(1LL << p.max_undecided);
    std::printf("               worst-case subsets to consider: 2^%d = %.0f\n", p.max_undecided, worst);
}

}  // namespace

int main() {
    std::printf("advisory tier search space: how much does grounded leave undecided?\n\n");

#define PROFILE(ns, label)                                                                       \
    profile_casebook<ns::CaseFramework, ns::CaseSolver>(label, ns::argument_count,               \
                                                        ns::kObservationCount,                   \
                                                        [](ns::CaseFramework& f, std::uint32_t m) { \
                                                            return ns::instantiate(f, m);        \
                                                        })

    const std::vector<Profile> profiles = {
        PROFILE(quorum::casebook::gen::centrifugal_pump, "pump"),
        PROFILE(quorum::casebook::gen::tank_409, "tank-409"),
        PROFILE(quorum::casebook::gen::piper_permit, "piper-permit"),
    };
#undef PROFILE

    std::int32_t overall_max = 0;
    for (const Profile& p : profiles) {
        report(p);
        overall_max = std::max(overall_max, p.max_undecided);
        std::printf("\n");
    }

    std::printf("─────────────────────────────────────────────────────────────────────────────\n");
    std::printf("largest undecided set anywhere: %d arguments\n\n", overall_max);

    // The design conclusion. An exact enumeration is only honest if it is actually exhaustive, and 2^k with a small k
    // is what makes that possible. If this number ever grows, the advisory tier needs a real SAT encoding and this
    // test is where that will show up first.
    if (overall_max <= 20) {
        std::printf("Small enough to enumerate EXACTLY. Preferred and stable semantics are NP-hard over all\n");
        std::printf("graphs, but the search here is confined to the grounded-undecided arguments, because the\n");
        std::printf("grounded labelling is the least complete one -- every argument it decides is decided the\n");
        std::printf("same way in every complete extension. An exact advisory tier is possible, and an exact one\n");
        std::printf("is what is needed: a tier that might have missed an extension cannot support any claim\n");
        std::printf("about the safety tier.\n");
    } else {
        std::printf("Too large for subset enumeration (2^%d). The advisory tier needs a SAT encoding.\n", overall_max);
        return 1;
    }
    return 0;
}
