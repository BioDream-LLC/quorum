// Does a compile-time preference ordering achieve what a runtime NP-hard search achieves?
//
// Two results arrived from opposite directions and the arithmetic lined up too neatly to leave alone.
//
//   * The severity experiment: declaring FMEA severities on the pump's claims resolved 48 of its 86 ambiguous states,
//     every one of them to `maintain`. That happens in the COMPILER. It costs the device nothing.
//   * The advisory tier: preferred semantics leaves 38 of the pump's contested states ambiguous. 86 - 38 = 48.
//
// If those are the same 48 states, it is a significant product finding and not a coincidence: a reliability engineer
// could get the advisory tier's benefit by writing one number per claim in a text file, on an $11 microcontroller, with
// no second box, no NP-hard search, and no WCET problem.
//
// If they are different sets, that is equally informative in the other direction: the two mechanisms would be
// complementary rather than redundant, and the Pi earns its place.
//
// Either answer is worth having. The point of writing this is that I do not know which it is.

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "quorum/advisory/semantics.hpp"
#include "quorum/arbiter/verdict.hpp"

#include "pump_generated.hpp"
#include "pump_severity_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace sev = quorum::casebook::gen::centrifugal_pump_severity;

using quorum::advisory::Analysis;
using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

/// Which action, if any, do ALL preferred extensions agree on? -1 for none selected, -2 for disagreement.
std::int32_t advisory_action(const Analysis& a) {
    std::set<std::int32_t> chosen;
    for (const auto& ext : a.preferred) {
        std::int32_t pick = -1;
        for (std::int32_t i = 0; i < pump::kActionCount; ++i) {
            if (ext.contains(Arg{pump::kActions[i]})) {
                pick = pump::kActions[i];
                break;
            }
        }
        chosen.insert(pick);
    }
    return chosen.size() == 1 ? *chosen.begin() : -2;
}

}  // namespace

int main() {
    std::printf("compile-time severity vs runtime preferred semantics\n\n");

    pump::CaseFramework fw;
    pump::CaseSolver grounded;
    pump::CasePolicy policy;

    sev::CaseFramework sfw;
    sev::CaseSolver sgrounded;
    sev::CasePolicy spolicy;

    if (pump::make_policy(policy) != Status::Ok || sev::make_policy(spolicy) != Status::Ok) {
        std::printf("FAIL: policy construction\n");
        return 1;
    }

    std::set<std::int32_t> indeterminate_baseline;
    std::set<std::int32_t> resolved_by_severity;
    std::set<std::int32_t> resolved_by_advisory;

    std::int32_t severity_and_advisory_agree = 0;
    std::int32_t severity_and_advisory_differ = 0;
    std::int32_t failures = 0;

    const std::int32_t states = 1 << pump::kObservationCount;
    for (std::int32_t s = 0; s < states; ++s) {
        const std::uint32_t mask = static_cast<std::uint32_t>(s);

        if (pump::instantiate(fw, mask) != Status::Ok) { continue; }
        const Verdict base = quorum::arbiter::decide(fw, grounded, policy);
        if (base.outcome != Outcome::Indeterminate) { continue; }
        indeterminate_baseline.insert(s);

        // What does the compile-time severity ordering do with this state?
        std::int32_t severity_action = -1;
        if (sev::instantiate(sfw, mask) == Status::Ok) {
            const Verdict sv = quorum::arbiter::decide(sfw, sgrounded, spolicy);
            if (sv.outcome == Outcome::Decided && sv.action.valid()) {
                resolved_by_severity.insert(s);
                severity_action = sv.action.get();
            }
        }

        // What does the runtime search do with it?
        pump::CaseSolver advisory_solver;
        const Analysis a = quorum::advisory::analyse(fw, advisory_solver);
        if (a.status != quorum::advisory::Status::Ok) {
            std::printf("FAIL state 0x%x: %s\n", static_cast<unsigned>(s),
                        quorum::advisory::status_name(a.status));
            ++failures;
            continue;
        }
        const std::int32_t adv = advisory_action(a);
        if (adv >= 0) { resolved_by_advisory.insert(s); }

        // Where both speak, do they say the same thing? A disagreement would be serious: it would mean the casebook
        // author's severity ordering contradicts the semantics, and the device would be confidently wrong.
        if (severity_action >= 0 && adv >= 0) {
            // Names, not indices: the two casebooks are separately compiled and their numbering need not coincide.
            const char* sname = sev::name_of(severity_action);
            const char* aname = pump::name_of(adv);
            const bool same = std::string(sname) == std::string(aname);
            if (same) {
                ++severity_and_advisory_agree;
            } else {
                ++severity_and_advisory_differ;
                if (severity_and_advisory_differ <= 5) {
                    std::printf("  DIFFER state 0x%-6x severity says %-10s advisory says %s\n",
                                static_cast<unsigned>(s), sname, aname);
                }
            }
        }
    }

    std::printf("baseline Indeterminate states           %zu\n", indeterminate_baseline.size());
    std::printf("resolved by compile-time severity       %zu\n", resolved_by_severity.size());
    std::printf("resolved by runtime preferred semantics  %zu\n", resolved_by_advisory.size());
    std::printf("\n");

    // The comparison the whole file exists for.
    std::vector<std::int32_t> only_severity;
    std::vector<std::int32_t> only_advisory;
    std::vector<std::int32_t> both;
    for (const std::int32_t s : indeterminate_baseline) {
        const bool sv = resolved_by_severity.count(s) > 0;
        const bool ad = resolved_by_advisory.count(s) > 0;
        if (sv && ad) { both.push_back(s); }
        else if (sv) { only_severity.push_back(s); }
        else if (ad) { only_advisory.push_back(s); }
    }

    std::printf("resolved by BOTH                        %zu\n", both.size());
    std::printf("resolved ONLY by severity (compile-time) %zu\n", only_severity.size());
    std::printf("resolved ONLY by advisory (runtime)      %zu\n", only_advisory.size());
    std::printf("resolved by neither                      %zu\n",
                indeterminate_baseline.size() - both.size() - only_severity.size() - only_advisory.size());
    std::printf("\nwhere both spoke: %d agreed, %d differed\n\n", severity_and_advisory_agree,
                severity_and_advisory_differ);

    std::printf("─────────────────────────────────────────────────────────────────────────────\n");
    if (severity_and_advisory_differ > 0) {
        std::printf("SERIOUS: the author's severity ordering contradicts preferred semantics in %d states.\n",
                    severity_and_advisory_differ);
        std::printf("The device would be confidently wrong there. Severity must not be able to do this.\n");
        ++failures;
    } else if (only_advisory.empty() && !both.empty()) {
        std::printf("Compile-time severity resolves everything the runtime search resolves, and %zu states\n",
                    only_severity.size());
        std::printf("the search cannot. A reliability engineer gets the benefit by writing one number per\n");
        std::printf("claim in a text file -- on an $11 part, with no second box and no NP-hard search.\n");
        std::printf("That narrows the advisory tier's justification considerably, and it should be said.\n");
    } else if (!only_advisory.empty()) {
        std::printf("The two mechanisms are COMPLEMENTARY: severity resolves %zu states the search cannot,\n",
                    only_severity.size());
        std::printf("and the search resolves %zu that severity cannot. The Pi earns its place, and the\n",
                    only_advisory.size());
        std::printf("right product answer is both rather than either.\n");
    }

    std::printf("\n%s\n", failures == 0 ? "no failures" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
