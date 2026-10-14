// An independent check on quorum's grounded labelling, with CaDiCaL doing the reasoning.
//
// ── Why this file exists, and why the obvious approach was not enough ───────
//
// The first attempt at independent verification asked a separate SAT-based ICCMA 2025 solver to confirm quorum's
// labels. 1,937 queries across four casebooks agreed. Then I read the ICCMA engine's source and found `Semantics::grounded()` --
// the SAME worklist algorithm quorum uses, because quorum was salvaged from that project. Its DS-PR path also takes
// shortcuts using grounded knowledge.
//
// I could not establish, by reading, whether a given query consulted CaDiCaL or took a shortcut through code quorum
// shares. An agreement produced by shared code is not corroboration, it is one implementation agreeing with itself, and
// reporting it as independent verification would have been the most consequential overclaim in the project.
//
// So this file removes the doubt instead of reasoning about it. Nothing here is shared with either engine:
//
//   * the SAT encoding below is transcribed directly from Dung 1995's definitions, in this file, ~40 lines
//   * every satisfiability question is answered by CaDiCaL -- Armin Biere's code, which has never heard of quorum
//   * the frameworks are read from a file, so no data structure is shared either
//
// The encoding is mine and could be wrong. The SEARCH is not mine, and that is the part no other test in this project
// escapes. A bug in my encoding would almost certainly disagree with quorum rather than silently agree, because the two
// arrive at an answer by completely different routes.
//
// ── The encoding ───────────────────────────────────────────────────────────
//
// One Boolean per argument: in[a] means "a is in the extension". A set S is a COMPLETE extension iff
//
//   (conflict-free)  for every attack a->b:        not in[a] or not in[b]
//   (complete)       for every argument b:         in[b]  <->  every attacker of b is attacked by S
//
// The right-hand side needs a name, so introduce def[b] meaning "b is defended by S", and for each attacker a of b a
// variable att[a] meaning "a is attacked by S". Then:
//
//   att[a]  <->  OR over attackers c of a of in[c]
//   def[b]  <->  AND over attackers a of b of att[a]
//   in[b]   <->  def[b]
//
// which collapses to in[b] <-> def[b]: complete semantics admits exactly the fixed points of the characteristic
// function. Grounded is the LEAST such fixed point.
//
// ── The two theorems being checked ─────────────────────────────────────────
//
//   quorum says IN   ->  a is in every complete extension.
//                        So "a complete extension excluding a" must be UNSAT.
//
//   quorum says OUT  ->  a is in no complete extension.
//                        So "a complete extension including a" must be UNSAT.
//
//   quorum says UNDEC -> a is in some complete extension and absent from another. BOTH must be SAT.
//
// The third is the one the ICCMA engine could not give me and it is the most valuable of the three: it is the only check that
// constrains the undecided labels, which is exactly where two implementations are most likely to diverge, because it is
// where the semantics stop forcing an answer. An engine that wrongly labelled something Undecided instead of In would
// pass both other checks and fail this one.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cadical.hpp"

namespace {

struct Graph {
    std::int32_t n = 0;
    std::vector<std::vector<std::int32_t>> attackers;  // attackers[b] = list of a with a -> b
    std::vector<std::pair<std::int32_t, std::int32_t>> edges;
    bool ok = false;
};

/// Read ICCMA i23: `p af <n>` then one-based `from to` per line.
Graph read_af(const std::string& path) {
    Graph g;
    std::ifstream in(path);
    if (!in) { return g; }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        std::istringstream ls(line);
        std::string first;
        ls >> first;
        if (first == "p") {
            std::string fmt;
            ls >> fmt >> g.n;
            g.attackers.assign(static_cast<std::size_t>(g.n), {});
            continue;
        }
        std::istringstream again(line);
        std::int32_t from = 0;
        std::int32_t to = 0;
        if (again >> from >> to) {
            // One-based on the wire, zero-based here.
            g.edges.emplace_back(from - 1, to - 1);
            g.attackers[static_cast<std::size_t>(to - 1)].push_back(from - 1);
        }
    }
    g.ok = g.n > 0;
    return g;
}

/// Is there a complete extension consistent with the given assumption about argument `focus`?
///
/// `want_in == true`  asks for a complete extension CONTAINING focus.
/// `want_in == false` asks for one EXCLUDING focus.
bool complete_extension_exists(const Graph& g, std::int32_t focus, bool want_in) {
    CaDiCaL::Solver s;

    const std::int32_t n = g.n;
    // Variable layout, one-based as DIMACS requires.
    //   in[a]  = 1 + a
    //   att[a] = 1 + n + a        "a is attacked by the extension"
    const auto in_var = [](std::int32_t a) { return a + 1; };
    const auto att_var = [n](std::int32_t a) { return n + a + 1; };

    // ── conflict-freeness: for every attack a->b, not both in ───────────────
    for (const auto& [a, b] : g.edges) {
        s.add(-in_var(a));
        s.add(-in_var(b));
        s.add(0);
    }

    // ── att[a] <-> OR over attackers c of a of in[c] ────────────────────────
    for (std::int32_t a = 0; a < n; ++a) {
        const auto& atk = g.attackers[static_cast<std::size_t>(a)];
        if (atk.empty()) {
            // Nothing attacks a, so a can never be attacked by the extension.
            s.add(-att_var(a));
            s.add(0);
            continue;
        }
        // att[a] -> OR in[c]
        s.add(-att_var(a));
        for (const std::int32_t c : atk) { s.add(in_var(c)); }
        s.add(0);
        // in[c] -> att[a], for each c
        for (const std::int32_t c : atk) {
            s.add(-in_var(c));
            s.add(att_var(a));
            s.add(0);
        }
    }

    // ── in[b] <-> every attacker of b is attacked ───────────────────────────
    // This is Dung's characteristic function, and complete extensions are exactly its fixed points.
    for (std::int32_t b = 0; b < n; ++b) {
        const auto& atk = g.attackers[static_cast<std::size_t>(b)];
        if (atk.empty()) {
            // Unattacked: defended vacuously, so it is in every complete extension.
            s.add(in_var(b));
            s.add(0);
            continue;
        }
        // in[b] -> att[a] for every attacker a of b
        for (const std::int32_t a : atk) {
            s.add(-in_var(b));
            s.add(att_var(a));
            s.add(0);
        }
        // (AND over a of att[a]) -> in[b], i.e. in[b] OR some att[a] is false
        s.add(in_var(b));
        for (const std::int32_t a : atk) { s.add(-att_var(a)); }
        s.add(0);
    }

    // ── the question ────────────────────────────────────────────────────────
    s.add(want_in ? in_var(focus) : -in_var(focus));
    s.add(0);

    return s.solve() == 10;  // 10 = SATISFIABLE
}

int g_failures = 0;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: cadical_check <framework.af> <labels.txt>\n"
                             "  labels.txt: '<one-based index> <IN|OUT|UNDEC> <name>' per line, from dump_af\n");
        return 2;
    }

    const Graph g = read_af(argv[1]);
    if (!g.ok) {
        std::fprintf(stderr, "could not read %s\n", argv[1]);
        return 1;
    }

    std::ifstream labels(argv[2]);
    if (!labels) {
        std::fprintf(stderr, "could not read %s\n", argv[2]);
        return 1;
    }

    std::int32_t checked_in = 0;
    std::int32_t checked_out = 0;
    std::int32_t checked_undec = 0;

    std::string line;
    while (std::getline(labels, line)) {
        std::istringstream ls(line);
        std::int32_t idx = 0;
        std::string label;
        std::string name;
        if (!(ls >> idx >> label)) { continue; }
        ls >> name;
        const std::int32_t a = idx - 1;
        if (a < 0 || a >= g.n) { continue; }

        if (label == "IN") {
            // Must be in EVERY complete extension: excluding it must be unsatisfiable.
            if (complete_extension_exists(g, a, false)) {
                std::printf("  MISMATCH arg %d (%s): quorum says IN, but CaDiCaL found a complete extension\n"
                            "           that excludes it.\n",
                            idx, name.c_str());
                ++g_failures;
            }
            ++checked_in;
        } else if (label == "OUT") {
            // Must be in NO complete extension: including it must be unsatisfiable.
            if (complete_extension_exists(g, a, true)) {
                std::printf("  MISMATCH arg %d (%s): quorum says OUT, but CaDiCaL found a complete extension\n"
                            "           that includes it.\n",
                            idx, name.c_str());
                ++g_failures;
            }
            ++checked_out;
        } else if (label == "UNDEC") {
            // Must be in some complete extension and out of another: BOTH must be satisfiable. This is the check that
            // constrains the undecided labels, and no other test in the project has one.
            const bool can_be_in = complete_extension_exists(g, a, true);
            const bool can_be_out = complete_extension_exists(g, a, false);
            if (!can_be_in) {
                std::printf("  MISMATCH arg %d (%s): quorum says UNDECIDED, but no complete extension contains it\n"
                            "           -- it should have been OUT.\n",
                            idx, name.c_str());
                ++g_failures;
            }
            if (!can_be_out) {
                std::printf("  MISMATCH arg %d (%s): quorum says UNDECIDED, but every complete extension contains it\n"
                            "           -- it should have been IN.\n",
                            idx, name.c_str());
                ++g_failures;
            }
            ++checked_undec;
        }
    }

    std::printf("  %d arguments: %d IN, %d OUT, %d UNDECIDED", checked_in + checked_out + checked_undec, checked_in,
                checked_out, checked_undec);
    if (g_failures == 0) {
        std::printf("   all consistent\n");
        return 0;
    }
    std::printf("   %d MISMATCHES\n", g_failures);
    return 1;
}
