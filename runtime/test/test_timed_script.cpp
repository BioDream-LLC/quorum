// The bench script cbc writes for a timed rulebook, played against the device's own Modbus map on its test clock.
//
// cbc works out every expected answer from the definitions in LANGUAGE.md over the whole history. The map runs the
// board's timer code. This test plays one against the other, through the same registers the studio's bench test
// uses, so a disagreement between the script and the device is found in CI and not first on a bench.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/layout.hpp"
#include "quorum/runtime/modbus_map.hpp"

namespace rt = quorum::runtime;
namespace mb = quorum::runtime::modbus;

// With --late N, every write and every read happens up to N ms after the script says, as on a bench that waits in
// real time and talks over a serial line. The script's margins must absorb that.
int main(int argc, char** argv) {
    if (argc != 3 && !(argc == 5 && std::string(argv[3]) == "--late")) {
        std::printf("usage: test_timed_script <casebook.qcb> <sheet> [--late <ms>]\n");
        return 2;
    }
    const std::uint64_t late = argc == 5 ? std::stoull(argv[4]) : 0U;
    std::uint64_t seed = 20261026U;
    const auto lateness = [&]() -> std::uint64_t {
        if (late == 0U) { return 0U; }
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed >> 33U) % (late + 1U);
    };
    std::ifstream bin(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
    static rt::Casebook cb;
    static rt::RtFramework fw;
    static rt::RtSolver solver;
    static rt::RtPolicy policy;
    static rt::RtTimers timers;
    if (rt::load(blob.data(), blob.size(), cb) != rt::BlobStatus::Ok || rt::make_policy(policy, cb) != quorum::solver::Status::Ok ||
        rt::make_timers(timers, cb) != quorum::binding::Status::Ok) {
        std::printf("FAIL  the image does not load\n");
        return 1;
    }
    static mb::QuorumMap map;
    map.attach(cb, nullptr, fw, solver, policy, &timers);

    // Real time stands still at 0; only the test clock moves, as on a bench.
    const auto write = [&](std::uint16_t a, std::vector<std::uint16_t> v) {
        return map.write(a, static_cast<std::uint16_t>(v.size()), v.data(), 0) == mb::Exception::None;
    };
    std::uint64_t t = 0;      // the board's clock, as far as the test clock has moved it
    std::uint64_t ideal = 0;  // the script's own clock: where the bench means to be, late or not
    const auto move_to = [&](std::uint64_t at) {
        ideal = std::max(ideal, at);
        const std::uint64_t target = at + lateness();
        while (t < target) {
            const std::uint64_t d = std::min<std::uint64_t>(target - t, mb::kMaxClockJumpMs);
            if (!write(11, {static_cast<std::uint16_t>(d >> 16U), static_cast<std::uint16_t>(d)})) { return false; }
            t += d;
        }
        return true;
    };
    if (!write(10, {mb::kTestClockKey}) || !write(16, {mb::kNone})) {
        std::printf("FAIL  the test clock does not turn on\n");
        return 1;
    }
    std::ifstream sheet(argv[2]);
    std::string line;
    std::uint64_t base = 0;
    int checks = 0;
    int failures = 0;
    int scenes = 0;
    while (std::getline(sheet, line)) {
        std::istringstream f(line);
        std::string op;
        std::getline(f, op, '\t');
        if (op == "scene") {
            std::uint64_t quiet = 0;
            f >> quiet;
            ++scenes;
            if (!write(8, {0, 0}) || !move_to(ideal + quiet)) { ++failures; }
            base = ideal;
        } else if (op == "set") {
            std::uint64_t at = 0;
            std::string hex;
            f >> at >> hex;
            const auto m = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
            if (!move_to(base + at) ||
                !write(8, {static_cast<std::uint16_t>(m >> 16U), static_cast<std::uint16_t>(m)})) {
                ++failures;
            }
        } else if (op == "expect") {
            std::string at, want, label;
            std::getline(f, at, '\t');
            std::getline(f, want, '\t');
            std::getline(f, label);
            std::uint16_t out[9]{};
            if (!move_to(base + std::stoull(at)) || map.read(16, 9, out, 0) != mb::Exception::None) {
                ++failures;
                continue;
            }
            const std::string got = out[2] < cb.action_count ? rt::name_of(cb, cb.actions[out[2]]) : "?";
            ++checks;
            if (got != want) {
                ++failures;
                std::printf("  FAIL  %s: the script says %s, the device says %s\n", label.c_str(), want.c_str(),
                            got.c_str());
            }
        }
    }
    std::printf("%s: %d scenes, %d checks against the device's Modbus map on its test clock%s, %d failures\n",
                argv[2], scenes, checks, late != 0U ? (", each step up to " + std::to_string(late) + " ms late").c_str() : "",
                failures);
    return checks > 0 && failures == 0 ? 0 : 1;
}
