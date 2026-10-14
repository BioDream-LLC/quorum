// Package layout mirrors runtime/include/quorum/runtime/layout.hpp. layout_test.go parses that header and fails if
// these numbers differ from it, so the image this tool writes lands where the firmware looks.
package layout

const (
	XIPBase          = 0x10000000 // RP2350 flash, memory-mapped
	Pico2BlobOffset  = 0x00300000
	BlobRegionBytes  = 0x00010000
	SweepLimit       = 20
	Pico2BlobAddress = XIPBase + Pico2BlobOffset

	UnoR4BlobAddress = 0x00030000 // absolute: RA4M1 code flash is mapped from 0
	UnoR4SketchBase  = 0x00004000 // the DFU upload's first byte
	UnoR4FlashEnd    = 0x00040000 // 256 KiB
)
