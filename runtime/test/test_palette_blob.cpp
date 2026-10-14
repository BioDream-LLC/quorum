// runtime_blob_palette -- a palette image (pump + tank on one device) decides as the compiled palette header does.
//
// 2^29 combined states is too many to sweep, so for each member every one of its own states is decided with the
// other member's observations at each of 10 settings (none, all, and 8 fixed pseudo-random masks). For each, both
// paths solve once and every member's policy is applied: outcome, action and step count must agree. The loader's
// refusals are exercised as for single casebooks, plus a member slice moved out of place with the CRC repaired.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "quorum/arbiter/verdict.hpp"
#include "quorum/runtime/blob.hpp"

#include "palette_generated.hpp"

namespace pal = quorum::casebook::gen::palette;
namespace rt = quorum::runtime;
using quorum::solver::Status;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++failures;
    }
}
void put_crc(std::vector<std::uint8_t>& b) {
    const std::uint32_t c = rt::detail::crc32(b.data(), b.size() - 4U);
    for (std::size_t k = 0; k < 4U; ++k) { b[b.size() - 4U + k] = static_cast<std::uint8_t>((c >> (8U * k)) & 0xFFU); }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> blob(std::istreambuf_iterator<char>(in), {});
    static rt::Palette p;
    expect(rt::blob_format(blob.data(), blob.size()) == 2U, "format 2");
    const rt::BlobStatus st = rt::load_palette(blob.data(), blob.size(), p);
    if (st != rt::BlobStatus::Ok) {
        std::printf("FAIL  load: %s\n", rt::blob_status_name(st));
        return 1;
    }
    expect(p.argument_count == pal::kArgumentCount && p.edge_count == pal::kCasebookEdges, "counts");
    bool edges = true;
    for (std::int32_t i = 0; edges && i < p.edge_count; ++i) {
        edges = p.edges[i].from == pal::kCasebook[i].from && p.edges[i].to == pal::kCasebook[i].to;
    }
    expect(edges, "every edge, in order");
    expect(p.member_count == 2 && std::strcmp(p.members[0].name, "centrifugal-pump") == 0 &&
               std::strcmp(p.members[1].name, "tank-409") == 0, "members");
    expect(p.members[0].observation_offset == pal::centrifugal_pump::kObservationOffset &&
               p.members[1].observation_offset == pal::tank_409::kObservationOffset &&
               p.members[0].closed_world == pal::centrifugal_pump::kClosedWorld &&
               p.members[1].closed_world == pal::tank_409::kClosedWorld, "member slices and closed worlds");
    bool names = true;
    for (std::int32_t a = 0; names && a < p.argument_count; ++a) {
        names = std::strcmp(rt::detail::nth(p.strings, a), pal::name_of(a)) == 0;
    }
    expect(names, "every argument name");

    static pal::CaseFramework hfw;
    static pal::CaseSolver hs;
    static pal::centrifugal_pump::CasePolicy hp0;
    static pal::tank_409::CasePolicy hp1;
    static rt::RtFramework rfw;
    static rt::RtSolver rs;
    static rt::RtPolicy rp0, rp1;
    expect(pal::centrifugal_pump::make_policy(hp0) == Status::Ok && pal::tank_409::make_policy(hp1) == Status::Ok &&
               rt::make_policy(rp0, p, 0) == Status::Ok && rt::make_policy(rp1, p, 1) == Status::Ok, "policies");

    std::uint32_t lcg = 12345U, solved = 0, differ = 0;
    for (std::int32_t m = 0; m < 2; ++m) {
        const rt::Member& me = p.members[m];
        const rt::Member& other = p.members[1 - m];
        const std::uint32_t other_all = ((1U << static_cast<std::uint32_t>(other.observation_count)) - 1U)
                                        << static_cast<std::uint32_t>(other.observation_offset);
        std::uint32_t others[10] = {0U, other_all};
        for (int k = 2; k < 10; ++k) {
            lcg = lcg * 1103515245U + 12345U;
            others[k] = lcg & other_all;
        }
        for (const std::uint32_t o : others) {
            for (std::uint32_t local = 0; local < (1U << static_cast<std::uint32_t>(me.observation_count)); ++local) {
                const std::uint32_t mask = (local << static_cast<std::uint32_t>(me.observation_offset)) | o;
                if (pal::instantiate(hfw, mask) != Status::Ok || rt::instantiate(rfw, p, mask) != Status::Ok) {
                    expect(false, "instantiate");
                    return 1;
                }
                const auto h0 = quorum::arbiter::decide(hfw, hs, hp0), v0 = quorum::arbiter::decide(rfw, rs, rp0);
                const auto h1 = quorum::arbiter::decide(hfw, hs, hp1), v1 = quorum::arbiter::decide(rfw, rs, rp1);
                const auto same = [](const quorum::arbiter::Verdict& a, const quorum::arbiter::Verdict& b) {
                    return a.outcome == b.outcome && a.steps == b.steps && a.action.valid() == b.action.valid() &&
                           (!a.action.valid() || a.action.get() == b.action.get());
                };
                if (!same(h0, v0) || !same(h1, v1)) { ++differ; }
                if (h0.steps > p.step_bound || h1.steps > p.step_bound) { expect(false, "a solve exceeded the bound"); }
                ++solved;
            }
        }
    }
    expect(differ == 0U, "every swept state decides identically for both members");

    static rt::Palette q;
    std::vector<std::uint8_t> b = blob;
    b[b.size() / 2U] ^= 0x01U;
    expect(rt::load_palette(b.data(), b.size(), q) == rt::BlobStatus::BadCrc, "a flipped byte is refused");
    // Swap the targets of the first two edges, CRC repaired: indices stay valid, only the fingerprint can tell.
    b = blob;
    for (std::size_t k = 0; k < 4U; ++k) { std::swap(b[4U * 12U + k], b[4U * 14U + k]); }
    put_crc(b);
    expect(rt::load_palette(b.data(), b.size(), q) == rt::BlobStatus::FingerprintMismatch, "swapped edges are refused");
    static rt::Casebook single;
    expect(rt::load(blob.data(), blob.size(), single) == rt::BlobStatus::BadFormat,
           "the single-casebook loader refuses a palette");
    if (failures != 0) { return 1; }
    std::printf("palette blob: %zu bytes, %u combined states swept, identical to the palette header for both members\n",
                blob.size(), solved);
    return 0;
}
