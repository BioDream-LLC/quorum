// A LEARNED casebook on an Arduino (any Cortex-M): every input state, on silicon, against the host.
//
// Same program as firmware/pico2/learned.cpp, in Arduino form. The casebook was written by quorum's learner (MONK-1,
// starting from a casebook that knew one of the two published rules), emitted as .cb, validated by cbc, and compiled to
// monks_learned_generated.hpp. This sketch decides every one of the 2^observations states and prints
//
//     DEVICE <name> states <N> asserts <A> fnv1a 0x<hash>
//
// which must equal build/learn/learned/<name>.expected, written on the host from the learner itself. FNV-1a over one
// byte per state, in state order: 1 asserted, 0 default stood, 2 not Decided.
//
// Unlike quorum_wcet, the report repeats from loop() every 3 s, so a serial monitor attached late still gets it.
// Staging: firmware/arduino/stage-portable.sh copies monks_learned_generated.hpp into the quorum library if it exists.
#include <Arduino.h>

#include <quorum/arbiter/verdict.hpp>
#include <quorum/bench/cycles.hpp>

#include <monks_learned_generated.hpp>

namespace cb = quorum::casebook::gen::monks_1_knows_2;
namespace bench = quorum::bench;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Status;

static cb::CaseFramework g_fw;
static cb::CaseSolver g_solver;
static cb::CasePolicy g_policy;
static volatile int32_t g_sink = 0;

static bool g_fingerprint_ok = false;
static bool g_policy_ok = false;
static bool g_counter_ok = false;
static uint32_t g_states = 0;
static uint32_t g_fnv = 2166136261UL;
static uint32_t g_asserts = 0;
static uint32_t g_not_decided = 0;
static uint32_t g_faults = 0;
static int32_t g_max_steps = 0;
static uint32_t g_worst_cycles = 0;
static uint32_t g_worst_mask = 0;
static uint32_t g_report = 0;

static void print_hex8(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        // Lower case, to match the host's %08x exactly; Serial.print(x, HEX) is upper case.
        Serial.print("0123456789abcdef"[(v >> static_cast<uint32_t>(shift)) & 0xFUL]);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    g_fingerprint_ok = cb::verify_casebook();
    g_policy_ok = cb::make_policy(g_policy) == Status::Ok;
    g_counter_ok = bench::enable();
    g_states = 1UL << cb::kObservationCount;
    if (!g_fingerprint_ok || !g_policy_ok) { return; }
    for (uint32_t m = 0; m < g_states; ++m) {
        Verdict v{};
        bool built = true;
        const auto probe = bench::measure(4U, [m, &v, &built] {
            built = cb::instantiate(g_fw, m) == Status::Ok;
            if (!built) { return; }
            v = quorum::arbiter::decide(g_fw, g_solver, g_policy);
            g_sink = v.steps;
        });
        if (!built) {
            ++g_faults;
            continue;
        }
        const bool decided = v.outcome == Outcome::Decided;
        const bool asserted = decided && v.action.valid() && v.action.get() == cb::kSafeAction;
        const uint32_t byte = !decided ? 2UL : (asserted ? 1UL : 0UL);
        g_fnv = (g_fnv ^ byte) * 16777619UL;
        if (asserted) { ++g_asserts; }
        if (!decided) { ++g_not_decided; }
        if (v.steps > g_max_steps) { g_max_steps = v.steps; }
        if (probe.valid && probe.max_cycles > g_worst_cycles) {
            g_worst_cycles = probe.max_cycles;
            g_worst_mask = m;
        }
    }
}

void loop() {
    const uint32_t hz = SystemCoreClock;
    const uint32_t mhz = hz / 1000000UL == 0UL ? 1UL : hz / 1000000UL;
    const uint32_t nanos = static_cast<uint32_t>((static_cast<uint64_t>(g_worst_cycles) * 1000ULL) / mhz);
    Serial.print(F("\n=== quorum: a LEARNED casebook on Arduino (report "));
    Serial.print(g_report++);
    Serial.println(F(") ==="));
    Serial.print(F("casebook        ")); Serial.println(cb::kCasebookName);
    Serial.print(F("core clock      ")); Serial.println(hz);
    Serial.print(F("arguments       ")); Serial.print(static_cast<int>(cb::kMaxArgs));
    Serial.print(F(", attacks ")); Serial.print(static_cast<int>(cb::kCasebookEdges));
    Serial.print(F(", observations ")); Serial.println(static_cast<int>(cb::kObservationCount));
    Serial.print(F("fingerprint     ")); Serial.println(g_fingerprint_ok ? F("OK") : F("MISMATCH"));
    Serial.print(F("policy          ")); Serial.println(g_policy_ok ? F("OK") : F("FAILED"));
    Serial.print(F("cycle counter   ")); Serial.println(g_counter_ok ? bench::source_name() : "NOT RUNNING");
    Serial.print(F("not Decided     ")); Serial.print(g_not_decided);
    Serial.print(F("   construction faults ")); Serial.println(g_faults);
    Serial.print(F("steps           max ")); Serial.print(g_max_steps);
    Serial.print(F(", proven bound ")); Serial.print(static_cast<int>(cb::kStepBound));
    Serial.println(g_max_steps <= cb::kStepBound ? F(", WITHIN") : F(", OVER"));
    Serial.print(F("worst cycle     ")); Serial.print(g_worst_cycles);
    Serial.print(F(" cycles = ")); Serial.print(nanos / 1000UL); Serial.print('.');
    const uint32_t frac = nanos % 1000UL;
    if (frac < 100UL) { Serial.print('0'); }
    if (frac < 10UL) { Serial.print('0'); }
    Serial.print(frac);
    Serial.print(F(" us (state 0x")); Serial.print(g_worst_mask, HEX); Serial.println(F(", cold, instantiate+decide)"));
    Serial.print(F("DEVICE ")); Serial.print(cb::kCasebookName);
    Serial.print(F(" states ")); Serial.print(g_states);
    Serial.print(F(" asserts ")); Serial.print(g_asserts);
    Serial.print(F(" fnv1a 0x")); print_hex8(g_fnv);
    Serial.println();
    delay(3000);
}
