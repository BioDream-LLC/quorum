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
        std::printf("usage: test_modbus <pump.qcb> <held.qcb>\n");
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
    check(v.size() == 10 && v[0] == 0x5155U && v[1] == 1U, "id and map version");
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

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
