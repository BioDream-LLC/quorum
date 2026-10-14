// FMEA severity via ASPIC+ preference ordering: what does it actually buy, and what does it cost?
//
// Severity is the one column of an FMEA that translates into the framework. Risk priority numbers do not: RPN is
// severity x occurrence x detection, and occurrence and detection are statements about a population of machines, not
// about the one the device is attached to. Severity is a property of the failure itself, and it answers a question
// the framework genuinely has to resolve -- when the evidence cannot distinguish two hypotheses, which should the
// machine act on?
//
// The mechanism is ASPIC+'s: a preference-dependent attack (rebutting, undermining) succeeds as a DEFEAT only if the
// attacker is not strictly less preferred than its target. Undercutting is preference-independent and untouched,
// because an undercut says the inference does not apply at all, which no amount of severity outranks.
//
// This test is a controlled experiment. `pump_severity.cb` is `pump.cb` with four severity annotations and NOTHING
// else changed, so the difference between the two reports is attributable to the annotations alone.
//
// The result to check is not "ambiguity went down". Any mechanism that makes the machine commit more often reduces
// ambiguity, including a bad one. The question is whether it commits in the RIGHT DIRECTION, and the assertions
// below are aimed at that: the permissive action and the drastic action must both be completely unchanged.

#include <cstdint>
#include <cstdio>

#include "pump_generated.hpp"
#include "pump_severity_generated.hpp"
#include "quorum/arbiter/verdict.hpp"

namespace plain = quorum::casebook::gen::centrifugal_pump;
namespace sev = quorum::casebook::gen::centrifugal_pump_severity;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

int g_failures = 0;

void ok(bool condition, const char* what) {
    std::printf("  %s  %s\n", condition ? "OK  " : "FAIL", what);
    if (!condition) { ++g_failures; }
}

struct Tally {
    std::int32_t outcomes[5] = {0, 0, 0, 0, 0};
    std::int32_t keep_running = 0;
    std::int32_t maintain = 0;
    std::int32_t shutdown = 0;
    std::int32_t worst_steps = 0;
};

}  // namespace

int main() {
    std::printf("FMEA severity through ASPIC+ preference ordering\n");
    std::printf("controlled experiment: pump.cb vs the same file plus four severity annotations\n\n");

    static_assert(plain::kObservationCount == sev::kObservationCount,
                  "the two casebooks must share an observation numbering for the comparison to mean anything");
    static_assert(static_cast<std::int32_t>(plain::argument_count) ==
                      static_cast<std::int32_t>(sev::argument_count),
                  "severity must not add or remove arguments -- it only cancels attacks");

    std::printf("  arguments      %d both\n", static_cast<int>(plain::argument_count));
    std::printf("  attacks        %d plain, %d with severity  (%d cancelled)\n",
                static_cast<int>(plain::kCasebookEdges), static_cast<int>(sev::kCasebookEdges),
                static_cast<int>(plain::kCasebookEdges) - static_cast<int>(sev::kCasebookEdges));

    static plain::CaseFramework pfw;
    static plain::CaseSolver psolver;
    static plain::CasePolicy ppolicy;
    static sev::CaseFramework sfw;
    static sev::CaseSolver ssolver;
    static sev::CasePolicy spolicy;

    if (plain::make_policy(ppolicy) != Status::Ok || sev::make_policy(spolicy) != Status::Ok) {
        std::printf("FAIL  policy setup\n");
        return 1;
    }

    Tally plain_t;
    Tally sev_t;
    std::int32_t resolved = 0;         // was Indeterminate, now Decided
    std::int32_t newly_ambiguous = 0;  // was Decided, now Indeterminate
    std::int32_t resolved_to_maintain = 0;
    std::int32_t resolved_to_shutdown = 0;
    std::int32_t resolved_to_continue = 0;
    std::int32_t action_changed_while_decided = 0;

    const std::uint32_t total = 1U << static_cast<std::uint32_t>(plain::kObservationCount);

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        if (plain::instantiate(pfw, mask) != Status::Ok || sev::instantiate(sfw, mask) != Status::Ok) {
            std::printf("FAIL  construction at state %u\n", mask);
            return 1;
        }
        const Verdict pv = quorum::arbiter::decide(pfw, psolver, ppolicy);
        const Verdict svd = quorum::arbiter::decide(sfw, ssolver, spolicy);

        ++plain_t.outcomes[static_cast<std::size_t>(pv.outcome)];
        ++sev_t.outcomes[static_cast<std::size_t>(svd.outcome)];
        if (pv.steps > plain_t.worst_steps) { plain_t.worst_steps = pv.steps; }
        if (svd.steps > sev_t.worst_steps) { sev_t.worst_steps = svd.steps; }

        auto count = [](const Verdict& v, Tally& t, std::int32_t keep, std::int32_t maint, std::int32_t shut) {
            if (v.outcome != Outcome::Decided) { return; }
            if (v.action == Arg{keep}) { ++t.keep_running; }
            if (v.action == Arg{maint}) { ++t.maintain; }
            if (v.action == Arg{shut}) { ++t.shutdown; }
        };
        count(pv, plain_t, plain::continue_, plain::maintain, plain::shutdown);
        count(svd, sev_t, sev::continue_, sev::maintain, sev::shutdown);

        const bool p_amb = pv.outcome == Outcome::Indeterminate;
        const bool s_amb = svd.outcome == Outcome::Indeterminate;

        if (p_amb && !s_amb) {
            ++resolved;
            if (svd.action == Arg{sev::continue_}) { ++resolved_to_continue; }
            if (svd.action == Arg{sev::maintain}) { ++resolved_to_maintain; }
            if (svd.action == Arg{sev::shutdown}) { ++resolved_to_shutdown; }
        }
        if (!p_amb && s_amb) { ++newly_ambiguous; }

        // Where both committed, did the chosen action change? Severity resolving an ambiguity is the intent;
        // severity changing an already-decided verdict would be a much stronger and more alarming effect.
        if (pv.outcome == Outcome::Decided && svd.outcome == Outcome::Decided) {
            const bool same = (pv.action == Arg{plain::continue_} && svd.action == Arg{sev::continue_}) ||
                              (pv.action == Arg{plain::maintain} && svd.action == Arg{sev::maintain}) ||
                              (pv.action == Arg{plain::shutdown} && svd.action == Arg{sev::shutdown});
            if (!same) { ++action_changed_while_decided; }
        }
    }

    std::printf("\n  over all %u states           plain    severity\n", total);
    for (std::int32_t i = 0; i < 5; ++i) {
        std::printf("    %-22s %7d %11d\n", outcome_name(static_cast<Outcome>(i)),
                    plain_t.outcomes[static_cast<std::size_t>(i)], sev_t.outcomes[static_cast<std::size_t>(i)]);
    }
    std::printf("    %-22s %7d %11d\n", "worst-case steps", plain_t.worst_steps, sev_t.worst_steps);
    std::printf("\n    %-22s %7d %11d\n", "continue", plain_t.keep_running, sev_t.keep_running);
    std::printf("    %-22s %7d %11d\n", "maintain", plain_t.maintain, sev_t.maintain);
    std::printf("    %-22s %7d %11d\n", "shutdown", plain_t.shutdown, sev_t.shutdown);

    std::printf("\n  %d ambiguous states resolved, %d newly ambiguous\n", resolved, newly_ambiguous);
    std::printf("    of the resolved: %d -> continue, %d -> maintain, %d -> shutdown\n", resolved_to_continue,
                resolved_to_maintain, resolved_to_shutdown);

    // ── what the experiment has to show ─────────────────────────────────────

    ok(resolved > 0, "severity resolves ambiguity that grounded semantics alone leaves open");
    ok(newly_ambiguous == 0, "and creates no new ambiguity");

    // THE SAFETY ASSERTIONS. Reducing ambiguity is easy and a bad mechanism would also do it. What matters is the
    // direction: the machine must not become more willing to keep running, and must not become trigger-happy either.
    ok(sev_t.keep_running == plain_t.keep_running,
       "severity does NOT make the machine more permissive -- 'continue' is selected in exactly the same states");
    ok(sev_t.shutdown == plain_t.shutdown,
       "and NOT more drastic -- 'shutdown' is selected in exactly the same states");
    ok(resolved_to_continue == 0, "no ambiguity was resolved in favour of carrying on");
    ok(resolved_to_shutdown == 0, "and none escalated straight to a shutdown");
    ok(resolved_to_maintain == resolved,
       "every resolved ambiguity became 'schedule maintenance' -- the middle action, which is the honest answer "
       "when two failure modes are indistinguishable but one is worse");
    ok(action_changed_while_decided == 0,
       "no state that was already Decided changed its action -- severity only breaks ties");

    ok(sev_t.outcomes[static_cast<std::size_t>(Outcome::Conflicted)] == 0, "no Conflicted states appear");
    ok(sev_t.outcomes[static_cast<std::size_t>(Outcome::Unsatisfied)] == 0, "no Unsatisfied states appear");
    ok(sev_t.worst_steps <= plain_t.worst_steps,
       "and the worst case does not grow: cancelling an attack removes work, it does not add any");

    std::printf("\n%s\n", g_failures == 0
                              ? "severity resolves ties in the conservative direction and changes nothing else"
                              : "SEVERITY CHANGED SOMETHING IT SHOULD NOT HAVE");
    return g_failures == 0 ? 0 : 1;
}
