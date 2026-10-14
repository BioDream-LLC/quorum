// Does the exported framework describe the framework the device runs?
//
// cbc can emit ICCMA `.af` and ASPARTIX `.apx` files, which is how a THIRD-PARTY argumentation solver could check
// quorum's answers. That matters more than any other test here, because every other check in this project compares my
// code against my other code: `test_grounded` validates the solver against a reference implementation I also wrote, so
// agreement proves consistency, not correctness. An independent solver reading an exported file is the only available
// escape from that circularity.
//
// But the bridge has never been checked. If the exported file describes a different framework than the device runs, then
// an independent solver would confirm the wrong thing -- and confirm it convincingly. That is precisely the failure mode
// this project keeps finding: a check that looks like verification and is not.
//
// So: parse the emitted files back, rebuild a framework from them, solve it with the engine, and require the labelling to
// match the generated header's framework exactly.
//
// The one honest complication is that closed-world edges depend on the cycle's readings and are NOT in a static file --
// the `.af` header says so. So the comparison is made at the state a static file can describe: everything asserted, where
// no closed-world edge is live. Anything else would be comparing two different frameworks and calling the difference a
// bug.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/solver/grounded.hpp"

#include "pump_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;

using quorum::solver::Arg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

int g_failures = 0;

void fail(const std::string& what) {
    std::printf("  FAIL  %s\n", what.c_str());
    ++g_failures;
}

const char* label_name(Label l) {
    switch (l) {
        case Label::In: return "In";
        case Label::Out: return "Out";
        case Label::Undecided: return "Undecided";
    }
    return "?";
}

struct Parsed {
    std::int32_t arguments = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> attacks;  // zero-based
    bool ok = false;
};

/// Parse ICCMA `.af`: `p af <n>` then one `from to` pair per line, one-based.
Parsed parse_af(const std::string& path) {
    Parsed p;
    std::ifstream in(path);
    if (!in) { return p; }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        std::istringstream ls(line);
        std::string first;
        ls >> first;
        if (first == "p") {
            std::string fmt;
            ls >> fmt >> p.arguments;
            continue;
        }
        // An attack line. One-based in the file, zero-based internally -- this conversion is the entire reason the
        // Index<Tag> types exist in the engine, and getting it wrong here would silently shift every edge by one.
        std::int32_t from = 0;
        std::int32_t to = 0;
        std::istringstream again(line);
        if (again >> from >> to) {
            p.attacks.emplace_back(from - 1, to - 1);
        }
    }
    p.ok = p.arguments > 0;
    return p;
}

/// Parse ASPARTIX `.apx`: `arg(name).` and `att(from,to).`, by NAME rather than index.
///
/// Parsing by name is the point. The `.af` check would not catch a systematic renumbering, because it compares indices
/// to indices. Names are what a human reads in a third-party solver's output, so if the names are wrong the export is
/// useless even when the structure is right.
Parsed parse_apx(const std::string& path, std::vector<std::string>& names) {
    Parsed p;
    std::ifstream in(path);
    if (!in) { return p; }

    std::vector<std::pair<std::string, std::string>> named_attacks;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '%') { continue; }
        const auto open = line.find('(');
        const auto close = line.rfind(')');
        if (open == std::string::npos || close == std::string::npos) { continue; }
        const std::string head = line.substr(0, open);
        const std::string body = line.substr(open + 1, close - open - 1);
        if (head == "arg") {
            names.push_back(body);
        } else if (head == "att") {
            const auto comma = body.find(',');
            if (comma == std::string::npos) { continue; }
            named_attacks.emplace_back(body.substr(0, comma), body.substr(comma + 1));
        }
    }

    p.arguments = static_cast<std::int32_t>(names.size());
    for (const auto& [f, t] : named_attacks) {
        std::int32_t fi = -1;
        std::int32_t ti = -1;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == f) { fi = static_cast<std::int32_t>(i); }
            if (names[i] == t) { ti = static_cast<std::int32_t>(i); }
        }
        if (fi < 0 || ti < 0) {
            p.ok = false;
            return p;
        }
        p.attacks.emplace_back(fi, ti);
    }
    p.ok = p.arguments > 0;
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_export <pump.af> <pump.apx>\n");
        return 2;
    }
    const std::string af_path = argv[1];
    const std::string apx_path = argv[2];

    std::printf("exported framework against the generated header\n\n");

    // The state a static file can describe: everything asserted, so no closed-world edge is live.
    const std::uint32_t all_asserted =
        pump::kObservationCount >= 32 ? 0xFFFFFFFFu
                                      : (1U << static_cast<std::uint32_t>(pump::kObservationCount)) - 1U;

    pump::CaseFramework device;
    if (pump::instantiate(device, all_asserted) != Status::Ok) {
        fail("could not instantiate the device framework");
        return 1;
    }
    pump::CaseSolver device_solver;
    if (device_solver.solve(device) != Status::Ok) {
        fail("device solve failed");
        return 1;
    }

    std::printf("  device framework   %d arguments, %d attacks (state 0x%X, everything asserted)\n",
                device.argument_count(), device.attack_count(), static_cast<unsigned>(all_asserted));

    // ── the .af file ────────────────────────────────────────────────────────
    const Parsed af = parse_af(af_path);
    if (!af.ok) {
        fail("could not parse " + af_path);
    } else {
        std::printf("  .af                %d arguments, %zu attacks\n", af.arguments, af.attacks.size());
        if (af.arguments != device.argument_count()) {
            fail("argument count differs: .af says " + std::to_string(af.arguments) + ", device has " +
                 std::to_string(device.argument_count()));
        }
    }

    // ── the .apx file, parsed by name ───────────────────────────────────────
    std::vector<std::string> apx_names;
    const Parsed apx = parse_apx(apx_path, apx_names);
    if (!apx.ok) {
        fail("could not parse " + apx_path);
    } else {
        std::printf("  .apx               %d arguments, %zu attacks (parsed by name)\n", apx.arguments,
                    apx.attacks.size());
        if (apx.arguments != device.argument_count()) {
            fail("argument count differs: .apx says " + std::to_string(apx.arguments));
        } else {
            // Names must match the author's vocabulary, in order. A third-party solver reports argument names; if these
            // are wrong or shifted, its output is unreadable even when the structure is right.
            std::int32_t name_mismatches = 0;
            for (std::int32_t i = 0; i < apx.arguments; ++i) {
                if (apx_names[static_cast<std::size_t>(i)] != std::string(pump::name_of(i))) {
                    if (name_mismatches < 3) {
                        std::printf("        name %d: .apx says '%s', header says '%s'\n", i,
                                    apx_names[static_cast<std::size_t>(i)].c_str(), pump::name_of(i));
                    }
                    ++name_mismatches;
                }
            }
            if (name_mismatches > 0) {
                fail(std::to_string(name_mismatches) + " argument names differ between .apx and the header");
            } else {
                std::printf("  all %d argument names match the header exactly\n", apx.arguments);
            }
        }
    }

    if (g_failures > 0) { return 1; }

    // ── the check that matters: same labelling ───────────────────────────────
    //
    // Structure and names agreeing is necessary but not sufficient. Rebuild a framework from the exported edges, solve
    // it with the same engine, and require the labelling to match argument for argument. An edge reversed or dropped
    // would pass the counts above and change the conclusions.
    for (const auto& [source, parsed] : {std::pair<const char*, const Parsed&>{".af", af},
                                         std::pair<const char*, const Parsed&>{".apx", apx}}) {
        Framework<pump::kMaxArgs, pump::kMaxAttacks> rebuilt;
        if (rebuilt.declare(parsed.arguments) != Status::Ok) {
            fail(std::string(source) + ": declare failed");
            continue;
        }
        bool built = true;
        for (const auto& [f, t] : parsed.attacks) {
            if (rebuilt.add_attack(Arg{f}, Arg{t}) != Status::Ok) {
                fail(std::string(source) + ": add_attack refused edge " + std::to_string(f) + "->" + std::to_string(t));
                built = false;
                break;
            }
        }
        if (!built) { continue; }
        if (rebuilt.finalize() != Status::Ok) {
            fail(std::string(source) + ": finalize failed");
            continue;
        }

        Grounded<pump::kMaxArgs> solver;
        if (solver.solve(rebuilt) != Status::Ok) {
            fail(std::string(source) + ": solve failed");
            continue;
        }

        std::int32_t diffs = 0;
        for (std::int32_t a = 0; a < device.argument_count(); ++a) {
            if (solver.label(Arg{a}) != device_solver.label(Arg{a})) {
                if (diffs < 5) {
                    std::printf("        %s arg %d (%s): exported %s, device %s\n", source, a, pump::name_of(a),
                                label_name(solver.label(Arg{a})), label_name(device_solver.label(Arg{a})));
                }
                ++diffs;
            }
        }
        if (diffs > 0) {
            fail(std::string(source) + ": " + std::to_string(diffs) + " labels differ from the device framework");
        } else {
            std::printf("  %-4s rebuilt: all %d labels identical to the device framework\n", source,
                        device.argument_count());
        }
    }

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("The exported files describe the framework the device runs, so a third-party solver reading them\n");
        std::printf("would be checking the real thing. That is the only escape available from the circularity of every\n");
        std::printf("other test here: they compare my code against my other code.\n");
        std::printf("\nStill not done: actually running an independent solver. The bridge is verified; nobody has\n");
        std::printf("crossed it.\n");
        return 0;
    }
    std::printf("%d failures\n", g_failures);
    return 1;
}
