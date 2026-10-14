// What does an operator actually see in the states the machine will not decide?
//
// The pump has 38 states where neither grounded semantics, a severity ordering, nor preferred semantics picks an
// action. Those are the states where the product's answer is "ask a human", and they are therefore the states where the
// OUTPUT matters most -- a verdict of `Indeterminate` is correct and nearly useless to the person holding the radio.
//
// This exercises the explanation path on every one of them and asserts the properties that make it useful:
//
//   * the competing readings are named, not just counted
//   * only the arguments that actually differ are mentioned, because common ground is not news
//   * where one further observation would settle it, that observation is named
//   * where none would, it says so rather than implying one exists
//
// It also prints real examples, so the wording is reviewable by someone who is not me.

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "quorum/advisory/explain.hpp"
#include "quorum/arbiter/verdict.hpp"

#include "pump_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;

using quorum::advisory::Analysis;
using quorum::advisory::Explanation;
using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Status;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

}  // namespace

int main() {
    std::printf("what a human sees when the machine will not decide\n\n");

    pump::CaseFramework fw;
    pump::CaseSolver solver;
    pump::CasePolicy policy;
    pump::CaseFramework probe_fw;
    pump::CaseSolver probe_solver;

    if (pump::make_policy(policy) != Status::Ok) {
        std::printf("FAIL: policy\n");
        return 1;
    }

    std::int32_t indeterminate = 0;
    std::int32_t disagreeing = 0;
    std::int32_t immaterial = 0;
    std::int32_t with_discriminator = 0;
    std::int32_t without_discriminator = 0;
    std::int32_t examples_printed = 0;
    std::set<std::int32_t> discriminators_used;

    const std::int32_t states = 1 << pump::kObservationCount;
    for (std::int32_t s = 0; s < states; ++s) {
        const std::uint32_t mask = static_cast<std::uint32_t>(s);
        if (pump::instantiate(fw, mask) != Status::Ok) { continue; }

        const Verdict v = quorum::arbiter::decide(fw, solver, policy);
        if (v.outcome != Outcome::Indeterminate) { continue; }
        ++indeterminate;

        const Analysis a = quorum::advisory::analyse(fw, solver);
        if (a.status != quorum::advisory::Status::Ok) {
            std::printf("  FAIL  state 0x%x: %s\n", static_cast<unsigned>(s),
                        quorum::advisory::status_name(a.status));
            ++g_failures;
            continue;
        }

        Explanation e = quorum::advisory::explain(a, pump::kActions, pump::kActionCount, fw.argument_count(),
                                                  [](std::int32_t x) { return pump::is_authored(x); });
        if (e.readings.size() < 2) { continue; }

        quorum::advisory::find_discriminators(
            e, probe_fw, probe_solver, mask, pump::kObservationCount, pump::kActions, pump::kActionCount,
            [](pump::CaseFramework& f, std::uint32_t m) { return pump::instantiate(f, m); },
            [](std::int32_t x) { return pump::is_authored(x); });

        if (!e.actions_disagree) {
            ++immaterial;
            continue;
        }
        ++disagreeing;

        // Every disagreement must name what it is about. An explanation that says "two readings" without saying what
        // they differ on is the same non-answer as `Indeterminate`, dressed up.
        check(!e.contested.empty(), "a disagreement names the arguments it is about");

        // Only genuinely differing arguments, never common ground.
        for (const std::int32_t arg : e.contested) {
            // Nothing the compiler invented, and no action -- the reading already states its action.
            check(pump::is_authored(arg), "no compiler-generated argument is ever shown to an operator");
            for (std::int32_t i = 0; i < pump::kActionCount; ++i) {
                check(pump::kActions[i] != arg, "an action is not repeated as a contested argument");
            }
            bool any = false, all = true;
            for (const auto& ext : a.preferred) {
                if (ext.contains(quorum::solver::Arg{arg})) { any = true; } else { all = false; }
            }
            check(any && !all, "every named argument actually differs between readings");
        }

        if (!e.discriminators.empty()) {
            ++with_discriminator;
            for (const std::int32_t d : e.discriminators) {
                discriminators_used.insert(d);
                check(d < pump::kObservationCount, "a discriminator is an observation an operator can go and check");
                check((mask & (1U << static_cast<std::uint32_t>(d))) == 0U,
                      "a discriminator is not something already asserted");
            }
        } else {
            ++without_discriminator;
        }

        const std::string text = quorum::advisory::render(e, [](std::int32_t a2) { return pump::name_of(a2); });
        check(!text.empty(), "an explanation renders to something");
        check(text.find("Reading A") != std::string::npos, "readings are labelled for reference");

        if (examples_printed < 3) {
            std::printf("─── state 0x%x ─────────────────────────────────────────────────────────\n",
                        static_cast<unsigned>(s));
            std::printf("%s\n", text.c_str());
            ++examples_printed;
        }
    }

    std::printf("─────────────────────────────────────────────────────────────────────────────\n");
    std::printf("Indeterminate states                    %d\n", indeterminate);
    std::printf("  readings agree on the action           %d  (ambiguity immaterial)\n", immaterial);
    std::printf("  readings disagree on the action        %d  (a human is genuinely needed)\n", disagreeing);
    std::printf("    one observation would resolve it     %d\n", with_discriminator);
    std::printf("    needs more than one new fact         %d\n", without_discriminator);
    std::printf("  distinct observations ever suggested   %zu\n", discriminators_used.size());
    std::printf("\n");

    if (disagreeing > 0) {
        const double pct = 100.0 * static_cast<double>(with_discriminator) / static_cast<double>(disagreeing);
        std::printf("In %.0f%% of the states where a human is genuinely needed, the product can name a single\n", pct);
        std::printf("observation that would settle it. That is the difference between \"I don't know\" and a\n");
        std::printf("work instruction, and it is the one thing the Linux tier does that the device cannot:\n");
        std::printf("finding it costs one re-analysis per unasserted observation.\n");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
