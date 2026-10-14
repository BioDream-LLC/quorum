// quorum_runtime for the Uno R4 Minima -- ONE prebuilt sketch that runs ANY validated casebook, loaded from flash.
//
// The counterpart of firmware/pico2/runtime.cpp. The casebook is not compiled in: quorum studio appends the output of
// `cbc --blob` to this sketch's .bin so that it lands at kUnoR4BlobAddress, and the DFU upload writes both. At boot
// the loader checks transport, structure and identity; on any failure the board reports why and does not decide.
// The image may be one casebook (format 1) or a palette (format 2). Then, as on the Pico, it decides every state
// (per member, others quiet, for a palette) with the shared sweep and prints DEVICE lines `qblob sweep` also prints.

#include <Arduino.h>
#include <new>

#include <quorum/runtime/blob.hpp>
#include <quorum/runtime/guard.hpp>
#include <quorum/runtime/layout.hpp>
#include <quorum/runtime/modbus.hpp>
#include <quorum/runtime/modbus_map.hpp>
#include <quorum/runtime/sweep.hpp>

#include "tables.h"

namespace rt = quorum::runtime;
namespace bench = quorum::bench;

using quorum::solver::Status;

static Tables g_tables;   // see tables.h
static rt::Casebook& g_cb = g_tables.cb;
static rt::Palette& g_pal = g_tables.pal;
static rt::RtFramework g_fw;
static rt::RtSolver g_solver;
static rt::RtPolicy g_policy[rt::kMaxMembers];
static rt::SweepResult g_result[rt::kMaxMembers];
static rt::GuardSweep g_guard[rt::kMaxMembers];
static rt::RtBinding g_binding;
static rt::modbus::QuorumMap g_map;
static rt::modbus::Slave g_slave(1);
static bool g_modbus = false;  // entered on the first valid request, never left; see the Pico runtime for why

static rt::BlobStatus g_loaded = rt::BlobStatus::BadMagic;
static bool g_palette = false;
static int32_t g_members = 0;
static bool g_policy_ok = false;
static bool g_counter_ok = false;
static uint32_t g_report = 0;

static void print_hex8(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        Serial.print("0123456789abcdef"[(v >> static_cast<uint32_t>(shift)) & 0xFUL]);  // lower case, as the host
    }
}

static void print_result(const char* name, const rt::SweepResult& r, int32_t bound, int32_t observations) {
    const uint32_t hz = SystemCoreClock;
    const uint32_t mhz = hz / 1000000UL == 0UL ? 1UL : hz / 1000000UL;
    const uint32_t nanos = static_cast<uint32_t>((static_cast<uint64_t>(r.worst_cycles) * 1000ULL) / mhz);
    Serial.print(F("not Decided     ")); Serial.print(r.not_decided);
    Serial.print(F("   construction faults ")); Serial.println(r.faults);
    Serial.print(F("steps           max ")); Serial.print(r.max_steps);
    Serial.print(F(", proven bound ")); Serial.print(bound);
    Serial.println(r.max_steps <= bound ? F(", WITHIN") : F(", OVER"));
    Serial.print(F("worst cycle     ")); Serial.print(r.worst_cycles);
    Serial.print(F(" cycles = ")); Serial.print(nanos / 1000UL); Serial.print('.');
    const uint32_t frac = nanos % 1000UL;
    if (frac < 100UL) { Serial.print('0'); }
    if (frac < 10UL) { Serial.print('0'); }
    Serial.print(frac);
    Serial.print(F(" us (state 0x")); Serial.print(r.worst_mask, HEX); Serial.println(F(", cold, instantiate+decide)"));
    Serial.print(F("DEVICE ")); Serial.print(name);
    if (r.full) {
        Serial.print(F(" states ")); Serial.print(r.states);
        Serial.print(F(" asserts ")); Serial.print(r.asserts);
        Serial.print(F(" fnv1a 0x")); print_hex8(r.fnv);
        Serial.println();
    } else {
        Serial.print(F(" worst-case state only (")); Serial.print(observations); Serial.println(F(" observations)"));
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    const auto* blob = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rt::kUnoR4BlobAddress));
    g_palette = rt::blob_format(blob, rt::kBlobRegionBytes) == 2U;
    if (g_palette) { new (&g_tables.pal) rt::Palette(); }   // switch the active member before using it
    g_loaded = g_palette ? rt::load_palette(blob, rt::kBlobRegionBytes, g_pal) : rt::load(blob, rt::kBlobRegionBytes, g_cb);
    g_members = g_loaded != rt::BlobStatus::Ok ? 0 : (g_palette ? g_pal.member_count : 1);
    g_policy_ok = g_loaded == rt::BlobStatus::Ok;
    for (int32_t m = 0; m < g_members; ++m) {
        g_policy_ok = g_policy_ok && (g_palette ? rt::make_policy(g_policy[m], g_pal, m)
                                                : rt::make_policy(g_policy[m], g_cb)) == Status::Ok;
    }
    g_counter_ok = bench::enable();
    if (!g_policy_ok) { return; }
    for (int32_t m = 0; m < g_members; ++m) {
        g_result[m] = g_palette ? rt::sweep(g_pal, m, g_fw, g_solver, g_policy[m]) : rt::sweep(g_cb, g_fw, g_solver, g_policy[m]);
        g_guard[m] = g_palette ? rt::guard_sweep(g_pal, m, g_fw, g_solver, g_policy[m])
                               : rt::guard_sweep(g_cb, g_fw, g_solver, g_policy[m]);
    }
    if (g_palette) {
        g_map.attach(g_pal, g_fw, g_solver, g_policy);
    } else {
        const bool bound = g_cb.channel_count > 0 && rt::make_binding(g_binding, g_cb) == quorum::binding::Status::Ok;
        g_map.attach(g_cb, bound ? &g_binding : nullptr, g_fw, g_solver, g_policy[0]);
    }
}

static void print_guard(const char* name, const rt::GuardSweep& g) {
    Serial.print(F("GUARD ")); Serial.print(name);
    Serial.print(F(" pairs ")); Serial.print(g.pairs);
    Serial.print(F(" accepted ")); Serial.print(g.accepted);
    Serial.print(F(" replaced ")); Serial.print(g.replaced);
    Serial.print(F(" weaker ")); Serial.print(g.weaker);
    Serial.print(F(" fnv1a 0x")); print_hex8(g.fnv);
    Serial.println();
}

/// Serve Modbus once a request has started arriving. Returns true while in Modbus mode.
static bool serve_modbus() {
    if (!g_policy_ok) { return false; }
    // Bytes are framed and checked from the start, but only a valid request (right CRC, our address or broadcast)
    // switches modes, so a stray byte from the host cannot silence the text reports.
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) { break; }
        const uint32_t before = g_slave.frames_handled();
        const size_t n = g_slave.feed(static_cast<uint8_t>(c), millis(), g_map);
        if (g_slave.frames_handled() != before) { g_modbus = true; }
        if (n > 0) { Serial.write(g_slave.response(), n); }
    }
    return g_modbus;
}

void loop() {
    if (serve_modbus()) { return; }
    Serial.print(F("\n=== quorum runtime on Arduino (report "));
    Serial.print(g_report++);
    Serial.println(F(") ==="));
    if (g_loaded != rt::BlobStatus::Ok) {
        Serial.print(F("NO CASEBOOK     "));
        Serial.print(rt::blob_status_name(g_loaded));
        Serial.println(F(" -- this device will not decide"));
        delay(3000);
        return;
    }
    Serial.print(F("core clock      ")); Serial.println(SystemCoreClock);
    Serial.print(F("policy          ")); Serial.println(g_policy_ok ? F("OK") : F("FAILED"));
    Serial.print(F("cycle counter   ")); Serial.println(g_counter_ok ? bench::source_name() : "NOT RUNNING");
    if (g_palette) {
        Serial.print(F("palette         ")); Serial.print(g_pal.member_count); Serial.print(F(" casebooks, "));
        Serial.print(g_pal.argument_count); Serial.print(F(" arguments, ")); Serial.print(g_pal.edge_count);
        Serial.print(F(" attacks, ")); Serial.print(g_pal.observation_count); Serial.println(F(" observations"));
        Serial.print(F("image           OK, fingerprint 0x")); print_hex8(g_pal.fingerprint); Serial.println();
        for (int32_t m = 0; m < g_members && g_policy_ok; ++m) {
            Serial.print(F("-- member       ")); Serial.print(g_pal.members[m].name); Serial.println(F(" (others quiet)"));
            print_result(g_pal.members[m].name, g_result[m], g_pal.step_bound, g_pal.members[m].observation_count);
            print_guard(g_pal.members[m].name, g_guard[m]);
        }
    } else {
        Serial.print(F("casebook        ")); Serial.print(g_cb.name); Serial.print(F(" v")); Serial.println(g_cb.version);
        Serial.print(F("source          ")); Serial.println(g_cb.source_document);
        Serial.print(F("arguments       ")); Serial.print(g_cb.argument_count);
        Serial.print(F(", attacks ")); Serial.print(g_cb.edge_count);
        Serial.print(F(", observations ")); Serial.println(g_cb.observation_count);
        Serial.print(F("image           OK, fingerprint 0x")); print_hex8(g_cb.fingerprint); Serial.println();
        if (g_policy_ok) {
            print_result(g_cb.name, g_result[0], g_cb.step_bound, g_cb.observation_count);
            print_guard(g_cb.name, g_guard[0]);
        }
    }
    Serial.println(F("modbus          send any Modbus RTU request (slave 1) to switch to Modbus mode"));
    for (int t = 0; t < 300; ++t) {
        if (serve_modbus()) { return; }
        delay(10);
    }
}
