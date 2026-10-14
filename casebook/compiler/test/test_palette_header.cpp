// A palette on one device: one framework, one solve, two independent verdicts.
//
// The palette guarantee has been argued structurally (no cross-casebook edge, each member preserved exactly) and
// confirmed by sweeping all 33.5 million pump+tank states in `test_palette.cpp`. This tests the thing those two do
// not: that the EMITTED header works, and that a device using it gets the same answers it would get from the two
// casebooks running separately.
//
// The distinction matters because the emitted palette is a different artefact from the merged `Lowered` the other
// tests examine. An off-by-one in an observation offset, a policy pointing at the wrong index, a closed-world
// argument attached to the wrong member -- none of those would show up in a structural check of the merge, and all
// of them would be catastrophic in the field and silent on a host.

#include <cstdint>
#include <cstdio>
#include <string>

#include "palette_generated.hpp"
#include "pump_generated.hpp"
#include "quorum/arbiter/verdict.hpp"
#include "tank_generated.hpp"

namespace pal = quorum::casebook::gen::palette;
namespace pump_alone = quorum::casebook::gen::centrifugal_pump;
namespace tank_alone = quorum::casebook::gen::tank_409;

using quorum::arbiter::Outcome;
using quorum::arbiter::Verdict;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

int g_failures = 0;

void ok(bool condition, const char* what) {
    std::printf("  %s  %s\n", condition ? "OK  " : "FAIL", what);
    if (!condition) { ++g_failures; }
}

/// The name of an action, stripped of its casebook qualifier, so the two sides can be compared by meaning.
const char* bare(const char* qualified) {
    const char* dot = qualified;
    for (const char* c = qualified; *c != '\0'; ++c) {
        if (*c == '.') { dot = c + 1; }
    }
    return dot;
}

bool same_action(const char* palette_name, const char* standalone_name) {
    const char* a = bare(palette_name);
    const char* b = standalone_name;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) { return false; }
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

int main() {
    std::printf("a palette on one device: %d casebooks, %d arguments, %d observations\n", pal::kCasebookCount,
                pal::kArgumentCount, pal::kObservationCount);

    static pal::CaseFramework fw;
    static pal::CaseSolver solver;
    static pal::centrifugal_pump::CasePolicy pump_policy;
    static pal::tank_409::CasePolicy tank_policy;

    ok(pal::centrifugal_pump::make_policy(pump_policy) == Status::Ok, "the pump policy configures");
    ok(pal::tank_409::make_policy(tank_policy) == Status::Ok, "the tank policy configures");

    static pump_alone::CaseFramework pfw;
    static pump_alone::CaseSolver psolver;
    static pump_alone::CasePolicy ppolicy;
    static tank_alone::CaseFramework tfw;
    static tank_alone::CaseSolver tsolver;
    static tank_alone::CasePolicy tpolicy;
    ok(pump_alone::make_policy(ppolicy) == Status::Ok, "the standalone pump policy configures");
    ok(tank_alone::make_policy(tpolicy) == Status::Ok, "the standalone tank policy configures");

    // Sweep every pump state against a spread of tank states and vice versa. Not the full 2^29 cross product -- that
    // is what test_palette.cpp is for, and the structural argument in cbc makes it unnecessary per build -- but wide
    // enough that any offset error shows up immediately.
    const std::uint32_t pump_states = 1U << static_cast<std::uint32_t>(pal::centrifugal_pump::kObservationCount);
    const std::uint32_t tank_states = 1U << static_cast<std::uint32_t>(pal::tank_409::kObservationCount);

    std::int64_t compared = 0;
    std::int32_t pump_differs = 0;
    std::int32_t tank_differs = 0;
    std::int32_t worst_steps = 0;
    std::int32_t worst_alone = 0;

    // A deterministic spread rather than a prefix: taking the first N tank states would only ever exercise low bits.
    auto spread = [](std::uint32_t i, std::uint32_t limit) { return (i * 2654435761U) % limit; };

    for (std::uint32_t pm = 0; pm < pump_states; ++pm) {
        for (std::uint32_t k = 0; k < 8; ++k) {
            const std::uint32_t tm = spread(pm * 8U + k, tank_states);

            const std::uint32_t combined = pal::centrifugal_pump::shift(pm) | pal::tank_409::shift(tm);

            if (pal::instantiate(fw, combined) != Status::Ok || solver.solve(fw) != Status::Ok) {
                std::printf("  FAIL  palette construction at pump=%u tank=%u\n", pm, tm);
                ++g_failures;
                return 1;
            }

            // ONE solve, TWO verdicts. That is the whole deployment model.
            const Verdict pv = quorum::arbiter::decide(fw, solver, pump_policy);
            const Verdict tv = quorum::arbiter::decide(fw, solver, tank_policy);
            if (solver.steps() > worst_steps) { worst_steps = solver.steps(); }

            if (pump_alone::instantiate(pfw, pm) != Status::Ok) { return 1; }
            const Verdict pa = quorum::arbiter::decide(pfw, psolver, ppolicy);
            if (tank_alone::instantiate(tfw, tm) != Status::Ok) { return 1; }
            const Verdict ta = quorum::arbiter::decide(tfw, tsolver, tpolicy);
            if (pa.steps > worst_alone) { worst_alone = pa.steps; }
            if (ta.steps > worst_alone) { worst_alone = ta.steps; }

            ++compared;

            if (pv.outcome != pa.outcome ||
                !same_action(pal::name_of(pv.action.get()), pump_alone::name_of(pa.action.get()))) {
                if (pump_differs == 0) {
                    std::printf("  FAIL  pump differs at pump=%u tank=%u: palette %s/%s vs alone %s/%s\n", pm, tm,
                                outcome_name(pv.outcome), pal::name_of(pv.action.get()), outcome_name(pa.outcome),
                                pump_alone::name_of(pa.action.get()));
                }
                ++pump_differs;
            }
            if (tv.outcome != ta.outcome ||
                !same_action(pal::name_of(tv.action.get()), tank_alone::name_of(ta.action.get()))) {
                if (tank_differs == 0) {
                    std::printf("  FAIL  tank differs at pump=%u tank=%u: palette %s/%s vs alone %s/%s\n", pm, tm,
                                outcome_name(tv.outcome), pal::name_of(tv.action.get()), outcome_name(ta.outcome),
                                tank_alone::name_of(ta.action.get()));
                }
                ++tank_differs;
            }
        }
    }

    std::printf("\n%lld state pairs compared\n", static_cast<long long>(compared));
    ok(pump_differs == 0, "the pump casebook gives identical verdicts in the palette and alone");
    ok(tank_differs == 0, "the tank casebook gives identical verdicts in the palette and alone");

    std::printf("\n  worst-case work, one solve serving both casebooks   %d steps\n", worst_steps);
    std::printf("  worst-case work, the larger casebook alone          %d steps\n", worst_alone);
    std::printf("  Note the union costs roughly the SUM, not the maximum: grounded is linear in arguments plus\n"
                "  attacks, so merging saves no reasoning work. What it saves is one framework object, one solver,\n"
                "  and one code path -- and since the framework is the largest RAM item on the device, that is\n"
                "  the saving that matters. The policies stay separate because merging them would be wrong.\n");

    // Names must stay qualified in the palette, because a witness that says "shutdown" on a device supervising a
    // pump and a tank has told the operator almost nothing.
    ok(std::string(pal::name_of(0)).find('.') != std::string::npos,
       "palette argument names are qualified by casebook, so a witness says which asset it means");

    std::printf("\n%s\n", g_failures == 0 ? "the emitted palette behaves exactly as the separate casebooks do"
                                          : "PALETTE HEADER DEFECT");
    return g_failures == 0 ? 0 : 1;
}
