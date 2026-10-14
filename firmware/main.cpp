// The device main loop, as a footprint harness.
//
// This is what quorum actually looks like on a microcontroller: static objects, one solve per control cycle, no
// allocation, no recursion, no exceptions. It is compiled at several configurations so the cost of each layer can be
// read off separately rather than guessed at:
//
//   QUORUM_CONFIG=1   engine only              -- framework + grounded
//   QUORUM_CONFIG=2   + arbiter + one casebook -- the minimum that produces a verdict
//   QUORUM_CONFIG=3   + binding                -- the minimum that reads real sensors
//   QUORUM_CONFIG=4   + witness                -- explains itself on demand
//   QUORUM_CONFIG=5   two casebooks            -- a palette, as it would actually ship
//
// Every result is written through a volatile, because a footprint measured after the optimiser deleted the work is
// not a measurement. The reads come through volatiles too, so nothing is constant-folded.

#include <cstdint>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"

#if QUORUM_CONFIG >= 2
#include "quorum/arbiter/verdict.hpp"
#include "tank_generated.hpp"
namespace tank = quorum::casebook::gen::tank_409;
#endif

#if QUORUM_CONFIG >= 3
#include "quorum/binding/binding.hpp"
#endif

#if QUORUM_CONFIG >= 4
#include "quorum/witness/derivation.hpp"
#endif

#if QUORUM_CONFIG >= 5
#include "pump_generated.hpp"
namespace pump = quorum::casebook::gen::centrifugal_pump;
#endif

using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

/// Stand-ins for memory-mapped peripherals. Volatile so the optimiser cannot fold the pipeline away, and placed in
/// RAM so they cost what a real peripheral shadow would.
volatile std::uint32_t g_raw_input = 0x2A3B;
volatile std::uint32_t g_now_ms = 0;
volatile std::int32_t g_action_out = 0;
volatile std::int32_t g_outcome_out = 0;
volatile std::int32_t g_steps_out = 0;
volatile std::int32_t g_witness_out = 0;

#if QUORUM_CONFIG == 1
// Engine only, sized to match the tank casebook so the comparison across configurations is meaningful.
quorum::solver::Framework<44, 71> g_fw;
quorum::solver::Grounded<44> g_solver;
#endif

#if QUORUM_CONFIG >= 2
tank::CaseFramework g_tank_fw;
tank::CaseSolver g_tank_solver;
tank::CasePolicy g_tank_policy;
#endif

#if QUORUM_CONFIG >= 3
tank::CaseBinding g_tank_binding;
#endif

#if QUORUM_CONFIG >= 4
// Sized from the bound cbc emits, not from a guess. `kMaxWitnessSteps` is the argument count, which `Derivation`
// guarantees cannot be exceeded because each argument is expanded at most once -- verified exhaustively in
// witness/test/test_bound.cpp over 2.16 million reconstructions.
//
// The first version of this file used Derivation<256, 44>, a plausible-looking round number. It cost 5,532 bytes to
// hold a structure that can never exceed 39 entries. Oversizing a fixed-capacity buffer is invisible on a host and
// expensive on a part.
quorum::witness::Derivation<tank::kMaxWitnessSteps, tank::kMaxArgs> g_derivation;
#endif

#if QUORUM_CONFIG >= 5
pump::CaseFramework g_pump_fw;
pump::CaseSolver g_pump_solver;
pump::CasePolicy g_pump_policy;
#endif

}  // namespace

int main() {
#if QUORUM_CONFIG == 1
    // The bare engine: build a framework from a volatile-derived shape and label it.
    for (;;) {
        g_fw.reset();
        if (g_fw.declare(44) != Status::Ok) { return 1; }
        const std::int32_t spread = static_cast<std::int32_t>(g_raw_input % 40U) + 1;
        for (std::int32_t i = 0; i < 40; ++i) {
            if (g_fw.add_attack(Arg{i}, Arg{(i + spread) % 44}) != Status::Ok) { break; }
        }
        if (g_fw.finalize() != Status::Ok) { return 1; }
        if (g_solver.solve(g_fw) != Status::Ok) { return 1; }
        g_steps_out = g_solver.steps();
        g_action_out = static_cast<std::int32_t>(g_solver.label(Arg{0}));
    }
#else
    if (tank::make_policy(g_tank_policy) != Status::Ok) { return 1; }
#if QUORUM_CONFIG >= 3
    if (tank::make_binding(g_tank_binding) != quorum::binding::Status::Ok) { return 1; }
#endif
#if QUORUM_CONFIG >= 5
    if (pump::make_policy(g_pump_policy) != Status::Ok) { return 1; }
#endif

    for (;;) {
        const std::uint32_t now = g_now_ms;
        (void)now;  // only the binding layer needs a clock

#if QUORUM_CONFIG >= 3
        // Feed every channel from the pretend peripheral, then let the binding layer decide what is assertable.
        for (std::int32_t c = 0; c < tank::kChannelCount; ++c) {
            const std::int32_t raw = static_cast<std::int32_t>(g_raw_input % 100000U);
            if ((g_raw_input & 0x8000U) != 0U) {
                (void)g_tank_binding.fault(c, now);
            } else {
                (void)g_tank_binding.sample(c, raw, now);
            }
        }
        const std::uint32_t mask = g_tank_binding.mask(now);
#else
        const std::uint32_t mask = g_raw_input;
#endif

        if (tank::instantiate(g_tank_fw, mask) != Status::Ok) { return 1; }
        const quorum::arbiter::Verdict v =
            quorum::arbiter::decide(g_tank_fw, g_tank_solver, g_tank_policy);

        g_outcome_out = static_cast<std::int32_t>(v.outcome);
        g_action_out = v.action.get();
        g_steps_out = v.steps;

#if QUORUM_CONFIG >= 4
        // An explanation is reconstructed on demand, not maintained continuously -- the whole reason the witness is
        // a separate component. A device only pays for this when somebody asks why.
        if ((g_raw_input & 1U) != 0U && v.action.valid()) {
            if (g_derivation.build(g_tank_fw, g_tank_solver, v.action, Arg{tank::kClosedWorld}) == Status::Ok) {
                g_witness_out = g_derivation.step_count();
            }
        }
#endif

#if QUORUM_CONFIG >= 5
        // A second casebook, on its own framework with its own policy. Separate frameworks rather than one merged
        // framework: simpler, and non-interference becomes structural rather than something to be proved. The pump
        // casebook declares no channels, so its mask arrives from the integrator -- which is exactly the mixed
        // deployment a real plant has, some signals wired and some entered.
        const std::uint32_t pump_mask = g_raw_input >> 4;
        if (pump::instantiate(g_pump_fw, pump_mask) != Status::Ok) { return 1; }
        const quorum::arbiter::Verdict pv =
            quorum::arbiter::decide(g_pump_fw, g_pump_solver, g_pump_policy);
        g_outcome_out += static_cast<std::int32_t>(pv.outcome);
        g_action_out += pv.action.get();
#endif
    }
#endif
}
