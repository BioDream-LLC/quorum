// Export an instantiated casebook framework, plus quorum's own labelling, for independent cross-checking.
//
// Every other test in this project compares my code against my other code. `test_grounded` checks the solver against a
// reference implementation I also wrote, so agreement proves consistency rather than correctness. This is the first step
// out of that circularity: dump the framework in ICCMA i23 format so a genuinely different solver can be asked what it
// thinks, and dump quorum's labelling so the answers can be compared.
//
// `cbc --af` already emits a static file, but only the FIXED attacks -- closed-world edges depend on the cycle's readings
// and cannot appear in a static export. That limits cross-checking to the single state where everything is asserted. This
// tool instantiates a chosen state and exports the framework the device would actually solve, closed-world edges
// included, so any state can be checked.
//
// Output on stdout: one line per argument, `<one-based index> <IN|OUT|UNDEC> <name>`.
// The framework goes to the file named by argv[2].

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "quorum/solver/grounded.hpp"

#include "permit_generated.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"
#include "vote_generated.hpp"

using quorum::solver::Arg;
using quorum::solver::Label;
using quorum::solver::Status;

namespace {

const char* label_name(Label l) {
    switch (l) {
        case Label::In: return "IN";
        case Label::Out: return "OUT";
        case Label::Undecided: return "UNDEC";
    }
    return "?";
}

/// Write the framework in ICCMA i23 format: `p af <n>`, then one `from to` pair per line, ONE-BASED.
///
/// The one-based conversion is the whole reason the engine has distinct `Arg` and `FileArg` types. Getting it wrong here
/// would shift every edge by one and produce a framework that still parses, still solves, and answers about the
/// neighbouring argument.
template <typename Fw>
bool write_af(const Fw& fw, const std::string& path) {
    std::ofstream out(path);
    if (!out) { return false; }
    out << "p af " << fw.argument_count() << "\n";
    for (std::int32_t a = 0; a < fw.argument_count(); ++a) {
        const std::int32_t end = fw.out_end(Arg{a});
        for (std::int32_t k = fw.out_begin(Arg{a}); k < end; ++k) {
            out << (a + 1) << " " << (fw.target(k).get() + 1) << "\n";
        }
    }
    return static_cast<bool>(out);
}

template <typename Fw, typename Solver, typename NameOf, typename Instantiate>
int run(std::uint32_t mask, const std::string& path, NameOf name_of, Instantiate instantiate) {
    Fw fw;
    Solver solver;
    if (instantiate(fw, mask) != Status::Ok) {
        std::fprintf(stderr, "instantiate failed\n");
        return 1;
    }
    if (solver.solve(fw) != Status::Ok) {
        std::fprintf(stderr, "solve failed\n");
        return 1;
    }
    if (!write_af(fw, path)) {
        std::fprintf(stderr, "could not write %s\n", path.c_str());
        return 1;
    }
    for (std::int32_t a = 0; a < fw.argument_count(); ++a) {
        std::printf("%d %s %s\n", a + 1, label_name(solver.label(Arg{a})), name_of(a));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: dump_af <casebook> <out.af> <state-mask-hex>\n"
                             "       casebook: pump | tank | permit | vote\n");
        return 2;
    }
    const std::string which = argv[1];
    const std::string path = argv[2];
    const std::uint32_t mask = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 16));

    namespace pump = quorum::casebook::gen::centrifugal_pump;
    namespace tank = quorum::casebook::gen::tank_409;
    namespace permit = quorum::casebook::gen::piper_permit;
    namespace vote = quorum::casebook::gen::vessel_hl_trip;

    if (which == "pump") {
        return run<pump::CaseFramework, pump::CaseSolver>(
            mask, path, [](std::int32_t a) { return pump::name_of(a); },
            [](pump::CaseFramework& f, std::uint32_t m) { return pump::instantiate(f, m); });
    }
    if (which == "tank") {
        return run<tank::CaseFramework, tank::CaseSolver>(
            mask, path, [](std::int32_t a) { return tank::name_of(a); },
            [](tank::CaseFramework& f, std::uint32_t m) { return tank::instantiate(f, m); });
    }
    if (which == "permit") {
        return run<permit::CaseFramework, permit::CaseSolver>(
            mask, path, [](std::int32_t a) { return permit::name_of(a); },
            [](permit::CaseFramework& f, std::uint32_t m) { return permit::instantiate(f, m); });
    }
    if (which == "vote") {
        return run<vote::CaseFramework, vote::CaseSolver>(
            mask, path, [](std::int32_t a) { return vote::name_of(a); },
            [](vote::CaseFramework& f, std::uint32_t m) { return vote::instantiate(f, m); });
    }
    std::fprintf(stderr, "unknown casebook '%s'\n", which.c_str());
    return 2;
}
