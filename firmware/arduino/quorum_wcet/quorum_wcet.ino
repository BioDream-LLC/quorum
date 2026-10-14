// quorum on a Cortex-M Arduino: worst-case execution time.
//
// The companion to firmware/pico2/. Same measurement, different vendor, different core, different clock -- which is
// the point. A single board's timing is a fact about that board; two boards from different vendors at different clock
// speeds start being a fact about the software.
//
// The Uno R4 is the interesting comparison: its RA4M1 runs at 48 MHz against the Pico 2's 150 MHz, so if the cycle
// COUNTS are close while the microsecond figures differ by roughly 3x, the numbers are measuring quorum rather than
// measuring RP2350.
//
// VERIFIED: compiles for Cortex-M3, M4, M7, M33 and M0+ under the same warnings-as-errors set as the host build
//           (tools/board-matrix.sh).
// NOT VERIFIED: nothing here has run on hardware.
//
// ── Setup ──────────────────────────────────────────────────────────────────
//
// This is a .ino sketch because the Arduino toolchain expects one, but it needs quorum's headers on the include path,
// which the IDE does not do for arbitrary directories. Two options:
//
//   A. arduino-cli, which takes include paths (recommended). See firmware/arduino/build.sh, which assembles the
//      rather long --build-property invocation from your install paths. In outline it is:
//
//        arduino-cli compile --fqbn arduino:renesas_uno:minima
//          --build-property "compiler.cpp.extra_flags=-std=gnu++17 -I.../solver/include ..."
//          firmware/arduino/quorum_wcet
//
//      The include paths must cover solver, arbiter, binding, bench, and the generated casebook headers in
//      build/casebook/compiler/gen. The host build has to run first, because cbc is a host tool and its exhaustive
//      validation is what identifies the worst-case state this sketch measures.
//
//   B. IDE: copy the four include trees and the generated headers into this sketch folder. It works, and the headers
//      go stale the moment a casebook changes -- which is the exact failure the generated-header design exists to
//      prevent. Use option A.
//
// Boards: arduino:renesas_uno:minima, arduino:mbed_nano:nano33ble, arduino:sam:arduino_due_x, arduino:mbed_giga:giga.
//
// Output: Serial at 115200.

#include <Arduino.h>

// Angle brackets, and a library rather than a sketch subfolder. The Arduino build does not put the sketch folder on
// the include path -- confirmed by reading the compiler command line it generates -- so a header that includes
// another header cannot resolve it from the sketch. A library's src/ IS on the path.
#include <quorum/arbiter/verdict.hpp>
#include <quorum/bench/cycles.hpp>
#include <quorum/binding/binding.hpp>

#include <pump_generated.hpp>
#include <tank_generated.hpp>

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace tank = quorum::casebook::gen::tank_409;
namespace bench = quorum::bench;

using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Status;

// Static storage, as a real integration would have it: nothing allocated in the cycle.
static pump::CaseFramework g_fw;
static pump::CaseSolver g_solver;
static pump::CasePolicy g_policy;
static tank::CaseBinding g_binding;

// Sinks, so the optimiser cannot delete the work being timed.
static volatile int32_t g_sink_action = 0;
static volatile int32_t g_sink_steps = 0;
static volatile uint32_t g_sink_mask = 0;

static const uint32_t kRuns = 2000;

static void report(const char* label, const bench::Measurement& m, uint32_t clock_hz) {
    if (!m.valid) {
        Serial.print("  ");
        Serial.print(label);
        Serial.println("  COUNTER NOT RUNNING");
        return;
    }
    // Integer arithmetic only, matching the discipline of the rest of the device code: no floating point anywhere,
    // which also keeps this working on parts with no FPU.
    const uint32_t mhz = clock_hz / 1000000UL;
    const uint32_t nanos = mhz == 0UL ? 0UL : (m.max_cycles * 1000UL) / mhz;

    Serial.print("  ");
    Serial.print(label);
    Serial.print("  min ");
    Serial.print(m.min_cycles);
    Serial.print("  mean ");
    Serial.print(m.mean_cycles());
    Serial.print("  max ");
    Serial.print(m.max_cycles);
    Serial.print(" cycles   ");
    Serial.print(nanos / 1000UL);
    Serial.print('.');
    const uint32_t frac = nanos % 1000UL;
    if (frac < 100UL) { Serial.print('0'); }
    if (frac < 10UL) { Serial.print('0'); }
    Serial.print(frac);
    Serial.println(" us");
}

/// Compare a measured CYCLE count against a static bound expressed in INSTRUCTIONS.
///
/// These are different units and conflating them was a defect in this file's own instructions, which told the reader
/// that a measured maximum above 2571 meant the machine model was wrong. It does not. The bound is an instruction
/// count; a cycle count on real silicon is almost always higher, because a flash access that stalls costs several
/// cycles and an instruction is still one instruction.
///
/// What the measurement can settle is the CYCLES PER INSTRUCTION assumption. README converts the bound to
/// microseconds at 2 CPI, and that conversion -- not the instruction count -- is the part real hardware can refute.
void report_cpi(const char* label, uint32_t max_cycles, uint32_t bound_instructions) {
    if (max_cycles == 0UL || bound_instructions == 0UL) { return; }
    // Hundredths, in integer arithmetic: no floating point anywhere on the device.
    const uint32_t cpi_x100 = (max_cycles * 100UL) / bound_instructions;
    Serial.print(F("  "));
    Serial.print(label);
    Serial.print(F("  max cycles / "));
    Serial.print(bound_instructions);
    Serial.print(F(" bound instructions = "));
    Serial.print(cpi_x100 / 100UL);
    Serial.print('.');
    if (cpi_x100 % 100UL < 10UL) { Serial.print('0'); }
    Serial.print(cpi_x100 % 100UL);
    Serial.print(F(" cycles per instruction -- "));
    if (cpi_x100 <= 200UL) {
        Serial.println(F("AT OR UNDER the 2 CPI that README assumes"));
    } else {
        Serial.println(F("OVER 2 CPI: the microsecond figures in README are UNDERSTATED"));
    }
}

void setup() {
    Serial.begin(115200);
    // Wait, but not forever: a board with nothing attached to Serial must still run, or a field unit hangs at boot.
    const unsigned long deadline = millis() + 5000UL;
    while (!Serial && millis() < deadline) {}
    delay(500);

    Serial.println();
    Serial.println(F("quorum on Cortex-M Arduino -- worst-case execution time"));
    Serial.println(F("======================================================"));

    // F_CPU is a compile-time constant the Arduino core defines for the selected board.
    const uint32_t clock_hz = F_CPU;
    Serial.print(F("core clock            "));
    Serial.println(clock_hz);
    Serial.print(F("casebook              "));
    Serial.print(pump::kCasebookName);
    Serial.print(F(" v"));
    Serial.println(static_cast<int>(pump::kCasebookVersion));
    Serial.print(F("arguments / attacks   "));
    Serial.print(static_cast<int>(pump::argument_count));
    Serial.print(F(" / "));
    Serial.println(static_cast<int>(pump::kCasebookEdges));
    Serial.print(F("worst-case state      0x"));
    Serial.print(pump::kWorstCaseMask, HEX);
    Serial.print(F("  ("));
    Serial.print(static_cast<int>(pump::kWorstCaseSteps));
    Serial.print(F(" steps, proven bound "));
    Serial.print(static_cast<int>(pump::kStepBound));
    Serial.println(F(")"));
    Serial.println();

    if (!bench::enable()) {
        Serial.println(F("FATAL: no cycle counter started. Every number would be zero, which reads like an"));
        Serial.println(F("       impossibly fast result rather than a broken rig, so nothing is measured."));
        for (;;) {}
    }
    Serial.print(F("cycle counter: "));
    Serial.println(bench::source_name());
    Serial.println();

    if (pump::make_policy(g_policy) != Status::Ok ||
        tank::make_binding(g_binding) != quorum::binding::Status::Ok) {
        Serial.println(F("FATAL: configuration failed"));
        for (;;) {}
    }

    // ── the measurements ────────────────────────────────────────────────────
    const bench::Measurement construct = bench::measure(kRuns, [] {
        const Status s = pump::instantiate(g_fw, pump::kWorstCaseMask);
        g_sink_steps = static_cast<int32_t>(s);
    });
    report("instantiate         ", construct, clock_hz);

    const bench::Measurement solve = bench::measure(kRuns, [] {
        const Status s = g_solver.solve(g_fw);
        g_sink_steps = s == Status::Ok ? g_solver.steps() : -1;
    });
    report("solve               ", solve, clock_hz);

    const bench::Measurement cycle = bench::measure(kRuns, [] {
        if (pump::instantiate(g_fw, pump::kWorstCaseMask) != Status::Ok) { return; }
        const Verdict v = quorum::arbiter::decide(g_fw, g_solver, g_policy);
        g_sink_action = v.action.get();
        g_sink_steps = v.steps;
    });
    report("FULL CYCLE          ", cycle, clock_hz);

    const bench::Measurement binding = bench::measure(kRuns, [] {
        for (int32_t c = 0; c < tank::kChannelCount; ++c) {
            (void)g_binding.sample(c, 45000, 1000UL);
        }
        g_sink_mask = g_binding.mask(1000UL);
    });
    report("binding sample+mask ", binding, clock_hz);

    // ── the comparison this board can actually make ──────────────────────────
    //
    // The static bound in wcet/ is 2,571 INSTRUCTIONS for the pump, counted from Cortex-M33 disassembly at -Os. The
    // same source compiled for Cortex-M4 -- this board's core -- produces an identical instruction count and an
    // identical opcode mix, differing only in scheduling, so the bound transfers to this part unchanged.
    //
    // What does NOT transfer is the microsecond figure. README converts the bound at 2 cycles per instruction, and
    // that assumption is what real silicon can refute: this part runs at 48 MHz from flash with wait states, not
    // from a cache at 150 MHz.
    Serial.println();
    Serial.println(F("  bound check (instructions vs cycles are DIFFERENT UNITS):"));
    // SOLVE, not FULL CYCLE. The 2,571-instruction bound is derived from `quorum_solve`, which is exactly
    // `g.solve(fw)` -- see tools/wcet-disassemble.sh. FULL CYCLE additionally instantiates the framework and runs the
    // arbiter, so dividing its cycles by a solve-only bound compares different scopes and overstates the ratio by
    // roughly four times. The first version of this line did that and reported that README was understated, which
    // was false: it is the third time a tool in this project has been unfair to correct work by mis-scoping a
    // comparison.
    report_cpi("solve               ", solve.max_cycles, 2571UL);

    // The check that has to come before the timings mean anything: if the device does different work than the
    // compiler predicted on the host, the numbers above are measuring something other than the validated casebook.
    Serial.println();
    if (pump::instantiate(g_fw, pump::kWorstCaseMask) == Status::Ok) {
        const Verdict v = quorum::arbiter::decide(g_fw, g_solver, g_policy);
        Serial.print(F("  steps on device "));
        Serial.print(static_cast<int>(v.steps));
        Serial.print(F(", predicted by cbc "));
        Serial.print(static_cast<int>(pump::kWorstCaseSteps));
        Serial.println(v.steps == pump::kWorstCaseSteps ? F("  MATCH") : F("  *** MISMATCH ***"));
        Serial.print(F("  verdict         "));
        Serial.print(outcome_name(v.outcome));
        Serial.print(F(" -> "));
        Serial.println(pump::name_of(v.action.get()));
    }

    Serial.println();
    Serial.print(F("  worst full cycle  "));
    Serial.print(cycle.max_microseconds(clock_hz));
    Serial.println(F(" us"));
    const uint32_t worst_us = cycle.max_microseconds(clock_hz);
    if (worst_us > 0UL) {
        Serial.print(F("  max sustainable   "));
        Serial.print(1000000UL / worst_us);
        Serial.println(F(" Hz"));
    }

    Serial.println();
    Serial.println(F("These are cycles to adjudicate a casebook at its worst state, with nothing else running."));
    Serial.println(F("They do not include reading real instruments, which on a slow bus will dominate them."));
    Serial.println(F("done."));
}

void loop() {}
