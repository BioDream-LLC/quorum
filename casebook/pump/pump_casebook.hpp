#pragma once

#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"

/// \file
/// The centrifugal pump casebook: one definition, shared by the scenario walkthrough and the exhaustive
/// validator.
///
/// HONEST SCOPE. The failure modes, signatures and confounders are drawn from published pump condition
/// monitoring literature, but this is NOT a real plant's FMEA. It tests the machinery -- flattening, gating,
/// whether verdicts read sensibly. It does not test the assumption, because the document is synthetic. Only a real
/// FMEA or HAZOP sourced from an operator does that.
///
/// WHAT THE LITERATURE GAVE US. Cavitation, bearing wear, impeller defect and misalignment all present as
/// vibration, which is why a single casing vibration channel classifies at roughly 89% while vibration combined
/// with motor current reaches about 97%. Separately, the dominant source of nuisance alarms is benign mode change
/// -- a pump changes duty cycle and the threshold fires -- plus baseline drift with age, load and season. Those
/// two facts are the whole reason an arbiter is interesting here: the evidence is genuinely ambiguous, and the
/// false positives have identifiable, writable causes.

namespace quorum::casebook::pump {

// ─────────────────────────────────────────────────────────────────────────────
// Arguments. An enum, so the source reads in engineering vocabulary while the framework sees indices. This is
// the shape the `Name <-> FileArg` type in ids.hpp needs to formalise; doing it by hand is how you discover you
// want it.
// ─────────────────────────────────────────────────────────────────────────────
enum Argument : std::int32_t {
    // Observations. The binding layer switches these on and off each cycle.
    obs_vib_high = 0,
    obs_vib_1x_dominant,        // 1x running speed dominant -> misalignment / unbalance signature
    obs_vib_bpf_harmonics,      // blade-pass-frequency harmonics -> impeller signature
    obs_bearing_temp_high,
    obs_suction_press_low,      // NPSH margin eroded -> cavitation precondition
    obs_head_low,               // developed head below curve
    obs_noise_crackling,        // audible signature of vapour bubble implosion
    obs_duty_changed,
    obs_throttled,
    obs_speed_near_critical,
    obs_mount_suspect,
    obs_calib_overdue,

    // Motor stator current channel. Added after exhaustive validation of the vibration-only casebook found 30
    // irreducibly ambiguous states. The literature predicts this: a single casing vibration channel classifies at
    // roughly 89%, vibration combined with motor current at roughly 97%.
    //
    // The discriminating physics is real, not a convenience. Cavitation is two-phase flow -- vapour bubbles make
    // the hydraulic load stochastic, so the current spectrum shows BROADBAND modulation. An impeller geometric
    // defect is deterministic and shaft-locked, so it shows DISCRETE sidebands at blade-pass frequency.
    //
    // These are the RAW sensor readings. They are deliberately not the arguments that discriminate diagnoses --
    // see `claim_current_*` and the note on `def_current_incoherent`.
    obs_current_broadband_raw,
    obs_current_sidebands_raw,
    observation_end,

    // Fault hypotheses. Presumptions: they stand unless defeated.
    dx_cavitation = observation_end,
    dx_bearing_wear,
    dx_impeller_defect,
    dx_misalignment,
    diagnosis_end,

    // Benign explanations. Also presumptions, competing with the fault hypotheses.
    benign_duty_change = diagnosis_end,
    benign_offdesign,
    benign_resonance,
    benign_end,

    // Defeaters on the evidence itself.
    def_mount_loose = benign_end,
    def_calib_stale,

    /// The current channel contradicts itself.
    ///
    /// Fires only when BOTH raw current signatures are present at once. Broadband modulation and discrete
    /// blade-locked sidebands are close to mutually exclusive in a real spectrum, so seeing both means the channel
    /// is untrustworthy -- a different maintenance action from seeing nothing.
    ///
    /// TWO THINGS THIS ARGUMENT COST, both worth recording.
    ///
    /// **Conjunction.** An AF attack is binary, so "fires only when A AND B" has no direct encoding. It does have
    /// an indirect one: give the argument one gate per conjunct, each gate defeated by its own observation. The
    /// argument then survives only when every gate is Out, which requires every observation In. So conjunction IS
    /// expressible, at one extra argument per conjunct. This is the n-ary problem the earlier notes flagged and
    /// dodged; here it is solved.
    ///
    /// **A latent odd cycle.** The obvious encoding has the defeater attack the observations that trigger it:
    /// `def_current_incoherent -> obs_raw -> gate -> def_current_incoherent` is a 3-cycle, and every argument in an
    /// odd cycle goes Undecided. The whole current channel would have silently become unknowable. The fix is to
    /// separate the RAW reading from the USABLE claim: raw readings trigger the incoherence check, claims carry the
    /// discrimination, and the defeater attacks only the claims. Acyclic, and the layering is now load-bearing
    /// rather than decorative.
    def_current_incoherent,

    // The usable current claims. These discriminate; the raw readings do not.
    claim_current_broadband,
    claim_current_sidebands,

    // Evidence gates. Closed-world plumbing; see `instantiate`.
    gate_no_vib,
    gate_no_1x,
    gate_no_bpf,
    gate_no_bearing_temp,
    gate_no_cav_evidence,
    gate_no_duty_evidence,
    gate_no_throttle_evidence,
    gate_no_resonance_evidence,
    gate_no_mount_suspicion,
    gate_no_calib_concern,
    gate_no_broadband_current,
    gate_no_sidebands_current,
    gate_no_fault,
    gate_no_seizure_risk,

    // Conclusions.
    act_continue,
    act_schedule_maintenance,
    act_shutdown_now,

    // Unattacked, therefore always In. What makes an absent observation decisively Out.
    cw_closed_world,

    argument_count
};

constexpr std::int32_t kObservationCount = observation_end;

[[nodiscard]] inline const char* name_of(std::int32_t a) noexcept {
    switch (a) {
        case obs_vib_high:               return "obs_vib_high";
        case obs_vib_1x_dominant:        return "obs_vib_1x_dominant";
        case obs_vib_bpf_harmonics:      return "obs_vib_bpf_harmonics";
        case obs_bearing_temp_high:      return "obs_bearing_temp_high";
        case obs_suction_press_low:      return "obs_suction_press_low";
        case obs_head_low:               return "obs_head_low";
        case obs_noise_crackling:        return "obs_noise_crackling";
        case obs_duty_changed:           return "obs_duty_changed";
        case obs_throttled:              return "obs_throttled";
        case obs_speed_near_critical:    return "obs_speed_near_critical";
        case obs_mount_suspect:          return "obs_mount_suspect";
        case obs_calib_overdue:          return "obs_calib_overdue";
        case obs_current_broadband_raw:  return "obs_current_broadband_raw";
        case obs_current_sidebands_raw:  return "obs_current_sidebands_raw";
        case def_current_incoherent:     return "def_current_incoherent";
        case claim_current_broadband:    return "claim_current_broadband";
        case claim_current_sidebands:    return "claim_current_sidebands";
        case gate_no_broadband_current:  return "gate_no_broadband_current";
        case gate_no_sidebands_current:  return "gate_no_sidebands_current";
        case dx_cavitation:              return "dx_cavitation";
        case dx_bearing_wear:            return "dx_bearing_wear";
        case dx_impeller_defect:         return "dx_impeller_defect";
        case dx_misalignment:            return "dx_misalignment";
        case benign_duty_change:         return "benign_duty_change";
        case benign_offdesign:           return "benign_offdesign";
        case benign_resonance:           return "benign_resonance";
        case def_mount_loose:            return "def_mount_loose";
        case def_calib_stale:            return "def_calib_stale";
        case gate_no_vib:                return "gate_no_vib";
        case gate_no_1x:                 return "gate_no_1x";
        case gate_no_bpf:                return "gate_no_bpf";
        case gate_no_bearing_temp:       return "gate_no_bearing_temp";
        case gate_no_cav_evidence:       return "gate_no_cav_evidence";
        case gate_no_duty_evidence:      return "gate_no_duty_evidence";
        case gate_no_throttle_evidence:  return "gate_no_throttle_evidence";
        case gate_no_resonance_evidence: return "gate_no_resonance_evidence";
        case gate_no_mount_suspicion:    return "gate_no_mount_suspicion";
        case gate_no_calib_concern:      return "gate_no_calib_concern";
        case gate_no_fault:              return "gate_no_fault";
        case gate_no_seizure_risk:       return "gate_no_seizure_risk";
        case act_continue:               return "act_continue";
        case act_schedule_maintenance:   return "act_schedule_maintenance";
        case act_shutdown_now:           return "act_shutdown_now";
        case cw_closed_world:            return "cw_closed_world";
        default:                         return "???";
    }
}

struct Edge {
    std::int32_t from;
    std::int32_t to;
};

// ─────────────────────────────────────────────────────────────────────────────
// The casebook: every attack relation, fixed across all cycles.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr Edge kCasebook[] = {
    // A bad sensor undermines the reading, not the diagnosis. Attacking the observation means one edge covers
    // every hypothesis resting on it -- fewer edges and the more faithful encoding.
    {def_mount_loose, obs_vib_high},
    {def_mount_loose, obs_vib_1x_dominant},
    {def_mount_loose, obs_vib_bpf_harmonics},
    {def_calib_stale, obs_vib_high},
    {def_calib_stale, obs_bearing_temp_high},

    // ...but a defeater is itself only a presumption, and needs grounds to be raised.
    {gate_no_mount_suspicion, def_mount_loose},
    {obs_mount_suspect, gate_no_mount_suspicion},
    {gate_no_calib_concern, def_calib_stale},
    {obs_calib_overdue, gate_no_calib_concern},

    // No vibration, no vibration-based diagnosis.
    {gate_no_vib, dx_cavitation},
    {gate_no_vib, dx_bearing_wear},
    {gate_no_vib, dx_impeller_defect},
    {gate_no_vib, dx_misalignment},
    {obs_vib_high, gate_no_vib},

    // Each hypothesis additionally needs its own discriminating signature, or every fault fires on vibration.
    {gate_no_cav_evidence, dx_cavitation},
    {obs_suction_press_low, gate_no_cav_evidence},
    {gate_no_bearing_temp, dx_bearing_wear},
    {obs_bearing_temp_high, gate_no_bearing_temp},
    {gate_no_bpf, dx_impeller_defect},
    {obs_vib_bpf_harmonics, gate_no_bpf},
    {gate_no_1x, dx_misalignment},
    {obs_vib_1x_dominant, gate_no_1x},

    // Benign explanations, and their gates.
    {gate_no_duty_evidence, benign_duty_change},
    {obs_duty_changed, gate_no_duty_evidence},
    {gate_no_throttle_evidence, benign_offdesign},
    {obs_throttled, gate_no_throttle_evidence},
    {gate_no_resonance_evidence, benign_resonance},
    {obs_speed_near_critical, gate_no_resonance_evidence},

    // A live benign explanation defeats the fault hypotheses it accounts for. This is the alarm-suppression
    // mechanism, and the reason the 40-90% nuisance rate is addressable at all.
    {benign_duty_change, dx_cavitation},
    {benign_duty_change, dx_impeller_defect},
    {benign_duty_change, dx_misalignment},
    {benign_offdesign, dx_cavitation},
    {benign_resonance, dx_misalignment},
    {benign_resonance, dx_bearing_wear},

    // Discriminating evidence defeats the benign explanation, reinstating the fault. A duty change does not erode
    // NPSH margin; resonance does not heat a bearing; structural ringing does not crackle.
    {obs_suction_press_low, benign_duty_change},
    {obs_suction_press_low, benign_offdesign},
    {obs_head_low, benign_duty_change},
    {obs_bearing_temp_high, benign_resonance},
    {obs_noise_crackling, benign_resonance},
    {obs_noise_crackling, benign_offdesign},

    // Cavitation and impeller wear both destroy head, so they compete. Mutual attack, deliberately: with no
    // discriminator the honest answer is "cannot tell", and mutual attack is how an AF says that.
    {dx_cavitation, dx_impeller_defect},
    {dx_impeller_defect, dx_cavitation},
    {obs_noise_crackling, dx_impeller_defect},

    // The motor current channel breaks that deadlock, in whichever direction the spectrum points. Note these are
    // the CLAIMS, not the raw readings -- the layering matters, see `def_current_incoherent`.
    {claim_current_broadband, dx_impeller_defect},   // stochastic signature is inconsistent with a geometric defect
    {claim_current_sidebands, dx_cavitation},        // blade-locked signature is inconsistent with two-phase flow

    // A claim requires its raw reading.
    {gate_no_broadband_current, claim_current_broadband},
    {obs_current_broadband_raw, gate_no_broadband_current},
    {gate_no_sidebands_current, claim_current_sidebands},
    {obs_current_sidebands_raw, gate_no_sidebands_current},

    // Conjunction: `def_current_incoherent` survives only when BOTH gates are Out, which requires BOTH raw
    // readings In. One gate per conjunct is how an AF expresses AND.
    {gate_no_broadband_current, def_current_incoherent},
    {gate_no_sidebands_current, def_current_incoherent},

    // ...and when it does fire, it invalidates both claims. It attacks the CLAIMS and never the raw readings,
    // which is what keeps this acyclic -- attacking the readings would close a 3-cycle through the gates and turn
    // the entire current channel Undecided.
    {def_current_incoherent, claim_current_broadband},
    {def_current_incoherent, claim_current_sidebands},

    // A current transformer has its own calibration, so staleness undermines the raw readings.
    {def_calib_stale, obs_current_broadband_raw},
    {def_calib_stale, obs_current_sidebands_raw},

    // Misalignment and resonance both put energy at running speed. Also mutual, also deliberately.
    {dx_misalignment, benign_resonance},

    // Conclusions.
    {gate_no_fault, act_schedule_maintenance},
    {dx_cavitation, gate_no_fault},
    {dx_bearing_wear, gate_no_fault},
    {dx_impeller_defect, gate_no_fault},
    {dx_misalignment, gate_no_fault},

    {gate_no_seizure_risk, act_shutdown_now},
    {dx_bearing_wear, gate_no_seizure_risk},

    {dx_cavitation, act_continue},
    {dx_bearing_wear, act_continue},
    {dx_impeller_defect, act_continue},
    {dx_misalignment, act_continue},
    {act_shutdown_now, act_continue},
    {act_shutdown_now, act_schedule_maintenance},
};

constexpr std::int32_t kCasebookEdges = static_cast<std::int32_t>(sizeof(kCasebook) / sizeof(kCasebook[0]));

constexpr std::int32_t kMaxArgs = 64;
constexpr std::int32_t kMaxAttacks = 256;

using PumpFramework = solver::Framework<kMaxArgs, kMaxAttacks>;
using PumpSolver = solver::Grounded<kMaxArgs>;
using PumpPolicy = arbiter::Policy<4>;

[[nodiscard]] inline constexpr bool is_observation(std::int32_t a) noexcept {
    return a >= obs_vib_high && a < observation_end;
}

/// The pump's action set and safe state.
///
/// A pump's safe state is stopped, so `act_shutdown_now` is the fallback: whenever the framework will not choose,
/// the pump stops. That is the de-energise-to-safe convention, and it is a per-asset decision -- a furnace would
/// want a controlled sequence and a drone has no stopped state at all.
[[nodiscard]] inline solver::Status make_policy(PumpPolicy& p) noexcept {
    solver::Status s = p.declare_action(solver::Arg{act_continue});
    if (s != solver::Status::Ok) { return s; }
    s = p.declare_action(solver::Arg{act_schedule_maintenance});
    if (s != solver::Status::Ok) { return s; }
    s = p.declare_action(solver::Arg{act_shutdown_now});
    if (s != solver::Status::Ok) { return s; }
    return p.set_fallback(solver::Arg{act_shutdown_now});
}

/// Instantiate the casebook for one control cycle. `present_mask` bit i means observation i is asserted.
///
/// THE DISCOVERY. Absence of an observation must be modelled as explicit defeat by an unattacked closed-world
/// argument. The two obvious encodings both fail:
///
///   * *Omit* the argument -- then nothing can reference it, and the numbering shifts between cycles, which
///     destroys any witness a human reads across time.
///   * *Self-attack* it -- the standard AF idiom for a self-defeating claim. This was the first attempt and it is
///     wrong: a self-attacker is Undecided, not Out, and an Undecided attacker blocks its target from ever being
///     In. The Undecided then propagates through every gate and the whole framework goes Undecided. The labelling
///     is not incorrect -- it correctly reports that a paradox poisons everything downstream -- but "sensor
///     quiet" is not a paradox.
///
/// An absent observation is decisively false, so it must be decisively Out, which in an AF means attacked by
/// something In. Hence `cw_closed_world`.
[[nodiscard]] inline solver::Status instantiate(PumpFramework& fw, std::uint32_t present_mask) noexcept {
    solver::Status s = fw.declare(argument_count);
    if (s != solver::Status::Ok) { return s; }

    for (std::int32_t i = 0; i < kCasebookEdges; ++i) {
        s = fw.add_attack(solver::Arg{kCasebook[i].from}, solver::Arg{kCasebook[i].to});
        if (s != solver::Status::Ok) { return s; }
    }

    for (std::int32_t a = 0; a < kObservationCount; ++a) {
        const bool asserted = (present_mask & (1U << static_cast<std::uint32_t>(a))) != 0U;
        if (!asserted) {
            s = fw.add_attack(solver::Arg{cw_closed_world}, solver::Arg{a});
            if (s != solver::Status::Ok) { return s; }
        }
    }

    return fw.finalize();
}

/// Build a mask from an explicit list, for readable scenarios.
[[nodiscard]] inline constexpr std::uint32_t mask_of(const std::int32_t* obs, std::int32_t count) noexcept {
    std::uint32_t m = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        m |= (1U << static_cast<std::uint32_t>(obs[i]));
    }
    return m;
}

}  // namespace quorum::casebook::pump
