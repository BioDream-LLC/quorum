// Is `vote k of` exactly the hand-written expansion?
//
// The same question asked of the compiler itself, and answered the same way: not by inspecting the generated code, but
// by comparing behaviour over every state. `vote.cb` uses `vote 2 of` for the collapse condition; `vote_manual.cb` is
// byte-identical except that the condition is written out as the disjunction of the three pairs. If the sugar is sound
// they must agree on every one of 512 states, at the level of both labels and verdicts.
//
// This matters more than it looks. A counting quantifier that is subtly wrong -- off by one, or missing a combination --
// produces a safety instrumented function that votes 1oo3 or 3oo3 while reading as 2oo3, and no amount of reviewing the
// source would show it. That is exactly the class of bug this casebook was written to expose.

#include <cstdint>
#include <cstdio>
#include <string>

#include "quorum/arbiter/verdict.hpp"

#include "vote_generated.hpp"
#include "vote_manual_generated.hpp"

namespace sugar = quorum::casebook::gen::vessel_hl_trip;
namespace manual = quorum::casebook::gen::vessel_hl_trip_manual;

using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Status;

int main() {
    sugar::CaseFramework sfw;
    sugar::CaseSolver ssolver;
    sugar::CasePolicy spolicy;

    manual::CaseFramework mfw;
    manual::CaseSolver msolver;
    manual::CasePolicy mpolicy;

    if (sugar::make_policy(spolicy) != Status::Ok || manual::make_policy(mpolicy) != Status::Ok) {
        std::printf("FAIL: policy construction\n");
        return 1;
    }

    static_assert(sugar::kObservationCount == manual::kObservationCount,
                  "the two casebooks must share an observation layout or the masks are not comparable");

    std::int32_t outcome_diffs = 0;
    std::int32_t action_diffs = 0;
    std::int32_t compared = 0;

    const std::int32_t states = 1 << sugar::kObservationCount;
    for (std::int32_t s = 0; s < states; ++s) {
        const std::uint32_t mask = static_cast<std::uint32_t>(s);
        if (sugar::instantiate(sfw, mask) != Status::Ok) { continue; }
        if (manual::instantiate(mfw, mask) != Status::Ok) { continue; }

        const Verdict sv = quorum::arbiter::decide(sfw, ssolver, spolicy);
        const Verdict mv = quorum::arbiter::decide(mfw, msolver, mpolicy);
        ++compared;

        if (sv.outcome != mv.outcome) {
            if (outcome_diffs < 5) {
                std::printf("  state 0x%x: outcome %s vs %s\n", static_cast<unsigned>(s), outcome_name(sv.outcome),
                            outcome_name(mv.outcome));
            }
            ++outcome_diffs;
        }

        // Compared by NAME, not index: the two casebooks are separately compiled and their argument numbering has no
        // reason to coincide.
        const char* sa = sv.action.valid() ? sugar::name_of(sv.action.get()) : "(none)";
        const char* ma = mv.action.valid() ? manual::name_of(mv.action.get()) : "(none)";
        if (std::string(sa) != std::string(ma)) {
            if (action_diffs < 5) {
                std::printf("  state 0x%x: action %s vs %s\n", static_cast<unsigned>(s), sa, ma);
            }
            ++action_diffs;
        }
    }

    std::printf("`vote 2 of` against the hand-written pair disjunction\n");
    std::printf("  states compared      %d\n", compared);
    std::printf("  outcome differences  %d\n", outcome_diffs);
    std::printf("  action differences   %d\n", action_diffs);

    if (outcome_diffs == 0 && action_diffs == 0) {
        std::printf("\nThe sugar is the expansion. A counting quantifier that silently voted 1oo3 or 3oo3 would read\n");
        std::printf("as correct in the source and fail only in the plant, so this is checked rather than reviewed.\n");
        return 0;
    }
    return 1;
}
