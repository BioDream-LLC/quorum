package bench

import (
	"context"
	"strings"
	"testing"
	"time"
)

const pumpSheet = "quorum-commissioning 1\ncasebook pump v3\nfingerprint 0x1a375f9d\nactions\tcontinue\tmaintain\tshutdown\n" +
	"channels\t0\nrest\tcontinue\tDecided\n" +
	"row\t0\tvib_high\tpresence bit 0\tcontinue\tDecided\tvib_1x\tmaintain\tcontinue\tbroadband vibration\n" +
	"row\t1\tvib_1x\tpresence bit 1\tcontinue\tDecided\tvib_high\tmaintain\tcontinue\t1x dominant\n" +
	"row\t2\toverspeed\tpresence bit 2\tshutdown\tDecided\talone\tshutdown\tcontinue\toverspeed\n"

// fakeBoard answers Modbus like a quorum board running the sheet above, after some start-up text.
type fakeBoard struct {
	mask    uint32
	out     []byte
	decide  func(uint32) uint16
	fp      uint32
	pending []byte
}

func (b *fakeBoard) Write(p []byte) (int, error) {
	b.pending = append(b.pending, p...)
	f := b.pending
	if len(f) < 8 {
		return len(p), nil
	}
	b.pending = nil
	reg := func(a uint16) uint16 {
		switch a {
		case 0:
			return 0x5155
		case 1:
			return 1
		case 2:
			return uint16(b.fp >> 16)
		case 3:
			return uint16(b.fp)
		case 5:
			return 1
		case 18:
			return b.decide(b.mask)
		}
		return 0
	}
	addr := uint16(f[2])<<8 | uint16(f[3])
	switch f[1] {
	case 3:
		n := uint16(f[4])<<8 | uint16(f[5])
		r := []byte{1, 3, byte(2 * n)}
		for i := uint16(0); i < n; i++ {
			v := reg(addr + i)
			r = append(r, byte(v>>8), byte(v))
		}
		b.out = append(b.out, frame(r)...)
	case 16:
		if addr == 8 {
			b.mask = uint32(f[7])<<24 | uint32(f[8])<<16 | uint32(f[9])<<8 | uint32(f[10])
		}
		b.out = append(b.out, frame([]byte{1, 16, f[2], f[3], f[4], f[5]})...)
	}
	return len(p), nil
}

func (b *fakeBoard) Read(p []byte) (int, error) {
	if len(b.out) == 0 {
		time.Sleep(time.Millisecond)
		return 0, nil
	}
	n := copy(p, b.out)
	b.out = b.out[n:]
	return n, nil
}

func (b *fakeBoard) Close() error { return nil }

func run(t *testing.T, b *fakeBoard) (Result, error) {
	t.Helper()
	sh, err := Parse(pumpSheet)
	if err != nil {
		t.Fatal(err)
	}
	old := Open
	Open = func(string) (Port, error) { return b, nil }
	defer func() { Open = old }()
	return Run(context.Background(), "COM9", sh)
}

// The pump's rules as the sheet states them: vib_high and vib_1x together mean maintain; overspeed means shutdown.
func pump(m uint32) uint16 {
	switch {
	case m&4 != 0:
		return 2
	case m&3 == 3:
		return 1
	}
	return 0
}

func TestBenchPassesABoardThatMatchesTheSheet(t *testing.T) {
	b := &fakeBoard{fp: 0x1a375f9d, decide: pump, out: []byte("=== quorum runtime on RP2350 ===\n")}
	r, err := run(t, b)
	if err != nil {
		t.Fatal(err)
	}
	// rest; vib_high alone, with vib_1x, vib_1x without it; the same for vib_1x; overspeed alone
	if r.Passed != 8 || r.Failed != 0 {
		t.Errorf("passed %d failed %d: %+v", r.Passed, r.Failed, r.Steps)
	}
}

func TestBenchFindsABoardThatDisagrees(t *testing.T) {
	wired := func(m uint32) uint16 { return pump(m &^ 4) } // the overspeed input never arrives
	r, err := run(t, &fakeBoard{fp: 0x1a375f9d, decide: wired})
	if err != nil {
		t.Fatal(err)
	}
	if r.Failed != 1 || !strings.Contains(r.Steps[len(r.Steps)-1].Label, "overspeed alone") || r.Steps[len(r.Steps)-1].Got != "continue" {
		t.Errorf("%+v", r.Steps)
	}
}

func TestBenchRefusesABoardWithAnotherImage(t *testing.T) {
	_, err := run(t, &fakeBoard{fp: 0xdeadbeef, decide: pump})
	if err == nil || !strings.Contains(err.Error(), "runs image 0xdeadbeef") {
		t.Errorf("err = %v", err)
	}
}

func TestSheetHTMLEscapesAndListsEveryReading(t *testing.T) {
	sh, _ := Parse(pumpSheet)
	sh.Rows[0].Description = "<script>"
	h := sh.HTML("pump.cb")
	if strings.Contains(h, "<script>") || strings.Count(h, "<tr><td>") != 3 || !strings.Contains(h, "0x1a375f9d") {
		t.Error(h)
	}
}
