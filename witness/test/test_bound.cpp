// Is the witness bound actually exact?
//
// `Derivation` documents that each argument is expanded at most once, so `MaxSteps == argument_count` can never
// truncate. cbc now emits that bound as `kMaxWitnessSteps`, and a device sizes a 5.5 KB object down to about 1.2 KB
// on the strength of it. A documented claim that firmware depends on is worth machine-checking.
//
// So: over every state of both casebooks, reconstruct the derivation of EVERY argument -- not just the actions, since
// an operator may reasonably ask why any claim came out as it did -- at exactly the emitted bound, and assert that
// `truncated()` is never set. A wrong bound would make a device emit half an explanation, which is worse than none
// because it reads like a whole one.

#include <cstdint>
#include <cstdio>

#include "pump_generated.hpp"
#include "quorum/witness/derivation.hpp"
#include "tank_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace tank = quorum::casebook::gen::tank_409;

using quorum::solver::Arg;
using quorum::solver::Status;

namespace {

int g_failures = 0;

template <typename Fw, typename Solver, typename Derivation>
void sweep(const char* name,
           Status (*instantiate)(Fw&, std::uint32_t),
           std::int32_t argument_count,
           std::int32_t observation_count,
           std::int32_t closed_world) {
    const std::uint32_t total = 1U << static_cast<std::uint32_t>(observation_count);

    std::int32_t max_steps = 0;
    std::int32_t max_depth = 0;
    std::int32_t truncations = 0;
    std::int64_t reconstructions = 0;
    std::int32_t max_assumed = 0;

    static Fw fw;
    static Solver solver;
    static Derivation d;

    for (std::uint32_t mask = 0; mask < total; ++mask) {
        if (instantiate(fw, mask) != Status::Ok || solver.solve(fw) != Status::Ok) {
            std::printf("  FAIL  setup at state %u\n", mask);
            ++g_failures;
            return;
        }

        for (std::int32_t a = 0; a < argument_count; ++a) {
            if (d.build(fw, solver, Arg{a}, Arg{closed_world}) != Status::Ok) {
                std::printf("  FAIL  derivation failed for argument %d at state %u\n", a, mask);
                ++g_failures;
                return;
            }
            ++reconstructions;
            if (d.truncated()) { ++truncations; }
            if (d.step_count() > max_steps) { max_steps = d.step_count(); }
            if (d.assumed_steps() > max_assumed) { max_assumed = d.assumed_steps(); }
            for (std::int32_t s = 0; s < d.step_count(); ++s) {
                if (d.step(s).depth > max_depth) { max_depth = d.step(s).depth; }
            }
        }
    }

    std::printf("\n%s: %d arguments, %u states, %lld reconstructions\n", name, argument_count, total,
                static_cast<long long>(reconstructions));
    std::printf("  worst observed step count   %d   (emitted bound %d)\n", max_steps, argument_count);
    std::printf("  worst observed depth        %d\n", max_depth);
    std::printf("  most assumed absences       %d\n", max_assumed);
    std::printf("  object size at this bound   %d bytes\n", static_cast<int>(sizeof(Derivation)));

    if (truncations == 0) {
        std::printf("  OK    never truncated at the emitted bound\n");
    } else {
        std::printf("  FAIL  truncated in %d reconstructions -- the bound is WRONG and firmware relies on it\n",
                    truncations);
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("witness bound: is MaxSteps == argument_count exact?\n");

    sweep<pump::CaseFramework, pump::CaseSolver,
          quorum::witness::Derivation<pump::kMaxWitnessSteps, pump::kMaxArgs>>(
        "pump", &pump::instantiate, pump::argument_count, pump::kObservationCount, pump::kClosedWorld);

    sweep<tank::CaseFramework, tank::CaseSolver,
          quorum::witness::Derivation<tank::kMaxWitnessSteps, tank::kMaxArgs>>(
        "tank", &tank::instantiate, tank::argument_count, tank::kObservationCount, tank::kClosedWorld);

    // And the comparison that motivated this: the oversized object a hand-written integration would have used.
    std::printf("\nRAM saved by emitting the bound instead of guessing:\n");
    std::printf("  Derivation<256, 44>  %5d bytes   (a plausible hand-picked value)\n",
                static_cast<int>(sizeof(quorum::witness::Derivation<256, 44>)));
    std::printf("  Derivation<44, 44>   %5d bytes   (the emitted bound)\n",
                static_cast<int>(sizeof(quorum::witness::Derivation<44, 44>)));
    std::printf("  saving               %5d bytes\n",
                static_cast<int>(sizeof(quorum::witness::Derivation<256, 44>) -
                                 sizeof(quorum::witness::Derivation<44, 44>)));

    std::printf("\n%s\n", g_failures == 0 ? "the emitted bound holds" : "BOUND VIOLATED");
    return g_failures == 0 ? 0 : 1;
}
