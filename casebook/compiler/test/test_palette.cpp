// The palette guarantee: non-interference and crash-resistance.
//
// The product thesis is that a customer buys the pump casebook, later buys the tank casebook, and they coexist on
// one device without re-certifying the first. ASPIC+ predicts this works -- two of the five rationality postulates
// say so, and both hold under grounded semantics, which is the only semantics this product ships on device:
//
//   non-interference   syntactically disjoint knowledge bases do not influence each other's conclusions
//   crash-resistance   no contaminating knowledge base can render the others irrelevant
//
// This is a PREDICTION. The theory says it should hold, it has never been tested here, and a failure would mean the
// lowering is wrong. That distinguishes it from the tank casebook, which reproduced an accident whose outcome was
// already known.
//
// The test measures interference at two layers, because they can fail independently:
//
//   LABEL layer    do the member's argument labels change when another casebook is present?
//   VERDICT layer  does the member's reported outcome change?
//
// The verdict layer is the suspicious one. `arbiter::decide` counts Undecided arguments across the WHOLE framework
// to tell Indeterminate ("plant is ambiguous, ask a human") from Unsatisfied ("casebook is broken, fix your FMEA").
// In a palette that counter sees other casebooks' arguments. Three scopings are measured side by side so the
// question is settled by evidence rather than by reading the code.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/cbc/cbc.hpp"

namespace cbc = quorum::cbc;

using quorum::arbiter::Outcome;
using quorum::arbiter::outcome_name;
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

// Tight capacities. A palette of pump + tank is 82 arguments and 142 attacks; the pathological casebooks are tiny.
// Sizing this loosely would matter: the framework is reset and rebuilt tens of millions of times, so per-iteration
// clearing cost is the whole runtime.
constexpr std::int32_t kMaxArgs = 128;
constexpr std::int32_t kMaxAttacks = 384;

using Fw = quorum::solver::Framework<kMaxArgs, kMaxAttacks>;
using Solver = quorum::solver::Grounded<kMaxArgs>;

int g_failures = 0;

void ok(bool condition, const std::string& what) {
    std::printf("  %s  %s\n", condition ? "OK  " : "FAIL", what.c_str());
    if (!condition) { ++g_failures; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Building and solving

/// Build a single casebook at a given presence mask. Reuses `fw` -- `reset()` touches only counters.
[[nodiscard]] Status build_one(Fw& fw, const cbc::Lowered& lo, std::uint32_t mask) {
    fw.reset();
    Status s = fw.declare(lo.count());
    if (s != Status::Ok) { return s; }
    for (const cbc::LoweredEdge& e : lo.edges) {
        s = fw.add_attack(Arg{e.from}, Arg{e.to});
        if (s != Status::Ok) { return s; }
    }
    for (std::int32_t a = 0; a < lo.observation_count; ++a) {
        if ((mask & (1U << static_cast<std::uint32_t>(a))) == 0U) {
            s = fw.add_attack(Arg{lo.closed_world}, Arg{a});
            if (s != Status::Ok) { return s; }
        }
    }
    return fw.finalize();
}

/// Build a palette. Each member's own closed-world argument attacks only its own absent readings; there is no
/// cross-member edge anywhere, which is what makes the disjointness claim real rather than nominal.
[[nodiscard]] Status build_palette(Fw& fw, const cbc::Palette& p, std::uint32_t mask) {
    fw.reset();
    Status s = fw.declare(p.all.count());
    if (s != Status::Ok) { return s; }
    for (const cbc::LoweredEdge& e : p.all.edges) {
        s = fw.add_attack(Arg{e.from}, Arg{e.to});
        if (s != Status::Ok) { return s; }
    }
    for (const cbc::PaletteMember& m : p.members) {
        for (std::int32_t a = 0; a < m.observation_count; ++a) {
            const std::uint32_t bit = 1U << static_cast<std::uint32_t>(m.observation_offset + a);
            if ((mask & bit) == 0U) {
                s = fw.add_attack(Arg{m.closed_world}, Arg{m.map[static_cast<std::size_t>(a)]});
                if (s != Status::Ok) { return s; }
            }
        }
    }
    return fw.finalize();
}

// ─────────────────────────────────────────────────────────────────────────────
// Classification, with the Undecided counter scoped three different ways.

struct Classified {
    std::int32_t actions_in = 0;
    std::int32_t chosen = -1;
    /// Outcome if Undecided is counted across the whole framework -- what arbiter::decide does today.
    Outcome by_framework = Outcome::Faulted;
    /// Outcome if Undecided is counted only among this member's own arguments.
    Outcome by_member = Outcome::Faulted;
    /// Outcome if Undecided is counted only among this member's declared ACTIONS.
    Outcome by_action = Outcome::Faulted;
};

Outcome classify(std::int32_t actions_in, std::int32_t undecided) {
    if (actions_in == 1) { return Outcome::Decided; }
    if (actions_in > 1) { return Outcome::Conflicted; }
    if (undecided > 0) { return Outcome::Indeterminate; }
    return Outcome::Unsatisfied;
}

Classified examine(const Solver& solver,
                   std::int32_t framework_count,
                   const std::vector<std::int32_t>& member_args,
                   const std::vector<std::int32_t>& actions) {
    Classified c;

    std::int32_t undecided_framework = 0;
    for (std::int32_t i = 0; i < framework_count; ++i) {
        if (solver.label(Arg{i}) == Label::Undecided) { ++undecided_framework; }
    }
    std::int32_t undecided_member = 0;
    for (const std::int32_t i : member_args) {
        if (solver.label(Arg{i}) == Label::Undecided) { ++undecided_member; }
    }
    std::int32_t undecided_action = 0;
    for (const std::int32_t a : actions) {
        const Label l = solver.label(Arg{a});
        if (l == Label::Undecided) { ++undecided_action; }
        if (l == Label::In) {
            ++c.actions_in;
            c.chosen = a;
        }
    }

    c.by_framework = classify(c.actions_in, undecided_framework);
    c.by_member = classify(c.actions_in, undecided_member);
    c.by_action = classify(c.actions_in, undecided_action);
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loading

[[nodiscard]] bool load(const std::string& path, cbc::Lowered& out) {
    std::ifstream in(path);
    if (!in) {
        std::printf("cannot read %s\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    cbc::Diagnostics diag;
    const cbc::Casebook cb = cbc::parse(ss.str(), diag);
    if (!diag.has_errors()) {
        out = cbc::lower(cb, diag);
        if (!diag.has_errors()) { cbc::check(out, diag); }
    }
    for (const cbc::Diagnostic& d : diag.items()) {
        if (d.severity == cbc::Diagnostic::Severity::Error) {
            std::printf("%s:%d: error: %s\n", path.c_str(), d.line, d.message.c_str());
        }
    }
    return !diag.has_errors();
}

// ─────────────────────────────────────────────────────────────────────────────
// Pathological casebooks, built directly.
//
// cbc REFUSES to emit any of these: `check` demands an idle and a safe action, and exhaustive validation rejects
// Conflicted and Unsatisfied outcomes. That is the first line of defence and it works. But crash-resistance is
// precisely the question "what if a broken casebook gets on the device anyway" -- from a third party, from an older
// toolchain, from a hand-written header -- so the test has to construct them itself.

struct Builder {
    cbc::Lowered lo;

    explicit Builder(const std::string& name) {
        lo.casebook = name;
        lo.version = 1;
    }
    std::int32_t arg(const std::string& n, cbc::Role r) {
        cbc::LoweredArgument a;
        a.name = n;
        a.description = n;
        a.role = r;
        lo.arguments.push_back(std::move(a));
        return lo.count() - 1;
    }
    void observation(const std::string& n) {
        (void)arg(n, cbc::Role::Observation);
        ++lo.observation_count;
    }
    void edge(std::int32_t f, std::int32_t t) {
        cbc::LoweredEdge e;
        e.from = f;
        e.to = t;
        lo.edges.push_back(std::move(e));
    }
    cbc::Lowered finish(std::int32_t idle, std::int32_t safe) {
        lo.closed_world = arg("closed_world", cbc::Role::ClosedWorld);
        lo.idle_action = idle;
        lo.safe_action = safe;
        lo.max_args = lo.count();
        lo.max_attacks = static_cast<std::int32_t>(lo.edges.size()) + lo.observation_count;
        return lo;
    }
};

/// A three-cycle. Odd cycles have no grounded resolution, so everything in one is Undecided forever.
cbc::Lowered pathology_odd_cycle() {
    Builder b("broken_cycle");
    const std::int32_t x = b.arg("x", cbc::Role::Claim);
    const std::int32_t y = b.arg("y", cbc::Role::Claim);
    const std::int32_t z = b.arg("z", cbc::Role::Claim);
    b.edge(x, y);
    b.edge(y, z);
    b.edge(z, x);
    const std::int32_t idle = b.arg("idle", cbc::Role::Action);
    const std::int32_t act = b.arg("act", cbc::Role::Action);
    b.edge(x, idle);
    b.edge(y, act);
    return b.finish(idle, act);
}

/// A self-attacking argument: the idiom this project rejected early, kept here as a contaminant.
cbc::Lowered pathology_self_attack() {
    Builder b("broken_self");
    const std::int32_t s = b.arg("s", cbc::Role::Claim);
    b.edge(s, s);
    const std::int32_t idle = b.arg("idle", cbc::Role::Action);
    b.edge(s, idle);
    return b.finish(idle, idle);
}

/// Two unattacked actions: both In at once, the dangerous direction.
cbc::Lowered pathology_conflicted() {
    Builder b("broken_conflict");
    const std::int32_t a1 = b.arg("open_valve", cbc::Role::Action);
    const std::int32_t a2 = b.arg("close_valve", cbc::Role::Action);
    return b.finish(a1, a2);
}

/// Every action decisively defeated: nothing Undecided, nothing In.
cbc::Lowered pathology_unsatisfied() {
    Builder b("broken_unsat");
    const std::int32_t veto = b.arg("veto", cbc::Role::Claim);
    const std::int32_t a1 = b.arg("a1", cbc::Role::Action);
    const std::int32_t a2 = b.arg("a2", cbc::Role::Action);
    b.edge(veto, a1);
    b.edge(veto, a2);
    return b.finish(a1, a2);
}

/// A mutually-attacking clique: maximum Undecided contamination for its size.
cbc::Lowered pathology_clique() {
    Builder b("broken_clique");
    std::vector<std::int32_t> v;
    for (int i = 0; i < 8; ++i) { v.push_back(b.arg("c" + std::to_string(i), cbc::Role::Claim)); }
    for (std::size_t i = 0; i < v.size(); ++i) {
        for (std::size_t j = 0; j < v.size(); ++j) {
            if (i != j) { b.edge(v[i], v[j]); }
        }
    }
    const std::int32_t idle = b.arg("idle", cbc::Role::Action);
    b.edge(v[0], idle);
    return b.finish(idle, idle);
}

/// A VICTIM casebook that genuinely reaches Unsatisfied on its own.
///
/// Needed because the pump never does -- it reports Unsatisfied in 0 of 16,384 states -- so the pump cannot expose
/// an Indeterminate/Unsatisfied confusion even if one exists. Without this, a clean result would only mean "the
/// pump happens not to trigger the bug".
cbc::Lowered victim_with_unsatisfied() {
    Builder b("victim");
    b.observation("trigger");
    const std::int32_t veto = b.arg("veto", cbc::Role::Claim);
    const std::int32_t act = b.arg("act", cbc::Role::Action);
    b.edge(veto, act);  // veto is unattacked, so `act` is always Out and nothing is ever Undecided
    return b.finish(act, act);
}

std::vector<std::int32_t> all_args_of(const cbc::PaletteMember& m) {
    std::vector<std::int32_t> v;
    for (const std::int32_t i : m.map) {
        if (i >= 0) { v.push_back(i); }
    }
    return v;
}

std::vector<std::int32_t> identity_args(const cbc::Lowered& lo) {
    std::vector<std::int32_t> v;
    for (std::int32_t i = 0; i < lo.count(); ++i) { v.push_back(i); }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────

struct Interference {
    std::int64_t states = 0;
    std::int64_t label_differs = 0;
    std::int64_t verdict_differs_framework = 0;
    std::int64_t verdict_differs_member = 0;
    std::int64_t verdict_differs_action = 0;
    Outcome example_alone = Outcome::Faulted;
    Outcome example_merged = Outcome::Faulted;
    bool have_example = false;
};

/// Compare one member inside a palette against that same casebook running alone, over every state of the OTHER
/// members. `vary` enumerates the full combined mask; the member's own bits are extracted to index the alone run.
void compare_member(const cbc::Palette& p,
                    std::size_t which,
                    const cbc::Lowered& alone,
                    std::uint32_t combined_states,
                    Interference& out) {
    const cbc::PaletteMember& m = p.members[which];
    const std::vector<std::int32_t> member_args = all_args_of(m);
    const std::vector<std::int32_t> alone_args = identity_args(alone);

    const std::uint32_t own_states = 1U << static_cast<std::uint32_t>(m.observation_count);
    const std::uint32_t own_shift = static_cast<std::uint32_t>(m.observation_offset);
    const std::uint32_t own_mask = (own_states - 1U) << own_shift;

    // Precompute the alone run once per own-state. Doing it inside the sweep would triple the runtime.
    const std::size_t n = static_cast<std::size_t>(alone.count());
    std::vector<std::uint8_t> alone_labels(static_cast<std::size_t>(own_states) * n);
    std::vector<Classified> alone_verdicts(own_states);
    {
        Fw fw;
        Solver solver;
        for (std::uint32_t s = 0; s < own_states; ++s) {
            if (build_one(fw, alone, s) != Status::Ok || solver.solve(fw) != Status::Ok) {
                ++out.label_differs;
                return;
            }
            for (std::size_t i = 0; i < n; ++i) {
                alone_labels[static_cast<std::size_t>(s) * n + i] =
                    static_cast<std::uint8_t>(solver.label(Arg{static_cast<std::int32_t>(i)}));
            }
            alone_verdicts[s] = examine(solver, alone.count(), alone_args, alone.actions);
        }
    }

    Fw fw;
    Solver solver;
    for (std::uint32_t combined = 0; combined < combined_states; ++combined) {
        if (build_palette(fw, p, combined) != Status::Ok || solver.solve(fw) != Status::Ok) {
            ++out.label_differs;
            return;
        }
        ++out.states;

        const std::uint32_t own = (combined & own_mask) >> own_shift;
        const std::uint8_t* base = &alone_labels[static_cast<std::size_t>(own) * n];

        for (std::size_t i = 0; i < n; ++i) {
            const Label merged = solver.label(Arg{m.map[i]});
            if (static_cast<std::uint8_t>(merged) != base[i]) { ++out.label_differs; }
        }

        const Classified& a = alone_verdicts[own];
        const Classified g = examine(solver, p.all.count(), member_args, m.actions);

        if (a.by_framework != g.by_framework) {
            ++out.verdict_differs_framework;
            if (!out.have_example) {
                out.example_alone = a.by_framework;
                out.example_merged = g.by_framework;
                out.have_example = true;
            }
        }
        if (a.by_member != g.by_member) { ++out.verdict_differs_member; }
        if (a.by_action != g.by_action) { ++out.verdict_differs_action; }
    }
}

void report(const std::string& title, const Interference& r) {
    std::printf("\n%s\n", title.c_str());
    std::printf("    states compared                          %lld\n", static_cast<long long>(r.states));
    ok(r.label_differs == 0, "LABEL layer: every argument label unchanged");
    if (r.label_differs != 0) {
        std::printf("        %lld label differences\n", static_cast<long long>(r.label_differs));
    }
    // The rule arbiter::decide actually uses. This one is load-bearing.
    ok(r.verdict_differs_action == 0, "VERDICT layer: outcome unchanged (Undecided scoped to declared actions)");

    // The two rejected scopings, retained as evidence for why the arbiter does not use them.
    std::printf("    for comparison, other scopings of the Undecided counter:\n");
    std::printf("      this casebook's own arguments    %lld differences\n",
                static_cast<long long>(r.verdict_differs_member));
    std::printf("      the whole framework              %lld differences%s\n",
                static_cast<long long>(r.verdict_differs_framework),
                r.verdict_differs_framework == 0 ? "" : "   <-- would interfere");
    if (r.have_example) {
        std::printf("        e.g. alone reports %s, in a palette would report %s\n",
                    outcome_name(r.example_alone), outcome_name(r.example_merged));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_palette <pump.cb> <tank.cb> [--full]\n");
        return 2;
    }
    const bool full = argc > 3 && std::string(argv[3]) == "--full";

    cbc::Lowered pump;
    cbc::Lowered tank;
    if (!load(argv[1], pump) || !load(argv[2], tank)) { return 1; }

    std::printf("palette guarantee: non-interference and crash-resistance\n");
    std::printf("  %-12s %3d arguments, %2d observations\n", pump.casebook.c_str(), pump.count(),
                pump.observation_count);
    std::printf("  %-12s %3d arguments, %2d observations\n", tank.casebook.c_str(), tank.count(),
                tank.observation_count);

    cbc::Diagnostics diag;
    const cbc::Palette both = cbc::merge({pump, tank}, diag);
    for (const cbc::Diagnostic& d : diag.items()) {
        std::printf("  merge: %s\n", d.message.c_str());
    }
    if (diag.has_errors()) { return 1; }

    std::printf("  merged       %3d arguments, %2d observations, %d attacks -> 2^%d = %u states\n",
                both.all.count(), both.all.observation_count, static_cast<int>(both.all.edges.size()),
                both.all.observation_count, 1U << static_cast<std::uint32_t>(both.all.observation_count));

    // Disjointness is a precondition, not a result. Check it before claiming anything follows from it.
    {
        std::int32_t cross = 0;
        std::vector<int> owner(static_cast<std::size_t>(both.all.count()), -1);
        for (std::size_t i = 0; i < both.members.size(); ++i) {
            for (const std::int32_t a : both.members[i].map) {
                if (a >= 0) { owner[static_cast<std::size_t>(a)] = static_cast<int>(i); }
            }
        }
        for (const cbc::LoweredEdge& e : both.all.edges) {
            if (owner[static_cast<std::size_t>(e.from)] != owner[static_cast<std::size_t>(e.to)]) { ++cross; }
        }
        std::printf("\nprecondition\n");
        ok(cross == 0, "the merged framework contains no cross-casebook edge");
    }

    const std::uint32_t all_states = 1U << static_cast<std::uint32_t>(both.all.observation_count);
    const std::uint32_t sweep = full ? all_states : (1U << 19);
    if (!full) {
        std::printf("\n(sampling the first %u of %u combined states; pass --full for all)\n", sweep, all_states);
    }

    std::printf("\n── NON-INTERFERENCE ───────────────────────────────────────────\n");
    Interference r_pump;
    Interference r_tank;
    compare_member(both, 0, pump, sweep, r_pump);
    compare_member(both, 1, tank, sweep, r_tank);
    report("pump, with the tank casebook present", r_pump);
    report("tank, with the pump casebook present", r_tank);

    std::printf("\n── CRASH-RESISTANCE ───────────────────────────────────────────\n");
    std::printf("Each contaminant below is something cbc REFUSES to emit. The question is what happens\n"
                "if one reaches the device anyway -- third party, older toolchain, hand-written header.\n");

    struct Contaminant {
        const char* label;
        cbc::Lowered lo;
    };
    std::vector<Contaminant> contaminants{
        {"a three-cycle (everything Undecided)", pathology_odd_cycle()},
        {"a self-attacking argument", pathology_self_attack()},
        {"two actions In at once (Conflicted)", pathology_conflicted()},
        {"every action defeated (Unsatisfied)", pathology_unsatisfied()},
        {"an 8-way mutual-attack clique", pathology_clique()},
    };

    for (const Contaminant& c : contaminants) {
        cbc::Diagnostics d2;
        const cbc::Palette p = cbc::merge({pump, c.lo}, d2);
        if (d2.has_errors()) {
            std::printf("  merge failed for %s\n", c.label);
            ++g_failures;
            continue;
        }
        Interference r;
        compare_member(p, 0, pump, 1U << static_cast<std::uint32_t>(p.all.observation_count), r);
        report(std::string("pump, contaminated by ") + c.label, r);
    }

    // And the same question asked of a casebook that can actually reach Unsatisfied.
    std::printf("\n── CRASH-RESISTANCE, with a victim that reaches Unsatisfied ────\n");
    std::printf("The pump reports Unsatisfied in 0 of 16,384 states, so it cannot expose a confusion\n"
                "between Unsatisfied and Indeterminate even if one exists. This one can.\n");
    {
        const cbc::Lowered victim = victim_with_unsatisfied();
        cbc::Diagnostics d3;
        const cbc::Palette p = cbc::merge({victim, pathology_odd_cycle()}, d3);
        if (!d3.has_errors()) {
            Interference r;
            compare_member(p, 0, victim, 1U << static_cast<std::uint32_t>(p.all.observation_count), r);
            report("a victim reporting Unsatisfied, contaminated by a three-cycle", r);
        }
    }

    std::printf("\n%s\n", g_failures == 0 ? "all palette properties hold"
                                          : "PALETTE PROPERTY VIOLATED");
    return g_failures == 0 ? 0 : 1;
}
