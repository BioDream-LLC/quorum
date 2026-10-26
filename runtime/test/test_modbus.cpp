// Modbus RTU: framing and exceptions, then the register map driven exactly as a PLC would drive it.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "quorum/runtime/blob.hpp"
#include "quorum/runtime/guard.hpp"
#include "quorum/runtime/layout.hpp"
#include "quorum/runtime/modbus.hpp"
#include "quorum/runtime/modbus_map.hpp"

namespace rt = quorum::runtime;
namespace mb = quorum::runtime::modbus;
using Bytes = std::vector<std::uint8_t>;

namespace {
int g_checks = 0;
int g_failures = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

Bytes with_crc(Bytes b) {
    const std::uint16_t c = mb::crc16(b.data(), b.size());
    b.push_back(static_cast<std::uint8_t>(c & 0xFFU));
    b.push_back(static_cast<std::uint8_t>(c >> 8U));
    return b;
}

/// Send a request byte by byte at `now`; return the response (empty if none).
Bytes transact(mb::Slave& s, mb::Registers& r, const Bytes& req, std::uint32_t now) {
    std::size_t n = 0;
    for (const std::uint8_t b : req) { n = s.feed(b, now, r); }
    return Bytes(s.response(), s.response() + n);
}

Bytes read_req(std::uint16_t addr, std::uint16_t count, std::uint8_t slave = 1) {
    return with_crc({slave, 3, static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr),
                     static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(count)});
}
Bytes write1(std::uint16_t addr, std::uint16_t v) {
    return with_crc({1, 6, static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr),
                     static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
}
Bytes write_n(std::uint16_t addr, const std::vector<std::uint16_t>& v) {
    Bytes b{1, 16, static_cast<std::uint8_t>(addr >> 8), static_cast<std::uint8_t>(addr), 0,
            static_cast<std::uint8_t>(v.size()), static_cast<std::uint8_t>(v.size() * 2)};
    for (const std::uint16_t x : v) {
        b.push_back(static_cast<std::uint8_t>(x >> 8));
        b.push_back(static_cast<std::uint8_t>(x));
    }
    return with_crc(b);
}
std::vector<std::uint16_t> regs_of(const Bytes& resp) {
    std::vector<std::uint16_t> v;
    for (std::size_t i = 3; i + 1 < resp.size() - 2; i += 2) {
        v.push_back(static_cast<std::uint16_t>((resp[i] << 8) | resp[i + 1]));
    }
    return v;
}
bool load(const char* path, rt::Casebook& cb) {
    std::ifstream in(path, std::ios::binary);
    const Bytes blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return rt::load(blob.data(), blob.size(), cb) == rt::BlobStatus::Ok;
}
}  // namespace

int main(int argc, char** argv) {
    std::printf("Modbus RTU framing\n");
    {
        // The specification's own example: read 10 registers from slave 1 at 0 is 01 03 00 00 00 0A C5 CD.
        const std::uint8_t ex[] = {1, 3, 0, 0, 0, 10};
        const std::uint16_t c = mb::crc16(ex, 6);
        check((c & 0xFFU) == 0xC5U && (c >> 8U) == 0xCDU, "CRC-16/MODBUS matches the published example");
    }
    if (argc < 3) {
        std::printf("usage: test_modbus <pump.qcb> <held.qcb> <timed.qcb>\n");
        return 2;
    }

    static rt::Casebook cb;
    static rt::RtFramework fw;
    static rt::RtSolver solver;
    static rt::RtPolicy policy;
    check(load(argv[1], cb), "the pump image loads");
    check(rt::make_policy(policy, cb) == quorum::solver::Status::Ok, "policy");
    static mb::QuorumMap map;
    map.attach(cb, nullptr, fw, solver, policy);
    mb::Slave slave(1);

    Bytes r = transact(slave, map, read_req(0, 10), 0);
    std::vector<std::uint16_t> v = regs_of(r);
    check(r.size() == 5 + 20 && v.size() == 10, "read 10 identity registers");
    check(v.size() == 10 && v[0] == 0x5155U && v[1] == 2U, "id and map version");
    check(v.size() == 10 && ((static_cast<std::uint32_t>(v[2]) << 16U) | v[3]) == cb.fingerprint,
          "the fingerprint register is the image's fingerprint");
    check(v.size() == 10 && v[4] == cb.observation_count && v[5] == 1U, "observation and member counts");

    // Every state, as a PLC would: write the mask, read the decision, compare with deciding directly.
    std::printf("every pump state through Modbus agrees with deciding directly\n");
    int disagree = 0;
    const std::uint32_t states = 1U << static_cast<std::uint32_t>(cb.observation_count);
    for (std::uint32_t m = 0; m < states; ++m) {
        (void)transact(slave, map, write_n(8, {static_cast<std::uint16_t>(m >> 16), static_cast<std::uint16_t>(m)}), m);
        const std::vector<std::uint16_t> out = regs_of(transact(slave, map, read_req(16, 9), m));
        (void)rt::instantiate(fw, cb, m);
        const auto d = quorum::arbiter::decide(fw, solver, policy);
        std::uint16_t pos = 0xFFFF;
        for (std::int32_t k = 0; k < cb.action_count; ++k) {
            if (cb.actions[k] == d.action.get()) { pos = static_cast<std::uint16_t>(k); }
        }
        if (out.size() != 9 || out[1] != static_cast<std::uint16_t>(d.outcome) || out[2] != pos ||
            out[3] != (d.is_fallback ? 1U : 0U) || out[4] != pos || out[8] != cb.action_count) {
            ++disagree;
        }
    }
    check(disagree == 0, std::to_string(disagree) + " of " + std::to_string(states) + " states disagree");
    {
        // The line a Modbus master computes against a board (tools/modbus/qmodbus.ps1): FNV-1a over each state's
        // outcome register and action-position register, in state order.
        std::uint32_t fnv = 2166136261U;
        for (std::uint32_t m = 0; m < states; ++m) {
            (void)transact(slave, map, write_n(8, {static_cast<std::uint16_t>(m >> 16), static_cast<std::uint16_t>(m)}), m);
            const std::vector<std::uint16_t> out = regs_of(transact(slave, map, read_req(17, 2), m));
            fnv = (fnv ^ out[0]) * 16777619U;
            fnv = (fnv ^ out[1]) * 16777619U;
        }
        std::printf("MODBUS %s states %u fnv1a 0x%08x\n", cb.name, states, fnv);
    }

    std::printf("the guard through Modbus\n");
    // Quiet state: the pump continues. Propose each action in turn.
    (void)transact(slave, map, write_n(8, {0, 0}), 0);
    std::vector<std::uint16_t> quiet = regs_of(transact(slave, map, read_req(16, 9), 0));
    const std::uint16_t cont = quiet.size() == 9 ? quiet[2] : 0;
    for (std::uint16_t k = 0; k < cb.action_count; ++k) {
        (void)transact(slave, map, write1(16, k), 0);
        const std::vector<std::uint16_t> g = regs_of(transact(slave, map, read_req(16, 9), 0));
        // In the quiet state the rulebook chose the LEAST cautious action, so every proposal is accepted.
        check(g.size() == 9 && g[0] == k && g[5] == 1U && g[4] == k, "quiet state: proposal accepted");
    }
    // Find a shutdown state and propose `continue`: replaced.
    for (std::uint32_t m = 0; m < states; ++m) {
        (void)rt::instantiate(fw, cb, m);
        const auto d = quorum::arbiter::decide(fw, solver, policy);
        if (!(d.decided() && d.action.get() == cb.safe_action)) { continue; }
        (void)transact(slave, map, write_n(8, {static_cast<std::uint16_t>(m >> 16), static_cast<std::uint16_t>(m)}), 1);
        (void)transact(slave, map, write1(16, cont), 1);
        const std::vector<std::uint16_t> g = regs_of(transact(slave, map, read_req(16, 9), 1));
        check(g.size() == 9 && g[4] != cont && g[4] == g[2] && g[5] == 0U &&
                  g[6] == static_cast<std::uint16_t>(rt::GuardReason::LessCautious),
              "shutdown state: a proposal to continue is replaced by shutdown, reason LessCautious");
        break;
    }
    (void)transact(slave, map, write1(16, 0xFFFF), 2);
    std::vector<std::uint16_t> none = regs_of(transact(slave, map, read_req(16, 9), 2));
    check(none.size() == 9 && none[0] == 0xFFFFU && none[6] == 0xFFFFU, "0xFFFF withdraws the proposal");

    std::printf("exceptions and silence\n");
    r = transact(slave, map, read_req(1000, 1), 3);
    check(r.size() == 5 && r[1] == 0x83 && r[2] == 2, "unmapped address: exception 02");
    r = transact(slave, map, write1(16, 99), 3);
    check(r.size() == 5 && r[1] == 0x86 && r[2] == 3, "proposal out of range: exception 03");
    r = transact(slave, map, write_n(8, {0xFFFF, 0xFFFF}), 3);
    check(r.size() == 5 && r[1] == 0x90 && r[2] == 3, "mask bits beyond the observations: exception 03");
    r = transact(slave, map, write1(4, 1), 3);
    check(r.size() == 5 && r[1] == 0x86 && r[2] == 2, "writing a read-only register: exception 02");
    r = transact(slave, map, with_crc({1, 5, 0, 0, 0xFF, 0}), 3);
    check(r.size() == 5 && r[1] == 0x85 && r[2] == 1, "unsupported function: exception 01");
    Bytes bad = read_req(0, 1);
    bad.back() ^= 0x55U;
    r = transact(slave, map, bad, 4);
    check(r.empty() && slave.crc_errors() == 1, "a bad CRC is dropped silently and counted");
    r = transact(slave, map, read_req(0, 1, 7), 5);
    check(r.empty(), "another slave's request is ignored");
    r = transact(slave, map, read_req(0, 1, 0), 6);
    check(r.empty(), "a broadcast is never answered");
    // A partial frame, then silence longer than the gap, then a good frame: resynchronises.
    (void)transact(slave, map, Bytes{1, 3, 0}, 100);
    r = transact(slave, map, read_req(0, 1), 200);
    check(r.size() == 7 && regs_of(r).size() == 1 && regs_of(r)[0] == 0x5155U, "resynchronises after a gap");

    std::printf("raw channel values, held thresholds and staleness through Modbus\n");
    static rt::Casebook held;
    check(load(argv[2], held), "the held image loads");
    static rt::RtPolicy hp;
    static rt::RtBinding binding;
    check(rt::make_policy(hp, held) == quorum::solver::Status::Ok, "held policy");
    check(rt::make_binding(binding, held) == quorum::binding::Status::Ok, "held binding");
    static mb::QuorumMap hmap;
    hmap.attach(held, &binding, fw, solver, hp);
    mb::Slave hs(1);
    const auto action_name = [&](std::uint32_t now) {
        const std::vector<std::uint16_t> o = regs_of(transact(hs, hmap, read_req(16, 9), now));
        return o.size() == 9 && o[2] < held.action_count ? std::string(rt::name_of(held, held.actions[o[2]]))
                                                         : std::string("?");
    };
    check(action_name(0) == "inspect", "before any sample the channels are blind, so: inspect");
    const auto sample = [&](std::uint16_t ch, std::int32_t milli, std::uint32_t now) {
        const auto u = static_cast<std::uint32_t>(milli);
        (void)transact(hs, hmap, write_n(static_cast<std::uint16_t>(256 + 2 * ch),
                                         {static_cast<std::uint16_t>(u >> 16), static_cast<std::uint16_t>(u)}), now);
    };
    sample(0, 95000, 1000);  // bearing 95.0 degC: above 90, but the hold is 5 s
    sample(1, 2000, 1000);   // vibration 2.0 mm/s
    check(action_name(1000) == "carry_on", "hot for 0 s of a 5 s hold: carry on");
    sample(1, 2100, 4000);
    check(action_name(5999) == "carry_on", "hot for 4.999 s: still carry on");
    sample(0, 95500, 5500);
    sample(1, 2000, 5500);
    check(action_name(6000) == "inspect", "hot for 5 s: the held reading asserts, overheating: inspect");
    sample(1, 12000, 6500);  // vibration extreme, no hold
    check(action_name(6500) == "trip", "and extreme vibration at once: seizing, trip");
    check(action_name(13000) == "inspect", "no samples for 6.5 s: vibration is stale, so blind: inspect");

    std::printf("timed readings through Modbus: written readings, the device's clock, and tick()\n");
    static rt::Casebook timed;
    check(load(argv[3], timed), "the timed image loads");
    static rt::RtPolicy tp;
    static rt::RtTimers timers;
    check(rt::make_policy(tp, timed) == quorum::solver::Status::Ok, "timed policy");
    check(rt::make_timers(timers, timed) == quorum::binding::Status::Ok, "timed readings configure");
    static mb::QuorumMap tmap;
    tmap.attach(timed, nullptr, fw, solver, tp, &timers);
    mb::Slave ts(1);
    // timed.cb: motor_started 0, pressure_high 1, overload_trip 2, bearing_hot 3, gauge_suspect 4, flushing 5;
    // timed readings dead_head 6, repeated_trips 7, ran_hot 8.
    const auto put_mask = [&](std::uint32_t m, std::uint32_t now) {
        (void)transact(ts, tmap, write_n(8, {static_cast<std::uint16_t>(m >> 16), static_cast<std::uint16_t>(m)}), now);
    };
    const auto act = [&](std::uint32_t now) {
        const std::vector<std::uint16_t> o = regs_of(transact(ts, tmap, read_req(16, 9), now));
        return o.size() == 9 && o[2] < timed.action_count ? std::string(rt::name_of(timed, timed.actions[o[2]]))
                                                         : std::string("?");
    };
    check(act(0) == "run", "nothing set: run");
    put_mask(1U << 0, 100);
    check(act(100) == "run", "a start: run");
    put_mask((1U << 0) | (1U << 1), 5000);
    check(act(5000) == "stop", "pressure high 4.9 s after the start: dead-heading, stop");
    put_mask(1U << 0, 6000);
    check(act(6000) == "run", "pressure back to normal: run");
    put_mask((1U << 0) | (1U << 1), 30000);
    check(act(30000) == "alarm", "pressure high 30 s after the start: a blockage, alarm");
    put_mask(0, 31000);
    check(act(31000) == "run", "all clear");
    put_mask(1U << 6, 31500);
    check(act(31500) == "run", "a master cannot set a timed reading by writing its bit");
    check((tmap.presence() & (1U << 6)) == 0U, "and the bit stays clear in the presence the device decided on");

    // The overheat must be remembered for 5 minutes even if the master stops asking; tick() keeps the clock moving.
    put_mask(1U << 3, 40000);
    check(act(40000) == "stop", "bearing hot now: stop");
    put_mask(0, 41000);
    for (std::uint32_t t = 41000; t < 340000; t += 1000) { tmap.tick(t); }
    // Last seen hot at 41000: the bit stayed set until the write that cleared it, and the device steps the timed
    // readings just before applying a write.
    check(act(341000) == "alarm", "last seen hot exactly 5 minutes ago: still remembered, alarm");
    check(act(341001) == "run", "and forgotten 1 ms later");

    // Three overload trips, each seen only through tick(), within an hour: lockout.
    std::uint32_t now = 400000;
    for (int trip = 0; trip < 3; ++trip) {
        put_mask(1U << 2, now += 60000);
        tmap.tick(now);
        put_mask(0, now += 1000);
        tmap.tick(now);
    }
    check(act(now) == "stop", "three trips in an hour: lockout, stop");
    check(act(now + 3600000U) == "run", "an hour after the first: run");

    // A reading written set and then clear in the same millisecond, with no read and no tick between: every written
    // state must still be seen. Three such trips, then a bearing that was hot for no time at all.
    now += 7200000U;
    check(act(now) == "run", "two hours later: run");
    for (int trip = 0; trip < 3; ++trip) {
        put_mask(1U << 2, now);
        put_mask(0, now);
        now += 1U;
    }
    check(act(now) == "stop", "three trips written and cleared within a millisecond each, no tick: lockout");
    now += 7200000U;
    put_mask(1U << 3, now);
    put_mask(0, now);
    check(act(now + 299999U) == "alarm", "bearing hot for no measurable time, no tick: still remembered");

    std::printf("the test clock: a bench moves the board's clock forward\n");
    {
        const auto refused = [](const Bytes& resp) { return resp.size() == 5 && (resp[1] & 0x80U) != 0 && resp[2] == 3; };
        const auto ok = [](const Bytes& resp) { return resp.size() == 8 && (resp[1] & 0x80U) == 0; };
        const auto jump = [](std::uint32_t ms) {
            return write_n(11, {static_cast<std::uint16_t>(ms >> 16), static_cast<std::uint16_t>(ms)});
        };
        static rt::RtTimers ct;
        check(rt::make_timers(ct, timed) == quorum::binding::Status::Ok, "a fresh set of timers");
        static mb::QuorumMap cm;
        cm.attach(timed, nullptr, fw, solver, tp, &ct);
        mb::Slave cs(1);
        const auto mask_at = [&](std::uint32_t m, std::uint32_t at) {
            return transact(cs, cm, write_n(8, {static_cast<std::uint16_t>(m >> 16), static_cast<std::uint16_t>(m)}), at);
        };
        const auto act_at = [&](std::uint32_t at) {
            const std::vector<std::uint16_t> o = regs_of(transact(cs, cm, read_req(16, 9), at));
            return o.size() == 9 && o[2] < timed.action_count ? std::string(rt::name_of(timed, timed.actions[o[2]]))
                                                             : std::string("?");
        };
        std::vector<std::uint16_t> c3 = regs_of(transact(cs, cm, read_req(10, 3), 0));
        check(c3.size() == 3 && c3[0] == 0 && c3[1] == 0 && c3[2] == 0, "off at start-up, nothing moved");
        check(refused(transact(cs, cm, jump(1000), 0)), "the clock cannot be moved while the test clock is off");
        check(refused(transact(cs, cm, write1(10, 1), 0)), "the wrong key does not turn it on");
        check(refused(transact(cs, cm, write_n(9, {0, mb::kTestClockKey}), 0)), "nor the key inside a longer write");
        check(ok(transact(cs, cm, write1(10, mb::kTestClockKey), 0)), "the key turns it on");
        c3 = regs_of(transact(cs, cm, read_req(10, 1), 0));
        check(c3.size() == 1 && c3[0] == 1, "and register 10 shows it");
        check(refused(transact(cs, cm, write1(11, 0), 0)), "half an advance is refused");
        check(refused(transact(cs, cm, jump(mb::kMaxClockJumpMs + 1U), 0)), "more than 24 days in one jump is refused");
        // Real time stands still at 1000 ms from here on; only the test clock moves.
        check(act_at(1000) == "run", "nothing set: run");
        (void)mask_at(1U << 3, 1000);
        (void)mask_at(0, 1000);
        check(ok(transact(cs, cm, jump(300000), 1000)), "jump 5 minutes");
        check(act_at(1000) == "alarm", "an overheat exactly 5 minutes ago on the test clock: remembered");
        (void)transact(cs, cm, jump(1), 1000);
        check(act_at(1000) == "run", "1 ms later on the test clock: forgotten");
        for (int trip = 0; trip < 3; ++trip) {
            (void)mask_at(1U << 2, 1000);
            (void)mask_at(0, 1000);
            (void)transact(cs, cm, jump(600000), 1000);
        }
        check(act_at(1000) == "stop", "three trips 10 minutes apart on the test clock: lockout");
        // The first trip was at 300,001 ms on the test clock and the third jump ended at 2,100,001 ms.
        (void)transact(cs, cm, jump(1800000), 1000);
        check(act_at(1000) == "stop", "an hour after the first trip, exactly: still locked out");
        (void)transact(cs, cm, jump(1), 1000);
        check(act_at(1000) == "run", "1 ms more: run");
        (void)transact(cs, cm, jump(mb::kMaxClockJumpMs), 1000);
        c3 = regs_of(transact(cs, cm, read_req(11, 2), 1000));
        check(c3.size() == 2 && ((static_cast<std::uint32_t>(c3[0]) << 16U) | c3[1]) == 300001U + 1800000U + 1800001U +
                                                                                      mb::kMaxClockJumpMs,
              "registers 11 and 12 report how far the clock has been moved");
        // Once a controller has written anything, the test clock can no longer be turned on.
        static rt::RtTimers ct2;
        (void)rt::make_timers(ct2, timed);
        static mb::QuorumMap cm2;
        cm2.attach(timed, nullptr, fw, solver, tp, &ct2);
        mb::Slave cs2(1);
        (void)transact(cs2, cm2, write_n(8, {0, 0}), 0);
        check(refused(transact(cs2, cm2, write1(10, mb::kTestClockKey), 0)),
              "after a controller has written the readings, the key is refused until the board is reset");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
