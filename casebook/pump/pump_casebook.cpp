// Scenario walkthrough for the centrifugal pump casebook.
//
// Seven hand-picked sensor states, each asserting the verdict a reliability engineer would give. Registered as a
// ctest, so a solver or casebook change that alters an engineering conclusion fails the build.
//
// The exhaustive validator next door checks all 4,096 states. This file exists for the ones a human should be
// able to read and agree with.

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

int g_failures = 0;

const char* label_text(Label l) {
    switch (l) {
        case Label::In:        return "IN ";
        case Label::Out:       return "out";
        case Label::Undecided: return " ? ";
    }
    return "???";
}

void expect_label(const pump::PumpSolver& g, std::int32_t a, Label want, const char* scenario) {
    const Label got = g.label(Arg{a});
    if (got != want) {
        ++g_failures;
        std::printf("    MISMATCH  %-28s expected %s got %s   [%s]\n",
                    pump::name_of(a), label_text(want), label_text(got), scenario);
    }
}

void expect_outcome(const Verdict& v, Outcome want, const char* scenario) {
    if (v.outcome != want) {
        ++g_failures;
        std::printf("    MISMATCH  outcome expected %s got %s   [%s]\n",
                    outcome_name(want), outcome_name(v.outcome), scenario);
    }
}

void expect_action(const Verdict& v, std::int32_t want, const char* scenario) {
    if (v.action != Arg{want}) {
        ++g_failures;
        std::printf("    MISMATCH  action expected %s got %s   [%s]\n",
                    pump::name_of(want), pump::name_of(v.action.get()), scenario);
    }
}

struct Expect {
    std::int32_t arg;
    Label label;
};

void run(const char* title,
         const char* narrative,
         const std::int32_t* present,
         std::int32_t present_count,
         Outcome want_outcome,
         std::int32_t want_action,
         const Expect* expectations,
         std::int32_t expectation_count) {
    pump::PumpFramework fw;
    pump::PumpSolver g;
    pump::PumpPolicy policy;

    std::printf("\n─── %s\n    %s\n", title, narrative);

    if (pump::make_policy(policy) != Status::Ok) {
        std::printf("    POLICY BUILD FAILED\n");
        ++g_failures;
        return;
    }
    if (pump::instantiate(fw, pump::mask_of(present, present_count)) != Status::Ok) {
        std::printf("    FRAMEWORK BUILD FAILED\n");
        ++g_failures;
        return;
    }

    const Verdict v = quorum::arbiter::decide(fw, g, policy);

    std::printf("    %-13s -> %s%s\n",
                outcome_name(v.outcome),
                pump::name_of(v.action.get()),
                v.is_fallback ? "   (fallback)" : "");
    std::printf("    diagnoses:");
    for (std::int32_t a = pump::dx_cavitation; a < pump::diagnosis_end; ++a) {
        std::printf("  %s=%s", pump::name_of(a) + 3, label_text(g.label(Arg{a})));
    }
    std::printf("\n    benign:   ");
    for (std::int32_t a = pump::benign_duty_change; a < pump::benign_end; ++a) {
        std::printf("  %s=%s", pump::name_of(a) + 7, label_text(g.label(Arg{a})));
    }
    std::printf("\n    undecided %d,  steps %d of bound %d\n",
                v.undecided,
                v.steps,
                pump::PumpSolver::step_bound(fw.argument_count(), fw.attack_count()));

    expect_outcome(v, want_outcome, title);
    expect_action(v, want_action, title);
    for (std::int32_t i = 0; i < expectation_count; ++i) {
        expect_label(g, expectations[i].arg, expectations[i].label, title);
    }
}

}  // namespace

int main() {
    std::printf("centrifugal pump casebook -- %d arguments, %d fixed attacks\n",
                static_cast<int>(pump::argument_count),
                static_cast<int>(pump::kCasebookEdges));

    {
        const std::int32_t obs[] = {0};
        const Expect exp[] = {
            {pump::act_continue, Label::In},
            {pump::act_schedule_maintenance, Label::Out},
            {pump::act_shutdown_now, Label::Out},
            {pump::dx_cavitation, Label::Out},
            {pump::dx_bearing_wear, Label::Out},
        };
        run("A. Quiet pump",
            "No sensor asserts anything. The closed-world argument puts every observation Out.",
            obs, 0, Outcome::Decided, pump::act_continue, exp, 5);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_suction_press_low,
                                   pump::obs_head_low, pump::obs_noise_crackling};
        const Expect exp[] = {
            {pump::dx_cavitation, Label::In},
            {pump::dx_impeller_defect, Label::Out},
            {pump::benign_duty_change, Label::Out},
        };
        run("B. Textbook cavitation",
            "Vibration, eroded NPSH margin, head below curve, audible crackling.",
            obs, 4, Outcome::Decided, pump::act_schedule_maintenance, exp, 3);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_duty_changed};
        const Expect exp[] = {
            {pump::benign_duty_change, Label::In},
            {pump::dx_cavitation, Label::Out},
        };
        run("C. Nuisance alarm -- benign duty change",
            "Vibration fired, but the pump changed duty point. This is the 40-90% case.",
            obs, 2, Outcome::Decided, pump::act_continue, exp, 2);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_duty_changed,
                                   pump::obs_suction_press_low, pump::obs_head_low,
                                   pump::obs_noise_crackling};
        const Expect exp[] = {
            {pump::benign_duty_change, Label::Out},
            {pump::dx_cavitation, Label::In},
        };
        run("D. Duty change, but NPSH says otherwise",
            "Same duty change plus low suction pressure and head loss. The benign story is defeated and the "
            "fault is reinstated -- the property that makes grounded semantics worth having.",
            obs, 5, Outcome::Decided, pump::act_schedule_maintenance, exp, 2);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_bearing_temp_high};
        const Expect exp[] = {
            {pump::dx_bearing_wear, Label::In},
            {pump::benign_resonance, Label::Out},
        };
        run("E. Bearing wear with seizure risk",
            "Vibration and a hot bearing. Temperature also kills the resonance excuse.",
            obs, 2, Outcome::Decided, pump::act_shutdown_now, exp, 2);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_bearing_temp_high,
                                   pump::obs_calib_overdue};
        const Expect exp[] = {
            {pump::def_calib_stale, Label::In},
            {pump::obs_vib_high, Label::Out},
            {pump::obs_bearing_temp_high, Label::Out},
            {pump::dx_bearing_wear, Label::Out},
        };
        run("F. Untrustworthy instrument",
            "Vibration and a hot bearing again, but calibration is overdue. Both readings are undermined, so no "
            "diagnosis is drawn. Refusing on bad data is the point.",
            obs, 3, Outcome::Decided, pump::act_continue, exp, 4);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_suction_press_low,
                                   pump::obs_head_low, pump::obs_vib_bpf_harmonics};
        const Expect exp[] = {
            {pump::dx_cavitation, Label::Undecided},
            {pump::dx_impeller_defect, Label::Undecided},
            {pump::def_current_incoherent, Label::Out},
        };
        run("G. Genuinely ambiguous -- head loss with no discriminator",
            "Cavitation and impeller wear both explain the evidence and nothing separates them. No current data "
            "at all, so the incoherence defeater stays Out.",
            obs, 4, Outcome::Indeterminate, pump::act_shutdown_now, exp, 3);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_suction_press_low,
                                   pump::obs_head_low, pump::obs_vib_bpf_harmonics,
                                   pump::obs_current_broadband_raw};
        const Expect exp[] = {
            {pump::claim_current_broadband, Label::In},
            {pump::def_current_incoherent, Label::Out},
            {pump::dx_impeller_defect, Label::Out},
            {pump::dx_cavitation, Label::In},
        };
        run("H. Same evidence, plus motor current",
            "Broadband current modulation is the two-phase-flow signature. It defeats the impeller hypothesis and "
            "the deadlock breaks -- 24 of the 30 ambiguous states resolve this way.",
            obs, 5, Outcome::Decided, pump::act_schedule_maintenance, exp, 4);
    }

    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_suction_press_low,
                                   pump::obs_head_low, pump::obs_vib_bpf_harmonics,
                                   pump::obs_current_broadband_raw, pump::obs_current_sidebands_raw};
        const Expect exp[] = {
            {pump::def_current_incoherent, Label::In},
            {pump::claim_current_broadband, Label::Out},
            {pump::claim_current_sidebands, Label::Out},
            {pump::dx_cavitation, Label::Undecided},
            {pump::dx_impeller_defect, Label::Undecided},
        };
        run("I. Contradictory current data",
            "Both current signatures at once, which a real spectrum cannot show. The incoherence defeater fires, "
            "both claims go decisively Out, and the witness now says WHY discrimination failed -- distinguishing "
            "a broken channel from a silent one.",
            obs, 6, Outcome::Indeterminate, pump::act_shutdown_now, exp, 5);
    }

    std::printf("\n%s  (%d mismatches)\n",
                g_failures == 0 ? "all expectations met" : "EXPECTATIONS VIOLATED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
