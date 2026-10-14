// sha256.hpp -- FIPS 180-4 SHA-256, for identifying a document revision.
//
// WHY THIS IS HERE RATHER THAN A LIBRARY CALL
//
// A review report states which revision of a worksheet it describes. Without that, a report and a document drift
// apart silently and a customer acts on findings about a file they have since edited -- which is the same class of
// failure as everything else this project chases.
//
// This is used for IDENTIFICATION, not security. There is no adversary here: nobody is trying to forge a HAZOP
// that collides with another HAZOP. What is needed is a short value that changes when the file changes, and that
// anyone can reproduce with `sha256sum` to confirm the report matches their copy.
//
// Hand-rolling a hash is normally a bad idea, and the reason it is acceptable here is that the algorithm is fully
// specified and the specification ships with test vectors. The implementation is checked against the published
// values for the empty string, "abc", and a 56-byte input that forces a second block and exercises the length
// padding -- the three cases where an implementation of this shape actually goes wrong. See test_sha256.cpp.

#ifndef QUORUM_CASEBOOK_FMEA_SHA256_HPP
#define QUORUM_CASEBOOK_FMEA_SHA256_HPP

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace quorum::sha256 {

namespace detail {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline std::uint32_t ror(const std::uint32_t x, const unsigned n) {
    return (x >> n) | (x << (32U - n));
}

inline void block(const unsigned char* p, std::array<std::uint32_t, 8>& h) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16)
             | (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint32_t, 8> v = h;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t S1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
        const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const std::uint32_t t1 = v[7] + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
        const std::uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const std::uint32_t t2 = S0 + mj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (std::size_t i = 0; i < 8; ++i) { h[i] += v[i]; }
}

}  // namespace detail

/// Hex digest of a byte string.
inline std::string hex(const std::string& data) {
    std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t n = data.size();
    std::size_t off = 0;
    while (n - off >= 64) { detail::block(p + off, h); off += 64; }

    // Padding: 0x80, then zeroes, then the length in BITS as a big-endian 64-bit value. A message whose tail is
    // 56..63 bytes needs two padding blocks, which is the case an implementation of this shape gets wrong.
    unsigned char tail[128] = {0};
    const std::size_t rem = n - off;
    for (std::size_t i = 0; i < rem; ++i) { tail[i] = p[off + i]; }
    tail[rem] = 0x80U;
    const std::size_t total = (rem >= 56) ? 128 : 64;
    const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[total - 1 - i] = static_cast<unsigned char>((bits >> (8U * i)) & 0xffU);
    }
    detail::block(tail, h);
    if (total == 128) { detail::block(tail + 64, h); }

    std::string out;
    out.reserve(64);
    for (const std::uint32_t x : h) {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", x);
        out += buf;
    }
    return out;
}

}  // namespace quorum::sha256

#endif  // QUORUM_CASEBOOK_FMEA_SHA256_HPP
