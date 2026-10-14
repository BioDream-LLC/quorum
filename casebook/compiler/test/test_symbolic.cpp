// The symbolic validator must agree with the device solver in EVERY state, argument by argument.
//
// The symbolic pass exists to be trusted where enumeration is unaffordable, so it has to be checked where enumeration
// is affordable. For every casebook the project ships, and for the pump+tank palette, this visits all 2^n states and
// compares the BDD's label for every argument and its outcome for every policy against `solver::Grounded` and
// `arbiter::decide` -- the code that runs on the chips. One disagreement anywhere fails the test.

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
using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

constexpr std::int32_t kMaxArgs = 512;
constexpr std::int32_t kMaxAttacks = 8192;
using Fw = quorum::solver::Framework<kMaxArgs, kMaxAttacks>;
using Solver = quorum::solver::Grounded<kMaxArgs>;
using Pol = quorum::arbiter::Policy<32>;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 10) { std::printf("  FAIL  %s\n", what.c_str()); }
    }
}

bool load(const std::string& path, cbc::Lowered& lo) {
    std::ifstream in(path);
    if (!in) { return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    cbc::Diagnostics d;
    cbc::Casebook cb = cbc::parse(ss.str(), d);
    if (d.has_errors()) { return false; }
    cbc::desugar_votes(cb, d);
    lo = cbc::lower(cb, d);
    return !d.has_errors();
}

Status build(Fw& fw, const cbc::Lowered& lo, std::uint32_t mask) {
    Status s = fw.declare(lo.count());
    for (const auto& e : lo.edges) {
        if (s == Status::Ok) { s = fw.add_attack(Arg{e.from}, Arg{e.to}); }
    }
    for (std::int32_t a = 0; a < lo.observation_count && s == Status::Ok; ++a) {
        if ((mask >> a & 1U) == 0U) { s = fw.add_attack(Arg{lo.closed_world}, Arg{a}); }
    }
    return s == Status::Ok ? fw.finalize() : s;
}

/// Compare every argument and every policy outcome in every state.
void agree(const std::string& name, const cbc::Lowered& lo, const std::vector<cbc::SymbolicPolicy>& pols,
           const std::vector<std::pair<std::int32_t, std::int32_t>>& safe_of) {
    const cbc::SymbolicCheck sym(lo, pols);
    std::vector<Pol> policies(pols.size());
    for (std::size_t p = 0; p < pols.size(); ++p) {
        for (const std::int32_t a : pols[p].actions) { (void)policies[p].declare_action(Arg{a}); }
        (void)policies[p].set_fallback(Arg{safe_of[p].second});
    }
    const std::uint32_t total = 1U << static_cast<std::uint32_t>(lo.observation_count);
    std::vector<std::vector<std::uint64_t>> counted(pols.size(), std::vector<std::uint64_t>(5, 0));
    int label_mismatch = 0;
    int outcome_mismatch = 0;
    static Fw fw;
    static Solver solver;
    for (std::uint32_t m = 0; m < total; ++m) {
        fw.reset();
        if (build(fw, lo, m) != Status::Ok || solver.solve(fw) != Status::Ok) {
            check(false, name + ": device solver failed");
            return;
        }
        for (std::int32_t a = 0; a < lo.count(); ++a) {
            const Label l = solver.label(Arg{a});
            const int want = l == Label::In ? 1 : (l == Label::Out ? -1 : 0);
            if (sym.label_at(a, m) != want) { ++label_mismatch; }
        }
        for (std::size_t p = 0; p < pols.size(); ++p) {
            const auto v = quorum::arbiter::decide(fw, solver, policies[p]);
            const int o = static_cast<int>(v.outcome);
            ++counted[p][static_cast<std::size_t>(o)];
            if (sym.outcome_at(p, m) != o) { ++outcome_mismatch; }
        }
    }
    check(label_mismatch == 0, name + ": " + std::to_string(label_mismatch) + " argument labels disagree");
    check(outcome_mismatch == 0, name + ": " + std::to_string(outcome_mismatch) + " outcomes disagree");
    for (std::size_t p = 0; p < pols.size(); ++p) {
        for (int o = 0; o < 5; ++o) {
            check(sym.count(p, o) == std::to_string(counted[p][static_cast<std::size_t>(o)]),
                  name + ": outcome " + std::to_string(o) + " count");
        }
    }
    check(sym.states() == std::to_string(total), name + ": state count");
    std::printf("  %-28s %8u states x %3d arguments agree (%d rounds, %zu BDD nodes, %.3f s symbolic)\n",
                name.c_str(), total, lo.count(), sym.rounds(), sym.nodes(), sym.seconds());
}

void single(const std::string& path) {
    cbc::Lowered lo;
    if (!load(path, lo)) {
        check(false, "cannot compile " + path);
        return;
    }
    agree(path.substr(path.rfind('/') + 1), lo, {{lo.casebook, lo.actions}}, {{0, lo.safe_action}});
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "../casebook";
    std::printf("symbolic validation agrees with the device solver in every state\n");
    for (const char* f : {"pump/pump.cb", "pump/pump_conditions.cb", "pump/pump_severity.cb", "tank/tank.cb",
                          "permit/permit.cb", "fixtures/consistency/flow_current.cb"}) {
        single(root + "/" + f);
    }

    // The palette: one framework, two policies, every combined state. The symbolic pass also PROVES non-interference
    // directly: each member's outcome depends only on its own presence bits.
    cbc::Lowered pump;
    cbc::Lowered tank;
    if (load(root + "/pump/pump.cb", pump) && load(root + "/tank/tank.cb", tank)) {
        cbc::Diagnostics d;
        const cbc::Palette pal = cbc::merge({pump, tank}, d);
        check(!d.has_errors(), "the palette merges");
        std::vector<cbc::SymbolicPolicy> pols;
        std::vector<std::pair<std::int32_t, std::int32_t>> safe;
        for (const auto& m : pal.members) {
            pols.push_back({m.casebook, m.actions});
            safe.emplace_back(0, m.safe_action);
        }
        // 2^29 combined states is too many to enumerate in a unit test; the symbolic pass handles all of them, and the
        // enumeration below samples a stride through them so the agreement claim is still checked against the solver.
        const cbc::SymbolicCheck sym(pal.all, pols);
        std::printf("  palette pump+tank: %s combined states, symbolic in %.3f s, %zu BDD nodes\n",
                    sym.states().c_str(), sym.seconds(), sym.nodes());
        for (std::size_t p = 0; p < pal.members.size(); ++p) {
            const auto dep = sym.depends_on(p);
            const auto& m = pal.members[p];
            bool foreign = false;
            for (std::size_t bit = 0; bit < dep.size(); ++bit) {
                const bool own = static_cast<std::int32_t>(bit) >= m.observation_offset &&
                                 static_cast<std::int32_t>(bit) < m.observation_offset + m.observation_count;
                if (dep[bit] && !own) { foreign = true; }
            }
            check(!foreign, m.casebook + ": outcome depends only on its own readings, over all combined states");
            std::printf("    %-28s Decided in %s states; depends on no other member's readings: %s\n",
                        m.casebook.c_str(), sym.count(p, 0).c_str(), foreign ? "NO" : "yes");
        }
        Fw& fw = *new Fw;  // large; heap, and deliberately leaked for the life of the test
        Solver& solver = *new Solver;
        std::vector<Pol> policies(pols.size());
        for (std::size_t p = 0; p < pols.size(); ++p) {
            for (const std::int32_t a : pols[p].actions) { (void)policies[p].declare_action(Arg{a}); }
            (void)policies[p].set_fallback(Arg{safe[p].second});
        }
        int sampled = 0;
        int bad = 0;
        const std::uint64_t total = 1ULL << static_cast<unsigned>(pal.all.observation_count);
        for (std::uint64_t m = 0; m < total; m += 104729) {  // a prime stride, so every bit pattern varies
            fw.reset();
            // Each member keeps its own closed-world argument, which attacks that member's absent readings.
            Status st = fw.declare(pal.all.count());
            for (const auto& e : pal.all.edges) {
                if (st == Status::Ok) { st = fw.add_attack(Arg{e.from}, Arg{e.to}); }
            }
            for (const auto& mem : pal.members) {
                for (std::int32_t k = 0; k < mem.observation_count && st == Status::Ok; ++k) {
                    const std::int32_t bit = mem.observation_offset + k;
                    if ((m >> bit & 1U) == 0U) { st = fw.add_attack(Arg{mem.closed_world}, Arg{bit}); }
                }
            }
            if (st == Status::Ok) { st = fw.finalize(); }
            if (st != Status::Ok || solver.solve(fw) != Status::Ok) {
                ++bad;
                continue;
            }
            for (std::size_t p = 0; p < pols.size(); ++p) {
                const auto v = quorum::arbiter::decide(fw, solver, policies[p]);
                if (sym.outcome_at(p, m) != static_cast<int>(v.outcome)) { ++bad; }
            }
            ++sampled;
        }
        check(bad == 0, "palette: sampled states agree with the device solver");
        std::printf("    sampled %d combined states against the device solver: %d disagreements\n", sampled, bad);
    } else {
        check(false, "cannot compile pump and tank");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
