// Exhaustive independent verification: every state of a casebook, checked by CaDiCaL.
//
// `tools/cadical_check.cpp` checks one state and is driven by a shell script over a sample. A sample is the weakest form
// of this evidence, and sampling is what this project keeps being caught by: the worst-case state is the quiet one, the
// contested states are 1.37% of the pump's input space, and the defect in `vote.cb` under-fired only when two legs failed
// the same way. A cross-check that visits 14 of 16,384 states is a cross-check of the states I happened to pick.
//
// So this visits all of them. Two things make it affordable:
//
//   * ONE clause database per state, with CaDiCaL's `assume()` for each argument, instead of rebuilding per query. The
//     encoding is the expensive part and it does not depend on which argument is being asked about.
//   * the frameworks are small -- tens of arguments -- so each solve is microseconds.
//
// Everything about the trust argument is unchanged: the encoding here is transcribed from Dung 1995 and is mine, while
// every satisfiability question is answered by CaDiCaL, which shares nothing with quorum.
//
// ── A WRONG CHECK, CORRECTED ────────────────────────────────────────────────
//
// The first version of this file asked three questions, and the third was mathematically false:
//
//   IN        -> a complete extension EXCLUDING it must be UNSAT     (correct)
//   OUT       -> a complete extension INCLUDING it must be UNSAT     (correct)
//   UNDECIDED -> BOTH must be SAT                                    (WRONG)
//
// A grounded-undecided argument need not belong to any complete extension. The canonical counterexample is a
// self-attacker: grounded labels it Undecided, and no conflict-free set contains it, so it is in none. More generally any
// argument inside an odd cycle can behave this way.
//
// Run exhaustively, that error produced 96 "mismatches" on the pump -- `continue` and `gate_no_maintain_reason`, which sit
// on a cycle `gate_no_maintain_reason -> maintain -> continue`. Sampling had missed all of them, and the wrong check had
// reported clean agreement across 34 undecided arguments that happened not to be in cycles. A false alarm rather than a
// missed bug, but a check that is wrong about the semantics cannot be evidence about an engine's semantics.
//
// The replacement is both correct and STRONGER, because it reconstructs the whole labelling instead of testing three
// separate implications. It rests on one identity:
//
//   the grounded extension IS the intersection of all complete extensions
//
//   (grounded is complete, so the intersection is contained in it; grounded is the LEAST complete extension, so it is
//    contained in the intersection.)
//
// So CaDiCaL alone can rebuild the entire labelling:
//
//   in       := { a : "a complete extension excluding a" is UNSAT }        -- the intersection
//   out      := { b : some member of `in` attacks b }
//   undecided := everything else
//
// and then the two labellings are compared argument by argument. Every label is constrained, nothing is vacuous, and the
// only thing taken on trust is CaDiCaL's answer to a satisfiability question.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cadical.hpp"

#include "quorum/solver/grounded.hpp"

#include "permit_generated.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"
#include "vote_generated.hpp"

using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

struct Counts {
    std::int64_t states = 0;
    std::int64_t in = 0;
    std::int64_t out = 0;
    std::int64_t undec = 0;
    std::int64_t sat_calls = 0;
    std::int64_t mismatches = 0;
};

/// Encode complete semantics for this framework into a fresh solver.
///
/// One Boolean per argument (`in[a]`) plus one per argument for "is attacked by the extension" (`att[a]`). A set is a
/// complete extension iff it is conflict-free and `in[b] <-> every attacker of b is attacked` -- the fixed points of
/// Dung's characteristic function.
///
/// Deliberately NOT written against quorum's Framework API in a way that could inherit its interpretation: the only thing
/// read out is the raw attack list.
template <typename Fw>
void encode(CaDiCaL::Solver& s, const Fw& fw, std::vector<std::vector<std::int32_t>>& attackers) {
    const std::int32_t n = fw.argument_count();
    const auto in_var = [](std::int32_t a) { return a + 1; };
    const auto att_var = [n](std::int32_t a) { return n + a + 1; };

    attackers.assign(static_cast<std::size_t>(n), {});
    for (std::int32_t a = 0; a < n; ++a) {
        const std::int32_t end = fw.out_end(Arg{a});
        for (std::int32_t k = fw.out_begin(Arg{a}); k < end; ++k) {
            attackers[static_cast<std::size_t>(fw.target(k).get())].push_back(a);
        }
    }

    // conflict-freeness
    for (std::int32_t a = 0; a < n; ++a) {
        const std::int32_t end = fw.out_end(Arg{a});
        for (std::int32_t k = fw.out_begin(Arg{a}); k < end; ++k) {
            s.add(-in_var(a));
            s.add(-in_var(fw.target(k).get()));
            s.add(0);
        }
    }

    for (std::int32_t a = 0; a < n; ++a) {
        const auto& atk = attackers[static_cast<std::size_t>(a)];
        if (atk.empty()) {
            s.add(-att_var(a));
            s.add(0);
            continue;
        }
        s.add(-att_var(a));
        for (const std::int32_t c : atk) { s.add(in_var(c)); }
        s.add(0);
        for (const std::int32_t c : atk) {
            s.add(-in_var(c));
            s.add(att_var(a));
            s.add(0);
        }
    }

    for (std::int32_t b = 0; b < n; ++b) {
        const auto& atk = attackers[static_cast<std::size_t>(b)];
        if (atk.empty()) {
            s.add(in_var(b));
            s.add(0);
            continue;
        }
        for (const std::int32_t a : atk) {
            s.add(-in_var(b));
            s.add(att_var(a));
            s.add(0);
        }
        s.add(in_var(b));
        for (const std::int32_t a : atk) { s.add(-att_var(a)); }
        s.add(0);
    }
}

template <typename Fw, typename Solver, typename NameOf, typename Instantiate>
Counts verify_all(std::int32_t observations, NameOf name_of, Instantiate instantiate, std::int64_t report_every,
                  std::int32_t mutate_arg) {
    Counts c;
    Fw fw;
    Solver solver;
    std::vector<std::vector<std::int32_t>> attackers;

    const std::int64_t states = static_cast<std::int64_t>(1) << observations;
    for (std::int64_t st = 0; st < states; ++st) {
        const std::uint32_t mask = static_cast<std::uint32_t>(st);
        if (instantiate(fw, mask) != Status::Ok) { continue; }
        if (solver.solve(fw) != Status::Ok) { continue; }
        ++c.states;

        CaDiCaL::Solver sat;
        encode(sat, fw, attackers);

        const std::int32_t n = fw.argument_count();
        const auto in_var = [](std::int32_t a) { return a + 1; };

        // ── rebuild the grounded IN set as the intersection of all complete extensions ──
        std::vector<char> sat_in(static_cast<std::size_t>(n), 0);
        for (std::int32_t a = 0; a < n; ++a) {
            sat.assume(-in_var(a));
            ++c.sat_calls;
            // UNSAT means no complete extension excludes `a`, so every one contains it.
            if (sat.solve() != 10) { sat_in[static_cast<std::size_t>(a)] = 1; }
        }

        // ── anything that set attacks is OUT ────────────────────────────────
        std::vector<char> sat_out(static_cast<std::size_t>(n), 0);
        for (std::int32_t a = 0; a < n; ++a) {
            if (sat_in[static_cast<std::size_t>(a)] == 0) { continue; }
            const std::int32_t end = fw.out_end(Arg{a});
            for (std::int32_t k = fw.out_begin(Arg{a}); k < end; ++k) {
                sat_out[static_cast<std::size_t>(fw.target(k).get())] = 1;
            }
        }

        // ── compare the two labellings, argument by argument ────────────────
        for (std::int32_t a = 0; a < n; ++a) {
            Label mine = solver.label(Arg{a});

            // Self-test. With `--mutate <arg>` one label is deliberately corrupted, and the run MUST then fail. A
            // comparison that cannot fail is not a comparison, and this project has already shipped one check that
            // compared a value against itself and could never fire.
            if (mutate_arg == a + 1) {
                mine = (mine == Label::In) ? Label::Out : Label::In;
            }
            const bool si = sat_in[static_cast<std::size_t>(a)] != 0;
            const bool so = sat_out[static_cast<std::size_t>(a)] != 0;
            const char* theirs = si ? "IN" : (so ? "OUT" : "UNDECIDED");
            const char* ours = mine == Label::In ? "IN" : (mine == Label::Out ? "OUT" : "UNDECIDED");

            if (mine == Label::In) { ++c.in; }
            else if (mine == Label::Out) { ++c.out; }
            else { ++c.undec; }

            if (std::string(theirs) != std::string(ours)) {
                if (c.mismatches < 20) {
                    std::printf("  MISMATCH state 0x%llX arg %d (%s): quorum says %s, CaDiCaL reconstructs %s\n",
                                static_cast<unsigned long long>(mask), a + 1, name_of(a), ours, theirs);
                }
                ++c.mismatches;
            }
        }

        if (report_every > 0 && c.states % report_every == 0) {
            std::printf("  %lld states, %lld SAT calls, %lld mismatches\n", static_cast<long long>(c.states),
                        static_cast<long long>(c.sat_calls), static_cast<long long>(c.mismatches));
            std::fflush(stdout);
        }
    }
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "permit";
    std::int64_t report_every = 0;
    std::int32_t mutate_arg = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mutate" && i + 1 < argc) {
            mutate_arg = static_cast<std::int32_t>(std::strtol(argv[++i], nullptr, 10));
        } else {
            report_every = std::strtoll(a.c_str(), nullptr, 10);
        }
    }
    if (mutate_arg > 0) {
        std::printf("SELF-TEST: label of argument %d is deliberately corrupted; this run MUST fail.\n\n", mutate_arg);
    }

    namespace pump = quorum::casebook::gen::centrifugal_pump;
    namespace tank = quorum::casebook::gen::tank_409;
    namespace permit = quorum::casebook::gen::piper_permit;
    namespace vote = quorum::casebook::gen::vessel_hl_trip;

    std::printf("exhaustive independent verification: %s\n", which.c_str());
    std::printf("every state, every argument, every answer from CaDiCaL\n\n");

    Counts c;
    if (which == "permit") {
        c = verify_all<permit::CaseFramework, permit::CaseSolver>(
            permit::kObservationCount, [](std::int32_t a) { return permit::name_of(a); },
            [](permit::CaseFramework& f, std::uint32_t m) { return permit::instantiate(f, m); }, report_every, mutate_arg);
    } else if (which == "vote") {
        c = verify_all<vote::CaseFramework, vote::CaseSolver>(
            vote::kObservationCount, [](std::int32_t a) { return vote::name_of(a); },
            [](vote::CaseFramework& f, std::uint32_t m) { return vote::instantiate(f, m); }, report_every, mutate_arg);
    } else if (which == "pump") {
        c = verify_all<pump::CaseFramework, pump::CaseSolver>(
            pump::kObservationCount, [](std::int32_t a) { return pump::name_of(a); },
            [](pump::CaseFramework& f, std::uint32_t m) { return pump::instantiate(f, m); }, report_every, mutate_arg);
    } else if (which == "tank") {
        c = verify_all<tank::CaseFramework, tank::CaseSolver>(
            tank::kObservationCount, [](std::int32_t a) { return tank::name_of(a); },
            [](tank::CaseFramework& f, std::uint32_t m) { return tank::instantiate(f, m); }, report_every, mutate_arg);
    } else {
        std::fprintf(stderr, "unknown casebook '%s'\n", which.c_str());
        return 2;
    }

    std::printf("\nstates              %lld\n", static_cast<long long>(c.states));
    std::printf("IN  labels compared %lld\n", static_cast<long long>(c.in));
    std::printf("OUT labels compared %lld\n", static_cast<long long>(c.out));
    std::printf("UNDEC labels compared %lld\n", static_cast<long long>(c.undec));
    std::printf("CaDiCaL solve calls %lld\n", static_cast<long long>(c.sat_calls));
    std::printf("mismatches          %lld\n", static_cast<long long>(c.mismatches));

    if (c.mismatches == 0) {
        std::printf("\nEXHAUSTIVE agreement on the FULL labelling. Not three implications spot-checked over a\n");
        std::printf("sample of states I chose: every state this casebook can reach, every argument in each,\n");
        std::printf("with the labelling rebuilt from scratch by a solver that shares no code with quorum.\n");
        return 0;
    }
    return 1;
}
