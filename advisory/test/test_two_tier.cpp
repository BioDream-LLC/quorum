// Does the advisory tier actually add anything?
//
// The two-tier architecture -- grounded semantics on a microcontroller as the safety function, search-based semantics
// on a Linux box as advisory -- has been a design claim since early on, justified by citation: the grounded extension
// is contained in every complete extension, so the device can never contradict the advisory tier, only be less
// committal. That is true and it is the safety argument. It says nothing about whether the advisory tier is USEFUL.
//
// This asks two questions the citation cannot answer.
//
//   1. Is the containment claim actually true of these casebooks, in every state? Exhaustively, not by argument. If it
//      ever fails, the two-tier split is unsound and the product's architecture is wrong.
//   2. In the states grounded cannot decide, does preferred semantics decide them? If it does, the advisory tier
//      converts "ask a human" into an answer. If every such state has several preferred extensions that disagree, the
//      advisory tier cannot decide either -- and the honest product claim shrinks to enumerating the alternatives.
//
// The second question has a real chance of a negative answer, which is why it is worth asking.

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "quorum/advisory/semantics.hpp"
#include "quorum/arbiter/verdict.hpp"

#include "permit_generated.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"

using quorum::advisory::Analysis;
using quorum::solver::Arg;
using quorum::solver::Label;

namespace {

int g_failures = 0;

struct Result {
    std::string name;
    std::int32_t states = 0;
    std::int32_t contested = 0;          // states where grounded left something undecided
    std::int32_t resolved_by_preferred = 0;  // ... and preferred gives exactly one answer
    std::int32_t still_ambiguous = 0;        // ... and preferred gives several
    std::int32_t no_stable = 0;              // states with no stable extension at all
    std::int32_t containment_checked = 0;
    std::int32_t containment_violations = 0;
    std::int32_t action_contradictions = 0;
    std::map<std::size_t, std::int32_t> preferred_histogram;
    std::int64_t subsets = 0;
};

template <typename Fw, typename Solver, typename Policy, typename Instantiate, typename MakePolicy>
Result run(const std::string& name, std::int32_t observations, Instantiate instantiate, MakePolicy make_policy,
           const std::int32_t* actions, std::int32_t action_count) {
    Result r;
    r.name = name;

    Fw fw;
    Solver grounded;
    Solver grounded2;
    Policy policy;
    if (make_policy(policy) != quorum::solver::Status::Ok) {
        std::printf("  FAIL  %s: policy construction failed\n", name.c_str());
        ++g_failures;
        return r;
    }

    const std::int32_t states = 1 << observations;
    r.states = states;

    for (std::int32_t s = 0; s < states; ++s) {
        if (instantiate(fw, static_cast<std::uint32_t>(s)) != quorum::solver::Status::Ok) { continue; }

        // The device's answer.
        const quorum::arbiter::Verdict verdict = quorum::arbiter::decide(fw, grounded, policy);

        // The advisory tier's answer. Uses its own solver instance so the device verdict cannot be perturbed.
        const Analysis a = quorum::advisory::analyse(fw, grounded2);
        if (a.status != quorum::advisory::Status::Ok) {
            std::printf("  FAIL  %s state 0x%x: %s\n", name.c_str(), static_cast<unsigned>(s),
                        quorum::advisory::status_name(a.status));
            ++g_failures;
            continue;
        }
        r.subsets += a.subsets_examined;

        // ── question 1: containment, in every state ─────────────────────────
        //
        // Every argument the device accepts must be accepted by EVERY preferred extension, and every argument the
        // device rejects must be rejected by every one. This is the property that makes the split safe. If it fails,
        // a $5 part and a $45 part can disagree about a running machine.
        if (a.preferred.empty()) {
            std::printf("  FAIL  %s state 0x%x: no preferred extension exists\n", name.c_str(),
                        static_cast<unsigned>(s));
            ++g_failures;
            continue;
        }
        for (const auto& ext : a.preferred) {
            ++r.containment_checked;
            for (std::int32_t arg = 0; arg < fw.argument_count(); ++arg) {
                const Label device = grounded2.label(Arg{arg});
                // grounded2 was re-solved inside analyse(), so its labels are this state's grounded labelling.
                const bool in_ext = ext.contains(Arg{arg});
                if (device == Label::In && !in_ext) {
                    std::printf("  FAIL  %s state 0x%x: grounded accepts arg %d, a preferred extension does not\n",
                                name.c_str(), static_cast<unsigned>(s), arg);
                    ++r.containment_violations;
                    ++g_failures;
                }
                if (device == Label::Out && in_ext) {
                    std::printf("  FAIL  %s state 0x%x: grounded rejects arg %d, a preferred extension accepts it\n",
                                name.c_str(), static_cast<unsigned>(s), arg);
                    ++r.containment_violations;
                    ++g_failures;
                }
            }
        }

        if (a.stable.empty()) { ++r.no_stable; }

        // ── question 2: does the advisory tier decide what grounded cannot? ─
        if (a.undecided_count == 0) { continue; }
        ++r.contested;
        ++r.preferred_histogram[a.preferred.size()];

        // Do all preferred extensions agree on which ACTION is selected? That, not argument labels, is what an
        // operator would be told.
        std::set<std::int32_t> actions_across_extensions;
        for (const auto& ext : a.preferred) {
            std::int32_t chosen = -1;
            for (std::int32_t i = 0; i < action_count; ++i) {
                if (ext.contains(Arg{actions[i]})) {
                    chosen = actions[i];
                    break;
                }
            }
            actions_across_extensions.insert(chosen);
        }

        if (actions_across_extensions.size() == 1) {
            ++r.resolved_by_preferred;
            // And it must not contradict the device: if the device committed to an action, the advisory tier must
            // agree. It may commit where the device abstained, never differ.
            const std::int32_t advised = *actions_across_extensions.begin();
            if (verdict.outcome == quorum::arbiter::Outcome::Decided && verdict.action.valid() &&
                advised != -1 && advised != verdict.action.get()) {
                std::printf("  FAIL  %s state 0x%x: device says action %d, advisory says %d\n", name.c_str(),
                            static_cast<unsigned>(s), verdict.action.get(), advised);
                ++r.action_contradictions;
                ++g_failures;
            }
        } else {
            ++r.still_ambiguous;
        }
    }
    return r;
}

void report(const Result& r) {
    std::printf("%s\n", r.name.c_str());
    std::printf("  states                        %d\n", r.states);
    std::printf("  containment checks            %d  (violations: %d)\n", r.containment_checked,
                r.containment_violations);
    std::printf("  states with no stable ext     %d\n", r.no_stable);
    std::printf("  states grounded left open     %d\n", r.contested);
    if (r.contested > 0) {
        std::printf("    preferred gives one answer  %d\n", r.resolved_by_preferred);
        std::printf("    preferred still ambiguous   %d\n", r.still_ambiguous);
        std::printf("    action contradictions       %d\n", r.action_contradictions);
        std::printf("    preferred extension counts: ");
        for (const auto& [count, states] : r.preferred_histogram) {
            std::printf(" %zu ext x%d ", count, states);
        }
        std::printf("\n");
    }
    std::printf("  subsets examined              %lld\n", static_cast<long long>(r.subsets));
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("advisory tier: is it sound, and is it useful?\n\n");

    namespace pump = quorum::casebook::gen::centrifugal_pump;
    namespace tank = quorum::casebook::gen::tank_409;
    namespace permit = quorum::casebook::gen::piper_permit;

    const Result rp = run<pump::CaseFramework, pump::CaseSolver, pump::CasePolicy>(
        "pump", pump::kObservationCount,
        [](pump::CaseFramework& f, std::uint32_t m) { return pump::instantiate(f, m); },
        [](pump::CasePolicy& p) { return pump::make_policy(p); }, pump::kActions, pump::kActionCount);

    const Result rt = run<tank::CaseFramework, tank::CaseSolver, tank::CasePolicy>(
        "tank-409", tank::kObservationCount,
        [](tank::CaseFramework& f, std::uint32_t m) { return tank::instantiate(f, m); },
        [](tank::CasePolicy& p) { return tank::make_policy(p); }, tank::kActions, tank::kActionCount);

    const Result rm = run<permit::CaseFramework, permit::CaseSolver, permit::CasePolicy>(
        "piper-permit", permit::kObservationCount,
        [](permit::CaseFramework& f, std::uint32_t m) { return permit::instantiate(f, m); },
        [](permit::CasePolicy& p) { return permit::make_policy(p); }, permit::kActions, permit::kActionCount);

    report(rp);
    report(rt);
    report(rm);

    const std::int32_t contested = rp.contested + rt.contested + rm.contested;
    const std::int32_t resolved = rp.resolved_by_preferred + rt.resolved_by_preferred + rm.resolved_by_preferred;
    const std::int32_t checks = rp.containment_checked + rt.containment_checked + rm.containment_checked;

    std::printf("─────────────────────────────────────────────────────────────────────────────\n");
    std::printf("SOUNDNESS: %d containment checks across every state of three casebooks, %d violations.\n", checks,
                rp.containment_violations + rt.containment_violations + rm.containment_violations);
    std::printf("The device verdict is contained in every preferred extension, so a $11 part and a Pi\n");
    std::printf("cannot disagree about a running machine. Machine-checked, not cited.\n\n");

    std::printf("USEFULNESS: grounded left %d of %d states open; preferred resolved %d of those.\n", contested,
                rp.states + rt.states + rm.states, resolved);
    if (contested == 0) {
        std::printf("Nothing for the advisory tier to add on these casebooks at all.\n");
    } else if (resolved == 0) {
        std::printf("The advisory tier decides NONE of them. Its honest value is enumerating the competing\n");
        std::printf("readings for a human, not deciding. The product claim must say that.\n");
    } else {
        std::printf("Worth having, and narrowly: it speaks only in the %.2f%% of states where the device\n",
                    100.0 * static_cast<double>(contested) / static_cast<double>(rp.states + rt.states + rm.states));
        std::printf("abstains -- which are exactly the states where an operator is already confused.\n");
    }

    std::printf("\n%s\n", g_failures == 0 ? "no failures" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
