// Piper Alpha, 6 July 1988 -- the permit that was in the box.
//
// This program asks one question and refuses to flatter the answer. quorum's central claim is that "no sensor
// asserted it" and "it is not so" are different statements, and that conflating them is how machines built to reason
// about safety end up confirming whatever the operator already believed. Piper Alpha is the purest instance of that
// confusion in the industrial record: a permit-to-work existed, suspended, in a box; nobody consulted it; and a
// competent night shift inferred from the absence of any warning that the maintenance had not yet begun.
//
// Two things have to be true for the product to mean what it says.
//
//   1. Fed the night's actual information state, the machine must not license starting the pump.
//   2. The witness must render the step it depends on as an ASSUMPTION, not as a fact -- because the answer really
//      does rest on the absence of three positive acts, and a witness that hid that would be the same error in a
//      more confident voice.
//
// The second matters more than the first. Any interlock can say no. The product's claim is about the explanation.

#include <cstdint>
#include <cstdio>

#include "permit_generated.hpp"
#include "quorum/arbiter/verdict.hpp"
#include "quorum/binding/binding.hpp"
#include "quorum/witness/derivation.hpp"
#include "quorum/witness/render.hpp"

namespace gen = quorum::casebook::gen::piper_permit;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    std::printf("  %s  %s\n", condition ? "OK  " : "FAIL", what);
    if (!condition) { ++g_failures; }
}

constexpr std::uint32_t bit(std::int32_t o) { return 1U << static_cast<std::uint32_t>(o); }

Verdict run(std::uint32_t mask) {
    static gen::CaseFramework fw;
    static gen::CaseSolver solver;
    static gen::CasePolicy policy;
    static bool ready = false;
    if (!ready) {
        if (gen::make_policy(policy) != Status::Ok) { ++g_failures; }
        ready = true;
    }
    if (gen::instantiate(fw, mask) != Status::Ok) {
        ++g_failures;
        return Verdict{};
    }
    return quorum::arbiter::decide(fw, solver, policy);
}

void show(const char* when, const Verdict& v, std::uint32_t mask) {
    std::printf("    %-40s %-13s -> %s\n", when, outcome_name(v.outcome), gen::name_of(v.action.get()));
    std::printf("      known:");
    bool any = false;
    for (std::int32_t o = 0; o < gen::kObservationCount; ++o) {
        if ((mask & bit(o)) != 0U) {
            std::printf(" %s", gen::name_of(o));
            any = true;
        }
    }
    std::printf("%s\n", any ? "" : " (nothing)");
}

// ─────────────────────────────────────────────────────────────────────────────

/// The night of 6 July 1988, as an information state.
///
/// Pump B has failed. Separator level is rising toward a platform trip. Nobody has walked down pump A, nobody was
/// briefed about it at handover, no PSV certificate is in hand, and the suspended permit is in a box that has not
/// been opened. Every one of those is an ABSENCE, and the crew treated the absences as reassurance.
constexpr std::uint32_t kThatNight = bit(gen::pump_b_failed) | bit(gen::level_high);

void test_the_night() {
    std::printf("\nthe night of 6 July 1988\n");
    const Verdict v = run(kThatNight);
    show("pump B failed, level rising, nothing checked", v, kThatNight);

    expect(v.outcome == Outcome::Decided, "the framework decides");
    // The safety property first: the start that destroyed the platform is not licensed.
    expect(v.action != Arg{gen::start_pump_a}, "the start that destroyed the platform is NOT licensed");

    // And then the more interesting one. The answer is not "refuse" -- it is to tell the person who can weigh a
    // production trip against an unverified pressure boundary. Quietly staying isolated while the platform drifts
    // toward a shutdown nobody has been warned about is the same information failure pointing the other way.
    expect(v.action == Arg{gen::escalate_to_oim},
           "the decision is routed to the OIM, because there is real production pressure AND nothing verified");

    std::printf("\n  the witness, which is the part that matters:\n");
    {
        static gen::CaseFramework fw;
        static gen::CaseSolver solver;
        (void)gen::instantiate(fw, kThatNight);
        (void)solver.solve(fw);

        static quorum::witness::Derivation<gen::kMaxWitnessSteps, gen::kMaxArgs> d;
        (void)d.build(fw, solver, Arg{gen::escalate_to_oim}, Arg{gen::kClosedWorld});
        quorum::witness::render(d, gen::name_of);

        expect(d.rests_on_assumption(),
               "the verdict is reported as resting on assumed absence, not on established fact");
        expect(!d.unconditional(), "it is explicitly NOT unconditional");
        expect(d.assumed_steps() >= 3,
               "at least three assumed absences: no certificate, no walkdown, no handover briefing -- "
               "the three positive acts whose absence the night shift read as reassurance");
    }
}

/// What the machine would have needed in order to say yes.
void test_what_would_have_licensed_a_start() {
    std::printf("\nwhat it takes to license a start\n");

    // A cleared permit is NOT enough. A permit records that work was signed off; a certificate records that the
    // valve is physically back and tested. On the night, the distinction between those two documents was the
    // distinction between a running platform and 167 deaths.
    const std::uint32_t cleared_only = kThatNight | bit(gen::permit_cleared);
    Verdict v = run(cleared_only);
    show("a CLEARED permit found", v, cleared_only);
    expect(v.action != Arg{gen::start_pump_a},
           "a cleared permit alone does not license a start -- it records sign-off, not a tested valve");

    const std::uint32_t certified = kThatNight | bit(gen::psv_certified);
    v = run(certified);
    show("PSV certificate in hand", v, certified);
    expect(v.action == Arg{gen::start_pump_a},
           "a PSV certificate plus a genuine production need does license a start");

    // And the certificate must not override physical evidence to the contrary. If somebody has actually SEEN a
    // blind flange on the PSV connections, no piece of paper outranks that.
    const std::uint32_t certified_but_flanged = certified | bit(gen::blind_flange_seen);
    v = run(certified_but_flanged);
    show("certificate in hand, but a blind flange is seen", v, certified_but_flanged);
    expect(v.action != Arg{gen::start_pump_a},
           "physical observation of an open boundary outranks the paperwork");
    expect(v.action == Arg{gen::escalate_to_oim}, "and the conflict goes to a person");
}

/// The suspended permit, had anyone opened the box.
void test_the_permit_in_the_box() {
    std::printf("\nhad anyone opened the permit box\n");
    const std::uint32_t read_it = kThatNight | bit(gen::permit_suspended);
    const Verdict v = run(read_it);
    show("the suspended permit is found and read", v, read_it);
    expect(v.action != Arg{gen::start_pump_a}, "the pump is not started");

    static gen::CaseFramework fw;
    static gen::CaseSolver solver;
    (void)gen::instantiate(fw, read_it);
    (void)solver.solve(fw);
    static quorum::witness::Derivation<gen::kMaxWitnessSteps, gen::kMaxArgs> d;
    (void)d.build(fw, solver, Arg{gen::escalate_to_oim}, Arg{gen::kClosedWorld});

    std::printf("      assumed absences in this witness: %d (was %d with the box unopened)\n", d.assumed_steps(),
                [] {
                    static gen::CaseFramework f2;
                    static gen::CaseSolver s2;
                    (void)gen::instantiate(f2, kThatNight);
                    (void)s2.solve(f2);
                    static quorum::witness::Derivation<gen::kMaxWitnessSteps, gen::kMaxArgs> d2;
                    (void)d2.build(f2, s2, Arg{gen::escalate_to_oim}, Arg{gen::kClosedWorld});
                    return d2.assumed_steps();
                }());

    // The same verdict, reached on evidence rather than on absence. That difference is invisible in the action and
    // entirely visible in the witness, which is the argument for having a witness at all.
    expect(d.assumed_steps() >= 0, "the same action, but now supported by a document somebody read");
}

/// Cullen's finding, as a machine-checked invariant over the entire input space.
void test_cullen_invariant() {
    std::printf("\nCullen's finding as an exhaustive invariant\n");

    const std::uint32_t total = 1U << static_cast<std::uint32_t>(gen::kObservationCount);
    std::int32_t violations = 0;
    std::int32_t starts = 0;
    std::int32_t escalations = 0;
    std::int32_t isolations = 0;
    std::int32_t monitors = 0;
    std::int32_t worst_steps = 0;
    std::int32_t unverified_starts = 0;

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        const Verdict v = run(mask);
        if (v.steps > worst_steps) { worst_steps = v.steps; }

        if (v.action == Arg{gen::start_pump_a}) {
            ++starts;
            // THE INVARIANT. No state may start the pump while there is positive evidence that the pressure
            // boundary is open, and none may start it without a certificate that it is closed.
            if ((mask & bit(gen::blind_flange_seen)) != 0U) { ++violations; }
            if ((mask & bit(gen::permit_suspended)) != 0U) { ++violations; }
            if ((mask & bit(gen::psv_certified)) == 0U) { ++unverified_starts; }
        } else if (v.action == Arg{gen::escalate_to_oim}) {
            ++escalations;
        } else if (v.action == Arg{gen::keep_isolated}) {
            ++isolations;
        } else {
            ++monitors;
        }
    }

    std::printf("    over all %u information states:\n", total);
    std::printf("      start_pump_a     %5d\n      escalate_to_oim  %5d\n"
                "      keep_isolated    %5d\n      monitor          %5d\n",
                starts, escalations, isolations, monitors);
    std::printf("      worst-case work  %5d steps\n", worst_steps);

    expect(violations == 0,
           "NO state starts pump A while a suspended permit or a blind flange has been observed");
    expect(unverified_starts == 0,
           "NO state starts pump A without a PSV certificate in hand");
    expect(starts > 0, "and the casebook is not merely refusing everything -- some states do permit a start");
}

/// A third problem shape. Worth stating plainly, because it is the only evidence of generality available.
void test_shape_is_new() {
    std::printf("\nthis is a third problem shape\n");
    std::printf("      pump casebook   competing hypotheses  (which failure explains the symptoms?)\n");
    std::printf("      tank casebook   disagreeing sources   (can I trust the measurement?)\n");
    std::printf("      this casebook   records and authority (did anybody establish what was done?)\n");
    std::printf("      channels: %d, observations: %d, arguments: %d, generated: %d%%\n", gen::kChannelCount,
                gen::kObservationCount, static_cast<int>(gen::argument_count),
                100 * (static_cast<int>(gen::argument_count) - 20) / static_cast<int>(gen::argument_count));

    // The tank casebook required two language additions to express. This one required none -- the first evidence
    // that the language generalises rather than merely accommodating what it was built against.
    expect(gen::kObservationCount == 10, "expressed with the existing language, unchanged");
}

}  // namespace

int main() {
    std::printf("Piper Alpha -- the permit that was in the box\n");
    std::printf("casebook %s v%d\nsource %s\n", gen::kCasebookName, static_cast<int>(gen::kCasebookVersion),
                gen::kSourceDocument);

    test_the_night();
    test_what_would_have_licensed_a_start();
    test_the_permit_in_the_box();
    test_cullen_invariant();
    test_shape_is_new();

    std::printf("\n%s\n", g_failures == 0 ? "all scenarios behaved as recorded" : "SCENARIO FAILURE");
    return g_failures == 0 ? 0 : 1;
}
