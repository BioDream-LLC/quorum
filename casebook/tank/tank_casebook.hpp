#pragma once

#include <cstdint>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"

/// \file
/// Tank 409 casebook, derived from a REAL HAZOP of a REAL accident.
///
/// ## Provenance
///
/// Source: the University of Michigan SAFEChE HAZOP tutorial and its worked study of the Caribbean Petroleum
/// Corporation (CAPECO) terminal explosion, Bayamón, Puerto Rico, 23 October 2009. Investigated by the U.S.
/// Chemical Safety Board, report 2010-02-I-PR, final October 2015.
///
/// What happened: the main gasoline storage tank was full, so an incoming shipment was routed to four smaller
/// tanks by a largely manual process. Tank 409's **level transmitter was broken**, so fill time was calculated by
/// hand -- and overestimated. The tank overfilled, gasoline sprayed out and formed a vapour cloud of roughly 100
/// acres, which found an ignition source and exploded. No fatalities; approximately 300 homes and businesses
/// damaged up to 1.25 miles away, with petroleum released into soil, waterways and wetlands.
///
/// The CSB's key regulatory finding, and the sentence this casebook exists to encode as a checkable property:
///
/// > NO REDUNDANT OR INDEPENDENT SAFEGUARDS TO PREVENT OVERFILLING A TANK
///
/// ## Why this is a better second casebook than an invented one
///
/// The pump casebook was synthetic, and every discovery it produced could in principle have been an artifact of my
/// own invention. This one comes from a published methodology applied to a documented accident, so its ground
/// truth is not a matter of opinion: continuing the transfer was the wrong answer, and we know because the tank
/// exploded.
///
/// It is also a completely different shape. The pump problem was *competing hypotheses explaining one symptom*.
/// This is *disagreeing sources for one quantity*, plus the question of whether any trustworthy source exists at
/// all. Almost none of the pump's structure transfers, which is exactly what makes it a test of generality.
///
/// ## HAZOP mapping
///
/// The node is Tank 409: gasoline from a tanker ship through block valve X1 into the tank, pump Y1 withdrawing
/// through valve X2, with a level gauge and a level transmitter fitted. The deviation modelled is **More Level**,
/// whose published typical causes are *"Level control failure; More input than output"*.
///
/// One convention deliberately NOT carried over: HAZOP writes consequences *"assuming any existing safeguards are
/// not working"*, because it is a design-time worst-casing exercise. An arbiter does the opposite -- it adjudicates
/// what is defensible given live evidence. The same document therefore yields different frameworks depending on
/// which question is being asked, and a compiler must be told which.

namespace quorum::casebook::tank {

// ─────────────────────────────────────────────────────────────────────────────
// Arguments
// ─────────────────────────────────────────────────────────────────────────────
enum Argument : std::int32_t {
    // ── Observations ────────────────────────────────────────────────────────
    obs_xmtr_says_normal = 0,   ///< level transmitter reports a normal level
    obs_xmtr_says_high,         ///< level transmitter reports a high level
    obs_xmtr_flatline,          ///< transmitter output frozen across the period -- the broken-sensor signature
    obs_gauge_read_normal,      ///< somebody physically read the local gauge: normal
    obs_gauge_read_high,        ///< somebody physically read the local gauge: high
    obs_fill_calc_says_room,    ///< the manual fill-time calculation says there is room. AN ESTIMATE.
    obs_inlet_flow,             ///< X1 open, transfer in progress
    obs_outlet_flow,            ///< pump Y1 running, withdrawing
    obs_hi_level_alarm,
    obs_gas_detected,           ///< vapour detected in the tank farm -- a consequence signal, already too late
    obs_dike_liquid,            ///< liquid in the diked region -- also a consequence signal
    observation_end,

    // ── Claims about the level ──────────────────────────────────────────────
    claim_level_safe = observation_end,
    claim_level_high,
    claim_overfilling,
    claim_end,

    // ── Defeaters ───────────────────────────────────────────────────────────
    /// The transmitter is not to be believed. A frozen reading is not a reading.
    def_xmtr_untrusted = claim_end,

    /// No trustworthy independent measurement of the level exists.
    ///
    /// THE CENTRAL ARGUMENT OF THIS CASEBOOK. Defeated by any live trustworthy reading -- transmitter or gauge, in
    /// either direction. Deliberately NOT defeated by `obs_fill_calc_says_room`, because an arithmetic estimate is
    /// not a measurement, and believing otherwise is what destroyed the terminal.
    def_no_independent_measure,

    // ── HAZOP deviation and the hazard it creates ───────────────────────────
    /// Guide word MORE + parameter LEVEL. Published typical cause: "More input than output."
    dev_more_level,

    /// Filling a tank with no independent measurement of its level.
    ///
    /// The CSB finding as a single argument. Holds only when `dev_more_level` AND `def_no_independent_measure`
    /// both hold -- conjunction, encoded as one gate per conjunct, the pattern the pump casebook established.
    hazard_unmeasured_filling,

    // ── Gates ───────────────────────────────────────────────────────────────
    gate_no_flatline,
    gate_no_safe_evidence,
    gate_no_high_evidence,
    gate_no_overfill_evidence,
    gate_no_inlet,
    gate_no_more_level,
    gate_no_measure_concern,
    gate_no_transfer_intent,
    gate_no_stop_reason,
    gate_no_stop_and_evacuate_reason,

    // ── Actions ─────────────────────────────────────────────────────────────
    act_monitor,                ///< the idle state: nothing to do
    act_continue_transfer,
    act_stop_transfer,          ///< close X1. The safe state for a tank is "stop adding material."
    act_stop_and_evacuate,

    cw_closed_world,
    argument_count
};

constexpr std::int32_t kObservationCount = observation_end;

[[nodiscard]] inline const char* name_of(std::int32_t a) noexcept {
    switch (a) {
        case obs_xmtr_says_normal:       return "obs_xmtr_says_normal";
        case obs_xmtr_says_high:         return "obs_xmtr_says_high";
        case obs_xmtr_flatline:          return "obs_xmtr_flatline";
        case obs_gauge_read_normal:      return "obs_gauge_read_normal";
        case obs_gauge_read_high:        return "obs_gauge_read_high";
        case obs_fill_calc_says_room:    return "obs_fill_calc_says_room";
        case obs_inlet_flow:             return "obs_inlet_flow";
        case obs_outlet_flow:            return "obs_outlet_flow";
        case obs_hi_level_alarm:         return "obs_hi_level_alarm";
        case obs_gas_detected:           return "obs_gas_detected";
        case obs_dike_liquid:            return "obs_dike_liquid";
        case claim_level_safe:           return "claim_level_safe";
        case claim_level_high:           return "claim_level_high";
        case claim_overfilling:          return "claim_overfilling";
        case def_xmtr_untrusted:         return "def_xmtr_untrusted";
        case def_no_independent_measure: return "def_no_independent_measure";
        case dev_more_level:             return "dev_more_level";
        case hazard_unmeasured_filling:  return "hazard_unmeasured_filling";
        case gate_no_flatline:           return "gate_no_flatline";
        case gate_no_safe_evidence:      return "gate_no_safe_evidence";
        case gate_no_high_evidence:      return "gate_no_high_evidence";
        case gate_no_overfill_evidence:  return "gate_no_overfill_evidence";
        case gate_no_inlet:              return "gate_no_inlet";
        case gate_no_more_level:         return "gate_no_more_level";
        case gate_no_measure_concern:    return "gate_no_measure_concern";
        case gate_no_transfer_intent:    return "gate_no_transfer_intent";
        case gate_no_stop_reason:        return "gate_no_stop_reason";
        case gate_no_stop_and_evacuate_reason:    return "gate_no_stop_and_evacuate_reason";
        case act_monitor:                return "act_monitor";
        case act_continue_transfer:      return "act_continue_transfer";
        case act_stop_transfer:          return "act_stop_transfer";
        case act_stop_and_evacuate:               return "act_stop_and_evacuate";
        case cw_closed_world:            return "cw_closed_world";
        default:                         return "???";
    }
}

struct Edge {
    std::int32_t from;
    std::int32_t to;
};

inline constexpr Edge kCasebook[] = {
    // ── Is the transmitter trustworthy? ─────────────────────────────────────
    // A flatlined transmitter is untrusted, and an untrusted transmitter's reading is worthless in EITHER
    // direction. This is the CAPECO mechanism: the instrument was broken and therefore silent, and silence was
    // read as reassurance.
    {gate_no_flatline, def_xmtr_untrusted},
    {obs_xmtr_flatline, gate_no_flatline},
    {def_xmtr_untrusted, obs_xmtr_says_normal},
    {def_xmtr_untrusted, obs_xmtr_says_high},

    // ── Does any independent measurement exist? ─────────────────────────────
    // Any live trustworthy reading defeats the concern. Note what is absent from this list:
    // `obs_fill_calc_says_room`. An estimate is not a measurement.
    {obs_xmtr_says_normal, def_no_independent_measure},
    {obs_xmtr_says_high, def_no_independent_measure},
    {obs_gauge_read_normal, def_no_independent_measure},
    {obs_gauge_read_high, def_no_independent_measure},

    // ── Level claims ────────────────────────────────────────────────────────
    {gate_no_safe_evidence, claim_level_safe},
    {obs_xmtr_says_normal, gate_no_safe_evidence},
    {obs_gauge_read_normal, gate_no_safe_evidence},
    // You may not claim the level is safe when you cannot measure it. The whole casebook in one edge.
    {def_no_independent_measure, claim_level_safe},
    {claim_level_high, claim_level_safe},
    {claim_overfilling, claim_level_safe},

    {gate_no_high_evidence, claim_level_high},
    {obs_xmtr_says_high, gate_no_high_evidence},
    {obs_gauge_read_high, gate_no_high_evidence},
    {obs_hi_level_alarm, gate_no_high_evidence},

    // Overfilling is established by consequence signals -- by the time these fire, it is already happening.
    {gate_no_overfill_evidence, claim_overfilling},
    {obs_gas_detected, gate_no_overfill_evidence},
    {obs_dike_liquid, gate_no_overfill_evidence},

    // ── HAZOP: MORE + LEVEL = more input than output ────────────────────────
    {gate_no_inlet, dev_more_level},
    {obs_inlet_flow, gate_no_inlet},
    {obs_outlet_flow, dev_more_level},

    // ── The CSB finding, as conjunction ─────────────────────────────────────
    {gate_no_more_level, hazard_unmeasured_filling},
    {dev_more_level, gate_no_more_level},
    {gate_no_measure_concern, hazard_unmeasured_filling},
    {def_no_independent_measure, gate_no_measure_concern},

    // ── Actions ─────────────────────────────────────────────────────────────
    // Continuing requires an actual transfer in progress...
    {gate_no_transfer_intent, act_continue_transfer},
    {obs_inlet_flow, gate_no_transfer_intent},
    // ...and is defeated by any of these. `obs_fill_calc_says_room` attacks NOTHING here, on purpose: the manual
    // estimate is what the operators relied on, and it must not be able to authorise continued filling.
    {def_no_independent_measure, act_continue_transfer},
    {claim_level_high, act_continue_transfer},
    {claim_overfilling, act_continue_transfer},
    {hazard_unmeasured_filling, act_continue_transfer},

    {gate_no_stop_reason, act_stop_transfer},
    {claim_level_high, gate_no_stop_reason},
    {hazard_unmeasured_filling, gate_no_stop_reason},
    {def_no_independent_measure, gate_no_stop_reason},

    {gate_no_stop_and_evacuate_reason, act_stop_and_evacuate},
    {claim_overfilling, gate_no_stop_and_evacuate_reason},

    // Precedence: stop-and-evacuate beats stop beats continue, and monitoring is what is left when nothing else applies.
    {act_stop_and_evacuate, act_stop_transfer},
    {act_stop_and_evacuate, act_continue_transfer},
    {act_stop_transfer, act_continue_transfer},
    {act_continue_transfer, act_monitor},
    {act_stop_transfer, act_monitor},
    {act_stop_and_evacuate, act_monitor},
};

constexpr std::int32_t kCasebookEdges = static_cast<std::int32_t>(sizeof(kCasebook) / sizeof(kCasebook[0]));

constexpr std::int32_t kMaxArgs = 64;
constexpr std::int32_t kMaxAttacks = 256;

using TankFramework = solver::Framework<kMaxArgs, kMaxAttacks>;
using TankSolver = solver::Grounded<kMaxArgs>;
using TankPolicy = arbiter::Policy<8>;

[[nodiscard]] inline constexpr bool is_observation(std::int32_t a) noexcept {
    return a >= obs_xmtr_says_normal && a < observation_end;
}

/// The tank's action set and safe state.
///
/// A tank's safe state is "stop adding material", so `act_stop_transfer` is the fallback. Note this differs from
/// the pump, whose safe state was a full shutdown -- which is the reason the fallback is declared per casebook
/// rather than baked into the arbiter.
[[nodiscard]] inline solver::Status make_policy(TankPolicy& p) noexcept {
    solver::Status s = p.declare_action(solver::Arg{act_monitor});
    if (s != solver::Status::Ok) { return s; }
    s = p.declare_action(solver::Arg{act_continue_transfer});
    if (s != solver::Status::Ok) { return s; }
    s = p.declare_action(solver::Arg{act_stop_transfer});
    if (s != solver::Status::Ok) { return s; }
    s = p.declare_action(solver::Arg{act_stop_and_evacuate});
    if (s != solver::Status::Ok) { return s; }
    return p.set_fallback(solver::Arg{act_stop_transfer});
}

[[nodiscard]] inline solver::Status instantiate(TankFramework& fw, std::uint32_t present_mask) noexcept {
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

[[nodiscard]] inline constexpr std::uint32_t mask_of(const std::int32_t* obs, std::int32_t count) noexcept {
    std::uint32_t m = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        m |= (1U << static_cast<std::uint32_t>(obs[i]));
    }
    return m;
}

}  // namespace quorum::casebook::tank
