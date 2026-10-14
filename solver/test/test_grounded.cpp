#include <cstdint>
#include <cstdio>
#include <random>
#include <type_traits>
#include <vector>

#include "quorum/solver/framework.hpp"
#include "quorum/solver/grounded.hpp"
#include "quorum/solver/ids.hpp"
#include "reference.hpp"

using quorum::solver::Arg;
using quorum::solver::FileArg;
using quorum::solver::Framework;
using quorum::solver::Grounded;
using quorum::solver::Label;
using quorum::solver::Status;

// ─────────────────────────────────────────────────────────────────────────────
// Minimal harness. No dependency, so this builds anywhere the library does.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void section(const char* name) { std::printf("%s\n", name); }

/// Build a finalized framework from an attack list. Aborts the test on any status error.
template <std::int32_t MA, std::int32_t MT>
bool build(Framework<MA, MT>& fw, std::int32_t n, const std::vector<quorum::reference::Attack>& attacks) {
    if (fw.declare(n) != Status::Ok) { return false; }
    for (const auto& at : attacks) {
        if (fw.add_attack(Arg{at.from}, Arg{at.to}) != Status::Ok) { return false; }
    }
    return fw.finalize() == Status::Ok;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Hand-computed cases. Each one is a shape whose grounded extension can be read off by eye, so a regression
// here points at the algorithm rather than at the oracle.
// ─────────────────────────────────────────────────────────────────────────────
static void test_hand_cases() {
    section("hand-computed cases");

    // Empty framework.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 0, {}), "empty: builds");
        check(g.solve(fw) == Status::Ok, "empty: solves");
        check(g.in_count() == 0, "empty: extension is empty");
    }

    // One unattacked argument is in.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 1, {}), "single: builds");
        check(g.solve(fw) == Status::Ok, "single: solves");
        check(g.accepted(Arg{0}), "single: unattacked argument is In");
        check(g.in_count() == 1, "single: extension size 1");
    }

    // A self-attacker is Undecided -- never In, and not Out either, because nothing in the extension attacks it.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 1, {{0, 0}}), "self-attack: builds");
        check(g.solve(fw) == Status::Ok, "self-attack: solves");
        check(g.label(Arg{0}) == Label::Undecided, "self-attack: Undecided, not In and not Out");
        check(g.in_count() == 0, "self-attack: extension is empty");
    }

    // a -> b. a is in, b is out.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 2, {{0, 1}}), "a->b: builds");
        check(g.solve(fw) == Status::Ok, "a->b: solves");
        check(g.label(Arg{0}) == Label::In, "a->b: a is In");
        check(g.label(Arg{1}) == Label::Out, "a->b: b is Out");
    }

    // a -> b -> c. c is reinstated, because a defends it by attacking b.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 3, {{0, 1}, {1, 2}}), "a->b->c: builds");
        check(g.solve(fw) == Status::Ok, "a->b->c: solves");
        check(g.label(Arg{0}) == Label::In, "a->b->c: a is In");
        check(g.label(Arg{1}) == Label::Out, "a->b->c: b is Out");
        check(g.label(Arg{2}) == Label::In, "a->b->c: c is reinstated (In)");
        check(g.in_count() == 2, "a->b->c: extension size 2");
    }

    // Two-cycle: mutual attack settles nothing.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 2, {{0, 1}, {1, 0}}), "2-cycle: builds");
        check(g.solve(fw) == Status::Ok, "2-cycle: solves");
        check(g.label(Arg{0}) == Label::Undecided, "2-cycle: a Undecided");
        check(g.label(Arg{1}) == Label::Undecided, "2-cycle: b Undecided");
        check(g.in_count() == 0, "2-cycle: extension is empty");
    }

    // Three-cycle: the odd cycle that makes stable semantics fail. Grounded declines cleanly.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 3, {{0, 1}, {1, 2}, {2, 0}}), "3-cycle: builds");
        check(g.solve(fw) == Status::Ok, "3-cycle: solves");
        check(g.in_count() == 0, "3-cycle: extension is empty");
        for (std::int32_t i = 0; i < 3; ++i) {
            check(g.label(Arg{i}) == Label::Undecided, "3-cycle: all Undecided");
        }
    }

    // A cycle does not contaminate an unrelated argument.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 3, {{0, 1}, {1, 0}}), "cycle+free: builds");
        check(g.solve(fw) == Status::Ok, "cycle+free: solves");
        check(g.label(Arg{2}) == Label::In, "cycle+free: unrelated argument still In");
        check(g.in_count() == 1, "cycle+free: extension size 1");
    }

    // Two independent attackers on one target: the target is Out once, not twice.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 3, {{0, 1}, {2, 1}}), "two attackers: builds");
        check(g.solve(fw) == Status::Ok, "two attackers: solves");
        check(g.label(Arg{0}) == Label::In, "two attackers: a In");
        check(g.label(Arg{2}) == Label::In, "two attackers: c In");
        check(g.label(Arg{1}) == Label::Out, "two attackers: b Out");
        check(g.in_count() == 2, "two attackers: extension size 2");
    }

    // A longer reinstatement chain, to catch an off-by-one in the propagation depth.
    // 0->1->2->3->4 : evens in, odds out.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(build(fw, 5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}}), "chain5: builds");
        check(g.solve(fw) == Status::Ok, "chain5: solves");
        check(g.label(Arg{0}) == Label::In, "chain5: 0 In");
        check(g.label(Arg{1}) == Label::Out, "chain5: 1 Out");
        check(g.label(Arg{2}) == Label::In, "chain5: 2 In");
        check(g.label(Arg{3}) == Label::Out, "chain5: 3 Out");
        check(g.label(Arg{4}) == Label::In, "chain5: 4 In");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A worked drone-safety framework, which is the shape this line exists to serve.
// ─────────────────────────────────────────────────────────────────────────────
static void test_drone_shape() {
    section("drone safety shape");

    // 0 continue_mission
    // 1 return_to_base       (battery low)
    // 2 land_now             (geofence breach)
    // 3 hold_position        (obstacle below)
    // 4 gps_stale            (undermines the geofence claim)
    //
    // land_now beats return_to_base beats continue_mission; hold_position blocks land_now because you cannot
    // descend into an obstacle; gps_stale undermines land_now's premise.
    const std::vector<quorum::reference::Attack> attacks = {
        {1, 0},   // return_to_base defeats continue_mission
        {2, 0},   // land_now defeats continue_mission
        {2, 1},   // land_now overrides return_to_base
        {3, 2},   // obstacle below blocks landing
        {4, 2},   // stale GPS undermines the geofence breach
    };

    Framework<32, 64> fw;
    Grounded<32> g;
    check(build(fw, 5, attacks), "drone: builds");
    check(g.solve(fw) == Status::Ok, "drone: solves");

    // hold_position and gps_stale are unattacked, so both are In; they jointly put land_now Out; with land_now
    // out, return_to_base is unopposed and reinstated; continue_mission is then Out.
    check(g.label(Arg{3}) == Label::In, "drone: hold_position In");
    check(g.label(Arg{4}) == Label::In, "drone: gps_stale In");
    check(g.label(Arg{2}) == Label::Out, "drone: land_now Out");
    check(g.label(Arg{1}) == Label::In, "drone: return_to_base reinstated");
    check(g.label(Arg{0}) == Label::Out, "drone: continue_mission Out");

    // Cross-check the whole thing against the oracle.
    const std::vector<bool> expected = quorum::reference::grounded(5, attacks);
    bool agrees = true;
    for (std::int32_t i = 0; i < 5; ++i) {
        if (g.accepted(Arg{i}) != expected[static_cast<std::size_t>(i)]) { agrees = false; }
    }
    check(agrees, "drone: agrees with the independent oracle");
    check(quorum::reference::admissible(expected, 5, attacks), "drone: extension is admissible");
}

// ─────────────────────────────────────────────────────────────────────────────
// Random cross-check against the oracle, plus the definitional and WCET properties.
// ─────────────────────────────────────────────────────────────────────────────
static void test_random_against_oracle() {
    section("random frameworks vs independent oracle");

    constexpr std::int32_t kCap = 64;
    constexpr std::int32_t kAttackCap = kCap * kCap;

    std::mt19937 rng(20260921U);
    int disagreements = 0;
    int bound_violations = 0;
    int non_admissible = 0;
    int not_fixpoint = 0;
    int cases = 0;

    // A spread of sizes and densities. The competition engine notes density predicts difficulty better than
    // size; here it mostly controls how much reinstatement happens.
    const std::int32_t sizes[] = {1, 2, 3, 5, 8, 13, 21, 34, 64};
    const int density_permille[] = {0, 20, 60, 150, 300, 600, 900};

    for (std::int32_t n : sizes) {
        for (int dens : density_permille) {
            for (int trial = 0; trial < 40; ++trial) {
                std::vector<quorum::reference::Attack> attacks;
                std::uniform_int_distribution<int> roll(0, 999);
                for (std::int32_t u = 0; u < n; ++u) {
                    for (std::int32_t v = 0; v < n; ++v) {
                        if (roll(rng) < dens) { attacks.push_back({u, v}); }
                    }
                }

                Framework<kCap, kAttackCap> fw;
                Grounded<kCap> g;
                if (!build(fw, n, attacks)) { continue; }
                if (g.solve(fw) != Status::Ok) { continue; }
                ++cases;

                const std::vector<bool> oracle = quorum::reference::grounded(n, attacks);

                for (std::int32_t i = 0; i < n; ++i) {
                    if (g.accepted(Arg{i}) != oracle[static_cast<std::size_t>(i)]) { ++disagreements; }
                }

                // The WCET bound is the whole reason this semantics was chosen. Check it every time.
                const std::int32_t m = static_cast<std::int32_t>(attacks.size());
                if (g.steps() > Grounded<kCap>::step_bound(n, m)) { ++bound_violations; }

                // Definitional properties, independent of both implementations.
                if (!quorum::reference::admissible(oracle, n, attacks)) { ++non_admissible; }
                if (quorum::reference::characteristic(oracle, n, attacks) != oracle) { ++not_fixpoint; }
            }
        }
    }

    std::printf("  %d random frameworks checked\n", cases);
    check(cases > 2000, "random: enough cases actually ran");
    check(disagreements == 0, "random: solver agrees with oracle on every argument");
    check(bound_violations == 0, "random: steps() never exceeds 2n + 2m");
    check(non_admissible == 0, "random: grounded extension is always admissible");
    check(not_fixpoint == 0, "random: grounded extension is a fixpoint of the characteristic function");
}

// ─────────────────────────────────────────────────────────────────────────────
// CSR construction. finalize() restores the offset array after using it as a scatter cursor, which is the one
// piece of cleverness in the framework and therefore the piece most worth pinning.
// ─────────────────────────────────────────────────────────────────────────────
static void test_csr_construction() {
    section("CSR construction");

    std::mt19937 rng(7U);
    int row_sum_wrong = 0;
    int missing_edge = 0;
    int extra_edge = 0;

    for (int trial = 0; trial < 200; ++trial) {
        const std::int32_t n = 1 + static_cast<std::int32_t>(rng() % 24);
        std::vector<quorum::reference::Attack> attacks;
        for (std::int32_t u = 0; u < n; ++u) {
            for (std::int32_t v = 0; v < n; ++v) {
                if (rng() % 100 < 25) { attacks.push_back({u, v}); }
            }
        }

        Framework<32, 1024> fw;
        if (!build(fw, n, attacks)) { continue; }

        // Row sizes must total m, and row 0 must start at 0.
        std::int32_t total = 0;
        for (std::int32_t u = 0; u < n; ++u) { total += fw.out_end(Arg{u}) - fw.out_begin(Arg{u}); }
        if (total != static_cast<std::int32_t>(attacks.size())) { ++row_sum_wrong; }
        if (fw.out_begin(Arg{0}) != 0) { ++row_sum_wrong; }

        // Every declared attack appears in the right row.
        for (const auto& at : attacks) {
            bool found = false;
            for (std::int32_t k = fw.out_begin(Arg{at.from}); k < fw.out_end(Arg{at.from}); ++k) {
                if (fw.target(k).get() == at.to) { found = true; break; }
            }
            if (!found) { ++missing_edge; }
        }

        // Nothing appears that was not declared.
        for (std::int32_t u = 0; u < n; ++u) {
            for (std::int32_t k = fw.out_begin(Arg{u}); k < fw.out_end(Arg{u}); ++k) {
                const std::int32_t v = fw.target(k).get();
                bool declared = false;
                for (const auto& at : attacks) {
                    if (at.from == u && at.to == v) { declared = true; break; }
                }
                if (!declared) { ++extra_edge; }
            }
        }

        // In-degree must match the attack list.
        for (std::int32_t v = 0; v < n; ++v) {
            std::int32_t expect = 0;
            for (const auto& at : attacks) { if (at.to == v) { ++expect; } }
            if (fw.in_degree(Arg{v}) != expect) { ++row_sum_wrong; }
        }
    }

    check(row_sum_wrong == 0, "csr: row sizes, base offset and in-degrees all correct");
    check(missing_edge == 0, "csr: every declared attack is present");
    check(extra_edge == 0, "csr: no attack appears that was not declared");
}

// ─────────────────────────────────────────────────────────────────────────────
// Refusal. There are no exceptions on this target, so every failure must be a returned status, and a test that
// cannot fail for the reason it names is not a test.
// ─────────────────────────────────────────────────────────────────────────────
static void test_status_paths() {
    section("status paths (no exceptions)");

    // Too many arguments for the compile-time bound.
    {
        Framework<4, 16> fw;
        check(fw.declare(5) == Status::CapacityExceeded, "status: declare beyond MaxArgs refuses");
        check(fw.declare(4) == Status::Ok, "status: declare at MaxArgs is fine");
    }

    // Negative argument count.
    {
        Framework<4, 16> fw;
        check(fw.declare(-1) == Status::BadArgument, "status: negative argument count refuses");
    }

    // Too many attacks.
    {
        Framework<4, 2> fw;
        check(fw.declare(4) == Status::Ok, "status: declare ok");
        check(fw.add_attack(Arg{0}, Arg{1}) == Status::Ok, "status: first attack ok");
        check(fw.add_attack(Arg{1}, Arg{2}) == Status::Ok, "status: second attack ok");
        check(fw.add_attack(Arg{2}, Arg{3}) == Status::CapacityExceeded, "status: attack beyond MaxAttacks refuses");
    }

    // Out-of-range arguments in an attack.
    {
        Framework<8, 8> fw;
        check(fw.declare(3) == Status::Ok, "status: declare 3");
        check(fw.add_attack(Arg{0}, Arg{3}) == Status::BadArgument, "status: attack target >= n refuses");
        check(fw.add_attack(Arg{-1}, Arg{0}) == Status::BadArgument, "status: negative attacker refuses");
    }

    // Solving before finalize must refuse rather than read an unbuilt CSR.
    {
        Framework<8, 8> fw;
        Grounded<8> g;
        check(fw.declare(2) == Status::Ok, "status: declare 2");
        check(fw.add_attack(Arg{0}, Arg{1}) == Status::Ok, "status: add attack");
        check(g.solve(fw) == Status::NotFinalized, "status: solve before finalize refuses");
        check(fw.finalize() == Status::Ok, "status: finalize ok");
        check(g.solve(fw) == Status::Ok, "status: solve after finalize ok");
    }

    // Mutation after finalize must refuse rather than silently corrupt the CSR.
    {
        Framework<8, 8> fw;
        check(build(fw, 2, {{0, 1}}), "status: build");
        check(fw.add_attack(Arg{1}, Arg{0}) == Status::AlreadyFinalized, "status: add_attack after finalize refuses");
        check(fw.declare(2) == Status::AlreadyFinalized, "status: declare after finalize refuses");
        check(fw.finalize() == Status::AlreadyFinalized, "status: double finalize refuses");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The numbering boundary. This is the bug class ids.hpp exists to make unrepresentable; these tests cover the
// runtime half that the type system cannot.
// ─────────────────────────────────────────────────────────────────────────────
static void test_numbering() {
    section("numbering boundary");

    check(quorum::solver::to_internal(FileArg{1}) == Arg{0}, "numbering: file 1 is internal 0");
    check(quorum::solver::to_file(Arg{0}) == FileArg{1}, "numbering: internal 0 is file 1");

    for (std::int32_t i = 1; i <= 100; ++i) {
        if (quorum::solver::to_file(quorum::solver::to_internal(FileArg{i})) != FileArg{i}) {
            check(false, "numbering: round trip is identity");
            break;
        }
    }
    check(true, "numbering: round trip is identity");

    Arg out{-99};
    check(quorum::solver::to_internal_checked(FileArg{0}, 5, out) == Status::BadArgument,
          "numbering: file argument 0 is rejected, not mapped to -1");
    check(quorum::solver::to_internal_checked(FileArg{6}, 5, out) == Status::BadArgument,
          "numbering: file argument n+1 is rejected");
    check(quorum::solver::to_internal_checked(FileArg{5}, 5, out) == Status::Ok,
          "numbering: file argument n is accepted");
    check(out == Arg{4}, "numbering: file argument n converts to internal n-1");
}

// ─────────────────────────────────────────────────────────────────────────────
// No heap, no hidden state: the objects must be plain enough to live in .bss.
// ─────────────────────────────────────────────────────────────────────────────
static void test_target_properties() {
    section("target properties");

    check(sizeof(Arg) == sizeof(std::int32_t), "target: Arg is a zero-cost wrapper");
    check(sizeof(FileArg) == sizeof(std::int32_t), "target: FileArg is a zero-cost wrapper");

    check(std::is_trivially_copyable<Framework<16, 32>>::value, "target: Framework is trivially copyable");
    check(std::is_trivially_copyable<Grounded<16>>::value, "target: Grounded is trivially copyable");
    check(std::is_trivially_destructible<Framework<16, 32>>::value, "target: Framework has no destructor");
    check(std::is_trivially_destructible<Grounded<16>>::value, "target: Grounded has no destructor");

    // Report the footprint of a plausible device configuration, so a change that blows the budget is visible.
    std::printf("  Framework<256,2048> = %zu bytes, Grounded<256> = %zu bytes, total %zu bytes\n",
                sizeof(Framework<256, 2048>),
                sizeof(Grounded<256>),
                sizeof(Framework<256, 2048>) + sizeof(Grounded<256>));
    check(sizeof(Framework<256, 2048>) + sizeof(Grounded<256>) < 64U * 1024U,
          "target: a 256-argument / 2048-attack arbiter fits in 64 KB");
}

int main() {
    std::printf("quorum/solver -- grounded core\n\n");

    test_hand_cases();
    test_drone_shape();
    test_random_against_oracle();
    test_csr_construction();
    test_status_paths();
    test_numbering();
    test_target_properties();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
