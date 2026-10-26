package bench

import (
	"context"
	"errors"
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
	return Run(context.Background(), "COM9", sh, Options{})
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

// A small timed rulebook for the fake board: "hot" is bit 0, and "ran_hot" (bit 1) is set while hot was seen in the
// last 300 ms. ran_hot means alarm; hot itself means stop. The script is what cbc would write for it.
const timedSheet = "quorum-commissioning 1\ncasebook t v1\nfingerprint 0x00000077\nactions\trun\talarm\tstop\n" +
	"channels\t0\nrest\trun\tDecided\n" +
	"row\t0\thot\tpresence bit 0\tstop\tDecided\talone\tstop\trun\thot\n" +
	"row\t1\tran_hot\ttimed: hot within the last 300 ms\talarm\tDecided\talone\talarm\trun\tran hot\n" +
	"scene\t0\nexpect\t0\trun\tnothing set\n" +
	"scene\t0\nset\t0\t0x1\nset\t30\t0x0\nexpect\t300\talarm\tran_hot: hot last seen 270 ms ago\n" +
	"expect\t360\trun\tran_hot: hot last seen 330 ms ago\n"

// timedBoard is a fake board for timedSheet with a real clock, a test clock, and the rule that the test clock can be
// turned on only before anything else is written.
type timedBoard struct {
	fakeBoard
	start    time.Time
	offset   uint64
	on       bool
	written  bool
	lastHot  int64 // ms on the board's clock, -1 = never
	memory   int64 // how long ran_hot lasts, ms
	advances int
}

func (b *timedBoard) now() int64 { return time.Since(b.start).Milliseconds() + int64(b.offset) }

func (b *timedBoard) step() {
	if b.mask&1 != 0 {
		b.lastHot = b.now()
	}
}

func (b *timedBoard) Write(p []byte) (int, error) {
	b.pending = append(b.pending, p...)
	f := b.pending
	if len(f) < 8 {
		return len(p), nil
	}
	b.pending = nil
	b.step()
	addr := uint16(f[2])<<8 | uint16(f[3])
	exc := func() { b.out = append(b.out, frame([]byte{1, f[1] | 0x80, 3})...) }
	switch f[1] {
	case 3:
		n := uint16(f[4])<<8 | uint16(f[5])
		r := []byte{1, 3, byte(2 * n)}
		for i := uint16(0); i < n; i++ {
			v := uint16(0)
			switch addr + i {
			case 0:
				v = 0x5155
			case 1:
				v = 2
			case 2:
				v = uint16(b.fp >> 16)
			case 3:
				v = uint16(b.fp)
			case 5:
				v = 1
			case 18:
				switch {
				case b.mask&1 != 0:
					v = 2
				case b.lastHot >= 0 && b.now()-b.lastHot <= b.memory:
					v = 1
				}
			}
			r = append(r, byte(v>>8), byte(v))
		}
		b.out = append(b.out, frame(r)...)
		return len(p), nil
	case 16:
		v := func(i int) uint16 { return uint16(f[7+2*i])<<8 | uint16(f[8+2*i]) }
		switch addr {
		case 10:
			if b.written || v(0) != 0x7E57 {
				exc()
				return len(p), nil
			}
			b.on = true
		case 11:
			if !b.on {
				exc()
				return len(p), nil
			}
			b.offset += uint64(v(0))<<16 | uint64(v(1))
			b.advances++
		case 8:
			b.written = true
			b.mask = uint32(v(0))<<16 | uint32(v(1))
		default:
			b.written = true
		}
		b.step()
		b.out = append(b.out, frame([]byte{1, 16, f[2], f[3], f[4], f[5]})...)
	}
	return len(p), nil
}

func runTimed(t *testing.T, b *timedBoard, opt Options) (Result, error) {
	t.Helper()
	sh, err := Parse(timedSheet)
	if err != nil {
		t.Fatal(err)
	}
	if len(sh.Script) != 7 || sh.Duration() != 360*time.Millisecond {
		t.Fatalf("script %d cues, %s", len(sh.Script), sh.Duration())
	}
	old := Open
	Open = func(string) (Port, error) { return b, nil }
	defer func() { Open = old }()
	return Run(context.Background(), "COM9", sh, opt)
}

func newTimedBoard() *timedBoard {
	return &timedBoard{fakeBoard: fakeBoard{fp: 0x77}, start: time.Now(), lastHot: -1, memory: 300}
}

func TestTimedBenchUsesTheTestClock(t *testing.T) {
	b := newTimedBoard()
	began := time.Now()
	r, err := runTimed(t, b, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if r.Passed != 3 || r.Failed != 0 || !r.TestClock || !r.Timed || b.advances == 0 {
		t.Errorf("passed %d failed %d testClock %v advances %d: %+v", r.Passed, r.Failed, r.TestClock, b.advances, r.Steps)
	}
	if time.Since(began) > 250*time.Millisecond {
		t.Errorf("took %s: the test clock should not wait", time.Since(began))
	}
}

func TestTimedBenchRefusesWithoutTheTestClockUnlessAllowedToWait(t *testing.T) {
	b := newTimedBoard()
	b.written = true // a controller has already written to it
	_, err := runTimed(t, b, Options{})
	if !errors.Is(err, ErrNoTestClock) || !strings.Contains(err.Error(), "Reset the board") {
		t.Fatalf("want ErrNoTestClock, got %v", err)
	}
	b = newTimedBoard()
	b.written = true
	r, err := runTimed(t, b, Options{RealTime: true})
	if err != nil {
		t.Fatal(err)
	}
	if r.Passed != 3 || r.Failed != 0 || r.TestClock || b.advances != 0 {
		t.Errorf("real time: passed %d failed %d testClock %v advances %d: %+v", r.Passed, r.Failed, r.TestClock,
			b.advances, r.Steps)
	}
}

func TestTimedBenchFindsABoardThatForgetsTooSoon(t *testing.T) {
	b := newTimedBoard()
	b.memory = 200 // the board remembers an overheat for 200 ms instead of 300 ms
	r, err := runTimed(t, b, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if r.Failed != 1 || !strings.Contains(r.Steps[1].Label, "270 ms ago") || r.Steps[1].Got != "run" {
		t.Errorf("%+v", r.Steps)
	}
}
