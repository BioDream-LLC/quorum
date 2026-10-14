// Witnesses for the centrifugal pump casebook.
//
// This is the artifact a safety manager signs. Everything before it produced a labelling; a labelling is not an
// explanation, and until this file existed the project's claim to be "auditable" was a claim rather than a
// deliverable.
//
// The assertions at the bottom are the ones that matter. In particular: a "no fault found" verdict MUST be flagged
// as resting on assumed absence, because a quiet sensor and a dead sensor are indistinguishable to the framework.

#include <cstdint>
#include <cstdio>

#include "pump_casebook.hpp"
#include "quorum/arbiter/verdict.hpp"
#include "quorum/witness/derivation.hpp"
#include "quorum/witness/render.hpp"

namespace pump = quorum::casebook::pump;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;
using quorum::witness::Derivation;
using quorum::witness::Ground;

namespace {

using PumpDerivation = Derivation<192, pump::kMaxArgs>;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("    FAIL  %s\n", what);
    }
}

const char* lookup(std::int32_t i) { return pump::name_of(i); }

/// Solve, decide, and write the witness for the action that was taken.
struct Session {
    pump::PumpFramework fw;
    pump::PumpSolver solver;
    pump::PumpPolicy policy;
    PumpDerivation derivation;
    Verdict verdict;

    [[nodiscard]] bool open(const std::int32_t* obs, std::int32_t count) {
        if (pump::make_policy(policy) != Status::Ok) { return false; }
        if (pump::instantiate(fw, pump::mask_of(obs, count)) != Status::Ok) { return false; }
        verdict = quorum::arbiter::decide(fw, solver, policy);
        return true;
    }

    [[nodiscard]] bool explain(std::int32_t subject) {
        return derivation.build(fw, solver, Arg{subject}, Arg{pump::cw_closed_world}) == Status::Ok;
    }
};

void banner(const char* title, const Verdict& v) {
    std::printf("\n══ %s\n   outcome: %s -> %s%s\n\n",
                title,
                outcome_name(v.outcome),
                pump::name_of(v.action.get()),
                v.is_fallback ? "  (fallback, the framework did not choose)" : "");
}

}  // namespace

int main() {
    std::printf("pump casebook witnesses\n");

    // ── 1. The dangerous case: a clean bill of health ────────────────────────
    // Nothing is wrong, and the reason nothing is wrong is that no sensor said anything. That is precisely the
    // derivation the Kotlin line warns about -- complete, correct-looking, and resting on an assumption.
    {
        const std::int32_t obs[] = {0};
        Session s;
        check(s.open(obs, 0), "quiet: session opens");
        banner("A quiet pump is told to continue -- but on what basis?", s.verdict);
        check(s.explain(pump::act_continue), "quiet: derivation builds");
        quorum::witness::render(s.derivation, lookup);

        check(s.verdict.outcome == Outcome::Decided, "quiet: decided");
        check(s.verdict.action == Arg{pump::act_continue}, "quiet: continue");
        check(s.derivation.rests_on_assumption(),
              "quiet: a clean bill of health MUST be flagged as resting on assumed absence");
        check(!s.derivation.unconditional(), "quiet: not unconditional");
        check(!s.derivation.truncated(), "quiet: not truncated");
    }

    // ── 2. A real fault, explained ──────────────────────────────────────────
    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_bearing_temp_high};
        Session s;
        check(s.open(obs, 2), "bearing: session opens");
        banner("Bearing wear forces a shutdown", s.verdict);
        check(s.explain(pump::dx_bearing_wear), "bearing: derivation builds");
        quorum::witness::render(s.derivation, lookup);

        check(s.verdict.action == Arg{pump::act_shutdown_now}, "bearing: shutdown");
        check(s.solver.label(Arg{pump::dx_bearing_wear}) == Label::In, "bearing: diagnosis holds");
        check(s.derivation.depth() >= 2, "bearing: the reasoning is multi-step");
    }

    // ── 3. Reinstatement, which is the property that justifies the whole approach ──
    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_duty_changed,
                                   pump::obs_suction_press_low, pump::obs_head_low,
                                   pump::obs_noise_crackling};
        Session s;
        check(s.open(obs, 5), "reinstate: session opens");
        banner("The duty-change excuse is overruled and cavitation is reinstated", s.verdict);
        check(s.explain(pump::dx_cavitation), "reinstate: derivation builds");
        quorum::witness::render(s.derivation, lookup);

        check(s.solver.label(Arg{pump::dx_cavitation}) == Label::In, "reinstate: cavitation holds");
        check(s.solver.label(Arg{pump::benign_duty_change}) == Label::Out, "reinstate: excuse defeated");

        // The witness must contain the defeat of the benign explanation -- that is the whole argument.
        bool names_the_excuse = false;
        for (std::int32_t i = 0; i < s.derivation.step_count(); ++i) {
            if (s.derivation.step(i).subject == Arg{pump::benign_duty_change}) { names_the_excuse = true; }
        }
        check(names_the_excuse, "reinstate: the witness names the defeated benign explanation");
    }

    // ── 4. Refusal, and why ─────────────────────────────────────────────────
    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_suction_press_low,
                                   pump::obs_head_low, pump::obs_vib_bpf_harmonics,
                                   pump::obs_current_broadband_raw, pump::obs_current_sidebands_raw};
        Session s;
        check(s.open(obs, 6), "incoherent: session opens");
        banner("Contradictory current data -- the arbiter refuses and stops the pump", s.verdict);
        check(s.explain(pump::dx_cavitation), "incoherent: derivation builds");
        quorum::witness::render(s.derivation, lookup);

        check(s.verdict.outcome == Outcome::Indeterminate, "incoherent: indeterminate");
        check(s.verdict.is_fallback, "incoherent: fell back");
        check(s.solver.label(Arg{pump::def_current_incoherent}) == Label::In,
              "incoherent: the incoherence defeater fired");

        // A refusal must say what is contested, or it is just a shrug.
        bool shows_contest = false;
        for (std::int32_t i = 0; i < s.derivation.step_count(); ++i) {
            if (s.derivation.step(i).ground == Ground::Contested) { shows_contest = true; }
        }
        check(shows_contest, "incoherent: the witness shows the contested arguments");

        std::printf("   and separately, why the current channel was discarded:\n\n");
        check(s.explain(pump::claim_current_broadband), "incoherent: claim derivation builds");
        quorum::witness::render(s.derivation, lookup);
        check(s.solver.label(Arg{pump::claim_current_broadband}) == Label::Out,
              "incoherent: the broadband claim is decisively Out, not merely unknown");
    }

    // ── 5. Structural properties of the walker ──────────────────────────────
    {
        const std::int32_t obs[] = {pump::obs_vib_high, pump::obs_bearing_temp_high};
        Session s;
        check(s.open(obs, 2), "props: session opens");

        // Every argument must be explainable, and no walk may fail to terminate or overflow.
        std::int32_t worst_steps = 0;
        std::int32_t worst_depth = 0;
        bool all_ok = true;
        bool any_truncated = false;
        for (std::int32_t a = 0; a < pump::argument_count; ++a) {
            if (!s.explain(a)) { all_ok = false; break; }
            if (s.derivation.step_count() > worst_steps) { worst_steps = s.derivation.step_count(); }
            if (s.derivation.depth() > worst_depth) { worst_depth = s.derivation.depth(); }
            if (s.derivation.truncated()) { any_truncated = true; }
            // No argument may appear twice as an expanded step; repeats must be AlreadyShown.
            for (std::int32_t i = 0; i < s.derivation.step_count(); ++i) {
                for (std::int32_t j = i + 1; j < s.derivation.step_count(); ++j) {
                    if (s.derivation.step(i).subject == s.derivation.step(j).subject &&
                        s.derivation.step(j).ground != Ground::AlreadyShown) {
                        all_ok = false;
                    }
                }
            }
        }
        std::printf("\n══ Walker properties\n");
        std::printf("   every one of %d arguments explained; worst case %d steps, depth %d\n",
                    static_cast<int>(pump::argument_count), worst_steps, worst_depth);
        check(all_ok, "props: every argument explains, and nothing is expanded twice");
        check(!any_truncated, "props: 192 steps is enough for this casebook");
    }

    std::printf("\n%s  (%d failures)\n", g_failures == 0 ? "all witness checks passed" : "WITNESS CHECKS FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
