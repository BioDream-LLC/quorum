// The blob loader is device code. This translation unit is compiled -ffreestanding -fno-exceptions -fno-rtti with
// -Werror, so an accidental hosted include or allocation fails here rather than on a Cortex-M.

#include "quorum/runtime/blob.hpp"

namespace {
quorum::runtime::Casebook g_casebook;
quorum::runtime::RtFramework g_framework;
}  // namespace

extern "C" int quorum_runtime_freestanding_probe(const unsigned char* p, unsigned n) {
    if (quorum::runtime::load(p, n, g_casebook) != quorum::runtime::BlobStatus::Ok) { return -1; }
    return quorum::runtime::instantiate(g_framework, g_casebook, 0U) == quorum::solver::Status::Ok ? 0 : -2;
}

namespace {
quorum::runtime::Palette g_palette;
quorum::runtime::RtPolicy g_policy;
}  // namespace

extern "C" int quorum_runtime_freestanding_palette_probe(const unsigned char* p, unsigned n) {
    if (quorum::runtime::blob_format(p, n) != 2U) { return -3; }
    if (quorum::runtime::load_palette(p, n, g_palette) != quorum::runtime::BlobStatus::Ok) { return -1; }
    if (quorum::runtime::make_policy(g_policy, g_palette, 0) != quorum::solver::Status::Ok) { return -4; }
    return quorum::runtime::instantiate(g_framework, g_palette, 0U) == quorum::solver::Status::Ok ? 0 : -2;
}
