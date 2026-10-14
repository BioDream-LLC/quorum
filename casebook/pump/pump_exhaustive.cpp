// Exhaustive validation of the centrifugal pump casebook.
//
// Twelve observations means 4,096 possible sensor states, which is the entire input space. Every one of them gets
// solved and checked against the casebook's invariants.
//
// This is the first thing in the project where the competition engineering actually earns its keep. It does not
// help with deployment -- a sparse 37-argument framework solves in microseconds either way -- but exhaustive
// validation of KNOWLEDGE is only affordable because solving is nearly free. A hand-picked scenario suite finds
// the bugs you thought of. This finds the ones you did not.
//
// Every invariant below is a claim about the casebook, not about the solver. If one fails, the FMEA translation
// is wrong.

#include <cstdint>
#include <cstdio>

#include "pump_casebook.hpp"
#include "quorum/arbiter/verdict.hpp"

namespace pump = quorum::casebook::pump;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

/// One invariant, and the states that break it.
struct Invariant {
    const char* name;
    std::int32_t violations = 0;
    std::uint32_t first_violation = 0;
    bool has_example = false;

    void observe(bool holds, std::uint32_t mask) {
        if (holds) { return; }
        ++violations;
        if (!has_example) {
            first_violation = mask;
            has_example = true;
        }
    }
};

void print_mask(std::uint32_t mask) {
    bool any = false;
    for (std::int32_t a = 0; a < pump::kObservationCount; ++a) {
        if ((mask & (1U << static_cast<std::uint32_t>(a))) != 0U) {
            std::printf(" %s", pump::name_of(a));
            any = true;
        }
    }
    if (!any) { std::printf(" (nothing asserted)"); }
}

}  // namespace

int main() {
    const std::uint32_t total = 1U << static_cast<std::uint32_t>(pump::kObservationCount);

    std::printf("exhaustive pump casebook validation\n");
    std::printf("%d observations -> %u states, %d arguments, %d fixed attacks\n\n",
                static_cast<int>(pump::kObservationCount),
                total,
                static_cast<int>(pump::argument_count),
                static_cast<int>(pump::kCasebookEdges));

    pump::PumpPolicy policy;
    if (pump::make_policy(policy) != Status::Ok) {
        std::printf("POLICY BUILD FAILED\n");
        return 1;
    }

    // ── Invariants ──────────────────────────────────────────────────────────
    // Safety-critical: these must hold in every reachable state.
    Invariant never_conflicted{"no state selects two actions at once"};
    Invariant never_unsatisfied{"no state rejects every action with nothing undecided"};
    Invariant never_faulted{"the solver completes in every state"};
    Invariant continue_excludes_fault{"act_continue is never In while a diagnosis is In"};
    Invariant shutdown_needs_seizure{"act_shutdown_now is In only when dx_bearing_wear is In"};
    Invariant maintenance_needs_fault{"act_schedule_maintenance is In only when some diagnosis is In"};
    Invariant within_wcet{"steps stays inside 2n + 2m"};
    Invariant action_always_valid{"a declared action is always returned"};
    // Liveness: a fault must always produce a response, never silence.
    Invariant fault_forces_response{"a live diagnosis never leaves act_continue In"};
    // The incoherence defeater. The fourth of these is the important one: it verifies that splitting raw readings
    // from usable claims actually removed the 3-cycle. If the layering were wrong, this argument would be
    // Undecided somewhere and the whole current channel would be silently unknowable.
    Invariant incoherent_iff_both{"def_current_incoherent is In exactly when both raw current readings are In"};
    Invariant incoherent_kills_claims{"def_current_incoherent In implies neither current claim is In"};
    Invariant claim_needs_raw{"a current claim is In only when its raw reading is In"};
    Invariant incoherent_never_undecided{"def_current_incoherent is never Undecided (no odd cycle)"};

    std::int32_t outcome_counts[5] = {0, 0, 0, 0, 0};
    std::int32_t max_steps = 0;
    std::uint32_t max_steps_mask = 0;
    std::int32_t indeterminate_example_count = 0;
    std::uint32_t indeterminate_examples[3] = {0, 0, 0};

    // ── The motor-current experiment ─────────────────────────────────────────
    // Exhaustive validation of the vibration-only casebook found 30 irreducibly ambiguous states. The claim under
    // test: a motor stator current channel resolves them. Measured directly -- for every ambiguous state with no
    // current evidence, does asserting one current signature turn it into a decision?
    const std::uint32_t bit_broadband = 1U << static_cast<std::uint32_t>(pump::obs_current_broadband_raw);
    const std::uint32_t bit_sidebands = 1U << static_cast<std::uint32_t>(pump::obs_current_sidebands_raw);
    const std::uint32_t current_bits = bit_broadband | bit_sidebands;

    std::int32_t indet_no_current = 0;
    std::int32_t indet_one_current = 0;
    std::int32_t indet_both_current = 0;
    std::int32_t resolved_by_broadband = 0;
    std::int32_t resolved_by_sidebands = 0;
    std::int32_t resolved_by_either = 0;
    std::int32_t unresolved_count = 0;
    std::uint32_t unresolved[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    auto classify = [&policy](std::uint32_t m, Outcome& out) -> bool {
        pump::PumpFramework f;
        pump::PumpSolver s;
        if (pump::instantiate(f, m) != Status::Ok) { return false; }
        out = quorum::arbiter::decide(f, s, policy).outcome;
        return true;
    };

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        pump::PumpFramework fw;
        pump::PumpSolver g;

        if (pump::instantiate(fw, mask) != Status::Ok) {
            std::printf("FRAMEWORK BUILD FAILED at mask %u\n", mask);
            return 1;
        }

        const Verdict v = quorum::arbiter::decide(fw, g, policy);
        ++outcome_counts[static_cast<std::size_t>(v.outcome)];

        if (v.steps > max_steps) {
            max_steps = v.steps;
            max_steps_mask = mask;
        }

        bool any_diagnosis_in = false;
        for (std::int32_t a = pump::dx_cavitation; a < pump::diagnosis_end; ++a) {
            if (g.label(Arg{a}) == Label::In) { any_diagnosis_in = true; }
        }

        const bool continue_in = g.label(Arg{pump::act_continue}) == Label::In;
        const bool maintenance_in = g.label(Arg{pump::act_schedule_maintenance}) == Label::In;
        const bool shutdown_in = g.label(Arg{pump::act_shutdown_now}) == Label::In;
        const bool bearing_in = g.label(Arg{pump::dx_bearing_wear}) == Label::In;

        never_conflicted.observe(v.outcome != Outcome::Conflicted, mask);
        never_unsatisfied.observe(v.outcome != Outcome::Unsatisfied, mask);
        never_faulted.observe(v.outcome != Outcome::Faulted, mask);
        continue_excludes_fault.observe(!(continue_in && any_diagnosis_in), mask);
        shutdown_needs_seizure.observe(!shutdown_in || bearing_in, mask);
        maintenance_needs_fault.observe(!maintenance_in || any_diagnosis_in, mask);
        within_wcet.observe(v.steps <= pump::PumpSolver::step_bound(fw.argument_count(), fw.attack_count()), mask);
        fault_forces_response.observe(!(any_diagnosis_in && continue_in), mask);

        bool action_declared = false;
        for (std::int32_t i = 0; i < policy.action_count(); ++i) {
            if (policy.action(i) == v.action) { action_declared = true; }
        }
        action_always_valid.observe(action_declared, mask);

        // ── the incoherence defeater ────────────────────────────────────────
        const Label incoherent = g.label(Arg{pump::def_current_incoherent});
        const bool raw_b_in = g.label(Arg{pump::obs_current_broadband_raw}) == Label::In;
        const bool raw_s_in = g.label(Arg{pump::obs_current_sidebands_raw}) == Label::In;
        const bool claim_b_in = g.label(Arg{pump::claim_current_broadband}) == Label::In;
        const bool claim_s_in = g.label(Arg{pump::claim_current_sidebands}) == Label::In;

        incoherent_iff_both.observe((incoherent == Label::In) == (raw_b_in && raw_s_in), mask);
        incoherent_kills_claims.observe(incoherent != Label::In || (!claim_b_in && !claim_s_in), mask);
        claim_needs_raw.observe((!claim_b_in || raw_b_in) && (!claim_s_in || raw_s_in), mask);
        incoherent_never_undecided.observe(incoherent != Label::Undecided, mask);

        if (v.outcome == Outcome::Indeterminate && indeterminate_example_count < 3) {
            indeterminate_examples[static_cast<std::size_t>(indeterminate_example_count)] = mask;
            ++indeterminate_example_count;
        }

        if (v.outcome == Outcome::Indeterminate) {
            const std::uint32_t present_current = mask & current_bits;
            if (present_current == 0U) {
                ++indet_no_current;

                // The experiment: does asserting a current signature resolve this state?
                Outcome with_broadband = Outcome::Faulted;
                Outcome with_sidebands = Outcome::Faulted;
                const bool ok_b = classify(mask | bit_broadband, with_broadband);
                const bool ok_s = classify(mask | bit_sidebands, with_sidebands);

                const bool b_resolves = ok_b && with_broadband == Outcome::Decided;
                const bool s_resolves = ok_s && with_sidebands == Outcome::Decided;
                if (b_resolves) { ++resolved_by_broadband; }
                if (s_resolves) { ++resolved_by_sidebands; }
                if (b_resolves || s_resolves) {
                    ++resolved_by_either;
                } else {
                    if (unresolved_count < 8) {
                        unresolved[static_cast<std::size_t>(unresolved_count)] = mask;
                    }
                    ++unresolved_count;
                }
            } else if (present_current == current_bits) {
                ++indet_both_current;
            } else {
                ++indet_one_current;
            }
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    std::printf("outcome distribution over all %u states\n", total);
    for (std::int32_t i = 0; i < 5; ++i) {
        const auto o = static_cast<Outcome>(i);
        const std::int32_t c = outcome_counts[static_cast<std::size_t>(i)];
        std::printf("  %-14s %5d   %5.1f%%\n",
                    outcome_name(o), c, 100.0 * static_cast<double>(c) / static_cast<double>(total));
    }

    std::printf("\nworst-case work: %d steps, at state", max_steps);
    print_mask(max_steps_mask);
    std::printf("\n");

    if (indeterminate_example_count > 0) {
        std::printf("\nexample ambiguous states (arbiter falls back to the safe state):\n");
        for (std::int32_t i = 0; i < indeterminate_example_count; ++i) {
            std::printf("  ");
            print_mask(indeterminate_examples[static_cast<std::size_t>(i)]);
            std::printf("\n");
        }
    }

    // ── The motor-current experiment, reported ───────────────────────────────
    const std::int32_t indet_total = outcome_counts[static_cast<std::size_t>(Outcome::Indeterminate)];
    std::printf("\nmotor-current discriminator\n");
    std::printf("  ambiguous states total                    %5d\n", indet_total);
    std::printf("    with no current evidence                %5d\n", indet_no_current);
    std::printf("    with exactly one current signature      %5d\n", indet_one_current);
    std::printf("    with both signatures (self-contradictory) %3d\n", indet_both_current);
    if (indet_no_current > 0) {
        std::printf("  of the %d with no current evidence:\n", indet_no_current);
        std::printf("    resolved by broadband modulation        %5d\n", resolved_by_broadband);
        std::printf("    resolved by blade-pass sidebands        %5d\n", resolved_by_sidebands);
        std::printf("    resolved by either                      %5d   %5.1f%%\n",
                    resolved_by_either,
                    100.0 * static_cast<double>(resolved_by_either) / static_cast<double>(indet_no_current));
        if (unresolved_count > 0) {
            std::printf("  %d states the current channel CANNOT resolve:\n", unresolved_count);
            const std::int32_t shown = unresolved_count < 8 ? unresolved_count : 8;
            for (std::int32_t i = 0; i < shown; ++i) {
                std::printf("   ");
                print_mask(unresolved[static_cast<std::size_t>(i)]);
                std::printf("\n");
            }
        }
    }

    Invariant* all[] = {
        &never_conflicted, &never_unsatisfied, &never_faulted,
        &continue_excludes_fault, &shutdown_needs_seizure, &maintenance_needs_fault,
        &within_wcet, &action_always_valid, &fault_forces_response,
        &incoherent_iff_both, &incoherent_kills_claims, &claim_needs_raw,
        &incoherent_never_undecided,
    };

    std::printf("\ninvariants\n");
    std::int32_t failed = 0;
    for (Invariant* inv : all) {
        if (inv->violations == 0) {
            std::printf("  OK    %s\n", inv->name);
        } else {
            ++failed;
            std::printf("  FAIL  %s  -- %d states, e.g.", inv->name, inv->violations);
            print_mask(inv->first_violation);
            std::printf("\n");
        }
    }

    std::printf("\n%s\n", failed == 0 ? "casebook validated over its entire input space"
                                      : "CASEBOOK DEFECTIVE");
    return failed == 0 ? 0 : 1;
}
