// A static worst-case bound on the safety function, derived from the target binary.
//
// ── Why this exists, given that hardware is on the way ──────────────────────
//
// A measured maximum over 2000 runs is not a worst-case execution time. It is the largest value observed on the paths
// that happened to execute, and a certification argument cannot rest on it: the whole point of WCET is the path nobody
// exercised. Measurement can only ever *refute* a bound or fail to.
//
// So the order matters. Derive the bound statically, then let the board validate it. If the measured maximum comes in
// under the static bound, both numbers gain credibility. If it comes in over, the static model is wrong and that is a
// finding worth having before anyone trusts either.
//
// ── The method: implicit path enumeration ───────────────────────────────────
//
// Standard WCET practice, and it applies cleanly here only because of properties the engine was built to have: no
// recursion, no dynamic dispatch, no allocation, and every loop bounded by a proven expression rather than by data.
//
//   worst instructions  =  prologue + epilogue
//                       +  memset(label_, n)
//                       +  L1_iterations x L1_cost
//                       +  L2_iterations x L2_worst_cost
//                       +  L3_iterations x L3_cost
//                       +  L4_iterations x L4_worst_cost
//                       +  L5_iterations x L5_worst_cost
//
// The per-loop instruction costs are counted from the actual Cortex-M33 disassembly at -Os (see wcet/MACHINE.md). The
// iteration counts are not estimated: they are computed exactly from the solved labelling, because the algorithm makes
// each one a closed form.
//
//   L1 = n                         every argument initialised once
//   L2 = n                         every argument tested for seeding once
//   L3 = |In|                      an argument is pushed only on Undecided->In, which happens at most once
//   L4 = sum of out-degrees of In arguments      each is dequeued exactly once
//   L5 = sum of out-degrees of Out arguments     each is set Out exactly once, guarded by its label
//
// That last pair is what makes this exhaustive rather than sampled: the counts are derivable for EVERY state without
// executing anything, so the maximum can be taken over all of them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quorum/solver/grounded.hpp"

#include "permit_generated.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"

using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

// ── the machine model ───────────────────────────────────────────────────────
//
// Instruction counts read off the Cortex-M33 disassembly of Grounded<44>::solve at -Os, GCC 12.2.1. Each loop is
// charged its most expensive path, which is what makes this an upper bound rather than a guess. Where a loop has
// several exits, the cost below is the longest.
//
// Reproduce with: tools/wcet-disassemble.sh
struct Model {
    // Entry, the finalized() check, ten register saves, the memset call site, and the epilogue. Counted from the
    // disassembly at 0x0-0x36 and 0x5a-0x60.
    std::int32_t prologue_epilogue = 21;

    // The compiler turns `label_[i] = Undecided` into memset(label_, 0, n). newlib's memset is not in this translation
    // unit, so it is bounded generously rather than counted.
    std::int32_t memset_fixed = 10;
    std::int32_t memset_per_word = 2;

    // L1: undecided_[i] = in_degree(i). 2 header + 4 body.
    std::int32_t l1 = 6;

    // L2: the seeding test. 2 header + 13 body on the expensive path (an argument actually seeded), 4 when not.
    // Note L2 has no step counter, which is why `steps_` was never a measure of total work.
    std::int32_t l2_worst = 15;

    // L3: dequeue and load the out-edge range. 3 header + 7 body.
    std::int32_t l3 = 10;

    // L4: load target, test its label. 2 header + 6 body on the dearer (continue) path.
    std::int32_t l4_worst = 8;

    // L5: decrement undecided_, and push on the Undecided->In transition. 2 header + 22 body at worst.
    std::int32_t l5_worst = 24;
};

/// Exact loop iteration counts for one state, computed from the solved labelling.
struct Counts {
    std::int32_t n = 0;
    std::int32_t l1 = 0;
    std::int32_t l2 = 0;
    std::int32_t l3 = 0;
    std::int64_t l4 = 0;
    std::int64_t l5 = 0;
    std::int32_t steps = 0;

    /// Total loop iterations: a machine-INDEPENDENT measure of work.
    ///
    /// This matters for a practical reason. cbc emits the state that firmware should measure, and cbc must not contain
    /// an ARM cost model -- it compiles casebooks, it does not know what silicon they will run on. If maximising total
    /// iterations picks the same state as maximising ARM instructions, then cbc has a portable rule it can apply and
    /// the machine-specific analysis stays here.
    [[nodiscard]] std::int64_t iterations() const {
        return static_cast<std::int64_t>(l1) + l2 + l3 + l4 + l5;
    }

    [[nodiscard]] std::int64_t instructions(const Model& m) const {
        const std::int64_t words = (n + 3) / 4;
        return m.prologue_epilogue + m.memset_fixed + words * m.memset_per_word + static_cast<std::int64_t>(l1) * m.l1 +
               static_cast<std::int64_t>(l2) * m.l2_worst + static_cast<std::int64_t>(l3) * m.l3 + l4 * m.l4_worst +
               l5 * m.l5_worst;
    }
};

template <typename Fw, typename Solver>
Counts count_state(Fw& fw, Solver& solver) {
    Counts c;
    c.n = fw.argument_count();
    c.l1 = c.n;
    c.l2 = c.n;
    c.steps = solver.steps();

    for (std::int32_t a = 0; a < c.n; ++a) {
        const std::int32_t out_degree = fw.out_end(Arg{a}) - fw.out_begin(Arg{a});
        switch (solver.label(Arg{a})) {
            case Label::In:
                // Pushed exactly once, therefore dequeued exactly once, therefore its out-edges walked once.
                ++c.l3;
                c.l4 += out_degree;
                break;
            case Label::Out:
                // Set Out exactly once -- the write is guarded by the label being Undecided -- so L5 runs once over
                // its out-edges.
                c.l5 += out_degree;
                break;
            case Label::Undecided:
                break;
        }
    }
    return c;
}

/// The rigorous answer to "which state should firmware measure?".
///
/// L1 and L2 always run exactly n times, so they are a constant per casebook. Instruction count is therefore
///
///     cost(state) = constant + c3*l3 + c4*l4 + c5*l5
///
/// with c3, c4, c5 all strictly positive on any machine. Cost is therefore monotone increasing in each of l3, l4, l5,
/// which means the maximising state -- for EVERY possible positive cost model, on every silicon -- must lie on the
/// Pareto frontier of those three coordinates.
///
/// So cbc does not need a cost model and does not need to guess. It needs to emit the Pareto-maximal states. If that
/// set is small, the firmware measures each and reports the maximum, and the result is correct by construction rather
/// than by a heuristic that happened to work on three casebooks.
struct Point {
    std::int32_t l3 = 0;
    std::int64_t l4 = 0;
    std::int64_t l5 = 0;
    std::uint32_t mask = 0;

    [[nodiscard]] bool dominates(const Point& o) const {
        return l3 >= o.l3 && l4 >= o.l4 && l5 >= o.l5 && (l3 > o.l3 || l4 > o.l4 || l5 > o.l5);
    }
};

struct Worst {
    std::string name;
    std::int32_t states = 0;

    std::uint32_t mask_by_instructions = 0;
    std::int64_t max_instructions = 0;

    std::uint32_t mask_by_steps = 0;
    std::int32_t max_steps = 0;

    std::uint32_t mask_by_iterations = 0;
    std::int64_t max_iterations = 0;

    // Candidate portable rules. The expensive loop is L5 at 27 instructions per iteration against L1's 8, so an
    // unweighted iteration count is the wrong summary. These test whether some simple structural quantity -- one cbc
    // could compute without knowing the target -- happens to pick the same state the ARM cost model does.
    std::uint32_t mask_by_l5 = 0;
    std::int64_t max_l5 = 0;
    std::uint32_t mask_by_l4_l5 = 0;
    std::int64_t max_l4_l5 = 0;
    std::uint32_t mask_by_attacks = 0;
    std::int32_t max_attacks = 0;

    std::vector<Point> pareto;

    /// Instructions at the state the firmware currently measures.
    std::int64_t instructions_at_declared_worst = 0;
    std::uint32_t declared_worst_mask = 0;

    std::int32_t step_bound = 0;
    std::int32_t arguments = 0;
    std::int32_t attacks_at_worst = 0;
};

template <typename Fw, typename Solver, typename Instantiate>
Worst analyse(const std::string& name, std::int32_t observations, std::int32_t step_bound,
              std::uint32_t declared_worst, Instantiate instantiate, const Model& model) {
    Worst w;
    w.name = name;
    w.step_bound = step_bound;
    w.declared_worst_mask = declared_worst;

    Fw fw;
    Solver solver;
    std::vector<Point> all;

    const std::int32_t states = 1 << observations;
    w.states = states;

    for (std::int32_t s = 0; s < states; ++s) {
        const std::uint32_t mask = static_cast<std::uint32_t>(s);
        if (instantiate(fw, mask) != Status::Ok) { continue; }
        if (solver.solve(fw) != Status::Ok) { continue; }

        const Counts c = count_state(fw, solver);
        const std::int64_t instr = c.instructions(model);

        if (instr > w.max_instructions) {
            w.max_instructions = instr;
            w.mask_by_instructions = mask;
            w.arguments = c.n;
            w.attacks_at_worst = fw.attack_count();
        }
        if (c.steps > w.max_steps) {
            w.max_steps = c.steps;
            w.mask_by_steps = mask;
        }
        if (c.iterations() > w.max_iterations) {
            w.max_iterations = c.iterations();
            w.mask_by_iterations = mask;
        }
        if (c.l5 > w.max_l5) {
            w.max_l5 = c.l5;
            w.mask_by_l5 = mask;
        }
        if (c.l4 + c.l5 > w.max_l4_l5) {
            w.max_l4_l5 = c.l4 + c.l5;
            w.mask_by_l4_l5 = mask;
        }
        if (fw.attack_count() > w.max_attacks) {
            w.max_attacks = fw.attack_count();
            w.mask_by_attacks = mask;
        }
        if (mask == declared_worst) { w.instructions_at_declared_worst = instr; }

        Point p;
        p.l3 = c.l3;
        p.l4 = c.l4;
        p.l5 = c.l5;
        p.mask = mask;
        all.push_back(p);
    }

    // Reduce to the Pareto frontier. O(k^2) on distinct points, which is fine: the coordinate space is small so the
    // number of DISTINCT triples is far below the number of states.
    std::vector<Point> distinct;
    for (const Point& p : all) {
        bool seen = false;
        for (const Point& d : distinct) {
            if (d.l3 == p.l3 && d.l4 == p.l4 && d.l5 == p.l5) {
                seen = true;
                break;
            }
        }
        if (!seen) { distinct.push_back(p); }
    }
    for (const Point& p : distinct) {
        bool dominated = false;
        for (const Point& q : distinct) {
            if (q.dominates(p)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) { w.pareto.push_back(p); }
    }
    return w;
}

}  // namespace

int main() {
    const Model model;
    int failures = 0;

    std::printf("static worst-case bound, derived from the Cortex-M33 binary\n");
    std::printf("===========================================================\n\n");

    namespace pump = quorum::casebook::gen::centrifugal_pump;
    namespace tank = quorum::casebook::gen::tank_409;
    namespace permit = quorum::casebook::gen::piper_permit;

    std::vector<Worst> results;
    results.push_back(analyse<pump::CaseFramework, pump::CaseSolver>(
        "pump", pump::kObservationCount, pump::kStepBound, pump::kWorstCaseMask,
        [](pump::CaseFramework& f, std::uint32_t m) { return pump::instantiate(f, m); }, model));
    results.push_back(analyse<tank::CaseFramework, tank::CaseSolver>(
        "tank-409", tank::kObservationCount, tank::kStepBound, tank::kWorstCaseMask,
        [](tank::CaseFramework& f, std::uint32_t m) { return tank::instantiate(f, m); }, model));
    results.push_back(analyse<permit::CaseFramework, permit::CaseSolver>(
        "piper-permit", permit::kObservationCount, permit::kStepBound, permit::kWorstCaseMask,
        [](permit::CaseFramework& f, std::uint32_t m) { return permit::instantiate(f, m); }, model));

    for (const Worst& w : results) {
        std::printf("%s\n", w.name.c_str());
        std::printf("  states examined                    %d\n", w.states);
        std::printf("  worst-case INSTRUCTIONS            %lld   at state 0x%x\n",
                    static_cast<long long>(w.max_instructions), w.mask_by_instructions);
        std::printf("  worst-case steps_                  %d of a %d bound, at state 0x%x\n", w.max_steps,
                    w.step_bound, w.mask_by_steps);

        // The check this analysis exists to make. `steps_` weights five loops equally and does not count the seeding
        // loop at all, so the state that maximises it need not be the state that maximises work.
        std::printf("  worst-case iterations              %lld  at state 0x%x\n",
                    static_cast<long long>(w.max_iterations), w.mask_by_iterations);

        if (w.mask_by_instructions != w.mask_by_steps) {
            const double under = 100.0 * static_cast<double>(w.max_instructions - w.instructions_at_declared_worst) /
                                 static_cast<double>(w.max_instructions);
            std::printf("  ** steps_ picks the WRONG state to measure **\n");
            std::printf("     firmware measures 0x%x (%lld instructions); true worst is 0x%x (%lld), %.1f%% higher\n",
                        w.declared_worst_mask, static_cast<long long>(w.instructions_at_declared_worst),
                        w.mask_by_instructions, static_cast<long long>(w.max_instructions), under);
        }

        std::printf("  distinct (l3,l4,l5) triples reduced to a Pareto frontier of %zu states\n", w.pareto.size());
        bool worst_on_frontier = false;
        for (const Point& p : w.pareto) {
            if (p.mask == w.mask_by_instructions) { worst_on_frontier = true; }
        }
        std::printf("  instruction-worst state on the frontier: %s\n", worst_on_frontier ? "yes" : "NO -- bug");
        if (!worst_on_frontier) { ++failures; }
        if (w.pareto.size() <= 12) {
            std::printf("  frontier:");
            for (const Point& p : w.pareto) {
                std::printf(" 0x%x(%d,%lld,%lld)", p.mask, p.l3, static_cast<long long>(p.l4),
                            static_cast<long long>(p.l5));
            }
            std::printf("\n");
        }

        std::printf("  candidate portable rules:  max L5 -> 0x%x   max L4+L5 -> 0x%x   max attacks -> 0x%x\n",
                    w.mask_by_l5, w.mask_by_l4_l5, w.mask_by_attacks);

        // Is total-iterations a valid portable substitute for the ARM cost model?
        if (w.mask_by_iterations == w.mask_by_instructions) {
            std::printf("  iterations and instructions agree -> cbc can pick the state portably\n");
        } else {
            std::printf("  ** iterations and instructions DISAGREE (0x%x vs 0x%x) **\n",
                        w.mask_by_iterations, w.mask_by_instructions);
        }

        // Cycle bounds, under an explicit and deliberately pessimistic machine model. Stated as a range because the
        // memory system, not the core, is the uncertainty: RP2350 executes from external QSPI flash through a cache,
        // and a miss costs far more than an instruction.
        const double at_1_0 = static_cast<double>(w.max_instructions);
        const double at_1_5 = at_1_0 * 1.5;
        const double at_2_0 = at_1_0 * 2.0;
        std::printf("  cycles at 1.0/1.5/2.0 CPI          %.0f / %.0f / %.0f\n", at_1_0, at_1_5, at_2_0);
        std::printf("  microseconds at 150 MHz            %.1f / %.1f / %.1f\n", at_1_0 / 150.0, at_1_5 / 150.0,
                    at_2_0 / 150.0);
        std::printf("\n");
    }

    std::int64_t overall = 0;
    for (const Worst& w : results) { overall = std::max(overall, w.max_instructions); }

    std::printf("─────────────────────────────────────────────────────────────────────────────\n");
    std::printf("Worst case across every state of every casebook: %lld instructions.\n", static_cast<long long>(overall));
    std::printf("At 150 MHz and a pessimistic 2 cycles per instruction that is %.1f us, so a 1 ms\n",
                static_cast<double>(overall) * 2.0 / 150.0);
    std::printf("control cycle has a margin of about %.0fx.\n\n", 1000.0 / (static_cast<double>(overall) * 2.0 / 150.0));

    std::printf("This is a BOUND, not a measurement, and it is the stronger of the two: it holds for\n");
    std::printf("every state, including the ones no test would think to run. What it does NOT cover is\n");
    std::printf("the memory system -- RP2350 runs from QSPI flash through a cache, and a miss costs far\n");
    std::printf("more than an instruction. That is precisely what the board will measure, and the\n");
    std::printf("measurement's job is to come in UNDER this number. If it comes in over, this model is\n");
    std::printf("wrong and that is worth knowing before anyone trusts either figure.\n");

    std::printf("\n");
    bool steps_wrong = false;
    bool portable = true;
    for (const Worst& w : results) {
        if (w.mask_by_instructions != w.mask_by_steps) { steps_wrong = true; }
        if (w.mask_by_iterations != w.mask_by_instructions) { portable = false; }
    }
    if (steps_wrong) {
        std::printf("FINDING: steps_ is not a proxy for work. It weights five loops equally and does not\n");
        std::printf("count the seeding loop at all, so the state maximising it is not the state maximising\n");
        std::printf("instructions. kWorstCaseMask is therefore the wrong state for a timing measurement.\n");
    }
    bool l5_rule = true, l4l5_rule = true;
    for (const Worst& w : results) {
        if (w.mask_by_l5 != w.mask_by_instructions) { l5_rule = false; }
        if (w.mask_by_l4_l5 != w.mask_by_instructions) { l4l5_rule = false; }
    }
    std::printf("portable rule check:  total iterations %s   max L5 %s   max L4+L5 %s\n",
                portable ? "WORKS" : "fails", l5_rule ? "WORKS" : "fails", l4l5_rule ? "WORKS" : "fails");
    std::size_t frontier_max = 0;
    for (const Worst& w : results) { frontier_max = std::max(frontier_max, w.pareto.size()); }
    std::printf("largest Pareto frontier: %zu states\n", frontier_max);
    if (!portable && !l5_rule && !l4l5_rule) {
        std::printf("\nNo single structural rule picks the instruction-worst state on all three casebooks.\n");
        std::printf("That settles the design: cbc must not guess. It should emit a small SET of candidate\n");
        std::printf("states and let the firmware measure each and report the maximum -- which needs no cost\n");
        std::printf("model anywhere, and stays correct on any silicon.\n");
    }
    if (failures > 0) {
        std::printf("\n%d problem(s).\n", failures);
        return 1;
    }
    std::printf("\nno failures\n");
    return 0;
}
