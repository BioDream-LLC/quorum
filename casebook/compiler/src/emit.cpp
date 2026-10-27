#include "quorum/cbc/cbc.hpp"

#include <algorithm>
#include <ios>
#include <set>
#include <ostream>
#include <iomanip>
#include <sstream>

namespace quorum::cbc {

/// See the declaration in cbc.hpp for why this is shared rather than duplicated.
std::uint32_t fingerprint(const Lowered& lo) {
    std::uint32_t h = 2166136261u;  // FNV-1a offset basis
    const auto mix = [&h](std::int32_t v) {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        for (int byte = 0; byte < 4; ++byte) {
            h = static_cast<std::uint32_t>(h ^ ((u >> (byte * 8)) & 0xFFu));
            h = static_cast<std::uint32_t>(h * 16777619u);
        }
    };
    mix(lo.count());
    mix(lo.observation_count);
    mix(lo.max_args);
    mix(lo.max_attacks);
    mix(static_cast<std::int32_t>(lo.edges.size()));
    for (const LoweredEdge& e : lo.edges) {
        mix(e.from);
        mix(e.to);
    }
    mix(static_cast<std::int32_t>(lo.actions.size()));
    for (const std::int32_t a : lo.actions) { mix(a); }
    // Since 2026-10-11 the fingerprint also covers the policy roles and the binding: which action is idle and which
    // is the fallback, and every setpoint, hysteresis, hold and staleness limit. Before, two images differing only in
    // their fallback or a threshold reported the same fingerprint, and a deliberately edited setpoint with a
    // recomputed CRC loaded. Same order everywhere: emit.cpp, the generated header, runtime/blob.hpp, tools/qcert.
    mix(lo.closed_world);
    mix(lo.idle_action);
    mix(lo.safe_action);
    mix(static_cast<std::int32_t>(lo.channels.size()));
    for (const LoweredChannel& c : lo.channels) {
        mix(c.valid_lo);
        mix(c.valid_hi);
        mix(static_cast<std::int32_t>(c.stale_after_ms));
        mix(static_cast<std::int32_t>(c.flatline_after_ms));
        mix(c.flatline_epsilon);
        mix(c.unavailable_obs);
        mix(c.flatline_obs);
    }
    mix(static_cast<std::int32_t>(lo.thresholds.size()));
    for (const LoweredThreshold& t : lo.thresholds) {
        mix(t.channel);
        mix(t.observation);
        mix(t.above ? 1 : 0);
        mix(t.on);
        mix(t.off);
        mix(static_cast<std::int32_t>(t.hold_ms));
    }
    // Timed readings, only when there are any, so every fingerprint from before they existed is unchanged.
    if (!lo.timers.empty()) {
        mix(static_cast<std::int32_t>(lo.timers.size()));
        for (const LoweredTimer& t : lo.timers) {
            mix(t.kind);
            mix(t.observation);
            mix(t.source);
            mix(t.after);
            mix(static_cast<std::int32_t>(t.window_ms));
            mix(t.times);
        }
    }
    return h;
}

namespace {

/// C++ keywords, because a domain name is not obliged to avoid them.
///
/// `action continue idle` is a perfectly reasonable thing for an engineer to write, and `continue` is a keyword.
/// The enum constant is therefore escaped with a trailing underscore while `name_of` keeps returning the name the
/// author actually wrote -- so a witness still reads in the engineer's vocabulary, not the compiler's.
const std::set<std::string>& cpp_keywords() {
    static const std::set<std::string> kw = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "co_await", "co_return", "co_yield", "compl",
        "concept", "const", "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype",
        "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq",
    };
    return kw;
}

std::string ident(const std::string& name) {
    std::string out;
    for (char c : name) {
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') ? c : '_');
    }
    if (out.empty()) { out = "_"; }
    if (std::isdigit(static_cast<unsigned char>(out[0])) != 0) { out.insert(out.begin(), '_'); }
    if (cpp_keywords().count(out) != 0) { out.push_back('_'); }
    return out;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); }
        out.push_back(c);
    }
    return out;
}

std::string pad(const std::string& s, std::size_t width) {
    std::string out = s;
    while (out.size() < width) { out.push_back(' '); }
    return out;
}

/// The binding tables: how physical signals become the observations above.
///
/// Emitted as `constexpr`, so on a microcontroller they live in flash and are never copied to RAM. Every constant
/// here was written in the .cb file and converted to milli-units by the compiler; the device compares integers only.
/// The timed readings: how the history of readings becomes the readings above. Only when the casebook has some.
void emit_timers(std::ostream& o, const Lowered& lo) {
    if (lo.timers.empty()) { return; }
    static const char* const kinds[] = {"After", "Recent", "Count", "First", "Held"};
    o << "// ── timed readings ─────────────────────────────────────────────────────────\n"
         "//\n"
         "// Include <quorum/binding/binding.hpp> to use these. Call step() once per cycle with the presence mask\n"
         "// WITHOUT the timer bits, and OR the result in. The framework never sees time: each of these is one\n"
         "// presence bit, and exhaustive validation covered every combination of them.\n"
         "constexpr std::int32_t kTimerCount = " << lo.timers.size() << ";\n\n"
      << "constexpr binding::TimerSpec kTimers[] = {\n";
    for (const LoweredTimer& t : lo.timers) {
        const std::string& self = lo.arguments[static_cast<std::size_t>(t.observation)].name;
        const std::string& src = lo.arguments[static_cast<std::size_t>(t.source)].name;
        o << "    {binding::TimerKind::" << kinds[t.kind] << ", " << t.observation << ", " << t.source << ", " << t.after
          << ", " << t.window_ms << "U, " << t.times << "},  // " << self << ": " << src;
        if (t.kind == 0) {
            o << " within " << t.window_ms << " ms after " << lo.arguments[static_cast<std::size_t>(t.after)].name;
        } else if (t.kind == 1) {
            o << " within last " << t.window_ms << " ms";
        } else {
            o << " " << t.times << " times in " << t.window_ms << " ms";
        }
        o << "\n";
    }
    o << "};\n\n"
      << "using CaseTimers = binding::Timers<" << lo.timers.size() << ">;\n\n"
      << "[[nodiscard]] inline binding::Status make_timers(CaseTimers& t) noexcept {\n"
      << "    return t.configure(kTimers, kTimerCount, kObservationCount);\n"
      << "}\n\n";
}

void emit_binding(std::ostream& o, const Lowered& lo) {
    if (lo.channels.empty()) {
        o << "// This casebook declares no channels, so the integrator supplies the presence mask directly.\n"
             "// Legitimate for discrete inputs and operator entries, but note what it costs: nothing here can\n"
             "// then tell a dead sensor from a sensor reporting nothing. Declare channels to get that.\n"
             "constexpr std::int32_t kChannelCount = 0;\n"
             "constexpr std::int32_t kThresholdCount = 0;\n\n";
        return;
    }

    o << "// ── binding ────────────────────────────────────────────────────────────────\n"
         "//\n"
         "// Include <quorum/binding/binding.hpp> to use these. Every threshold and timeout below was declared in\n"
         "// the casebook source, so it is covered by the provenance block at the top of this file: changing one\n"
         "// changes the document, the version and the witness, rather than only the firmware.\n";

    o << "constexpr std::int32_t kChannelCount = " << lo.channels.size() << ";\n"
      << "constexpr std::int32_t kThresholdCount = " << lo.thresholds.size() << ";\n\n";

    o << "enum Channel : std::int32_t {\n";
    for (std::size_t i = 0; i < lo.channels.size(); ++i) {
        const LoweredChannel& c = lo.channels[i];
        o << "    " << pad("ch_" + ident(c.name), 40) << " = " << i << ",";
        if (!c.unit.empty()) { o << "   // " << c.unit; }
        o << "\n";
    }
    o << "};\n\n";

    o << "constexpr binding::ChannelSpec kChannels[] = {\n";
    for (const LoweredChannel& c : lo.channels) {
        o << "    {" << c.valid_lo << ", " << c.valid_hi << ", " << c.stale_after_ms << "U, "
          << c.flatline_after_ms << "U, " << c.flatline_epsilon << ", " << c.unavailable_obs << ", "
          << c.flatline_obs << "},  // " << c.name;
        if (!c.unit.empty()) { o << " in " << c.unit; }
        o << "\n";
    }
    o << "};\n\n";

    o << "constexpr binding::ThresholdSpec kThresholds[] = {\n";
    for (const LoweredThreshold& t : lo.thresholds) {
        o << "    {" << t.channel << ", " << t.observation << ", "
          << (t.above ? "binding::Sense::Above" : "binding::Sense::Below") << ", " << t.on << ", " << t.off
          << (t.hold_ms != 0U ? ", " + std::to_string(t.hold_ms) + "U" : std::string{})
          << "},  // " << lo.channels[static_cast<std::size_t>(t.channel)].name << " "
          << (t.above ? "above " : "below ") << t.on
          << (t.hold_ms != 0U ? " for " + std::to_string(t.hold_ms) + " ms" : std::string{}) << " asserts "
          << lo.arguments[static_cast<std::size_t>(t.observation)].name << ", releases at " << t.off << "\n";
    }
    o << "};\n\n";

    o << "using CaseBinding = binding::Binding<" << lo.channels.size() << ", " << lo.thresholds.size() << ">;\n\n";

    o << "[[nodiscard]] inline binding::Status make_binding(CaseBinding& b) noexcept {\n"
      << "    return b.configure(kChannels, kChannelCount, kThresholds, kThresholdCount);\n"
      << "}\n\n";

    o << "/// The unit each channel is sampled in, for the witness.\n"
      << "[[nodiscard]] inline const char* unit_of(std::int32_t channel) noexcept {\n"
      << "    switch (channel) {\n";
    for (std::size_t i = 0; i < lo.channels.size(); ++i) {
        o << "        case " << i << ": return \"" << escape(lo.channels[i].unit) << "\";\n";
    }
    o << "        default: return \"\";\n    }\n}\n\n";

    o << "[[nodiscard]] inline const char* channel_name(std::int32_t channel) noexcept {\n"
      << "    switch (channel) {\n";
    for (std::size_t i = 0; i < lo.channels.size(); ++i) {
        o << "        case " << i << ": return \"" << escape(lo.channels[i].name) << "\";\n";
    }
    o << "        default: return \"?\";\n    }\n}\n\n";
}

}  // namespace

std::string emit_header(const Lowered& lo) {
    std::ostringstream o;
    const std::string ns = ident(lo.casebook);

    o << "#pragma once\n"
      << "//\n"
      << "// GENERATED by cbc from casebook '" << lo.casebook << "' v" << lo.version << " -- DO NOT EDIT.\n"
      << "//\n"
      << "// Provenance, carried from the source so the artefact is self-describing:\n"
      << "//   document : " << lo.provenance.document << "\n"
      << "//   dated    : " << (lo.provenance.date.empty() ? "(none)" : lo.provenance.date) << "\n"
      << "//   author   : " << (lo.provenance.author.empty() ? "(none)" : lo.provenance.author) << "\n";
    for (const std::string& n : lo.provenance.notes) { o << "//   note     : " << n << "\n"; }

    std::int32_t written = 0;
    std::int32_t generated = 0;
    for (const LoweredArgument& a : lo.arguments) { (a.generated ? generated : written)++; }
    o << "//\n"
      << "// " << lo.count() << " arguments (" << written << " written, " << generated << " generated), "
      << lo.edges.size() << " fixed attacks.\n"
      << "//\n\n";

    o << "#include <cstdint>\n\n"
      << "#include \"quorum/arbiter/verdict.hpp\"\n"
      << "#include \"quorum/solver/framework.hpp\"\n"
      << "#include \"quorum/solver/grounded.hpp\"\n"
      << "#include \"quorum/solver/ids.hpp\"\n";
    // A generated header must be self-contained. Emitting references to binding::ChannelSpec without the include
    // produced a header that compiled only when the translation unit happened to include the binding layer first --
    // which the host tests did and the firmware did not. Found by the first real cross-compile.
    if (!lo.channels.empty() || !lo.timers.empty()) { o << "#include \"quorum/binding/binding.hpp\"\n"; }
    o << "\n"
      << "namespace quorum::casebook::gen::" << ns << " {\n\n";

    // ── arguments ───────────────────────────────────────────────────────────
    o << "enum Argument : std::int32_t {\n";
    std::size_t width = 0;
    for (const LoweredArgument& a : lo.arguments) { width = std::max(width, ident(a.name).size()); }
    width = std::max(width, std::string("observation_end").size());

    Role last_role = Role::Observation;
    bool first = true;
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        const LoweredArgument& a = lo.arguments[static_cast<std::size_t>(i)];
        if (i == lo.observation_count) {
            o << "    " << pad("observation_end", width) << " = " << lo.observation_count
              << ",   // observations are a contiguous prefix; the binding layer toggles exactly these\n";
        }
        if (!first && a.role != last_role) { o << "\n"; }
        o << "    " << pad(ident(a.name), width) << " = " << i << ",";
        o << "   // " << role_name(a.role);
        if (a.generated) { o << ", generated"; }
        if (!a.relates_to.empty()) { o << " <- " << a.relates_to; }
        if (!a.description.empty()) { o << " : " << a.description; }
        o << "\n";
        last_role = a.role;
        first = false;
    }
    if (lo.observation_count == lo.count()) {
        o << "    " << pad("observation_end", width) << " = " << lo.observation_count << ",\n";
    }
    o << "    " << pad("argument_count", width) << " = " << lo.count() << ",\n";
    o << "};\n\n";

    o << "constexpr std::int32_t kObservationCount = observation_end;\n\n";

    // ── names ───────────────────────────────────────────────────────────────
    o << "/// The name the author wrote, not the escaped C++ identifier. Witnesses must read in the engineer's\n"
      << "/// vocabulary.\n"
      << "[[nodiscard]] inline const char* name_of(std::int32_t a) noexcept {\n"
      << "    switch (a) {\n";
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        o << "        case " << i << ": return \"" << escape(lo.arguments[static_cast<std::size_t>(i)].name)
          << "\";\n";
    }
    o << "        default: return \"???\";\n    }\n}\n\n";

    o << "[[nodiscard]] inline const char* description_of(std::int32_t a) noexcept {\n"
      << "    switch (a) {\n";
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        const LoweredArgument& a = lo.arguments[static_cast<std::size_t>(i)];
        if (a.description.empty()) { continue; }
        o << "        case " << i << ": return \"" << escape(a.description) << "\";\n";
    }
    o << "        default: return \"\";\n    }\n}\n\n";

    // ── edges ───────────────────────────────────────────────────────────────
    o << "struct Edge {\n    std::int32_t from;\n    std::int32_t to;\n};\n\n"
      << "inline constexpr Edge kCasebook[] = {\n";
    for (const LoweredEdge& e : lo.edges) {
        o << "    {" << e.from << ", " << e.to << "},";
        o << "   // " << lo.arguments[static_cast<std::size_t>(e.from)].name << " -> "
          << lo.arguments[static_cast<std::size_t>(e.to)].name;
        if (!e.reason.empty()) { o << " : " << e.reason; }
        o << "\n";
    }
    o << "};\n\n"
      << "constexpr std::int32_t kCasebookEdges = static_cast<std::int32_t>(sizeof(kCasebook) / "
         "sizeof(kCasebook[0]));\n\n";

    // ── capacities, sized exactly ───────────────────────────────────────────
    o << "// Sized exactly: no headroom, because the capacity is known at compile time and a device should not\n"
      << "// carry slack it can never use. kMaxAttacks covers the fixed edges plus one closed-world edge per\n"
      << "// observation, which is the worst case (every reading absent).\n"
      << "constexpr std::int32_t kMaxArgs = " << lo.max_args << ";\n"
      << "constexpr std::int32_t kMaxAttacks = " << lo.max_attacks << ";\n\n";

    o << "using CaseFramework = solver::Framework<kMaxArgs, kMaxAttacks>;\n"
      << "using CaseSolver = solver::Grounded<kMaxArgs>;\n"
      << "using CasePolicy = arbiter::Policy<" << std::max<std::size_t>(lo.actions.size(), 1) << ">;\n\n";

    // ── action metadata ─────────────────────────────────────────────────────
    o << "inline constexpr std::int32_t kActions[] = {";
    if (lo.actions.empty()) {
        o << "0";
    } else {
        for (std::size_t i = 0; i < lo.actions.size(); ++i) { o << (i ? ", " : "") << lo.actions[i]; }
    }
    o << "};\n"
      << "constexpr std::int32_t kActionCount = " << lo.actions.size() << ";\n"
      << "constexpr std::int32_t kIdleAction = " << lo.idle_action << ";\n"
      << "constexpr std::int32_t kSafeAction = " << lo.safe_action << ";\n\n";

    // Claims, for validators that want to assert things about diagnoses.
    std::vector<std::int32_t> claims;
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        if (lo.arguments[static_cast<std::size_t>(i)].role == Role::Claim) { claims.push_back(i); }
    }
    o << "inline constexpr std::int32_t kClaims[] = {";
    if (claims.empty()) {
        o << "0";
    } else {
        for (std::size_t i = 0; i < claims.size(); ++i) { o << (i ? ", " : "") << claims[i]; }
    }
    o << "};\n"
      << "constexpr std::int32_t kClaimCount = " << claims.size() << ";\n\n";

    // Exclusive groups, flattened. These are instrument-fault detectors, not states to be skipped.
    o << "// Exclusive groups: readings that cannot physically co-occur. When every member of a group is present\n"
      << "// the instrument is at fault, which is a reachable condition -- so a validator should REPORT these\n"
      << "// states separately, not skip them.\n"
      << "inline constexpr std::int32_t kExclusiveFlat[] = {";
    std::vector<std::int32_t> offsets;
    {
        std::vector<std::int32_t> flat;
        offsets.push_back(0);
        for (const auto& g : lo.exclusive_groups) {
            for (const std::int32_t m : g) { flat.push_back(m); }
            offsets.push_back(static_cast<std::int32_t>(flat.size()));
        }
        if (flat.empty()) {
            o << "0";
        } else {
            for (std::size_t i = 0; i < flat.size(); ++i) { o << (i ? ", " : "") << flat[i]; }
        }
    }
    o << "};\n"
      << "inline constexpr std::int32_t kExclusiveOffset[] = {";
    for (std::size_t i = 0; i < offsets.size(); ++i) { o << (i ? ", " : "") << offsets[i]; }
    o << "};\n"
      << "constexpr std::int32_t kExclusiveGroupCount = " << lo.exclusive_groups.size() << ";\n\n";

    // ── provenance as data ──────────────────────────────────────────────────
    o << "constexpr const char* kCasebookName = \"" << escape(lo.casebook) << "\";\n"
      << "constexpr std::int32_t kCasebookVersion = " << lo.version << ";\n"
      << "constexpr const char* kSourceDocument = \"" << escape(lo.provenance.document) << "\";\n"
      << "constexpr const char* kSourceDate = \"" << escape(lo.provenance.date) << "\";\n"
      << "constexpr const char* kSourceAuthor = \"" << escape(lo.provenance.author) << "\";\n\n";

    // ── helpers ─────────────────────────────────────────────────────────────
    o << "[[nodiscard]] inline constexpr bool is_observation(std::int32_t a) noexcept {\n"
      << "    return a >= 0 && a < observation_end;\n"
      << "}\n\n";

    // Which arguments did the author actually write?
    //
    // Operator-facing text must never name generated arguments. `gate_no_maintain_reason` is an artefact of how an
    // argumentation framework encodes "absent" -- it is not a fact about the pump, and showing it to someone holding a
    // radio is worse than showing nothing, because it invites them to look for a thing that does not exist.
    //
    // Found when the advisory tier's explanation renderer printed "rejects: continue, gate_no_maintain_reason" to what
    // is meant to be an operator-facing explanation.
    //
    // The test is ROLE, not the `generated` flag. Those differ, and using `generated` was wrong: a channel-derived
    // claim and an `exclusive` incoherence argument are both generated, yet both mean something a maintenance engineer
    // needs -- "the broadband and sideband readings cannot both be true" is exactly the kind of instrument-integrity
    // finding this product exists to surface. Only Gate and ClosedWorld are pure encoding machinery with no referent
    // in the plant.
    o << "[[nodiscard]] inline constexpr bool is_authored(std::int32_t a) noexcept {\n"
      << "    switch (a) {\n";
    bool any_generated = false;
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        const Role r = lo.arguments[static_cast<std::size_t>(i)].role;
        if (r == Role::Gate || r == Role::ClosedWorld) {
            o << "        case " << i << ":   // " << lo.arguments[static_cast<std::size_t>(i)].name
              << " (" << role_name(r) << ")\n";
            any_generated = true;
        }
    }
    if (any_generated) { o << "            return false;\n"; }
    o << "        default: return a >= 0 && a < argument_count;\n"
      << "    }\n"
      << "}\n\n";

    o << "[[nodiscard]] inline solver::Status make_policy(CasePolicy& p) noexcept {\n"
      << "    solver::Status s = solver::Status::Ok;\n";
    for (const std::int32_t a : lo.actions) {
        o << "    s = p.declare_action(solver::Arg{" << a << "});   // "
          << lo.arguments[static_cast<std::size_t>(a)].name << "\n"
          << "    if (s != solver::Status::Ok) { return s; }\n";
    }
    if (lo.safe_action >= 0) {
        o << "    return p.set_fallback(solver::Arg{" << lo.safe_action << "});   // safe state: "
          << lo.arguments[static_cast<std::size_t>(lo.safe_action)].name << "\n";
    } else {
        o << "    return solver::Status::BadArgument;   // no safe action declared\n";
    }
    o << "}\n\n";

    o << "/// Instantiate for one cycle. Bit i of `present_mask` means observation i is asserted.\n"
      << "///\n"
      << "/// Resets first, so the same framework object can be reused every control cycle -- which is the intended\n"
      << "/// device pattern, since a fixed-capacity framework is a static object and nothing is allocated per tick.\n"
      << "/// Without the reset the second cycle returns AlreadyFinalized forever: a device that worked once and then\n"
      << "/// faulted permanently. Found by writing real firmware, not by any host test, because every host test\n"
      << "/// happened to construct a fresh framework per iteration.\n"
      << "[[nodiscard]] inline solver::Status instantiate(CaseFramework& fw, std::uint32_t present_mask) "
         "noexcept {\n"
      << "    fw.reset();\n"
      << "    solver::Status s = fw.declare(argument_count);\n"
      << "    if (s != solver::Status::Ok) { return s; }\n\n"
      << "    for (std::int32_t i = 0; i < kCasebookEdges; ++i) {\n"
      << "        s = fw.add_attack(solver::Arg{kCasebook[i].from}, solver::Arg{kCasebook[i].to});\n"
      << "        if (s != solver::Status::Ok) { return s; }\n"
      << "    }\n\n"
      << "    for (std::int32_t a = 0; a < kObservationCount; ++a) {\n"
      << "        const bool asserted = (present_mask & (1U << static_cast<std::uint32_t>(a))) != 0U;\n"
      << "        if (!asserted) {\n"
      << "            s = fw.add_attack(solver::Arg{" << lo.closed_world << "}, solver::Arg{a});\n"
      << "            if (s != solver::Status::Ok) { return s; }\n"
      << "        }\n"
      << "    }\n\n"
      << "    return fw.finalize();\n"
      << "}\n\n";

    o << "[[nodiscard]] inline constexpr std::uint32_t mask_of(const std::int32_t* obs, std::int32_t count) "
         "noexcept {\n"
      << "    std::uint32_t m = 0;\n"
      << "    for (std::int32_t i = 0; i < count; ++i) { m |= (1U << static_cast<std::uint32_t>(obs[i])); }\n"
      << "    return m;\n"
      << "}\n\n";

    o << "constexpr std::int32_t kClosedWorld = " << lo.closed_world << ";\n\n";

    // The witness bound, computed rather than guessed.
    //
    // `Derivation` expands each argument at most once, so `MaxSteps == argument_count` can never truncate. Emitting
    // it means an integrator cannot get this wrong, and getting it wrong is expensive in the direction nobody
    // notices: the first firmware built here used 256 steps for a 44-argument casebook, and the resulting
    // Derivation object was 5,532 bytes -- 76% of the entire RAM budget -- to hold a structure that cannot exceed
    // 44 entries. Oversizing a fixed-capacity buffer is invisible on a host and expensive on a part.
    o << "/// Exact bound on witness steps: each argument is expanded at most once, so this can never truncate.\n"
      << "constexpr std::int32_t kMaxWitnessSteps = " << lo.count() << ";\n"
      << "// using CaseDerivation = witness::Derivation<kMaxWitnessSteps, kMaxArgs>;\n"
      << "//   ^ declare this yourself after including <quorum/witness/derivation.hpp>; the witness is optional and\n"
      << "//     this header does not drag it in.\n\n";

    // The worst case, found by exhaustive validation rather than guessed.
    //
    // Emitted because a timing measurement taken on a typical state has no bearing on worst-case execution time, and
    // WCET is the whole certification argument. Firmware measures THIS state.
    //
    // For most casebooks the worst case turns out to be the QUIET state, where nothing is asserted -- because every
    // closed-world edge is then live and the framework carries its maximum number of attacks. That is also a healthy
    // machine's most common state, so the worst case is not a rare corner: it is what the device does all day.
    o << "/// The state that costs the most work, found by solving every state. Measure timing on this one.\n"
      << "constexpr std::uint32_t kWorstCaseMask = 0x" << std::hex << std::uppercase << lo.worst_case_mask
      << std::dec << std::nouppercase << "U;\n"
      << "constexpr std::int32_t kWorstCaseSteps = " << lo.worst_case_steps << ";\n"
      << "/// The proven bound on steps: 2n + 2m. Measured work must never exceed it.\n"
      << "constexpr std::int32_t kStepBound = " << (2 * lo.count() + 2 * lo.max_attacks) << ";\n\n";

    emit_binding(o, lo);
    emit_timers(o, lo);

    // ── integrity: the link from the validated casebook to the running one ───
    //
    // Provenance above says which document a casebook came from. It says nothing about whether the tables below are
    // still the ones that were validated. Nothing stopped a developer hand-editing a generated header, pairing a stale
    // header with a newer engine, or building a palette from two different compiler versions -- and every one of those
    // produces a device that runs a framework no exhaustive validation was ever performed on, while still reporting
    // correct-looking provenance.
    //
    // An assessor asks for traceability from the verification evidence to the artefact in the field. This is that link:
    // cbc prints the fingerprint alongside the validation report, and the device recomputes it from the tables it
    // actually holds. A mismatch is not a warning -- it means the evidence does not describe this device, so the only
    // honest response is to surrender to the interlock.
    //
    // FNV-1a, 32-bit. Chosen because it is four lines, has no dependencies, is byte-exact across compilers and
    // architectures, and is being asked to detect corruption and editing rather than to resist an adversary. It is not
    // a security measure and the comment in the header says so.
    {
        const std::uint32_t h = fingerprint(lo);

        o << "/// Fingerprint of the structure cbc validated: argument and observation counts, capacities, every\n"
          << "/// attack edge in order, the action list, the idle and fallback actions, and every binding setpoint.\n"
          << "/// FNV-1a, 32-bit.\n"
          << "///\n"
          << "/// NOT a security measure -- it detects corruption, a hand-edited header, or a stale header paired with\n"
          << "/// a newer engine. It cannot resist anyone who wants to forge it, and it is not trying to.\n"
          << "constexpr std::uint32_t kCasebookFingerprint = 0x" << std::hex << h << std::dec << "u;\n\n";

        o << "/// Recompute the fingerprint from the tables this build actually holds.\n"
          << "///\n"
          << "/// Call once at startup and treat a mismatch as fatal. Do not call it every cycle: it is O(edges) and\n"
          << "/// the thing it detects cannot change while the device is running.\n"
          << "[[nodiscard]] inline std::uint32_t computed_fingerprint() noexcept {\n"
          << "    std::uint32_t h = 2166136261u;\n"
          << "    const auto mix = [&h](std::int32_t v) noexcept {\n"
          << "        const std::uint32_t u = static_cast<std::uint32_t>(v);\n"
          << "        for (int byte = 0; byte < 4; ++byte) {\n"
          << "            h = static_cast<std::uint32_t>(h ^ ((u >> (byte * 8)) & 0xFFu));\n"
          << "            h = static_cast<std::uint32_t>(h * 16777619u);\n"
          << "        }\n"
          << "    };\n"
          << "    mix(argument_count);\n"
          << "    mix(kObservationCount);\n"
          << "    mix(kMaxArgs);\n"
          << "    mix(kMaxAttacks);\n"
          << "    mix(kCasebookEdges);\n"
          << "    for (std::int32_t i = 0; i < kCasebookEdges; ++i) {\n"
          << "        mix(kCasebook[i].from);\n"
          << "        mix(kCasebook[i].to);\n"
          << "    }\n"
          << "    mix(kActionCount);\n"
          << "    for (std::int32_t i = 0; i < kActionCount; ++i) { mix(kActions[i]); }\n"
          << "    mix(kClosedWorld);\n"
          << "    mix(kIdleAction);\n"
          << "    mix(kSafeAction);\n"
          << "    mix(kChannelCount);\n";
        if (!lo.channels.empty()) {
            o << "    for (std::int32_t i = 0; i < kChannelCount; ++i) {\n"
              << "        const binding::ChannelSpec& c = kChannels[i];\n"
              << "        mix(c.valid_lo);\n"
              << "        mix(c.valid_hi);\n"
              << "        mix(static_cast<std::int32_t>(c.stale_after_ms));\n"
              << "        mix(static_cast<std::int32_t>(c.flatline_after_ms));\n"
              << "        mix(c.flatline_epsilon);\n"
              << "        mix(c.unavailable_obs);\n"
              << "        mix(c.flatline_obs);\n"
              << "    }\n";
        }
        o << "    mix(kThresholdCount);\n";
        if (!lo.thresholds.empty()) {
            o << "    for (std::int32_t i = 0; i < kThresholdCount; ++i) {\n"
              << "        const binding::ThresholdSpec& t = kThresholds[i];\n"
              << "        mix(t.channel);\n"
              << "        mix(t.observation);\n"
              << "        mix(t.sense == binding::Sense::Above ? 1 : 0);\n"
              << "        mix(t.on);\n"
              << "        mix(t.off);\n"
              << "        mix(static_cast<std::int32_t>(t.hold_ms));\n"
              << "    }\n";
        }
        if (!lo.timers.empty()) {
            o << "    mix(kTimerCount);\n"
              << "    for (std::int32_t i = 0; i < kTimerCount; ++i) {\n"
              << "        const binding::TimerSpec& t = kTimers[i];\n"
              << "        mix(static_cast<std::int32_t>(t.kind));\n"
              << "        mix(t.observation);\n"
              << "        mix(t.source);\n"
              << "        mix(t.after);\n"
              << "        mix(static_cast<std::int32_t>(t.window_ms));\n"
              << "        mix(t.times);\n"
              << "    }\n";
        }
        o << "    return h;\n"
          << "}\n\n"
          << "/// True when this build holds the structure cbc validated.\n"
          << "[[nodiscard]] inline bool verify_casebook() noexcept {\n"
          << "    return computed_fingerprint() == kCasebookFingerprint;\n"
          << "}\n\n";
    }


    o << "}  // namespace quorum::casebook::gen::" << ns << "\n";
    return o.str();
}

std::string emit_palette_header(const Palette& p) {
    std::ostringstream o;

    o << "// GENERATED by cbc. DO NOT EDIT.\n"
         "//\n"
         "// A PALETTE: several casebooks on one device. One framework, one solver, one solve per control cycle --\n"
         "// and one policy per casebook, applied to that single labelling.\n"
         "//\n"
         "// The policies are deliberately NOT merged. A pump action and a tank action being In at the same time is\n"
         "// correct behaviour on a device that supervises both; a merged policy would see two actions In and report\n"
         "// `Conflicted`, turning correct behaviour into a reported defect.\n"
         "//\n"
         "// Every name is qualified by its casebook. There is no cross-casebook attack anywhere in the framework\n"
         "// below -- that is checked by the compiler, not assumed, and it is the precondition under which ASPIC+'s\n"
         "// non-interference and crash-resistance postulates apply.\n"
         "//\n"
         "// casebooks in this palette:\n";
    for (const PaletteMember& m : p.members) {
        o << "//   " << pad(m.casebook, 24) << " observations " << m.observation_offset << ".."
          << (m.observation_offset + m.observation_count - 1) << "\n";
    }
    o << "//\n\n";

    o << "#pragma once\n\n"
      << "#include <cstdint>\n\n"
      << "#include \"quorum/arbiter/verdict.hpp\"\n"
      << "#include \"quorum/solver/framework.hpp\"\n"
      << "#include \"quorum/solver/grounded.hpp\"\n"
      << "#include \"quorum/solver/ids.hpp\"\n";
    if (!p.all.channels.empty()) { o << "#include \"quorum/binding/binding.hpp\"\n"; }
    o << "\n";

    o << "namespace quorum::casebook::gen::palette {\n\n"
      << "namespace solver = quorum::solver;\n"
      << "namespace arbiter = quorum::arbiter;\n\n";

    // ── the combined framework ──────────────────────────────────────────────
    o << "constexpr std::int32_t kArgumentCount = " << p.all.count() << ";\n"
      << "constexpr std::int32_t kObservationCount = " << p.all.observation_count << ";\n"
      << "constexpr std::int32_t kMaxArgs = " << p.all.max_args << ";\n"
      << "constexpr std::int32_t kMaxAttacks = " << p.all.max_attacks << ";\n\n";

    o << "using CaseFramework = solver::Framework<kMaxArgs, kMaxAttacks>;\n"
      << "using CaseSolver = solver::Grounded<kMaxArgs>;\n\n";

    o << "/// Argument names, for the witness. Qualified, so an explanation says which casebook it came from.\n"
      << "[[nodiscard]] inline const char* name_of(std::int32_t a) noexcept {\n"
      << "    switch (a) {\n";
    for (std::int32_t i = 0; i < p.all.count(); ++i) {
        o << "        case " << i << ": return \"" << escape(p.all.arguments[static_cast<std::size_t>(i)].name)
          << "\";\n";
    }
    o << "        default: return \"?\";\n    }\n}\n\n";

    o << "struct Edge {\n    std::int32_t from;\n    std::int32_t to;\n};\n\n"
      << "constexpr std::int32_t kCasebookEdges = " << p.all.edges.size() << ";\n"
      << "constexpr Edge kCasebook[] = {\n";
    if (p.all.edges.empty()) {
        o << "    {0, 0},   // unused; C++ forbids a zero-size array\n";
    } else {
        for (const LoweredEdge& e : p.all.edges) {
            o << "    {" << e.from << ", " << e.to << "},\n";
        }
    }
    o << "};\n\n";

    // Closed-world argument per member, so absence stays a member-local notion.
    o << "/// Each casebook keeps its OWN closed-world argument. Sharing one would behave identically -- both are\n"
      << "/// unattacked, so both are always In -- but it would introduce a syntactic edge between casebooks and\n"
      << "/// make any non-interference claim vacuous.\n"
      << "constexpr std::int32_t kClosedWorld[] = {\n";
    for (const PaletteMember& m : p.members) {
        o << "    " << m.closed_world << ",   // " << m.casebook << "\n";
    }
    o << "};\n"
      << "constexpr std::int32_t kCasebookCount = " << p.members.size() << ";\n\n";

    o << "/// Build the whole palette for one cycle. Bit i of `present_mask` is observation i of the COMBINED\n"
      << "/// numbering; each casebook's slice is given by its kObservationOffset below.\n"
      << "///\n"
      << "/// Resets first, so the same framework object is reused every control cycle.\n"
      << "[[nodiscard]] inline solver::Status instantiate(CaseFramework& fw, std::uint32_t present_mask) "
         "noexcept {\n"
      << "    fw.reset();\n"
      << "    solver::Status s = fw.declare(kArgumentCount);\n"
      << "    if (s != solver::Status::Ok) { return s; }\n\n"
      << "    for (std::int32_t i = 0; i < kCasebookEdges; ++i) {\n"
      << "        s = fw.add_attack(solver::Arg{kCasebook[i].from}, solver::Arg{kCasebook[i].to});\n"
      << "        if (s != solver::Status::Ok) { return s; }\n"
      << "    }\n\n";

    for (std::size_t i = 0; i < p.members.size(); ++i) {
        const PaletteMember& m = p.members[i];
        o << "    // " << m.casebook << "\n"
          << "    for (std::int32_t a = 0; a < " << m.observation_count << "; ++a) {\n"
          << "        const std::uint32_t bit = 1U << static_cast<std::uint32_t>(" << m.observation_offset
          << " + a);\n"
          << "        if ((present_mask & bit) == 0U) {\n"
          << "            s = fw.add_attack(solver::Arg{" << m.closed_world << "}, solver::Arg{"
          << m.observation_offset << " + a});\n"
          << "            if (s != solver::Status::Ok) { return s; }\n"
          << "        }\n"
          << "    }\n";
    }
    o << "\n    return fw.finalize();\n}\n\n";

    // ── one namespace per member ────────────────────────────────────────────
    for (const PaletteMember& m : p.members) {
        const std::string ns = ident(m.casebook);
        o << "// ───────────────────────────────────────────────────────────────────────────\n"
          << "namespace " << ns << " {\n\n"
          << "constexpr std::int32_t kObservationOffset = " << m.observation_offset << ";\n"
          << "constexpr std::int32_t kObservationCount = " << m.observation_count << ";\n"
          << "constexpr std::int32_t kClosedWorld = " << m.closed_world << ";\n\n";

        o << "/// This casebook's arguments, as indices into the COMBINED framework.\n"
          << "enum Argument : std::int32_t {\n";
        for (std::size_t k = 0; k < m.map.size(); ++k) {
            const std::int32_t combined = m.map[k];
            if (combined < 0) { continue; }
            std::string local = p.all.arguments[static_cast<std::size_t>(combined)].name;
            const std::size_t dot = local.find('.');
            if (dot != std::string::npos) { local = local.substr(dot + 1); }
            o << "    " << pad(ident(local), 40) << " = " << combined << ",\n";
        }
        o << "};\n\n";

        o << "using CasePolicy = arbiter::Policy<" << (m.actions.size() + 1) << ">;\n\n"
          << "[[nodiscard]] inline solver::Status make_policy(CasePolicy& p) noexcept {\n"
          << "    solver::Status s = solver::Status::Ok;\n";
        for (const std::int32_t a : m.actions) {
            o << "    s = p.declare_action(solver::Arg{" << a << "});\n"
              << "    if (s != solver::Status::Ok) { return s; }\n";
        }
        if (m.safe_action >= 0) {
            o << "    return p.set_fallback(solver::Arg{" << m.safe_action << "});   // "
              << p.all.arguments[static_cast<std::size_t>(m.safe_action)].name << "\n";
        } else {
            o << "    return solver::Status::BadArgument;   // no safe action declared\n";
        }
        o << "}\n\n";

        o << "/// Shift this casebook's own mask into the combined numbering.\n"
          << "[[nodiscard]] inline constexpr std::uint32_t shift(std::uint32_t local_mask) noexcept {\n"
          << "    return local_mask << static_cast<std::uint32_t>(kObservationOffset);\n"
          << "}\n\n"
          << "}  // namespace " << ns << "\n\n";
    }

    o << "}  // namespace quorum::casebook::gen::palette\n";
    return o.str();
}

std::string emit_af(const Lowered& lo) {
    std::ostringstream o;
    o << "# GENERATED by cbc from casebook '" << lo.casebook << "' v" << lo.version << "\n"
      << "# ICCMA .af format: one-based argument numbering, one attack per line.\n"
      << "# Fixed attacks only -- closed-world edges depend on the cycle's readings and are not representable\n"
      << "# in a static file.\n#\n"
      << "# argument names, in order:\n";
    for (std::int32_t i = 0; i < lo.count(); ++i) {
        o << "#   " << (i + 1) << " " << lo.arguments[static_cast<std::size_t>(i)].name << "\n";
    }
    o << "p af " << lo.count() << "\n";
    for (const LoweredEdge& e : lo.edges) { o << (e.from + 1) << " " << (e.to + 1) << "\n"; }
    return o.str();
}

std::string emit_apx(const Lowered& lo) {
    std::ostringstream o;
    o << "% GENERATED by cbc from casebook '" << lo.casebook << "' v" << lo.version << "\n"
      << "% ASPARTIX apx format, for interchange with other argumentation solvers.\n";
    for (const LoweredArgument& a : lo.arguments) { o << "arg(" << a.name << ").\n"; }
    for (const LoweredEdge& e : lo.edges) {
        o << "att(" << lo.arguments[static_cast<std::size_t>(e.from)].name << ","
          << lo.arguments[static_cast<std::size_t>(e.to)].name << ").\n";
    }
    return o.str();
}

std::string emit_report(const Casebook& cb, const Lowered& lo) {
    std::ostringstream o;

    o << "casebook   " << lo.casebook << " v" << lo.version << "\n"
      << "source     " << lo.provenance.document;
    if (!lo.provenance.date.empty()) { o << ", dated " << lo.provenance.date; }
    if (!lo.provenance.author.empty()) { o << ", by " << lo.provenance.author; }
    o << "\n";

    // The evidence basis, shown to whoever runs the compiler.
    //
    // `note` was parsed into Provenance::notes and emitted into the generated header as comments, so it survived
    // compilation -- but only for someone who opens a generated file. Nobody reviewing a casebook saw it, and the notes
    // are where the author records WHY the casebook is shaped the way it is. pump.cb records that vibration alone
    // classifies at roughly 89% while vibration with motor stator current reaches roughly 97%, which is the entire
    // justification for its claims needing two symptoms, and it was invisible in the one place a reviewer looks.
    //
    // Report only, deliberately. These are author prose of unbounded length and have no business on a device with a
    // 2,620-byte safety function; kSourceDocument / kSourceDate / kSourceAuthor are short, bounded, and remain the
    // machine-readable provenance.
    for (const std::string& n : lo.provenance.notes) {
        o << "  evidence   " << n << "\n";
    }
    o << "\n";

    std::int32_t written = 0;
    std::int32_t generated = 0;
    std::map<std::string, int> by_role;
    for (const LoweredArgument& a : lo.arguments) {
        (a.generated ? generated : written)++;
        by_role[role_name(a.role)]++;
    }

    o << "authored\n"
      << "  observations      " << cb.observes.size() << "\n"
      << "  exclusive groups  " << cb.exclusives.size() << "\n"
      << "  presumptions      " << cb.presumptions.size() << "\n"
      << "  attack lines      " << cb.attacks.size() << "\n"
      << "  actions           " << cb.actions.size() << "\n\n";

    o << "lowered\n";
    for (const auto& [role, n] : by_role) { o << "  " << pad(role, 18) << n << "\n"; }
    o << "  " << pad("TOTAL arguments", 18) << lo.count() << "  (" << written << " written, " << generated
      << " generated = " << (lo.count() > 0 ? (100 * generated / lo.count()) : 0) << "% plumbing)\n"
      << "  " << pad("fixed attacks", 18) << lo.edges.size() << "\n"
      << "  " << pad("capacity", 18) << "Framework<" << lo.max_args << ", " << lo.max_attacks << ">\n\n";

    std::vector<std::string> splits;
    for (const LoweredArgument& a : lo.arguments) {
        if (a.role == Role::UsableClaim) { splits.push_back(a.relates_to); }
    }
    if (!splits.empty()) {
        o << "raw/claim splits applied (a doubt discarded evidence it also depends on)\n";
        for (const std::string& s : splits) { o << "  " << s << " -> claim_" << s << "\n"; }
        o << "\n";
    }

    if (!lo.exclusive_groups.empty()) {
        o << "instrument-incoherence doubts generated from `exclusive`\n";
        for (const auto& g : lo.exclusive_groups) {
            o << "  ";
            for (std::size_t i = 0; i < g.size(); ++i) {
                o << (i ? " + " : "") << lo.arguments[static_cast<std::size_t>(g[i])].name;
            }
            o << "\n";
        }
        o << "\n";
    }

    if (!lo.preference_resolutions.empty()) {
        o << "attacks cancelled by FMEA severity (ASPIC+ preference ordering)\n"
             "  a less severe claim cannot defeat a more severe one, so the machine acts on the worse reading of\n"
             "  ambiguous evidence rather than escalating every ambiguity to a human\n";
        for (const auto& r : lo.preference_resolutions) {
            o << "  " << pad(r.attacker + " (severity " + std::to_string(r.attacker_severity) + ")", 40)
              << " no longer defeats " << r.target << " (severity " << r.target_severity << ")\n";
        }
        o << "\n";
    }

    o << "states to validate  " << (1L << lo.observation_count) << "  (2^" << lo.observation_count << ")\n";
    if (lo.idle_action >= 0) {
        o << "idle action         " << lo.arguments[static_cast<std::size_t>(lo.idle_action)].name << "\n";
    }
    if (lo.safe_action >= 0) {
        o << "safe state          " << lo.arguments[static_cast<std::size_t>(lo.safe_action)].name << "\n";
    }
    {
        // The traceability line. An assessor matches this against the device's startup banner; if they differ, the
        // validation evidence does not describe the device in the field.
        std::ostringstream fp;
        fp << "fingerprint         0x" << std::hex << std::setw(8) << std::setfill('0') << fingerprint(lo) << std::dec
           << "   (the device must report the same, or it is not this casebook)\n";
        o << fp.str();
    }

    return o.str();
}

}  // namespace quorum::cbc
