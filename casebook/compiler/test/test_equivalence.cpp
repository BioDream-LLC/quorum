// Equivalence: compiler output versus the hand-written casebook.
//
// This is the test that matters. The pump casebook exists twice -- once hand-written in C++ over several hours,
// once as 46 lines of source compiled by cbc. If the two disagree on any of the 16,384 sensor states, one of them
// is wrong, and the disagreement is the finding.
//
// The two argument numberings differ, so the comparison is positional on observations (both declare the same 14 in
// the same order) and by meaning on actions. Nothing else is assumed.

#include <cstdint>
#include <cstdio>

#include "pump_casebook.hpp"    // hand-written
#include "pump_generated.hpp"   // emitted by cbc from pump.cb
#include "quorum/arbiter/verdict.hpp"

namespace hand = quorum::casebook::pump;
namespace gen = quorum::casebook::gen::centrifugal_pump;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

/// A verdict expressed in terms neither casebook owns, so the two can be compared at all.
enum class Choice { Continue, Maintain, Shutdown, Unknown };

const char* choice_name(Choice c) {
    switch (c) {
        case Choice::Continue: return "continue";
        case Choice::Maintain: return "maintain";
        case Choice::Shutdown: return "shutdown";
        case Choice::Unknown:  return "unknown";
    }
    return "?";
}

Choice hand_choice(const Verdict& v) {
    if (v.action == Arg{hand::act_continue}) { return Choice::Continue; }
    if (v.action == Arg{hand::act_schedule_maintenance}) { return Choice::Maintain; }
    if (v.action == Arg{hand::act_shutdown_now}) { return Choice::Shutdown; }
    return Choice::Unknown;
}

Choice gen_choice(const Verdict& v) {
    if (v.action == Arg{gen::continue_}) { return Choice::Continue; }
    if (v.action == Arg{gen::maintain}) { return Choice::Maintain; }
    if (v.action == Arg{gen::shutdown}) { return Choice::Shutdown; }
    return Choice::Unknown;
}

void print_mask(std::uint32_t mask) {
    bool any = false;
    for (std::int32_t a = 0; a < hand::kObservationCount; ++a) {
        if ((mask & (1U << static_cast<std::uint32_t>(a))) != 0U) {
            std::printf(" %s", hand::name_of(a));
            any = true;
        }
    }
    if (!any) { std::printf(" (nothing asserted)"); }
}

}  // namespace

int main() {
    std::printf("hand-written vs compiled pump casebook\n\n");

    static_assert(hand::kObservationCount == gen::kObservationCount,
                  "the two casebooks must declare the same number of observations for masks to correspond");

    std::printf("  hand-written : %d arguments, %d attacks\n",
                static_cast<int>(hand::argument_count), static_cast<int>(hand::kCasebookEdges));
    std::printf("  compiled     : %d arguments, %d attacks\n",
                static_cast<int>(gen::argument_count), static_cast<int>(gen::kCasebookEdges));
    std::printf("  source       : %s v%d, from %s\n\n",
                gen::kCasebookName, static_cast<int>(gen::kCasebookVersion), gen::kSourceDocument);

    hand::PumpPolicy hand_policy;
    gen::CasePolicy gen_policy;
    if (hand::make_policy(hand_policy) != Status::Ok || gen::make_policy(gen_policy) != Status::Ok) {
        std::printf("FAIL  policy construction\n");
        return 1;
    }

    const std::uint32_t total = 1U << static_cast<std::uint32_t>(hand::kObservationCount);

    std::int32_t action_differs = 0;
    std::int32_t outcome_differs = 0;
    std::int32_t agree = 0;
    std::uint32_t first_action_diff = 0;
    std::uint32_t first_outcome_diff = 0;
    Choice fa_hand = Choice::Unknown;
    Choice fa_gen = Choice::Unknown;

    std::int32_t hand_counts[5] = {0, 0, 0, 0, 0};
    std::int32_t gen_counts[5] = {0, 0, 0, 0, 0};
    std::int32_t hand_max_steps = 0;
    std::int32_t gen_max_steps = 0;

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        hand::PumpFramework hfw;
        hand::PumpSolver hsolver;
        gen::CaseFramework gfw;
        gen::CaseSolver gsolver;

        if (hand::instantiate(hfw, mask) != Status::Ok || gen::instantiate(gfw, mask) != Status::Ok) {
            std::printf("FAIL  construction at state %u\n", mask);
            return 1;
        }

        const Verdict hv = quorum::arbiter::decide(hfw, hsolver, hand_policy);
        const Verdict gv = quorum::arbiter::decide(gfw, gsolver, gen_policy);

        ++hand_counts[static_cast<std::size_t>(hv.outcome)];
        ++gen_counts[static_cast<std::size_t>(gv.outcome)];
        if (hv.steps > hand_max_steps) { hand_max_steps = hv.steps; }
        if (gv.steps > gen_max_steps) { gen_max_steps = gv.steps; }

        const Choice hc = hand_choice(hv);
        const Choice gc = gen_choice(gv);

        if (hv.outcome != gv.outcome) {
            if (outcome_differs == 0) { first_outcome_diff = mask; }
            ++outcome_differs;
        }
        if (hc != gc) {
            if (action_differs == 0) {
                first_action_diff = mask;
                fa_hand = hc;
                fa_gen = gc;
            }
            ++action_differs;
        } else {
            ++agree;
        }
    }

    std::printf("outcome distribution\n");
    std::printf("  %-14s %8s %8s\n", "", "hand", "compiled");
    for (std::int32_t i = 0; i < 5; ++i) {
        std::printf("  %-14s %8d %8d\n", outcome_name(static_cast<Outcome>(i)),
                    hand_counts[static_cast<std::size_t>(i)], gen_counts[static_cast<std::size_t>(i)]);
    }
    std::printf("  %-14s %8d %8d\n\n", "worst-case steps", hand_max_steps, gen_max_steps);

    std::printf("%u states compared\n", total);
    std::printf("  actions agree     %d\n", agree);
    std::printf("  actions differ    %d\n", action_differs);
    std::printf("  outcomes differ   %d\n", outcome_differs);

    if (action_differs > 0) {
        std::printf("\n  first action disagreement at state:");
        print_mask(first_action_diff);
        std::printf("\n    hand-written says %s, compiled says %s\n", choice_name(fa_hand), choice_name(fa_gen));
    }
    if (outcome_differs > 0) {
        std::printf("\n  first outcome disagreement at state:");
        print_mask(first_outcome_diff);
        std::printf("\n");
    }

    const bool ok = action_differs == 0 && outcome_differs == 0;
    std::printf("\n%s\n", ok ? "EQUIVALENT -- the compiler reproduces the hand-written casebook exactly"
                             : "NOT EQUIVALENT");
    return ok ? 0 : 1;
}
