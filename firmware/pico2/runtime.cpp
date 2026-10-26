// quorum_runtime -- ONE prebuilt RP2350 firmware that runs ANY validated casebook, loaded from flash.
//
// The casebook is not compiled in. `cbc --blob` writes it as a data image; the desktop tool places that image at
// kPico2BlobOffset in the same UF2 as this firmware. At boot the loader checks transport (CRC), structure (every index
// and capacity) and identity (fingerprint recomputed from the loaded tables). Anything but Ok and the device does not
// decide: it reports why, every few seconds, and nothing else. A device with no believable rulebook must not act on
// one.
//
// The image may be one casebook (format 1) or a palette of several (format 2). Either way the firmware then decides
// every state (per member, others quiet, for a palette), fingerprints the decisions, checks every step count against
// the proven bound, and times every state cold. Each DEVICE line has the format `qblob sweep` prints, so equal lines
// mean the board decided every state as the host did. A GUARD line does the same for the AI guard: every state and
// every possible proposal, and `weaker` must be 0.
//
// MODBUS. The first valid request received on the USB serial port switches the board into Modbus RTU mode, slave address 1
// (register map in runtime/include/quorum/runtime/modbus.hpp): the text reports stop, so they cannot corrupt a
// master's framing, and from then on the board only answers requests. The first request may arrive mid-report and be
// lost; masters retry. A casebook's channels are driven through the binding, so holds and staleness apply.

#include <cstdint>
#include <cstdio>

#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "pico/stdlib.h"

#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/guard.hpp"
#include "quorum/runtime/layout.hpp"
#include "quorum/runtime/modbus.hpp"
#include "quorum/runtime/modbus_map.hpp"
#include "quorum/runtime/sweep.hpp"

namespace bench = quorum::bench;
namespace rt = quorum::runtime;

using quorum::solver::Status;

namespace {

rt::Casebook g_cb;
rt::Palette g_pal;
rt::RtFramework g_fw;
rt::RtSolver g_solver;
rt::RtPolicy g_policy[rt::kMaxMembers];
rt::SweepResult g_result[rt::kMaxMembers];
rt::GuardSweep g_guard[rt::kMaxMembers];
rt::RtBinding g_binding;
rt::RtTimers g_timers;
rt::modbus::QuorumMap g_map;
rt::modbus::Slave g_slave(1);

void print_guard(const char* name, const rt::GuardSweep& g) {
    std::printf("GUARD %s pairs %lu accepted %lu replaced %lu weaker %lu fnv1a 0x%08lx\n", name,
                static_cast<unsigned long>(g.pairs), static_cast<unsigned long>(g.accepted),
                static_cast<unsigned long>(g.replaced), static_cast<unsigned long>(g.weaker),
                static_cast<unsigned long>(g.fnv));
}

/// Modbus mode, entered on the first VALID request (right CRC, addressed to us or broadcast) and never left. Until then
/// received bytes are framed and checked but otherwise ignored, so a stray byte -- Windows has sent at least one to a
/// freshly enumerated port, 2026-10-10 -- cannot silence the text reports. Returns only if no valid request has arrived.
void serve_modbus_if_asked() {
    bool modbus = false;
    for (;;) {
        const int c = getchar_timeout_us(modbus ? 1000 : 0);
        if (c == PICO_ERROR_TIMEOUT) {
            if (!modbus) { return; }
            // Timed readings must see their sources between requests too: about once a millisecond.
            g_map.tick(to_ms_since_boot(get_absolute_time()));
            continue;
        }
        const std::uint32_t before = g_slave.frames_handled();
        const std::size_t n = g_slave.feed(static_cast<std::uint8_t>(c), to_ms_since_boot(get_absolute_time()), g_map);
        if (g_slave.frames_handled() != before) { modbus = true; }
        for (std::size_t i = 0; i < n; ++i) { putchar_raw(g_slave.response()[i]); }
        if (n > 0) { stdio_flush(); }
    }
}

void print_result(const char* name, const rt::SweepResult& r, std::int32_t bound, std::uint32_t clock_hz,
                  std::int32_t observations) {
    const std::uint64_t mhz = clock_hz / 1000000U;
    const std::uint64_t nanos = (static_cast<std::uint64_t>(r.worst_cycles) * 1000U) / (mhz == 0U ? 1U : mhz);
    std::printf("not Decided     %lu   construction faults %lu\n", static_cast<unsigned long>(r.not_decided),
                static_cast<unsigned long>(r.faults));
    std::printf("steps           max %d, proven bound %d, %s\n", static_cast<int>(r.max_steps), static_cast<int>(bound),
                r.max_steps <= bound ? "WITHIN" : "OVER");
    std::printf("worst cycle     %lu cycles = %lu.%03lu us at %lu Hz (state 0x%lX, cold, instantiate+decide)\n",
                static_cast<unsigned long>(r.worst_cycles), static_cast<unsigned long>(nanos / 1000U),
                static_cast<unsigned long>(nanos % 1000U), static_cast<unsigned long>(clock_hz),
                static_cast<unsigned long>(r.worst_mask));
    if (r.full) {
        std::printf("DEVICE %s states %lu asserts %lu fnv1a 0x%08lx\n", name, static_cast<unsigned long>(r.states),
                    static_cast<unsigned long>(r.asserts), static_cast<unsigned long>(r.fnv));
    } else {
        std::printf("DEVICE %s worst-case state only (%d observations)\n", name, static_cast<int>(observations));
    }
}

}  // namespace

int main() {
    stdio_init_all();
    sleep_ms(2000);
    const std::uint32_t clock_hz = clock_get_hz(clk_sys);

    const auto* blob = reinterpret_cast<const std::uint8_t*>(XIP_BASE + rt::kPico2BlobOffset);
    const bool palette = rt::blob_format(blob, rt::kBlobRegionBytes) == 2U;
    const rt::BlobStatus loaded = palette ? rt::load_palette(blob, rt::kBlobRegionBytes, g_pal)
                                          : rt::load(blob, rt::kBlobRegionBytes, g_cb);
    const std::int32_t members = loaded != rt::BlobStatus::Ok ? 0 : (palette ? g_pal.member_count : 1);
    bool policy_ok = loaded == rt::BlobStatus::Ok;
    for (std::int32_t m = 0; m < members; ++m) {
        policy_ok = policy_ok && (palette ? rt::make_policy(g_policy[m], g_pal, m)
                                          : rt::make_policy(g_policy[m], g_cb)) == Status::Ok;
    }
    const bool counter_ok = bench::enable();
    if (policy_ok) {
        for (std::int32_t m = 0; m < members; ++m) {
            g_result[m] = palette ? rt::sweep(g_pal, m, g_fw, g_solver, g_policy[m])
                                  : rt::sweep(g_cb, g_fw, g_solver, g_policy[m]);
            g_guard[m] = palette ? rt::guard_sweep(g_pal, m, g_fw, g_solver, g_policy[m])
                                 : rt::guard_sweep(g_cb, g_fw, g_solver, g_policy[m]);
        }
        if (palette) {
            g_map.attach(g_pal, g_fw, g_solver, g_policy);
        } else {
            const bool bound = g_cb.channel_count > 0 && rt::make_binding(g_binding, g_cb) == quorum::binding::Status::Ok;
            const bool timed = g_cb.timer_count > 0 && rt::make_timers(g_timers, g_cb) == quorum::binding::Status::Ok;
            g_map.attach(g_cb, bound ? &g_binding : nullptr, g_fw, g_solver, g_policy[0], timed ? &g_timers : nullptr);
        }
    }

    for (unsigned report = 0;; ++report) {
        std::printf("\n=== quorum runtime on RP2350 (report %u) ===\n", report);
        if (loaded != rt::BlobStatus::Ok) {
            std::printf("NO CASEBOOK     %s -- this device will not decide\n", rt::blob_status_name(loaded));
            sleep_ms(3000);
            continue;
        }
        std::printf("policy          %s\n", policy_ok ? "OK" : "FAILED");
        std::printf("cycle counter   %s\n", counter_ok ? bench::source_name() : "NOT RUNNING");
        if (palette) {
            std::printf("palette         %d casebooks, %d arguments, %d attacks, %d observations\n",
                        static_cast<int>(g_pal.member_count), static_cast<int>(g_pal.argument_count),
                        static_cast<int>(g_pal.edge_count), static_cast<int>(g_pal.observation_count));
            std::printf("image           OK, fingerprint 0x%08lx\n", static_cast<unsigned long>(g_pal.fingerprint));
            for (std::int32_t m = 0; m < members && policy_ok; ++m) {
                std::printf("-- member       %s (others quiet)\n", g_pal.members[m].name);
                print_result(g_pal.members[m].name, g_result[m], g_pal.step_bound, clock_hz,
                             g_pal.members[m].observation_count);
                print_guard(g_pal.members[m].name, g_guard[m]);
            }
        } else {
            std::printf("casebook        %s v%d\n", g_cb.name, static_cast<int>(g_cb.version));
            std::printf("source          %s\n", g_cb.source_document);
            std::printf("written by      %s\n", g_cb.source_author);
            std::printf("arguments       %d, attacks %d, observations %d\n", static_cast<int>(g_cb.argument_count),
                        static_cast<int>(g_cb.edge_count), static_cast<int>(g_cb.observation_count));
            std::printf("image           OK, fingerprint 0x%08lx\n", static_cast<unsigned long>(g_cb.fingerprint));
            if (policy_ok) {
                print_result(g_cb.name, g_result[0], g_cb.step_bound, clock_hz, g_cb.observation_count);
                print_guard(g_cb.name, g_guard[0]);
            }
        }
        std::printf("modbus          send any Modbus RTU request (slave 1) to switch to Modbus mode\n");
        for (int t = 0; t < 300; ++t) {
            if (policy_ok) { serve_modbus_if_asked(); }
            sleep_ms(10);
        }
    }
}
