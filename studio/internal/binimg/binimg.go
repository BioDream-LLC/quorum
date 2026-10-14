// Package binimg places a casebook image after a raw firmware binary, for boards whose upload takes a flat .bin
// (the Uno R4's DFU upload writes a .bin from the sketch's first byte).
package binimg

import (
	"errors"
	"fmt"
)

// WithPayload returns firmware, padded with 0xFF (erased flash) up to addr, followed by payload.
//
// base is the flash address of the firmware's first byte. Refuses: an empty payload, a payload larger than
// regionBytes, a region that does not fit in flashEnd, and firmware that reaches into the region.
func WithPayload(firmware, payload []byte, base, addr, regionBytes, flashEnd uint32) ([]byte, error) {
	if len(firmware) == 0 {
		return nil, errors.New("binimg: empty firmware")
	}
	if len(payload) == 0 {
		return nil, errors.New("binimg: empty payload")
	}
	if uint64(len(payload)) > uint64(regionBytes) {
		return nil, fmt.Errorf("binimg: casebook image is %d bytes; the device region holds %d", len(payload), regionBytes)
	}
	if addr < base || uint64(addr)+uint64(regionBytes) > uint64(flashEnd) {
		return nil, fmt.Errorf("binimg: region 0x%08X+0x%X does not fit between 0x%08X and 0x%08X", addr, regionBytes, base, flashEnd)
	}
	end := uint64(base) + uint64(len(firmware))
	if end > uint64(addr) {
		return nil, fmt.Errorf("binimg: firmware ends at 0x%08X, inside the casebook region at 0x%08X", end, addr)
	}
	out := make([]byte, int(addr-base)+len(payload))
	copy(out, firmware)
	for i := len(firmware); i < int(addr-base); i++ {
		out[i] = 0xFF
	}
	copy(out[addr-base:], payload)
	return out, nil
}
