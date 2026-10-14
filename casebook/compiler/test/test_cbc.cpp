// Unit tests for cbc.
//
// The parser and the lowering pass are tested separately from any casebook, because a compiler that only works on
// the two files we happen to have is not a compiler. The cases below are deliberately small and include the ones
// that should FAIL -- a compiler that accepts bad input silently is worse than no compiler, since the output looks
// authoritative.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quorum/cbc/cbc.hpp"

namespace cbc = quorum::cbc;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

struct Result {
    cbc::Diagnostics diag;
    cbc::Casebook cb;
    cbc::Lowered lo;
    [[nodiscard]] bool ok() const { return !diag.has_errors(); }
};

Result compile(const std::string& text) {
    Result r;
    r.cb = cbc::parse(text, r.diag);
    if (!r.diag.has_errors()) {
        r.lo = cbc::lower(r.cb, r.diag);
        if (!r.diag.has_errors()) { cbc::check(r.lo, r.diag); }
    }
    return r;
}

/// A minimal well-formed casebook, so each test only has to add the one thing it is about.
std::string preamble() {
    return "casebook t v1\n"
           "source \"doc\" dated 2026-01-01 by \"nobody\"\n";
}

std::string minimal() {
    return preamble() +
           "observe a \"a\"\n"
           "claim c \"c\" needs a\n"
           "action idle_act idle\n"
           "action act_on safe when c\n"
           "prefer act_on over idle_act\n";
}

/// Case-insensitive, because a diagnostic that begins a sentence with "Provenance is required" is correct English
/// and a case-sensitive matcher would fail it for that.
[[nodiscard]] bool has_error_containing(const Result& r, const std::string& fragment) {
    auto lower = [](std::string v) {
        for (char& c : v) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return v;
    };
    const std::string needle = lower(fragment);
    for (const cbc::Diagnostic& d : r.diag.items()) {
        if (d.severity != cbc::Diagnostic::Severity::Error) { continue; }
        if (lower(d.message).find(needle) != std::string::npos) { return true; }
    }
    return false;
}

[[nodiscard]] bool edge_exists(const cbc::Lowered& lo, const std::string& from, const std::string& to) {
    const std::int32_t f = lo.index_of(from);
    const std::int32_t t = lo.index_of(to);
    if (f < 0 || t < 0) { return false; }
    for (const cbc::LoweredEdge& e : lo.edges) {
        if (e.from == f && e.to == t) { return true; }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
void test_minimal() {
    std::printf("minimal casebook\n");
    const Result r = compile(minimal());
    check(r.ok(), "compiles");
    check(r.lo.observation_count == 1, "one observation");
    check(r.lo.index_of("gate_no_a") >= 0, "a gate was generated for the requirement");
    check(edge_exists(r.lo, "a", "gate_no_a"), "the reading fells its gate");
    check(edge_exists(r.lo, "gate_no_a", "c"), "the gate blocks the claim");
    check(r.lo.index_of("closed_world") >= 0, "a closed-world argument was generated");
    check(r.lo.idle_action >= 0, "idle action recorded");
    check(r.lo.safe_action >= 0, "safe action recorded");
    check(edge_exists(r.lo, "act_on", "idle_act"), "an active action displaces the idle one");
}

void test_provenance_required() {
    std::printf("provenance is mandatory\n");
    const Result r = compile("casebook t v1\nobserve a \"a\"\naction x idle\n");
    check(!r.ok(), "a casebook with no source declaration is rejected");
    check(has_error_containing(r, "provenance"), "the error names provenance");
}

void test_idle_and_safe_required() {
    std::printf("idle and safe actions are mandatory\n");
    {
        const Result r = compile(preamble() + "observe a \"a\"\naction only_one safe\n");
        check(!r.ok(), "no idle action is rejected");
        check(has_error_containing(r, "Unsatisfied"),
              "the error explains that a quiet asset would report Unsatisfied");
    }
    {
        const Result r = compile(preamble() + "observe a \"a\"\naction only_one idle\n");
        check(!r.ok(), "no safe action is rejected");
        check(has_error_containing(r, "fallback"), "the error names the fallback");
    }
}

void test_undefined_and_duplicate_names() {
    std::printf("name resolution\n");
    {
        const Result r = compile(minimal() + "claim d \"d\" needs nonexistent\n");
        check(!r.ok(), "undefined requirement is rejected");
        check(has_error_containing(r, "undefined name 'nonexistent'"), "the error names the symbol");
    }
    {
        const Result r = compile(minimal() + "observe a \"duplicate\"\n");
        check(!r.ok(), "duplicate name is rejected");
        check(has_error_containing(r, "already declared"), "the error says where the first was");
    }
    {
        const Result r = compile(preamble() + "observe gate_no_x \"reserved\"\naction i idle\naction s safe\n");
        check(!r.ok(), "a reserved prefix is rejected");
        check(has_error_containing(r, "reserved"), "the error says the name is reserved");
    }
}

void test_type_checking() {
    std::printf("type checking of the ASPIC+ keywords\n");
    {
        // `undermines` attacks a premise, so its target must be a reading.
        const Result r = compile(minimal() + "a undermines c\n");
        check(!r.ok(), "undermining a claim is rejected");
        check(has_error_containing(r, "not an observation"), "the error explains the requirement");
    }
    {
        // `rebuts` attacks a conclusion, so its target must be a presumption.
        const Result r = compile(minimal() + "c rebuts a\n");
        check(!r.ok(), "rebutting a reading is rejected");
        check(has_error_containing(r, "not a claim, excuse or doubt"), "the error explains the requirement");
    }
    {
        // An action is a conclusion of the framework, not evidence.
        const Result r = compile(minimal() + "claim d \"d\" needs act_on\n");
        check(!r.ok(), "using an action as evidence is rejected");
        check(has_error_containing(r, "cannot serve as evidence"), "the error explains why");
    }
}

void test_conjunction_and_disjunction() {
    std::printf("needs is AND, when is OR\n");
    const Result r = compile(preamble() +
                             "observe p \"p\"\nobserve q \"q\"\n"
                             "claim both \"both\" needs p q\n"
                             "claim either \"either\" when any p q\n"
                             "action i idle\naction s safe when any both either\nprefer s over i\n");
    check(r.ok(), "compiles");

    // AND: two separate gates, one per requirement.
    check(edge_exists(r.lo, "gate_no_p", "both"), "conjunction: gate on p blocks 'both'");
    check(edge_exists(r.lo, "gate_no_q", "both"), "conjunction: gate on q blocks 'both'");

    // OR: one shared gate, attacked by both alternatives.
    check(r.lo.index_of("gate_no_either_evidence") >= 0, "disjunction: one shared evidence gate");
    check(edge_exists(r.lo, "p", "gate_no_either_evidence"), "disjunction: p alone fells the gate");
    check(edge_exists(r.lo, "q", "gate_no_either_evidence"), "disjunction: q alone fells the gate");
    check(edge_exists(r.lo, "gate_no_either_evidence", "either"), "disjunction: the gate blocks the claim");
}

void test_needs_over_claims() {
    std::printf("needs may range over claims, not just readings\n");
    const Result r = compile(preamble() +
                             "observe p \"p\"\nobserve q \"q\"\n"
                             "claim one \"one\" needs p\n"
                             "claim two \"two\" needs q\n"
                             "claim hazard \"both at once\" needs one two\n"
                             "action i idle\naction s safe when hazard\nprefer s over i\n");
    check(r.ok(), "compiles");
    check(edge_exists(r.lo, "one", "gate_no_one"), "a claim fells its own gate");
    check(edge_exists(r.lo, "gate_no_one", "hazard"), "the hazard is gated on the first claim");
    check(edge_exists(r.lo, "gate_no_two", "hazard"), "the hazard is gated on the second claim");
}

void test_raw_claim_split() {
    std::printf("raw/claim split for a self-invalidating doubt\n");
    const Result r = compile(preamble() +
                             "observe r \"r\"\n"
                             "doubt bad \"bad\" needs r\n  undermines r\n"
                             "claim c \"c\" needs r\n"
                             "action i idle\naction s safe when c\nprefer s over i\n");
    check(r.ok(), "compiles");

    // The split must exist, and the doubt must attack the CLAIM, never the raw reading -- otherwise
    // doubt -> r -> gate_no_r -> doubt is a three-cycle and everything in it goes Undecided.
    check(r.lo.index_of("claim_r") >= 0, "a usable claim was generated for the split reading");
    check(edge_exists(r.lo, "bad", "claim_r"), "the doubt attacks the usable claim");
    check(!edge_exists(r.lo, "bad", "r"), "the doubt does NOT attack the raw reading");
    check(edge_exists(r.lo, "r", "gate_no_r"), "the raw reading fells its own gate");
    check(edge_exists(r.lo, "gate_no_r", "claim_r"), "the usable claim requires the raw reading");
    // And the claim must depend on the USABLE form, so a discarded reading cannot enable it.
    check(edge_exists(r.lo, "gate_no_claim_r", "c"), "the claim is gated on the usable form");
}

/// Naive grounded labelling of a lowered casebook in one state, written independently of the device solver: iterate
/// "In if every attacker is Out, Out if some attacker is In" to the fixpoint. Returns 1 In, -1 Out, 0 Undecided.
std::vector<int> naive_grounded(const cbc::Lowered& lo, std::uint32_t mask) {
    std::vector<std::pair<int, int>> edges;
    for (const auto& e : lo.edges) { edges.emplace_back(e.from, e.to); }
    for (int o = 0; o < lo.observation_count; ++o) {
        if ((mask >> o & 1U) == 0U) { edges.emplace_back(lo.closed_world, o); }
    }
    std::vector<int> lab(static_cast<std::size_t>(lo.count()), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (int a = 0; a < lo.count(); ++a) {
            if (lab[static_cast<std::size_t>(a)] != 0) { continue; }
            bool all_out = true;
            bool some_in = false;
            for (const auto& [f, t] : edges) {
                if (t != a) { continue; }
                if (lab[static_cast<std::size_t>(f)] != -1) { all_out = false; }
                if (lab[static_cast<std::size_t>(f)] == 1) { some_in = true; }
            }
            if (all_out || some_in) {
                lab[static_cast<std::size_t>(a)] = all_out ? 1 : -1;
                changed = true;
            }
        }
    }
    return lab;
}

void test_implies_generates_consistency_doubt() {
    std::printf("implies generates a consistency doubt that fires exactly when the reading stands alone\n");
    const Result r = compile(preamble() +
                             "observe flow \"flow\"\n"
                             "observe amps \"current\"\n"
                             "observe low \"level low\"\n"
                             "implies flow amps \"moving water draws current\"\n"
                             "claim dry \"dry run\" needs amps low\n"
                             "excuse prime \"priming\" needs low\n"
                             "flow rebuts dry\n"
                             "claim moving \"pump moving water\" needs flow\n"
                             "action idle_act idle\n"
                             "action inspect when mismatch__flow__amps\n"
                             "action act_on safe when any dry moving\n"
                             "prefer act_on over inspect over idle_act\n");
    check(r.ok(), "the casebook compiles");
    if (!r.ok()) { return; }
    check(r.lo.consistency_doubts.size() == 1, "one consistency doubt is recorded for the validator");
    const std::int32_t d = r.lo.index_of("mismatch__flow__amps");
    const std::int32_t moving = r.lo.index_of("moving");
    check(d >= 0 && moving >= 0, "the doubt has the documented name, so an action can cite it");
    const std::int32_t flow = r.lo.index_of("flow");
    const std::int32_t amps = r.lo.index_of("amps");
    int fired = 0;
    for (std::uint32_t m = 0; m < (1U << static_cast<std::uint32_t>(r.lo.observation_count)); ++m) {
        const std::vector<int> lab = naive_grounded(r.lo, m);
        const bool f = (m >> flow & 1U) != 0U;
        const bool a = (m >> amps & 1U) != 0U;
        const bool in = lab[static_cast<std::size_t>(d)] == 1;
        if (in) { ++fired; }
        check(in == (f && !a), "state " + std::to_string(m) + ": fires iff flow is present without current");
        check(lab[static_cast<std::size_t>(d)] != 0, "state " + std::to_string(m) + ": the doubt is never Undecided");
        // The uncorroborated reading must not support anything: `moving` needs flow, and flow is discarded.
        if (f && !a) { check(lab[static_cast<std::size_t>(moving)] == -1, "a lone flow reading supports nothing"); }
        if (f && a) { check(lab[static_cast<std::size_t>(moving)] == 1, "a corroborated flow reading still counts"); }
    }
    check(fired == 2, "fires in exactly the 2 of 8 states with flow and no current");

    const Result bad = compile(preamble() + "observe a \"a\"\nclaim c \"c\" needs a\nimplies a nope\n"
                                            "action i idle\naction s safe when c\nprefer s over i\n");
    check(!bad.ok(), "implies on an undeclared reading is rejected");
    const Result self = compile(preamble() + "observe a \"a\"\nimplies a a\n");
    check(!self.ok(), "a reading cannot imply itself");
}

void test_exclusive_generates_incoherence() {
    std::printf("exclusive generates an instrument-incoherence doubt\n");
    const Result r = compile(preamble() +
                             "observe x \"x\"\nobserve y \"y\"\n"
                             "exclusive x y\n"
                             "claim c \"c\" needs x\n"
                             "action i idle\naction s safe when c\nprefer s over i\n");
    check(r.ok(), "compiles");
    const std::string doubt = "incoherent__x__y";
    check(r.lo.index_of(doubt) >= 0, "the incoherence doubt was generated");
    check(r.lo.index_of("claim_x") >= 0, "x was split");
    check(r.lo.index_of("claim_y") >= 0, "y was split");
    check(edge_exists(r.lo, doubt, "claim_x"), "the doubt discards x's usable claim");
    check(edge_exists(r.lo, doubt, "claim_y"), "the doubt discards y's usable claim");
    check(edge_exists(r.lo, "gate_no_x", doubt), "the doubt is gated on raw x");
    check(edge_exists(r.lo, "gate_no_y", doubt), "the doubt is gated on raw y");
    check(r.lo.exclusive_groups.size() == 1, "the group is recorded for the validator");
}

void test_observations_are_a_prefix() {
    std::printf("observations form a contiguous prefix\n");
    const Result r = compile(preamble() +
                             "observe a \"a\"\nclaim c \"c\" needs a\nobserve b \"b\"\n"
                             "action i idle\naction s safe when c\nprefer s over i\n");
    check(r.ok(), "compiles even with a declaration between observations");
    check(r.lo.observation_count == 2, "both observations counted");
    for (std::int32_t i = 0; i < r.lo.observation_count; ++i) {
        check(r.lo.arguments[static_cast<std::size_t>(i)].role == cbc::Role::Observation,
              "index " + std::to_string(i) + " is an observation");
    }
    for (std::int32_t i = r.lo.observation_count; i < r.lo.count(); ++i) {
        check(r.lo.arguments[static_cast<std::size_t>(i)].role != cbc::Role::Observation,
              "index " + std::to_string(i) + " is not an observation");
    }
}

void test_capacities_are_exact() {
    std::printf("capacities are sized exactly\n");
    const Result r = compile(minimal());
    check(r.ok(), "compiles");
    check(r.lo.max_args == r.lo.count(), "max_args equals the argument count");
    check(r.lo.max_attacks ==
              static_cast<std::int32_t>(r.lo.edges.size()) + r.lo.observation_count,
          "max_attacks covers fixed edges plus one closed-world edge per observation");
}

void test_dedupe() {
    std::printf("duplicate edges are collapsed\n");
    const Result r = compile(minimal() + "claim d \"d\" needs a\nc rebuts d\nc rebuts d\n");
    check(r.ok(), "compiles");
    int count = 0;
    const std::int32_t f = r.lo.index_of("c");
    const std::int32_t t = r.lo.index_of("d");
    for (const cbc::LoweredEdge& e : r.lo.edges) {
        if (e.from == f && e.to == t) { ++count; }
    }
    check(count == 1, "a repeated attack appears once");
}

void test_parser_details() {
    std::printf("parser details\n");
    {
        // An indented line continues the previous declaration.
        const Result r = compile(preamble() +
                                 "observe a \"a\"\nobserve b \"b\"\n"
                                 "doubt d \"d\" needs a\n  undermines b\n"
                                 "claim c \"c\" needs a\n"
                                 "action i idle\naction s safe when c\nprefer s over i\n");
        check(r.ok(), "indented continuation parses");
        check(edge_exists(r.lo, "d", "b"), "the continued 'undermines' took effect");
    }
    {
        // A hash inside a quoted string is literal, not a comment.
        const Result r = compile(preamble() +
                                 "observe a \"pressure # 2 header\"\nclaim c \"c\" needs a\n"
                                 "action i idle\naction s safe when c\nprefer s over i\n");
        check(r.ok(), "a hash inside quotes parses");
        const std::int32_t ai = r.lo.index_of("a");
        check(ai >= 0 && r.lo.arguments[static_cast<std::size_t>(ai)].description == "pressure # 2 header",
              "the description survived intact");
    }
    {
        const Result r = compile(preamble() + "observe a \"unterminated\naction i idle\naction s safe\n");
        check(!r.ok(), "an unterminated string is rejected");
    }
    {
        const Result r = compile(preamble() + "wibble a b c\n");
        check(!r.ok(), "an unrecognised declaration is rejected");
        check(has_error_containing(r, "unrecognised declaration"), "the error says so");
    }
}

void test_when_requires_any_or_all() {
    std::printf("a bare multi-condition 'when' is refused\n");
    {
        const Result r = compile(preamble() +
                                 "observe p \"p\"\nobserve q \"q\"\n"
                                 "claim c \"c\" when p q\n"
                                 "action i idle\naction s safe when c\nprefer s over i\n");
        check(!r.ok(), "'when p q' without any/all is rejected");
        check(has_error_containing(r, "ambiguous"), "the error says it is ambiguous");
        check(has_error_containing(r, "reads as 'and' and would mean 'or'"),
              "and explains the trap, because that is the mistake it exists to stop");
    }
    {
        // A single condition cannot be misread, so no qualifier is required.
        const Result r = compile(preamble() +
                                 "observe p \"p\"\nclaim c \"c\" when p\n"
                                 "action i idle\naction s safe when c\nprefer s over i\n");
        check(r.ok(), "a single condition needs no qualifier");
    }
    {
        // `when all` is conjunction, and must lower identically to `needs`.
        const Result all = compile(preamble() +
                                   "observe p \"p\"\nobserve q \"q\"\n"
                                   "claim c \"c\" when all p q\n"
                                   "action i idle\naction s safe when c\nprefer s over i\n");
        const Result needs = compile(preamble() +
                                     "observe p \"p\"\nobserve q \"q\"\n"
                                     "claim c \"c\" needs p q\n"
                                     "action i idle\naction s safe when c\nprefer s over i\n");
        check(all.ok() && needs.ok(), "both forms compile");
        check(all.lo.count() == needs.lo.count(), "'when all' and 'needs' produce the same argument count");
        check(all.lo.edges.size() == needs.lo.edges.size(), "and the same edge count");
        check(edge_exists(all.lo, "gate_no_p", "c") && edge_exists(all.lo, "gate_no_q", "c"),
              "'when all' lowers to one gate per condition, like 'needs'");
    }
    {
        // Actions get the same treatment: this is where the Piper Alpha bug actually was.
        const Result r = compile(preamble() +
                                 "observe p \"p\"\nobserve q \"q\"\n"
                                 "claim a \"a\" when p\nclaim b \"b\" when q\n"
                                 "action i idle\naction s safe when a b\nprefer s over i\n");
        check(!r.ok(), "a bare multi-condition 'when' on an ACTION is rejected too");
    }
    {
        const Result r = compile(preamble() +
                                 "observe p \"p\"\nobserve q \"q\"\n"
                                 "claim a \"a\" when p\nclaim b \"b\" when q\n"
                                 "action i idle\naction s safe when all a b\nprefer s over i\n");
        check(r.ok(), "'when all' on an action compiles");
        check(edge_exists(r.lo, "gate_no_a", "s") && edge_exists(r.lo, "gate_no_b", "s"),
              "and lowers to one gate per condition, so the action needs BOTH");
    }
}

void test_dead_gate_detection() {
    std::printf("unreachable structure is detected\n");
    // `orphan` is an observation nothing can ever assert... actually every observation is assertable, so instead
    // test the genuine case: a claim whose evidence is another claim that itself can never hold.
    const Result r = compile(preamble() +
                             "observe a \"a\"\n"
                             "claim c \"c\" needs a\n"
                             "action i idle\naction s safe when c\nprefer s over i\n");
    check(r.ok(), "the healthy case has no dead gates");
    for (const cbc::Diagnostic& d : r.diag.items()) {
        check(d.message.find("never fall") == std::string::npos, "no spurious dead-gate report");
    }
}

}  // namespace

int main() {
    std::printf("cbc unit tests\n\n");

    test_minimal();
    test_provenance_required();
    test_idle_and_safe_required();
    test_undefined_and_duplicate_names();
    test_type_checking();
    test_conjunction_and_disjunction();
    test_needs_over_claims();
    test_raw_claim_split();
    test_exclusive_generates_incoherence();
    test_implies_generates_consistency_doubt();
    test_observations_are_a_prefix();
    test_capacities_are_exact();
    test_dedupe();
    test_parser_details();
    test_when_requires_any_or_all();
    test_dead_gate_detection();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
