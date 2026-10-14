// Tank 409: scenarios, exhaustive validation, and the witness for the accident state.
//
// The scenario that matters is #3. It reproduces the sensor state at CAPECO on 23 October 2009 -- transmitter
// flatlined, fill time calculated by hand, transfer in progress, nobody sent to read the local gauge -- and asks
// what the arbiter would have done. The ground truth is not a matter of opinion: continuing was wrong, and we know
// because the tank exploded.

#include <cstdint>
#include <cstdio>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/witness/derivation.hpp"
#include "quorum/witness/render.hpp"
#include "tank_casebook.hpp"

namespace tank = quorum::casebook::tank;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;
using quorum::witness::Derivation;

namespace {

using TankDerivation = Derivation<192, tank::kMaxArgs>;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("    FAIL  %s\n", what);
    }
}

const char* lookup(std::int32_t i) { return tank::name_of(i); }

const char* label_text(Label l) {
    switch (l) {
        case Label::In:        return "IN ";
        case Label::Out:       return "out";
        case Label::Undecided: return " ? ";
    }
    return "???";
}

struct Expect {
    std::int32_t arg;
    Label label;
};

void scenario(const char* title,
              const char* narrative,
              const std::int32_t* present,
              std::int32_t count,
              Outcome want_outcome,
              std::int32_t want_action,
              const Expect* expectations,
              std::int32_t expectation_count) {
    tank::TankFramework fw;
    tank::TankSolver g;
    tank::TankPolicy policy;

    std::printf("\n─── %s\n    %s\n", title, narrative);

    if (tank::make_policy(policy) != Status::Ok || tank::instantiate(fw, tank::mask_of(present, count)) != Status::Ok) {
        std::printf("    BUILD FAILED\n");
        ++g_failures;
        return;
    }

    const Verdict v = quorum::arbiter::decide(fw, g, policy);

    std::printf("    %-13s -> %s%s\n",
                outcome_name(v.outcome),
                tank::name_of(v.action.get()),
                v.is_fallback ? "   (fallback)" : "");
    std::printf("    level:  safe=%s high=%s overfilling=%s\n",
                label_text(g.label(Arg{tank::claim_level_safe})),
                label_text(g.label(Arg{tank::claim_level_high})),
                label_text(g.label(Arg{tank::claim_overfilling})));
    std::printf("    trust:  xmtr_untrusted=%s  no_independent_measure=%s  unmeasured_filling=%s\n",
                label_text(g.label(Arg{tank::def_xmtr_untrusted})),
                label_text(g.label(Arg{tank::def_no_independent_measure})),
                label_text(g.label(Arg{tank::hazard_unmeasured_filling})));

    if (v.outcome != want_outcome) {
        ++g_failures;
        std::printf("    FAIL  outcome expected %s got %s\n", outcome_name(want_outcome), outcome_name(v.outcome));
    }
    if (v.action != Arg{want_action}) {
        ++g_failures;
        std::printf("    FAIL  action expected %s got %s\n",
                    tank::name_of(want_action), tank::name_of(v.action.get()));
    }
    for (std::int32_t i = 0; i < expectation_count; ++i) {
        if (g.label(Arg{expectations[i].arg}) != expectations[i].label) {
            ++g_failures;
            std::printf("    FAIL  %s expected %s got %s\n",
                        tank::name_of(expectations[i].arg),
                        label_text(expectations[i].label),
                        label_text(g.label(Arg{expectations[i].arg})));
        }
    }
}

}  // namespace

int main() {
    std::printf("Tank 409 casebook (CAPECO, 23 Oct 2009) -- %d arguments, %d attacks\n",
                static_cast<int>(tank::argument_count), static_cast<int>(tank::kCasebookEdges));

    // ── Scenarios ───────────────────────────────────────────────────────────
    {
        const std::int32_t obs[] = {tank::obs_xmtr_says_normal};
        const Expect exp[] = {{tank::claim_level_safe, Label::In},
                              {tank::def_no_independent_measure, Label::Out}};
        scenario("1. Idle tank, working transmitter",
                 "Transmitter reports normal, no transfer under way.",
                 obs, 1, Outcome::Decided, tank::act_monitor, exp, 2);
    }

    {
        const std::int32_t obs[] = {tank::obs_xmtr_says_normal, tank::obs_inlet_flow,
                                   tank::obs_fill_calc_says_room};
        const Expect exp[] = {{tank::def_no_independent_measure, Label::Out},
                              {tank::hazard_unmeasured_filling, Label::Out},
                              {tank::claim_level_safe, Label::In}};
        scenario("2. Normal transfer",
                 "Transfer in progress, transmitter healthy and reading normal. This is what should happen.",
                 obs, 3, Outcome::Decided, tank::act_continue_transfer, exp, 3);
    }

    {
        // THE ACCIDENT STATE.
        const std::int32_t obs[] = {tank::obs_xmtr_flatline, tank::obs_fill_calc_says_room,
                                   tank::obs_inlet_flow};
        const Expect exp[] = {{tank::def_xmtr_untrusted, Label::In},
                              {tank::def_no_independent_measure, Label::In},
                              {tank::dev_more_level, Label::In},
                              {tank::hazard_unmeasured_filling, Label::In},
                              {tank::claim_level_safe, Label::Out},
                              {tank::act_continue_transfer, Label::Out}};
        scenario("3. THE CAPECO STATE",
                 "Transmitter flatlined, fill time calculated by hand, transfer running, nobody sent to read the "
                 "local gauge. CAPECO continued the transfer. The arbiter must not.",
                 obs, 3, Outcome::Decided, tank::act_stop_transfer, exp, 6);
    }

    {
        const std::int32_t obs[] = {tank::obs_xmtr_flatline, tank::obs_fill_calc_says_room,
                                   tank::obs_inlet_flow, tank::obs_gauge_read_normal};
        const Expect exp[] = {{tank::def_xmtr_untrusted, Label::In},
                              {tank::def_no_independent_measure, Label::Out},
                              {tank::hazard_unmeasured_filling, Label::Out},
                              {tank::claim_level_safe, Label::In}};
        scenario("4. Same, but somebody walked out and read the gauge",
                 "One independent measurement is the whole difference. This is the recovery CAPECO never made.",
                 obs, 4, Outcome::Decided, tank::act_continue_transfer, exp, 4);
    }

    {
        const std::int32_t obs[] = {tank::obs_xmtr_flatline, tank::obs_inlet_flow,
                                   tank::obs_gas_detected, tank::obs_dike_liquid};
        const Expect exp[] = {{tank::claim_overfilling, Label::In},
                              {tank::act_continue_transfer, Label::Out}};
        scenario("5. Too late",
                 "Vapour in the tank farm and liquid in the dike. These are consequence signals: by the time they "
                 "fire, the release has already happened.",
                 obs, 4, Outcome::Decided, tank::act_stop_and_evacuate, exp, 2);
    }

    {
        const std::int32_t obs[] = {tank::obs_xmtr_says_normal, tank::obs_gauge_read_high,
                                   tank::obs_inlet_flow};
        const Expect exp[] = {{tank::claim_level_high, Label::In},
                              {tank::claim_level_safe, Label::Out},
                              {tank::def_no_independent_measure, Label::Out}};
        scenario("6. Sources disagree",
                 "Transmitter says normal, the human at the gauge says high. The high reading wins -- a measurement "
                 "of danger is not cancelled by a measurement of safety.",
                 obs, 3, Outcome::Decided, tank::act_stop_transfer, exp, 3);
    }

    // ── Exhaustive validation ───────────────────────────────────────────────
    std::printf("\n══ exhaustive validation\n");
    const std::uint32_t total = 1U << static_cast<std::uint32_t>(tank::kObservationCount);

    tank::TankPolicy policy;
    if (tank::make_policy(policy) != Status::Ok) { std::printf("policy failed\n"); return 1; }

    std::int32_t outcome_counts[5] = {0, 0, 0, 0, 0};
    std::int32_t v_csb = 0;            // the CSB finding, as an invariant
    std::int32_t v_conflict = 0;
    std::int32_t v_unsat = 0;
    std::int32_t v_faulted = 0;
    std::int32_t v_hazard_forces_stop = 0;
    std::int32_t v_safe_needs_measure = 0;
    std::int32_t v_estimate_never_authorises = 0;
    std::int32_t v_wcet = 0;
    std::int32_t max_steps = 0;
    std::uint32_t csb_example = 0;

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        tank::TankFramework fw;
        tank::TankSolver g;
        if (tank::instantiate(fw, mask) != Status::Ok) { std::printf("build failed at %u\n", mask); return 1; }

        const Verdict v = quorum::arbiter::decide(fw, g, policy);
        ++outcome_counts[static_cast<std::size_t>(v.outcome)];
        if (v.steps > max_steps) { max_steps = v.steps; }

        const bool continuing = g.label(Arg{tank::act_continue_transfer}) == Label::In;
        const bool no_measure = g.label(Arg{tank::def_no_independent_measure}) == Label::In;
        const bool hazard = g.label(Arg{tank::hazard_unmeasured_filling}) == Label::In;
        const bool stopping = g.label(Arg{tank::act_stop_transfer}) == Label::In;
        const bool evacuating = g.label(Arg{tank::act_stop_and_evacuate}) == Label::In;
        const bool safe = g.label(Arg{tank::claim_level_safe}) == Label::In;

        // THE CSB FINDING: never continue filling without an independent measurement.
        if (continuing && no_measure) {
            ++v_csb;
            if (csb_example == 0) { csb_example = mask; }
        }
        // The named hazard must always force a protective response.
        if (hazard && !(stopping || evacuating)) { ++v_hazard_forces_stop; }
        // You may not claim safety you cannot measure.
        if (safe && no_measure) { ++v_safe_needs_measure; }
        // An estimate must never be the thing that authorises filling: if the ONLY level input is the manual
        // calculation, continuing must not be the verdict.
        {
            const bool calc_only = ((mask >> static_cast<std::uint32_t>(tank::obs_fill_calc_says_room)) & 1U) != 0U
                                   && no_measure;
            if (calc_only && continuing) { ++v_estimate_never_authorises; }
        }
        if (v.outcome == Outcome::Conflicted) { ++v_conflict; }
        if (v.outcome == Outcome::Unsatisfied) { ++v_unsat; }
        if (v.outcome == Outcome::Faulted) { ++v_faulted; }
        if (v.steps > tank::TankSolver::step_bound(fw.argument_count(), fw.attack_count())) { ++v_wcet; }
    }

    std::printf("   %u states\n", total);
    for (std::int32_t i = 0; i < 5; ++i) {
        std::printf("   %-14s %5d   %5.1f%%\n",
                    outcome_name(static_cast<Outcome>(i)),
                    outcome_counts[static_cast<std::size_t>(i)],
                    100.0 * static_cast<double>(outcome_counts[static_cast<std::size_t>(i)]) /
                        static_cast<double>(total));
    }
    std::printf("   worst-case work %d steps\n\n", max_steps);

    check(v_csb == 0, "CSB FINDING: transfer never continues without an independent level measurement");
    check(v_hazard_forces_stop == 0, "unmeasured filling always forces a stop (alone or with evacuation)");
    check(v_safe_needs_measure == 0, "claim_level_safe never holds without a measurement");
    check(v_estimate_never_authorises == 0, "a manual estimate alone never authorises continued filling");
    check(v_conflict == 0, "no state selects two actions");
    check(v_unsat == 0, "every state selects an action");
    check(v_faulted == 0, "the solver completes in every state");
    check(v_wcet == 0, "steps stays inside 2n + 2m");
    if (v_csb != 0) {
        std::printf("   CSB violation example: mask %u\n", csb_example);
    }

    // ── Witness for the accident state ──────────────────────────────────────
    {
        std::printf("\n══ witness: why the arbiter would have stopped the CAPECO transfer\n\n");
        tank::TankFramework fw;
        tank::TankSolver g;
        TankDerivation d;
        const std::int32_t obs[] = {tank::obs_xmtr_flatline, tank::obs_fill_calc_says_room, tank::obs_inlet_flow};
        if (tank::instantiate(fw, tank::mask_of(obs, 3)) != Status::Ok) { return 1; }
        const Verdict v = quorum::arbiter::decide(fw, g, policy);
        check(v.action == Arg{tank::act_stop_transfer}, "witness: stops the transfer");

        check(d.build(fw, g, Arg{tank::act_stop_transfer}, Arg{tank::cw_closed_world}) == Status::Ok,
              "witness: derivation builds");
        quorum::witness::render(d, lookup);

        check(d.rests_on_assumption(),
              "witness: the verdict must be flagged as resting on assumed absence -- nobody read the gauge");
    }

    std::printf("\n%s  (%d failures)\n", g_failures == 0 ? "TANK CASEBOOK VALIDATED" : "TANK CASEBOOK FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
