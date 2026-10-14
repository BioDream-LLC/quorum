// A LEARNED casebook on a Raspberry Pi Pico 2: every input state, on silicon, against the host.
//
// The casebook in flash was written by quorum's learner (learn/), not by a person: AA-CBR over the MONK-1 training
// set, starting from a casebook that knew only one of the two published rules. It was emitted as .cb source, compiled
// and exhaustively validated by cbc, and turned into the header included below -- the same path as pump.cb.
//
// This program decides every one of the 2^observations states and prints one line in exactly the format of the host's
// `<name>.expected` file (written by test_learned_casebook), so the two can be compared by eye or by diff:
//
//     <name> states <N> asserts <A> fnv1a 0x<hash>
//
// The hash is FNV-1a over one byte per state, in state order: 1 if the casebook asserted its outcome, 0 if the default
// stood, 2 if the state was not Decided. Equal lines mean the device decided every state exactly as the learner did.
// It also times every state, cold, and reports the worst against the proven step bound.
//
// Printed repeatedly, every few seconds, so a listener that attaches late still captures a complete report.
#include <cstdint>
#include <cstdio>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "quorum/arbiter/verdict.hpp"
#include "quorum/bench/cycles.hpp"

#include QUORUM_LEARNED_HEADER

namespace cb = QUORUM_LEARNED_NAMESPACE;
namespace bench = quorum::bench;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::solver::Status;

namespace {

cb::CaseFramework g_fw;
cb::CaseSolver g_solver;
cb::CasePolicy g_policy;

volatile std::int32_t g_sink = 0;

}  // namespace

int main() {
    stdio_init_all();
    sleep_ms(2000);
    const std::uint32_t clock_hz = clock_get_hz(clk_sys);

    const bool fingerprint_ok = cb::verify_casebook();
    const bool policy_ok = cb::make_policy(g_policy) == Status::Ok;
    const bool counter_ok = bench::enable();

    const std::uint32_t states = 1UL << cb::kObservationCount;
    std::uint32_t fnv = 2166136261U;
    std::uint32_t asserts = 0;
    std::uint32_t not_decided = 0;
    std::uint32_t faults = 0;
    std::int32_t max_steps = 0;
    std::uint32_t worst_cycles = 0;
    std::uint32_t worst_mask = 0;

    if (fingerprint_ok && policy_ok) {
        for (std::uint32_t m = 0; m < states; ++m) {
            Verdict v{};
            bool built = true;
            // Four runs: the first is cold, which is the one an integrator must budget for.
            const auto probe = bench::measure(4U, [m, &v, &built] {
                built = cb::instantiate(g_fw, m) == Status::Ok;
                if (!built) { return; }
                v = quorum::arbiter::decide(g_fw, g_solver, g_policy);
                g_sink = v.steps;
            });
            if (!built) {
                ++faults;
                continue;
            }
            const bool asserted = v.outcome == Outcome::Decided && v.action.valid() && v.action.get() == cb::kSafeAction;
            const std::uint32_t byte = v.outcome != Outcome::Decided ? 2U : (asserted ? 1U : 0U);
            fnv = (fnv ^ byte) * 16777619U;
            asserts += asserted ? 1U : 0U;
            not_decided += v.outcome != Outcome::Decided ? 1U : 0U;
            if (v.steps > max_steps) { max_steps = v.steps; }
            if (probe.valid && probe.max_cycles > worst_cycles) {
                worst_cycles = probe.max_cycles;
                worst_mask = m;
            }
        }
    }

    const std::uint64_t mhz = clock_hz / 1000000U;
    const std::uint64_t nanos = (static_cast<std::uint64_t>(worst_cycles) * 1000U) / (mhz == 0U ? 1U : mhz);
    for (unsigned report = 0;; ++report) {
        std::printf("\n=== quorum: a LEARNED casebook on RP2350 (report %u) ===\n", report);
        std::printf("casebook        %s v%d\n", cb::kCasebookName, static_cast<int>(cb::kCasebookVersion));
        std::printf("source          %s\n", cb::kSourceDocument);
        std::printf("written by      %s\n", cb::kSourceAuthor);
        std::printf("arguments       %d, attacks %d, observations %d\n", static_cast<int>(cb::kMaxArgs),
                    static_cast<int>(cb::kCasebookEdges), static_cast<int>(cb::kObservationCount));
        std::printf("fingerprint     %s\n", fingerprint_ok ? "OK" : "MISMATCH -- not the casebook cbc validated");
        std::printf("policy          %s\n", policy_ok ? "OK" : "FAILED");
        std::printf("cycle counter   %s\n", counter_ok ? bench::source_name() : "NOT RUNNING");
        std::printf("not Decided     %lu   construction faults %lu\n", static_cast<unsigned long>(not_decided),
                    static_cast<unsigned long>(faults));
        std::printf("steps           max %d, proven bound %d, %s\n", static_cast<int>(max_steps),
                    static_cast<int>(cb::kStepBound), max_steps <= cb::kStepBound ? "WITHIN" : "OVER");
        std::printf("worst cycle     %lu cycles = %lu.%03lu us at %lu Hz (state 0x%lX, cold, instantiate+decide)\n",
                    static_cast<unsigned long>(worst_cycles), static_cast<unsigned long>(nanos / 1000U),
                    static_cast<unsigned long>(nanos % 1000U), static_cast<unsigned long>(clock_hz),
                    static_cast<unsigned long>(worst_mask));
        std::printf("DEVICE %s states %lu asserts %lu fnv1a 0x%08lx\n", cb::kCasebookName,
                    static_cast<unsigned long>(states), static_cast<unsigned long>(asserts),
                    static_cast<unsigned long>(fnv));
        sleep_ms(3000);
    }
}
