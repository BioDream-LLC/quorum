// The learning piece of the compiler, end to end: learn -> .cb -> cbc -> decide, on every input state.
//
//   test_learned_casebook <monks data dir> <output dir>
//
// For each MONK's problem and each casebook it might start from, this fits the v4 hybrid (the repaired method in
// PREREGISTRATION.md), writes it out as casebook SOURCE, and hands that text to the same compiler passes a
// hand-written casebook goes through -- parse, resolve, lower, check. Then it solves the compiled framework in every
// one of the 2^observations states and requires the compiled decision to equal the learner's own prediction in every
// one. The .cb files are left in <output dir> so ctest can also run the cbc binary itself over them -- the full
// validation pass, exactly as for pump.cb.
//
// What this establishes: the translation from a learned model to a casebook loses and adds nothing, on every state,
// for these models. What it does not: anything about accuracy, which is PREREGISTRATION.md's business.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/cbc/cbc.hpp"
#include "quorum/learn/emit_cb.hpp"
#include "quorum/learn/hybrid.hpp"

using quorum::solver::Arg;
using quorum::solver::Status;
using quorum::learn::AaCbr;
using quorum::learn::Case;
using quorum::learn::Features;
using quorum::learn::Folds;
using quorum::learn::Knowledge;
using quorum::learn::Method;

namespace {

int g_failures = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr int kFeatureCount = 17;
constexpr int kBase[7] = {0, 0, 3, 6, 8, 11, 15};
constexpr Features attr(int a, int v) { return Features{1} << (kBase[a] + v - 1); }
const char* const kNames[32] = {"a1_is_1", "a1_is_2", "a1_is_3", "a2_is_1", "a2_is_2", "a2_is_3", "a3_is_1",
                                "a3_is_2", "a4_is_1", "a4_is_2", "a4_is_3", "a5_is_1", "a5_is_2", "a5_is_3",
                                "a5_is_4", "a6_is_1", "a6_is_2", "unused17", "unused18", "unused19", "unused20",
                                "unused21", "unused22", "unused23", "unused24", "unused25", "unused26", "unused27",
                                "unused28", "unused29", "unused30", "unused31"};

std::vector<Case> load(const std::string& path) {
    std::ifstream in(path);
    std::vector<Case> out;
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        int cls = 0, v = 0;
        if (!(ss >> cls)) { continue; }
        Features x = 0;
        for (int a = 1; a <= 6; ++a) {
            ss >> v;
            x |= attr(a, v);
        }
        out.push_back({x, cls == 1});
    }
    return out;
}

// The host framework, built from the lowered casebook exactly as cbc's own validator does: every lowered edge, plus
// closed-world attacks on each absent observation. Written out here rather than shared, so that a mistake in cbc's
// construction is not silently reproduced by the check that is supposed to catch it.
using HostFw = quorum::solver::Framework<512, 8192>;
using HostSolver = quorum::solver::Grounded<512>;
using HostPolicy = quorum::arbiter::Policy<32>;

Status build(HostFw& fw, const quorum::cbc::Lowered& lo, std::uint32_t mask) {
    Status s = fw.declare(lo.count());
    for (const auto& e : lo.edges) {
        if (s == Status::Ok) { s = fw.add_attack(Arg{e.from}, Arg{e.to}); }
    }
    for (std::int32_t o = 0; o < lo.observation_count && s == Status::Ok; ++o) {
        if ((mask & (1U << static_cast<std::uint32_t>(o))) == 0U) { s = fw.add_attack(Arg{lo.closed_world}, Arg{o}); }
    }
    return s == Status::Ok ? fw.finalize() : s;
}

struct Result {
    std::int32_t states = 0;
    std::int32_t agree = 0;
    std::int32_t undecided = 0;
    std::int32_t arguments = 0;
    std::int32_t edges = 0;
    std::int32_t observations = 0;
    std::int32_t learned_cases = 0;
    std::int32_t rules = 0;
    std::int32_t pruned = 0;
    std::int32_t asserts = 0;
    std::uint32_t fnv = 2166136261U;  // FNV-1a over one byte per state: 1 assert, 0 default, 2 not Decided
};

Result compile_and_compare(AaCbr& model, Features mask, const std::string& name, const std::string& out_dir,
                           const std::string& document) {
    Result r;
    quorum::learn::EmitOptions opt;
    opt.name = name;
    opt.document = document;
    opt.dated = "2026-09-30";
    opt.by = "quorum learn (AA-CBR, v4 hybrid)";
    opt.feature_names = kNames;
    opt.asserts = "positive";
    opt.otherwise = "negative";
    const quorum::learn::CasebookText cb = quorum::learn::emit_casebook(model, mask, opt);
    r.pruned = cb.spikes_pruned;
    {
        std::ofstream f(out_dir + "/" + name + ".cb");
        f << cb.source;
        expect(f.good(), name + ": could not write the .cb file");
    }
    for (std::int32_t i = 1; i <= static_cast<std::int32_t>(model.cases().size()); ++i) {
        (model.is_prior(i) ? r.rules : r.learned_cases) += 1;
    }

    quorum::cbc::Diagnostics diag;
    quorum::cbc::Casebook ast = quorum::cbc::parse(cb.source, diag);
    quorum::cbc::desugar_votes(ast, diag);
    quorum::cbc::Lowered lo = quorum::cbc::lower(ast, diag);
    if (!diag.has_errors()) { quorum::cbc::check(lo, diag); }
    for (const auto& d : diag.items()) {
        std::printf("    %s: line %d: %s %s\n", name.c_str(), d.line,
                    d.severity == quorum::cbc::Diagnostic::Severity::Error ? "error" : "warning", d.message.c_str());
    }
    expect(!diag.has_errors(), name + ": cbc rejected the learned casebook");
    if (diag.has_errors()) { return r; }

    // The words matter to the reviewer even where the semantics would not: a case concluding the asserted outcome
    // must read as a `claim`, one concluding the default as an `excuse`.
    for (std::int32_t i = 1; i <= static_cast<std::int32_t>(model.cases().size()); ++i) {
        const Case& c = model.cases()[static_cast<std::size_t>(i - 1)];
        const std::string cname = std::string(model.is_prior(i) ? "rule_" : "case_") + std::to_string(i);
        const std::int32_t ix = lo.index_of(cname);
        if (ix < 0) { continue; }  // an omitted spike
        const auto role = lo.arguments[static_cast<std::size_t>(ix)].role;
        const bool asserting = c.outcome != model.default_outcome();
        expect(role == (asserting ? quorum::cbc::Role::Claim : quorum::cbc::Role::Excuse),
               name + ": " + cname + " is written as the wrong kind");
    }

    r.arguments = lo.count();
    r.edges = static_cast<std::int32_t>(lo.edges.size());
    r.observations = lo.observation_count;
    expect(r.observations == static_cast<std::int32_t>(cb.observation_feature.size()),
           name + ": observation count changed in compilation");

    HostPolicy policy;
    for (std::int32_t a : lo.actions) { expect(policy.declare_action(Arg{a}) == Status::Ok, name + ": policy"); }
    expect(policy.set_fallback(Arg{lo.safe_action}) == Status::Ok, name + ": fallback");
    const std::int32_t assert_ix = lo.index_of(cb.assert_action);

    const std::uint32_t total = 1U << static_cast<std::uint32_t>(lo.observation_count);
    for (std::uint32_t s = 0; s < total; ++s) {
        HostFw fw;
        HostSolver solver;
        if (build(fw, lo, s) != Status::Ok) {
            expect(false, name + ": framework construction failed");
            return r;
        }
        const quorum::arbiter::Verdict v = quorum::arbiter::decide(fw, solver, policy);
        ++r.states;
        if (v.outcome != quorum::arbiter::Outcome::Decided) { ++r.undecided; }
        Features q = 0;
        for (std::size_t o = 0; o < cb.observation_feature.size(); ++o) {
            if ((s >> o) & 1U) { q |= Features{1} << cb.observation_feature[o]; }
        }
        const bool device_asserts = v.action.valid() && v.action.get() == assert_ix;
        const std::uint32_t byte = v.outcome != quorum::arbiter::Outcome::Decided ? 2U : (device_asserts ? 1U : 0U);
        r.fnv = (r.fnv ^ byte) * 16777619U;
        r.asserts += device_asserts ? 1 : 0;
        const bool learner_asserts = model.predict(q).outcome != model.default_outcome();
        r.agree += device_asserts == learner_asserts ? 1 : 0;
    }
    expect(r.agree == r.states, name + ": the compiled casebook and the learner disagree on some state");
    {
        // What a device running this casebook must reproduce, state for state. firmware/pico2/learned.cpp prints the
        // same three numbers computed on silicon.
        std::ofstream f(out_dir + "/" + name + ".expected");
        char line[160];
        std::snprintf(line, sizeof line, "%s states %d asserts %d fnv1a 0x%08x\n", name.c_str(), r.states, r.asserts,
                      r.fnv);
        f << line;
    }
    expect(r.undecided == 0, name + ": some state was not Decided");
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: test_learned_casebook <monks dir> <output dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string out = argv[2];

    struct Term {
        std::vector<Case> cases;
    };
    std::vector<Case> m3t2;
    for (int a5 = 1; a5 <= 3; ++a5) {
        for (int a2 = 1; a2 <= 2; ++a2) { m3t2.push_back({attr(5, a5) | attr(2, a2), true}); }
    }
    struct Problem {
        const char* name;
        std::vector<Term> terms;
    };
    const std::vector<Problem> problems = {
        {"monks-1", {{{{attr(1, 1) | attr(2, 1), true}, {attr(1, 2) | attr(2, 2), true}, {attr(1, 3) | attr(2, 3), true}}},
                     {{{attr(5, 1), true}}}}},
        {"monks-3", {{{{attr(5, 3) | attr(4, 1), true}}}, {m3t2}}},
    };

    std::printf("learned casebooks through the compiler: every state, compiled decision against the learner\n");
    std::printf("  %-22s %4s %6s %6s %7s %5s %6s %8s %8s\n", "casebook", "k", "rules", "cases", "pruned", "obs", "args",
                "edges", "states");
    for (const Problem& p : problems) {
        const std::vector<Case> train = load(dir + "/" + p.name + ".train");
        expect(!train.empty(), std::string(p.name) + ": no training data");
        std::vector<std::size_t> order(train.size());
        for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
        std::mt19937 rng(20260929U);
        std::shuffle(order.begin(), order.end(), rng);
        Folds inner(5);
        for (std::size_t i = 0; i < order.size(); ++i) {
            for (std::size_t f = 0; f < 5U; ++f) { (i % 5U == f ? inner[f].second : inner[f].first).push_back(train[order[i]]); }
        }
        for (unsigned s = 1; s < 4U; ++s) {  // the empty casebook is plain learning; covered by the conditions below
            Knowledge kb;
            kb.min_exception_support = 2;
            kb.may_decline = true;
            for (unsigned t = 0; t < 2U; ++t) {
                if (((s >> t) & 1U) == 0U) { continue; }
                for (const Case& c : p.terms[t].cases) {
                    kb.priors.push_back(c);
                    kb.features |= c.x;
                }
            }
            const int k = quorum::learn::choose_k(Method::Hybrid, kb, inner, kFeatureCount);
            Features mask = 0;
            const auto model = quorum::learn::fit_aacbr(Method::Hybrid, kb, train, k, kFeatureCount, &mask);
            const std::string name = std::string(p.name) + "-knows-" + std::to_string(s);
            const Result r = compile_and_compare(*model, mask, name, out,
                                                 std::string("UCI MONK's problems, ") + p.name + ".train");
            std::printf("  %-22s %4d %6d %6d %7d %5d %6d %8d %8d   %s\n", name.c_str(), k, r.rules, r.learned_cases,
                        r.pruned, r.observations, r.arguments, r.edges, r.states,
                        r.agree == r.states && r.states > 0 ? "all agree" : "DISAGREE");
        }
    }
    if (g_failures != 0) {
        std::printf("test_learned_casebook: %d failed\n", g_failures);
        return 1;
    }
    std::printf("test_learned_casebook: all passed\n");
    return 0;
}
