// quorum on a Raspberry Pi Pico 2: the WCET measurement.
//
// This is the program that answers the last open question about this product. Footprint, stack, and step counts are
// measured facts; how long a step takes on real silicon is not, and a polynomial algorithm with a proven step bound is
// necessary for a safety function without being sufficient. Somebody has to state the microseconds.
//
// What it measures, and why each part is chosen:
//
//   * THE WORST CASE, not a typical state. `kWorstCaseMask` comes from cbc, which found it by solving every state of
//     the casebook. For most casebooks it is the QUIET state -- nothing asserted -- because every closed-world edge
//     is then live and the framework carries its maximum number of attacks. That is also a healthy machine's most
//     common state, so the worst case is not a rare corner: it is what the device does all day.
//
//   * The FULL CYCLE, and each stage separately: binding, framework construction, solve, arbitration, witness. A
//     single end-to-end number cannot be apportioned, and if the answer is too slow the next question is immediately
//     "which part".
//
//   * MINIMUM as well as maximum. On a part with XIP flash caching the first run includes cache misses and later runs
//     do not. A mean would hide both. For WCET the maximum over many runs is the number that counts.
//
// Output goes to UART0 at 115200 on the default pins (GP0/GP1), so `minicom`/`screen` on the debug probe or a USB
// serial adapter shows the numbers with no host tooling.

#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "quorum/arbiter/verdict.hpp"
#include "quorum/bench/cycles.hpp"
#include "quorum/binding/binding.hpp"
#include "quorum/witness/derivation.hpp"

#include "pump_generated.hpp"
#include "tank_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace tank = quorum::casebook::gen::tank_409;
namespace bench = quorum::bench;

using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

// Static storage, exactly as a real integration would have it: no allocation anywhere in the cycle.
pump::CaseFramework g_pump_fw;
pump::CaseSolver g_pump_solver;
pump::CasePolicy g_pump_policy;

tank::CaseFramework g_tank_fw;
tank::CaseSolver g_tank_solver;
tank::CasePolicy g_tank_policy;
tank::CaseBinding g_tank_binding;

quorum::witness::Derivation<tank::kMaxWitnessSteps, tank::kMaxArgs> g_derivation;

// Sinks. Every measured result is written here so the optimiser cannot delete the work being timed.
volatile std::int32_t g_sink_action = 0;
volatile std::int32_t g_sink_steps = 0;
volatile std::uint32_t g_sink_mask = 0;

constexpr std::uint32_t kRuns = 2000;

void report(const char* label, const bench::Measurement& m, std::uint32_t clock_hz) {
    if (!m.valid) {
        std::printf("  %-34s COUNTER NOT RUNNING\n", label);
        return;
    }
    // Microseconds to three decimals, computed in integers: no floating point on the device, same discipline as the
    // binding layer.
    const std::uint64_t mhz = clock_hz / 1000000U;
    const std::uint64_t nanos = (static_cast<std::uint64_t>(m.max_cycles) * 1000U) / (mhz == 0U ? 1U : mhz);
    std::printf("  %-34s min %6lu  mean %6lu  max %6lu cycles   %lu.%03lu us\n", label,
                static_cast<unsigned long>(m.min_cycles), static_cast<unsigned long>(m.mean_cycles()),
                static_cast<unsigned long>(m.max_cycles), static_cast<unsigned long>(nanos / 1000U),
                static_cast<unsigned long>(nanos % 1000U));
}

}  // namespace

int main() {
    stdio_init_all();
    sleep_ms(2000);  // let a terminal attach before the first line

    const std::uint32_t clock_hz = clock_get_hz(clk_sys);

    std::printf("\n\n");
    std::printf("quorum on RP2350 -- worst-case execution time\n");
    std::printf("=============================================\n");
    std::printf("core clock            %lu Hz\n", static_cast<unsigned long>(clock_hz));
    std::printf("casebook              %s v%d\n", pump::kCasebookName, static_cast<int>(pump::kCasebookVersion));
    std::printf("source                %s\n", pump::kSourceDocument);
    std::printf("arguments / attacks   %d / %d\n", static_cast<int>(pump::argument_count),
                static_cast<int>(pump::kCasebookEdges));
    std::printf("worst-case state      0x%lX  (%d steps, proven bound %d)\n",
                static_cast<unsigned long>(pump::kWorstCaseMask), static_cast<int>(pump::kWorstCaseSteps),
                static_cast<int>(pump::kStepBound));
    std::printf("runs per measurement  %lu\n\n", static_cast<unsigned long>(kRuns));

    // ── integrity, before anything else ─────────────────────────────────────
    //
    // The tables in flash must be the ones cbc validated. A hand-edited header, a stale header paired with a newer
    // engine, a palette built from two compiler versions, or corrupted flash all produce a device that runs a framework
    // no exhaustive validation was ever performed on -- while still reporting correct-looking provenance.
    //
    // Checked once, here, not per cycle: it is O(edges) and what it detects cannot change while running. A mismatch is
    // fatal rather than a warning, because the validation evidence does not describe this device, and continuing would
    // mean acting on conclusions nothing has ever checked.
    std::printf("fingerprint  expected 0x%08lX  computed 0x%08lX  %s\n",
                static_cast<unsigned long>(pump::kCasebookFingerprint),
                static_cast<unsigned long>(pump::computed_fingerprint()),
                pump::verify_casebook() ? "OK" : "MISMATCH");
    if (!pump::verify_casebook() || !tank::verify_casebook()) {
        std::printf("FATAL: this is not the casebook that was validated. Surrendering to the interlock.\n");
        for (;;) {}
    }
    std::printf("\n");

    if (!bench::enable()) {
        std::printf("FATAL: the DWT cycle counter will not start. Every number below would be zero, which reads\n"
                    "       like an impossibly fast result rather than a broken measurement, so nothing is run.\n");
        for (;;) { tight_loop_contents(); }
    }
    std::printf("cycle counter: %s\n\n", bench::source_name());
    if (pump::make_policy(g_pump_policy) != Status::Ok || tank::make_policy(g_tank_policy) != Status::Ok ||
        tank::make_binding(g_tank_binding) != quorum::binding::Status::Ok) {
        std::printf("FATAL: configuration failed\n");
        for (;;) { tight_loop_contents(); }
    }

    // ── the pump casebook, at its worst case ────────────────────────────────
    // ── sweep every state, rather than trusting one ─────────────────────────
    //
    // kWorstCaseMask is the state maximising `steps_`, and static analysis showed that is NOT the state maximising
    // work: steps_ weights five loops equally and does not count the seeding loop at all. On the pump the true
    // instruction-worst state costs 1.5% more than the state this firmware used to measure -- a WCET figure that was
    // quietly optimistic, and looked entirely plausible.
    //
    // The fix is not a better heuristic. Sweeping all 16,384 states costs about 0.3 s at 150 MHz, so there is no need
    // to choose: measure every state, keep the worst. Correct by construction, on any silicon, with no cost model
    // anywhere. kWorstCaseMask survives as a cross-check rather than as an assumption.
    std::printf("sweeping all %lu states to find the true worst case...\n",
                static_cast<unsigned long>(1UL << pump::kObservationCount));

    std::uint32_t sweep_worst_mask = 0;
    std::uint32_t sweep_worst_cycles = 0;
    std::int32_t sweep_worst_steps = 0;
    for (std::uint32_t m = 0; m < (1UL << pump::kObservationCount); ++m) {
        // A handful of runs per state: enough to clear a cold cache, cheap enough to afford 16,384 times.
        const auto probe = bench::measure(4U, [m] {
            if (pump::instantiate(g_pump_fw, m) != Status::Ok) { return; }
            const Verdict v = quorum::arbiter::decide(g_pump_fw, g_pump_solver, g_pump_policy);
            g_sink_action = v.action.get();
            g_sink_steps = v.steps;
        });
        if (probe.valid && probe.max_cycles > sweep_worst_cycles) {
            sweep_worst_cycles = probe.max_cycles;
            sweep_worst_mask = m;
            sweep_worst_steps = g_sink_steps;
        }
    }
    std::printf("  worst state found     0x%lX  (%lu cycles, %d steps)\n",
                static_cast<unsigned long>(sweep_worst_mask), static_cast<unsigned long>(sweep_worst_cycles),
                static_cast<int>(sweep_worst_steps));
    std::printf("  cbc's kWorstCaseMask  0x%lX  %s\n", static_cast<unsigned long>(pump::kWorstCaseMask),
                sweep_worst_mask == pump::kWorstCaseMask ? "(agrees)" : "(differs -- the sweep is authoritative)");

    // What the sweep's number is, stated precisely, because the obvious thing to do with it is wrong. The probe above
    // times `instantiate + decide` -- a whole cycle -- while the 2,571-instruction static bound covers `solve` ALONE
    // (see tools/wcet-disassemble.sh, which isolates quorum_solve). Dividing one by the other is a units-and-scope
    // error that reported 8.15 cycles per instruction and made a bound that actually holds look badly violated.
    //
    // This was the FOURTH time a tool in this project has mis-scoped a comparison and been unfair to correct work,
    // after a LOPA transcription accused a correct paper of an arithmetic error (PROC-D13), a report heading filed a
    // study's own conclusion under "contradicts itself" (PROC-D15), and the Arduino sketch made this identical
    // mistake (PROC-D18). PROC-D18 was registered and the Arduino sketch was fixed the same day -- and this copy was
    // not, because the logic is duplicated across two firmware trees. That is PROC-D16 exactly: two copies guarantee
    // the fix lands in one of them.
    //
    // The CPI comparison now lives where `solve` is measured, below, and the sweep reports only what it can support.
    std::printf("  scope                 instantiate + decide, cold cache, 4 runs per state\n");
    std::printf("                        NOT comparable to the 2571-instruction bound, which covers solve alone\n");
    std::printf("\n");

    std::printf("pump casebook, measured worst state 0x%lX\n", static_cast<unsigned long>(sweep_worst_mask));

    const auto construct = bench::measure(kRuns, [sweep_worst_mask] {
        const Status s = pump::instantiate(g_pump_fw, sweep_worst_mask);
        g_sink_steps = static_cast<std::int32_t>(s);
    });
    report("instantiate (build + finalize)", construct, clock_hz);

    const auto solve = bench::measure(kRuns, [] {
        const Status s = g_pump_solver.solve(g_pump_fw);
        g_sink_steps = s == Status::Ok ? g_pump_solver.steps() : -1;
    });
    report("solve (grounded fixpoint)", solve, clock_hz);

    // THE comparison the static bound licenses: solve's measured cycles against solve's instruction count. Both sides
    // are now the same scope. At >= 1 cycle per instruction a correct measurement cannot come in below the bound; if
    // it does, either the instruction count or this measurement is wrong.
    std::printf("\n  static bound          2571 instructions (solve only)\n");
    std::printf("  measured / bound      %lu.%02lu cycles per instruction  -- %s the 2 CPI the README converts at\n",
                static_cast<unsigned long>(solve.max_cycles / 2571U),
                static_cast<unsigned long>((solve.max_cycles * 100U / 2571U) % 100U),
                solve.max_cycles <= 2U * 2571U ? "AT OR UNDER" : "OVER");
    if (solve.max_cycles < 2571U) {
        std::printf("  ** measured max is BELOW the static instruction bound -- one of them is wrong **\n");
    }
    std::printf("\n");

    const auto cycle = bench::measure(kRuns, [sweep_worst_mask] {
        if (pump::instantiate(g_pump_fw, sweep_worst_mask) != Status::Ok) { return; }
        const Verdict v = quorum::arbiter::decide(g_pump_fw, g_pump_solver, g_pump_policy);
        g_sink_action = v.action.get();
        g_sink_steps = v.steps;
    });
    report("FULL CYCLE (instantiate+decide)", cycle, clock_hz);

    // Cold cache versus warm, same scope and same state, which is the one thing this part can show and the RA4M1
    // cannot. The sweep above touched each state once with a cold cache; by the time the loop above has run kRuns
    // times the working set is resident. RP2350 fetches from external QSPI flash through a cache, so the difference
    // is the memory system rather than the algorithm -- on an Arduino Uno R4 (internal flash, no cache) the same
    // measurement returned min == mean == max exactly, with no spread at all.
    //
    // This matters for WCET specifically: a bound derived from an instruction count says nothing about a stall, so
    // the cold figure is the one an integrator has to budget for unless they can prove the cache is warm.
    if (cycle.valid && cycle.max_cycles > 0U) {
        std::printf("  cold (sweep) vs warm  %lu vs %lu cycles = %lu.%02lu x penalty from the memory system\n",
                    static_cast<unsigned long>(sweep_worst_cycles),
                    static_cast<unsigned long>(cycle.max_cycles),
                    static_cast<unsigned long>(sweep_worst_cycles / cycle.max_cycles),
                    static_cast<unsigned long>((sweep_worst_cycles * 100U / cycle.max_cycles) % 100U));
    }

    // Sanity: the work done on device must match what the compiler predicted on the host. If these disagree, the
    // timing numbers are measuring something other than the validated casebook.
    if (pump::instantiate(g_pump_fw, sweep_worst_mask) == Status::Ok) {
        const Verdict v = quorum::arbiter::decide(g_pump_fw, g_pump_solver, g_pump_policy);
        // Note the casts. On arm-none-eabi `std::int32_t` is `long int`, not `int`, so passing one to %d is a
        // format mismatch that -Wformat catches on the target and cannot catch on an x86-64 host, where the two are
        // the same type. Any integrator printing quorum's types needs to know this.
        std::printf("\n  steps on device %d, predicted by cbc %d  %s\n", static_cast<int>(v.steps),
                    static_cast<int>(pump::kWorstCaseSteps),
                    v.steps == pump::kWorstCaseSteps ? "MATCH" : "*** MISMATCH ***");
        std::printf("  verdict         %s -> %s\n", outcome_name(v.outcome), pump::name_of(v.action.get()));
        std::printf("  within bound    %s (%d <= %d)\n", v.steps <= pump::kStepBound ? "yes" : "NO",
                    static_cast<int>(v.steps), static_cast<int>(pump::kStepBound));
    }

    // ── the tank casebook, including the binding layer ──────────────────────
    std::printf("\ntank casebook, worst-case state 0x%lX, with the binding layer\n",
                static_cast<unsigned long>(tank::kWorstCaseMask));

    const auto sample = bench::measure(kRuns, [] {
        for (std::int32_t c = 0; c < tank::kChannelCount; ++c) {
            (void)g_tank_binding.sample(c, 45000, 1000U);
        }
        g_sink_mask = g_tank_binding.mask(1000U);
    });
    report("binding: sample all + mask", sample, clock_hz);

    const auto tank_cycle = bench::measure(kRuns, [] {
        if (tank::instantiate(g_tank_fw, tank::kWorstCaseMask) != Status::Ok) { return; }
        const Verdict v = quorum::arbiter::decide(g_tank_fw, g_tank_solver, g_tank_policy);
        g_sink_action = v.action.get();
    });
    report("FULL CYCLE (instantiate+decide)", tank_cycle, clock_hz);

    // ── the witness, which is paid for only when somebody asks ──────────────
    (void)tank::instantiate(g_tank_fw, tank::kWorstCaseMask);
    (void)g_tank_solver.solve(g_tank_fw);
    const Verdict tv = quorum::arbiter::decide(g_tank_fw, g_tank_solver, g_tank_policy);

    const auto witness = bench::measure(kRuns, [&tv] {
        if (g_derivation.build(g_tank_fw, g_tank_solver, tv.action, Arg{tank::kClosedWorld}) == Status::Ok) {
            g_sink_steps = g_derivation.step_count();
        }
    });
    report("witness: reconstruct derivation", witness, clock_hz);

    // ── what it all means for a control loop ────────────────────────────────
    std::printf("\nheadroom\n");
    // "Worst" has to mean the worst thing measured, not the first cycle measured. This used to report the WARM pump
    // cycle alone (94 us on the first run) while the same printout showed the tank cycle with binding at 140 us and the
    // cold-cache sweep at 145 us -- so the headroom lines overstated the margin by half. Another instance of the scope
    // error recorded as PROC-D18: a label claiming more than the number behind it. Every candidate is now compared and
    // the winner is named, so a reader can see which scope the headroom is computed from.
    const std::uint32_t pump_warm_us = cycle.max_microseconds(clock_hz);
    const std::uint32_t tank_us      = tank_cycle.max_microseconds(clock_hz);
    const std::uint32_t pump_cold_us = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(sweep_worst_cycles) * 1000000ULL) / clock_hz);
    std::uint32_t worst_us = pump_warm_us;
    const char* worst_is = "pump, warm cache";
    if (tank_us > worst_us)      { worst_us = tank_us;      worst_is = "tank + binding, warm cache"; }
    if (pump_cold_us > worst_us) { worst_us = pump_cold_us; worst_is = "pump, cold cache (sweep)"; }
    std::printf("  pump full cycle, warm     %lu us\n", static_cast<unsigned long>(pump_warm_us));
    std::printf("  tank full cycle, warm     %lu us\n", static_cast<unsigned long>(tank_us));
    std::printf("  pump full cycle, cold     %lu us\n", static_cast<unsigned long>(pump_cold_us));
    std::printf("  worst of the above        %lu us  (%s)\n", static_cast<unsigned long>(worst_us), worst_is);
    if (worst_us > 0U) {
        std::printf("  cycles available at 10 ms  %lu\n", static_cast<unsigned long>(10000U / worst_us));
        std::printf("  cycles available at 1 ms   %lu\n", static_cast<unsigned long>(1000U / worst_us));
        std::printf("  max sustainable rate       %lu Hz\n", static_cast<unsigned long>(1000000U / worst_us));
    }

    std::printf("\nNote what these numbers are and are not. They are the cost of adjudicating a casebook at its\n"
                "worst state on one core of an RP2350 at the clock printed above. They do not include reading real\n"
                "instruments over a real bus, which on a slow Modbus link will dominate everything measured here.\n");

    std::printf("\ndone.\n");
    for (;;) { tight_loop_contents(); }
}
