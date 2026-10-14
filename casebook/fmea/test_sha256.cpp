// The digest that ties a review report to a document revision.
//
// Hand-rolling a hash is normally a bad idea. It is acceptable here only because the algorithm is fully specified
// and ships with test vectors -- so the implementation is held to them rather than to my confidence in it.
//
// The three vectors are chosen for where an implementation of this shape actually breaks: the empty input (padding
// with no data), a short input (one block), and a 56-byte input, which is the case where the length field no longer
// fits in the first block and a SECOND padding block is required. An implementation that omits that second block
// passes the first two vectors and fails silently on most real files.
//
// The fourth assertion is the one that matters in practice: agreement with sha256sum. The report tells a customer
// they can verify the digest themselves, and that promise is only as good as this.
#include "sha256.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    int bad = 0;
    struct V { const char* in; const char* want; const char* why; };
    const V vs[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "empty input: padding with no data at all"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "one short block"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "56 bytes: the length field no longer fits, so a SECOND padding block is required"},
    };
    for (const V& v : vs) {
        const std::string got = quorum::sha256::hex(v.in);
        if (got != v.want) {
            std::fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", v.why, got.c_str(), v.want);
            ++bad;
        }
    }

    // Agreement with the system tool, on a file large enough to span many blocks. The caller passes the path and
    // the expected digest, so this test does not shell out.
    if (argc >= 3) {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "FAIL cannot open %s\n", argv[1]);
            return 1;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string got = quorum::sha256::hex(ss.str());
        if (got != argv[2]) {
            std::fprintf(stderr, "FAIL %s disagrees with sha256sum\n  got  %s\n  want %s\n",
                         argv[1], got.c_str(), argv[2]);
            ++bad;
        } else {
            std::printf("OK: %zu bytes of %s agree with sha256sum\n", ss.str().size(), argv[1]);
        }
    }
    if (bad == 0) { std::printf("OK: 3 FIPS 180-4 vectors including the two-block padding case\n"); }
    return bad;
}
