// Package uf2 reads and writes UF2, the drag-and-drop flashing format the Pico's bootloader accepts.
//
// It does one job for quorum studio: take the prebuilt runtime firmware's UF2 and add a casebook image at a fixed
// flash address, producing ONE file the user drops on the board. Everything here refuses rather than guesses: a block
// it does not understand, a family it did not expect, or an image that would overlap the firmware is an error.
package uf2

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
)

const (
	BlockSize   = 512
	PayloadSize = 256 // what every Pico tool writes, and what the RP2350 bootrom expects

	magicStart0 = 0x0A324655
	magicStart1 = 0x9E5D5157
	magicEnd    = 0x0AB16F30

	flagNotMainFlash = 0x00000001
	flagFamilyID     = 0x00002000

	// FamilyRP2350ArmSecure is the family the Pico SDK writes for an RP2350 Arm image.
	FamilyRP2350ArmSecure = 0xE48BFF59
)

// Block is one 256-byte payload destined for a flash address.
type Block struct {
	Addr   uint32
	Data   []byte // exactly PayloadSize bytes
	Family uint32
	Flags  uint32
	Seq    uint32 // as numbered in the file: per family, since a UF2 may concatenate several
	Total  uint32
	raw    []byte // the original 512 bytes, for blocks passed through untouched
}

// Parse reads a UF2 file. Every block must carry the family flag and a 256-byte payload, and every block of the
// MAIN family (the one with the most blocks) must be numbered 0..n-1 of n.
//
// Blocks of other families are kept but not interpreted. The Pico SDK writes one for every RP2350 image: a lone block
// of the "absolute" family at 0x10FFFF00, numbered 0 of 2 on purpose, which works around bootrom erratum RP2350-E10.
// It must reach the board unchanged, so it is never renumbered or rewritten.
func Parse(b []byte) ([]Block, error) {
	if len(b) == 0 || len(b)%BlockSize != 0 {
		return nil, fmt.Errorf("uf2: %d bytes is not a whole number of %d-byte blocks", len(b), BlockSize)
	}
	n := len(b) / BlockSize
	out := make([]Block, 0, n)
	le := binary.LittleEndian
	for i := 0; i < n; i++ {
		blk := b[i*BlockSize : (i+1)*BlockSize]
		if le.Uint32(blk[0:]) != magicStart0 || le.Uint32(blk[4:]) != magicStart1 || le.Uint32(blk[508:]) != magicEnd {
			return nil, fmt.Errorf("uf2: block %d has bad magic", i)
		}
		flags := le.Uint32(blk[8:])
		if flags&flagNotMainFlash != 0 {
			return nil, fmt.Errorf("uf2: block %d is not for main flash; refusing an image this tool cannot place", i)
		}
		if flags&flagFamilyID == 0 {
			return nil, fmt.Errorf("uf2: block %d has no family ID", i)
		}
		if size := le.Uint32(blk[16:]); size != PayloadSize {
			return nil, fmt.Errorf("uf2: block %d carries %d bytes, expected %d", i, size, PayloadSize)
		}
		seq, total := le.Uint32(blk[20:]), le.Uint32(blk[24:])
		if seq >= total {
			return nil, fmt.Errorf("uf2: block %d is numbered %d of %d", i, seq, total)
		}
		data := make([]byte, PayloadSize)
		copy(data, blk[32:32+PayloadSize])
		raw := make([]byte, BlockSize)
		copy(raw, blk)
		out = append(out, Block{Addr: le.Uint32(blk[12:]), Data: data, Family: le.Uint32(blk[28:]), Flags: flags,
			Seq: seq, Total: total, raw: raw})
	}
	fam := MainFamily(out)
	var k uint32
	main := 0
	for _, bl := range out {
		if bl.Family == fam {
			main++
		}
	}
	for i, bl := range out {
		if bl.Family != fam {
			continue
		}
		if bl.Seq != k || bl.Total != uint32(main) {
			return nil, fmt.Errorf("uf2: block %d is numbered %d of %d; expected %d of %d", i, bl.Seq, bl.Total, k, main)
		}
		k++
	}
	return out, nil
}

// MainFamily is the family with the most blocks: the firmware itself.
func MainFamily(blocks []Block) uint32 {
	count := map[uint32]int{}
	best, bestN := uint32(0), -1
	for _, b := range blocks {
		count[b.Family]++
	}
	for _, b := range blocks { // first-seen order, so ties are deterministic
		if count[b.Family] > bestN {
			best, bestN = b.Family, count[b.Family]
		}
	}
	return best
}

// Write serialises blocks in the order given. Blocks of the main family are numbered 0..n-1 of n; blocks of other
// families that came from Parse are written back byte for byte.
func Write(blocks []Block) []byte {
	out := make([]byte, len(blocks)*BlockSize)
	le := binary.LittleEndian
	fam := MainFamily(blocks)
	var total uint32
	for _, bl := range blocks {
		if bl.Family == fam {
			total++
		}
	}
	var seq uint32
	for i, bl := range blocks {
		blk := out[i*BlockSize : (i+1)*BlockSize]
		if bl.Family != fam && bl.raw != nil {
			copy(blk, bl.raw)
			continue
		}
		flags := bl.Flags
		if flags == 0 {
			flags = flagFamilyID
		}
		le.PutUint32(blk[0:], magicStart0)
		le.PutUint32(blk[4:], magicStart1)
		le.PutUint32(blk[8:], flags)
		le.PutUint32(blk[12:], bl.Addr)
		le.PutUint32(blk[16:], PayloadSize)
		if bl.Family == fam {
			le.PutUint32(blk[20:], seq)
			le.PutUint32(blk[24:], total)
			seq++
		} else {
			le.PutUint32(blk[20:], bl.Seq)
			le.PutUint32(blk[24:], bl.Total)
		}
		le.PutUint32(blk[28:], bl.Family)
		copy(blk[32:32+PayloadSize], bl.Data)
		le.PutUint32(blk[508:], magicEnd)
	}
	return out
}

// Extent is the lowest and one-past-highest address the blocks of one family write.
func Extent(blocks []Block, family uint32) (lo, hi uint32) {
	first := true
	for _, b := range blocks {
		if b.Family != family {
			continue
		}
		if first || b.Addr < lo {
			lo = b.Addr
		}
		if first || b.Addr+PayloadSize > hi {
			hi = b.Addr + PayloadSize
		}
		first = false
	}
	return lo, hi
}

// WithPayload returns the firmware with payload placed at addr, as one UF2.
//
// Every original block is kept, in order. The payload follows as new blocks of the firmware's own family, padded
// with 0xFF (erased flash) to whole blocks, and the firmware family is renumbered to include them. Refuses: an empty
// payload, an addr that is not block-aligned, a payload larger than regionBytes, and any overlap between the region
// and the firmware -- which would silently corrupt one or the other on the device.
func WithPayload(firmware []byte, payload []byte, addr, regionBytes uint32) ([]byte, error) {
	fw, err := Parse(firmware)
	if err != nil {
		return nil, err
	}
	if len(payload) == 0 {
		return nil, errors.New("uf2: empty payload")
	}
	if addr%PayloadSize != 0 {
		return nil, fmt.Errorf("uf2: address 0x%08X is not %d-byte aligned", addr, PayloadSize)
	}
	if uint64(len(payload)) > uint64(regionBytes) {
		return nil, fmt.Errorf("uf2: casebook image is %d bytes; the device region holds %d", len(payload), regionBytes)
	}
	family := MainFamily(fw)
	lo, hi := Extent(fw, family)
	regionEnd := uint64(addr) + uint64(regionBytes)
	if uint64(lo) < regionEnd && uint64(hi) > uint64(addr) {
		return nil, fmt.Errorf("uf2: firmware occupies 0x%08X-0x%08X, which overlaps the casebook region at 0x%08X",
			lo, hi, addr)
	}

	blocks := append([]Block(nil), fw...)
	for off := 0; off < len(payload); off += PayloadSize {
		data := make([]byte, PayloadSize)
		for i := range data {
			data[i] = 0xFF
		}
		copy(data, payload[off:min(off+PayloadSize, len(payload))])
		blocks = append(blocks, Block{Addr: addr + uint32(off), Data: data, Family: family, Flags: flagFamilyID})
	}
	return Write(blocks), nil
}

// Read reassembles the bytes the blocks of one family write to [addr, addr+n), for verification. Bytes no block
// writes are reported as an error rather than invented.
func Read(blocks []Block, family uint32, addr uint32, n int) ([]byte, error) {
	sorted := append([]Block(nil), blocks...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].Addr < sorted[j].Addr })
	out := make([]byte, n)
	covered := make([]bool, n)
	for _, b := range sorted {
		if b.Family != family {
			continue
		}
		for i := 0; i < PayloadSize; i++ {
			a := uint64(b.Addr) + uint64(i)
			if a >= uint64(addr) && a < uint64(addr)+uint64(n) {
				out[a-uint64(addr)] = b.Data[i]
				covered[a-uint64(addr)] = true
			}
		}
	}
	for i, c := range covered {
		if !c {
			return nil, fmt.Errorf("uf2: byte at 0x%08X is not written by any block", addr+uint32(i))
		}
	}
	return out, nil
}
