package uf2

import (
	"bytes"
	"strings"
	"testing"
)

const flashBase = 0x10000000

const familyAbsolute = 0xE48BFF57

// firmware builds a UF2 shaped like the Pico SDK's RP2350 output: the RP2350-E10 workaround block first (absolute
// family, numbered 0 of 2 on purpose), then the image.
func firmware(t *testing.T, blocks int) []byte {
	t.Helper()
	e10 := Block{Addr: 0x10FFFF00, Data: bytes.Repeat([]byte{0xEF}, PayloadSize), Family: familyAbsolute,
		Flags: flagFamilyID | 0x8000, Seq: 0, Total: 2}
	bl := []Block{e10}
	for i := 0; i < blocks; i++ {
		d := bytes.Repeat([]byte{byte(i)}, PayloadSize)
		bl = append(bl, Block{Addr: flashBase + uint32(i*PayloadSize), Data: d, Family: FamilyRP2350ArmSecure})
	}
	return Write(bl)
}

func TestRoundTrip(t *testing.T) {
	fw := firmware(t, 5)
	got, err := Parse(fw)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(Write(got), fw) {
		t.Fatal("Parse then Write changed the file")
	}
}

func TestWithPayloadPlacesEveryByte(t *testing.T) {
	fw := firmware(t, 4)
	payload := make([]byte, 1000) // not a multiple of 256: the last block is padded
	for i := range payload {
		payload[i] = byte(i * 7)
	}
	const addr = flashBase + 0x300000
	out, err := WithPayload(fw, payload, addr, 0x10000)
	if err != nil {
		t.Fatal(err)
	}
	blocks, err := Parse(out)
	if err != nil {
		t.Fatalf("our own output does not parse: %v", err)
	}
	if len(blocks) != 1+4+4 {
		t.Fatalf("got %d blocks, want 9", len(blocks))
	}
	if !bytes.Equal(out[:BlockSize], fw[:BlockSize]) {
		t.Fatal("the RP2350-E10 workaround block was not passed through byte for byte")
	}
	back, err := Read(blocks, FamilyRP2350ArmSecure, addr, len(payload))
	if err != nil || !bytes.Equal(back, payload) {
		t.Fatalf("payload did not survive: %v", err)
	}
	pad, err := Read(blocks, FamilyRP2350ArmSecure, addr+uint32(len(payload)), 4*PayloadSize-len(payload))
	if err != nil || bytes.Count(pad, []byte{0xFF}) != len(pad) {
		t.Fatal("padding is not erased-flash 0xFF")
	}
	orig, _ := Parse(fw)
	for i := range orig {
		if blocks[i].Addr != orig[i].Addr || !bytes.Equal(blocks[i].Data, orig[i].Data) {
			t.Fatalf("firmware block %d changed", i)
		}
	}
	for i, b := range blocks[1:] {
		if b.Seq != uint32(i) || b.Total != 8 {
			t.Fatalf("image block %d numbered %d of %d, want %d of 8", i, b.Seq, b.Total, i)
		}
	}
}

func TestRefusals(t *testing.T) {
	fw := firmware(t, 4)
	cases := []struct {
		name    string
		fw      []byte
		payload []byte
		addr    uint32
		want    string
	}{
		{"overlap", fw, []byte{1}, flashBase + 0x100, "overlaps"},
		{"unaligned", fw, []byte{1}, flashBase + 0x300001, "aligned"},
		{"too large", fw, make([]byte, 0x10001), flashBase + 0x300000, "region holds"},
		{"empty", fw, nil, flashBase + 0x300000, "empty"},
		{"not uf2", []byte("hello"), []byte{1}, flashBase + 0x300000, "whole number"},
	}
	for _, c := range cases {
		_, err := WithPayload(c.fw, c.payload, c.addr, 0x10000)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: got %v, want an error containing %q", c.name, err, c.want)
		}
	}
	renum := firmware(t, 3)
	renum[2*512+20] = 5 // an image block out of sequence
	if _, err := Parse(renum); err == nil {
		t.Error("a misnumbered block was accepted")
	}
}
