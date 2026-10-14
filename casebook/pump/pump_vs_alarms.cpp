// Does quorum actually beat the alarm system it would replace?
//
// ── The claim nobody has tested ─────────────────────────────────────────────
//
// Everything verified so far is internal consistency: the engine agrees with CaDiCaL, the compiler reproduces a
// hand-written casebook, the palette does not interfere with itself. None of it addresses the commercial question, which
// is whether a referee adjudicating conflicting claims makes BETTER decisions than the thing already installed.
//
// What is already installed is prioritised alarm annunciation. Every plant has it: each measurement has a threshold, each
// threshold raises an alarm, each alarm carries a priority, and the operator acts on the highest one. It is simple,
// certifiable, and it works most of the time. Any argument for replacing it has to be quantitative, and it has to be made
// on the same inputs.
//
// So: implement the alarm system honestly, run both over all 16,384 states of the pump casebook, and count the
// disagreements. Then classify them, because a raw disagreement count proves nothing -- the interesting question is who is
// right when they differ.
//
// ── Implementing the baseline honestly ──────────────────────────────────────
//
// The temptation is to build a strawman. The alarm logic below is the real thing as a competent engineer would configure
// it from the same FMEA the casebook came from:
//
//   * bearing temperature high          -> shutdown  (highest priority: bearing failure wrecks the machine)
//   * vibration high                    -> maintain
//   * suction low (NPSH margin eroded)  -> maintain
//   * head low                          -> maintain
//   * otherwise                         -> continue
//
// That is not a caricature. It is what the thresholds in this FMEA support, it takes the most severe consequence
// seriously, and it is exactly the mapping a P&ID review would produce. What it CANNOT do is the thing an alarm system
// structurally cannot do: notice that the evidence for an alarm has been discredited.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "quorum/arbiter/verdict.hpp"

#include "pump_conditions_generated.hpp"
#include "pump_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace cond = quorum::casebook::gen::centrifugal_pump_conditions;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Status;

namespace {

/// Prioritised alarm annunciation: the baseline every plant already has.
///
/// Reads the same observation bits the casebook does, so neither side gets information the other lacks.
const char* alarm_system(std::uint32_t mask) {
    const auto on = [mask](std::int32_t bit) { return (mask & (1U << static_cast<std::uint32_t>(bit))) != 0U; };

    // Priority order, most severe first.
    if (on(pump::bearing_temp_high)) { return "shutdown"; }
    if (on(pump::vib_high)) { return "maintain"; }
    if (on(pump::suction_low)) { return "maintain"; }
    if (on(pump::head_low)) { return "maintain"; }
    return "continue";
}

/// Is the vibration evidence discredited in this state?
///
/// `mount_suspect` undermines the vibration readings; `calib_overdue` undermines vibration and temperature. An alarm
/// system has no representation for this: a threshold crossing is a threshold crossing.
bool evidence_discredited(std::uint32_t mask) {
    const auto on = [mask](std::int32_t bit) { return (mask & (1U << static_cast<std::uint32_t>(bit))) != 0U; };
    return on(pump::mount_suspect) || on(pump::calib_overdue);
}

/// Is there a benign explanation for the readings?
bool benign_explanation(std::uint32_t mask) {
    const auto on = [mask](std::int32_t bit) { return (mask & (1U << static_cast<std::uint32_t>(bit))) != 0U; };
    return on(pump::duty_changed) || on(pump::throttled) || on(pump::near_critical);
}

struct Bucket {
    std::int32_t count = 0;
    std::uint32_t example = 0;
    bool have = false;
};

void note(Bucket& b, std::uint32_t mask) {
    ++b.count;
    if (!b.have) {
        b.example = mask;
        b.have = true;
    }
}

void describe(std::uint32_t mask) {
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
    pump::CaseFramework fw;
    pump::CaseSolver solver;
    pump::CasePolicy policy;
    if (pump::make_policy(policy) != Status::Ok) {
        std::printf("FAIL: policy\n");
        return 1;
    }

    std::int32_t agree = 0;
    std::int32_t differ = 0;

    // The categories that matter. Each is a different argument about which system to install.
    Bucket quorum_less_alarmist;   // alarm acts, quorum does not, because the evidence is discredited
    Bucket quorum_explains;        // alarm acts, quorum does not, because a benign cause explains it
    Bucket quorum_more_cautious;   // quorum acts, alarm does not
    Bucket quorum_asks_human;      // quorum declines to decide; the alarm system always decides
    Bucket unexplained;            // differs for none of the above reasons -- the honest residual

    std::map<std::string, std::int32_t> alarm_actions;
    std::map<std::string, std::int32_t> quorum_actions;

    const std::int32_t states = 1 << pump::kObservationCount;
    for (std::int32_t s = 0; s < states; ++s) {
        const std::uint32_t mask = static_cast<std::uint32_t>(s);
        if (pump::instantiate(fw, mask) != Status::Ok) { continue; }
        const Verdict v = quorum::arbiter::decide(fw, solver, policy);

        const std::string a = alarm_system(mask);
        ++alarm_actions[a];

        std::string q;
        if (v.outcome == Outcome::Decided && v.action.valid()) {
            q = pump::name_of(v.action.get());
        } else {
            q = "ask-a-human";
        }
        ++quorum_actions[q];

        if (a == q) {
            ++agree;
            continue;
        }
        ++differ;

        const bool alarm_acts = a != "continue";
        const bool quorum_acts = q != "continue" && q != "ask-a-human";

        if (q == "ask-a-human") {
            note(quorum_asks_human, mask);
        } else if (alarm_acts && !quorum_acts && evidence_discredited(mask)) {
            note(quorum_less_alarmist, mask);
        } else if (alarm_acts && !quorum_acts && benign_explanation(mask)) {
            note(quorum_explains, mask);
        } else if (quorum_acts && !alarm_acts) {
            note(quorum_more_cautious, mask);
        } else {
            note(unexplained, mask);
        }
    }

    std::printf("quorum vs prioritised alarm annunciation, over all %d states\n", states);
    std::printf("============================================================\n\n");

    std::printf("action distribution\n");
    std::printf("  %-14s %8s %8s\n", "", "alarms", "quorum");
    for (const auto& [name, n] : alarm_actions) {
        const auto it = quorum_actions.find(name);
        std::printf("  %-14s %8d %8d\n", name.c_str(), n, it == quorum_actions.end() ? 0 : it->second);
    }
    if (quorum_actions.count("ask-a-human") != 0) {
        std::printf("  %-14s %8d %8d\n", "ask-a-human", 0, quorum_actions["ask-a-human"]);
    }
    std::printf("\n");

    std::printf("agreement        %6d   %5.1f%%\n", agree, 100.0 * agree / states);
    std::printf("disagreement     %6d   %5.1f%%\n\n", differ, 100.0 * differ / states);

    const auto show = [](const char* label, const Bucket& b, const char* why) {
        std::printf("  %-38s %6d\n", label, b.count);
        if (b.have) {
            std::printf("      %s\n      e.g. state 0x%X:", why, b.example);
            describe(b.example);
            std::printf("\n");
        }
    };

    std::printf("where they differ\n");
    show("alarm acts, quorum says evidence is bad", quorum_less_alarmist,
         "the instrument itself is in question, so the reading is not evidence of anything");
    show("alarm acts, quorum has a benign explanation", quorum_explains,
         "something innocuous produces the same reading");
    show("quorum acts, alarm does not", quorum_more_cautious,
         "a conclusion followed from a combination no single threshold sees");
    show("quorum declines to decide", quorum_asks_human,
         "two diagnoses fit equally and imply different actions");
    show("differs for none of the above", unexplained, "the residual, reported rather than hidden");

    std::printf("\n");
    std::printf("─────────────────────────────────────────────────────────────────────────────\n");

    // The honest reading. Every category is a DIFFERENT commercial argument, and two of them are arguments AGAINST.
    // ── the finding that mattered, and the fix ───────────────────────────────
    //
    // The residual bucket is what made this comparison worth building. It is every state where the two differ for none of
    // the flattering reasons, and inspecting it showed something simple and bad: pump.cb responds to NO single symptom.
    // Every claim in it is `needs A B`, because it was built for differential diagnosis, and that cost it the ability to
    // act on one unambiguous reading. A pump with a hot bearing and nothing else got `continue` while the alarm system
    // said `shutdown`.
    std::printf("single-symptom response, the two pump casebooks\n");
    {
        cond::CaseFramework cfw;
        cond::CaseSolver csolver;
        cond::CasePolicy cpolicy;
        if (cond::make_policy(cpolicy) != Status::Ok) {
            std::printf("  (conditions casebook unavailable)\n");
        } else {
            std::int32_t silent_v3 = 0;
            std::int32_t silent_v4 = 0;
            for (std::int32_t b = 0; b < pump::kObservationCount; ++b) {
                const std::uint32_t m = 1U << static_cast<std::uint32_t>(b);
                if (pump::instantiate(fw, m) == Status::Ok) {
                    const Verdict v = quorum::arbiter::decide(fw, solver, policy);
                    if (v.action.valid() && std::string(pump::name_of(v.action.get())) == "continue") { ++silent_v3; }
                }
                if (cond::instantiate(cfw, m) == Status::Ok) {
                    const Verdict v = quorum::arbiter::decide(cfw, csolver, cpolicy);
                    if (v.action.valid() && std::string(cond::name_of(v.action.get())) == "continue") { ++silent_v4; }
                }
            }
            std::printf("  pump.cb (diagnoses only)        %2d of %d single symptoms produce NO response\n", silent_v3,
                        pump::kObservationCount);
            std::printf("  pump_conditions.cb (+ 3 claims) %2d of %d\n", silent_v4, pump::kObservationCount);
            std::printf("  The three that changed -- hot bearing, eroded NPSH margin, audible cavitation -- are\n");
            std::printf("  CONDITIONS rather than diagnoses: damaging states of affairs rather than hypotheses about\n");
            std::printf("  a cause. The remaining silences are correct. A single spectral feature identifies no\n");
            std::printf("  fault, and `mount_suspect` is instrument health rather than machine health.\n\n");
        }
    }

    std::printf("What this does and does not show.\n\n");
    std::printf("FIRST, the %.1f%% disagreement figure is an artefact of enumerating states uniformly and must\n",
                100.0 * differ / states);
    std::printf("not be quoted on its own. `mount_suspect` or `calib_overdue` is asserted in three quarters of\n");
    std::printf("all states here, which is why the discredited-evidence bucket dominates. On a real plant a\n");
    std::printf("questioned accelerometer mount is rare. The bucket SIZES are properties of the enumeration;\n");
    std::printf("only the bucket KINDS are properties of the two systems.\n\n");
    std::printf("The %d states where quorum suppresses an alarm because the instrument is in question are\n",
                quorum_less_alarmist.count);
    std::printf("the strongest case for it: an alarm system cannot represent 'this reading is not\n");
    std::printf("evidence', so it annunciates anyway, and that is a documented source of alarm flood and\n");
    std::printf("of operators learning to ignore alarms.\n\n");
    std::printf("But %d states where quorum ACTS and the alarm system does not is the opposite argument:\n",
                quorum_more_cautious.count);
    std::printf("on a real plant those would be judged nuisance trips until someone proved otherwise, and\n");
    std::printf("each one has to be defended individually.\n\n");
    std::printf("And %d states where quorum declines to decide are a cost, not a feature, to anyone whose\n",
                quorum_asks_human.count);
    std::printf("alternative always produces an answer. The defence is that the answer it produces there is\n");
    std::printf("arbitrary -- but that is an argument, not a measurement.\n\n");
    std::printf("What is NOT shown: which system is right. Both are driven by the same synthetic state\n");
    std::printf("space, with no failure statistics and no field data. This measures the DIFFERENCE and\n");
    std::printf("characterises it. Establishing that the difference is an improvement needs a plant.\n");

    return 0;
}
