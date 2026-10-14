#ifndef QUORUM_RUNTIME_LAYOUT_HPP
#define QUORUM_RUNTIME_LAYOUT_HPP

// Where a casebook image lives in each board's flash. One definition, read by the firmware and -- by parsing this
// file in its tests -- by the desktop tool that writes the image, so the two cannot drift apart silently.

#include <cstdint>

namespace quorum::runtime {

/// Pico 2 (RP2350, 4 MiB flash): the image sits 3 MiB in, far past any runtime firmware, in its own 64 KiB region.
constexpr std::uint32_t kPico2BlobOffset = 0x00300000U;

/// Uno R4 Minima (Renesas RA4M1, 256 KiB code flash at 0x0, sketch at 0x4000): the image sits in the top 64 KiB, an
/// ABSOLUTE address because code flash is memory-mapped from 0. The sketch must stay below it (it is about 50 KiB).
constexpr std::uint32_t kUnoR4BlobAddress = 0x00030000U;
/// Where the Uno R4's DFU upload starts writing: the sketch's first byte.
constexpr std::uint32_t kUnoR4SketchBase = 0x00004000U;

/// The most bytes the loader will consider. A blob is a few KiB; the region is generous and fixed.
constexpr std::uint32_t kBlobRegionBytes = 0x00010000U;

/// The device decides every state at boot only up to this many observations (2^20 states takes seconds). Above it,
/// it times the worst-case state alone; exhaustive validation already happened on the host, in cbc.
constexpr std::int32_t kSweepLimit = 20;

}  // namespace quorum::runtime

#endif  // QUORUM_RUNTIME_LAYOUT_HPP
