package bench

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"
)

// Port is a serial port: Read returns what has arrived, waiting at most a short time (0 bytes and no error on a
// timeout).
type Port interface {
	io.ReadWriteCloser
}

// Open opens a serial port by name (COM3, /dev/ttyACM0, /dev/cu.usbmodem1101) in raw mode. Defined per OS.
var Open func(name string) (Port, error) = openPort

// crc is CRC-16/MODBUS.
func crc(b []byte) uint16 {
	c := uint16(0xFFFF)
	for _, x := range b {
		c ^= uint16(x)
		for k := 0; k < 8; k++ {
			if c&1 != 0 {
				c = c>>1 ^ 0xA001
			} else {
				c >>= 1
			}
		}
	}
	return c
}

func frame(b []byte) []byte {
	c := crc(b)
	return append(b, byte(c), byte(c>>8))
}

// Client is a Modbus RTU master for slave 1, the way a PLC talks to a quorum board.
type Client struct {
	p   Port
	buf []byte
}

// exchange sends one request and returns the reply's payload (after the function code), checking address, function
// code and CRC. want is the full reply length for a normal answer.
func (c *Client) exchange(ctx context.Context, req []byte, want int, wait time.Duration) ([]byte, error) {
	if _, err := c.p.Write(frame(req)); err != nil {
		return nil, err
	}
	deadline := time.Now().Add(wait)
	c.buf = c.buf[:0]
	tmp := make([]byte, 512)
	for time.Now().Before(deadline) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		n, err := c.p.Read(tmp)
		if err != nil {
			return nil, err
		}
		c.buf = append(c.buf, tmp[:n]...)
		// The board may still be printing its start-up report as text; a reply starts at a byte 0x01 followed by
		// our function code (or it with the exception bit). Find the first well-formed one.
		for i := 0; i+5 <= len(c.buf); i++ {
			if c.buf[i] != 1 || (c.buf[i+1]&0x7F) != req[1] {
				continue
			}
			l := want
			if c.buf[i+1]&0x80 != 0 {
				l = 5
			}
			if i+l > len(c.buf) {
				break
			}
			f := c.buf[i : i+l]
			if crc(f[:l-2]) != uint16(f[l-2])|uint16(f[l-1])<<8 {
				continue
			}
			if f[1]&0x80 != 0 {
				return nil, fmt.Errorf("the board answered with Modbus exception %d", f[2])
			}
			return f[2 : l-2], nil
		}
	}
	return nil, errors.New("no answer from the board")
}

// Read reads holding registers.
func (c *Client) Read(ctx context.Context, addr, count uint16) ([]uint16, error) {
	req := []byte{1, 3, byte(addr >> 8), byte(addr), byte(count >> 8), byte(count)}
	p, err := c.exchange(ctx, req, 5+2*int(count), 700*time.Millisecond)
	if err != nil {
		return nil, err
	}
	if int(p[0]) != 2*int(count) {
		return nil, errors.New("malformed answer from the board")
	}
	out := make([]uint16, count)
	for i := range out {
		out[i] = uint16(p[1+2*i])<<8 | uint16(p[2+2*i])
	}
	return out, nil
}

// Write writes holding registers with function 16.
func (c *Client) Write(ctx context.Context, addr uint16, v ...uint16) error {
	req := []byte{1, 16, byte(addr >> 8), byte(addr), 0, byte(len(v)), byte(2 * len(v))}
	for _, x := range v {
		req = append(req, byte(x>>8), byte(x))
	}
	_, err := c.exchange(ctx, req, 8, 700*time.Millisecond)
	return err
}

// Result is a bench test: every state of Steps driven on the board, and what it answered.
type Result struct {
	Port        string `json:"port"`
	Fingerprint uint32 `json:"fingerprint"`
	Steps       []Step `json:"steps"`
	Passed      int    `json:"passed"`
	Failed      int    `json:"failed"`
}

// Run drives the board on port through every step of the sheet and compares each decision with the sheet. It
// refuses a board that runs a different image, and a rulebook with raw channels: those readings come from samples
// the controller must keep writing, which a bench test cannot stand in for honestly. It refuses timed readings for the
// same reason: the board sets them from the history of other readings and ignores a written bit.
func Run(ctx context.Context, port string, s Sheet) (Result, error) {
	res := Result{Port: port}
	if s.Channels > 0 {
		return res, errors.New("this rulebook reads raw channels, which the bench test does not drive; use the " +
			"printed sheet on the plant instead")
	}
	if n := s.Timed(); n > 0 {
		return res, fmt.Errorf("this rulebook has %d timed readings, which the board works out itself from other "+
			"readings over time, so the bench test cannot set them by writing a bit; use the printed sheet on the plant "+
			"instead", n)
	}
	steps, err := s.Steps()
	if err != nil {
		return res, err
	}
	p, err := Open(port)
	if err != nil {
		return res, fmt.Errorf("cannot open %s: %w", port, err)
	}
	defer p.Close()
	c := &Client{p: p}

	// The board switches to Modbus on its first valid request, which can be lost while it is still printing its
	// start-up report, so the first one is retried.
	var id []uint16
	for try := 0; try < 6; try++ {
		if id, err = c.Read(ctx, 0, 6); err == nil {
			break
		}
		if ctx.Err() != nil {
			return res, ctx.Err()
		}
	}
	if err != nil {
		return res, fmt.Errorf("%s: %w (is a quorum board on this port?)", port, err)
	}
	if id[0] != 0x5155 {
		return res, fmt.Errorf("the device on %s is not a quorum board", port)
	}
	res.Fingerprint = uint32(id[2])<<16 | uint32(id[3])
	if res.Fingerprint != s.Fingerprint {
		return res, fmt.Errorf("the board on %s runs image 0x%08x, not this rulebook (0x%08x): write this image to it first",
			port, res.Fingerprint, s.Fingerprint)
	}
	if id[5] != 1 {
		return res, errors.New("the board carries several casebooks; test each on its own board")
	}
	// No proposal: the rulebook's own decision is what is compared.
	if err := c.Write(ctx, 16, 0xFFFF); err != nil {
		return res, err
	}
	for _, st := range steps {
		if err := c.Write(ctx, 8, uint16(st.Mask>>16), uint16(st.Mask)); err != nil {
			return res, fmt.Errorf("%s: %w", st.Label, err)
		}
		v, err := c.Read(ctx, 16+2, 1)
		if err != nil {
			return res, fmt.Errorf("%s: %w", st.Label, err)
		}
		st.Got = fmt.Sprintf("action #%d", v[0])
		if int(v[0]) < len(s.Actions) {
			st.Got = s.Actions[v[0]]
		}
		st.Pass = st.Got == st.Expect
		if st.Pass {
			res.Passed++
		} else {
			res.Failed++
		}
		res.Steps = append(res.Steps, st)
	}
	// Leave the board at rest.
	_ = c.Write(ctx, 8, 0, 0)
	return res, nil
}
