package binimg

import (
	"bytes"
	"strings"
	"testing"
)

func TestPlacesPayloadAtAddress(t *testing.T) {
	fw := bytes.Repeat([]byte{0xA5}, 1000)
	pl := []byte("QCB1 payload")
	out, err := WithPayload(fw, pl, 0x4000, 0x30000, 0x10000, 0x40000)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(out[:1000], fw) || !bytes.Equal(out[0x30000-0x4000:], pl) {
		t.Fatal("firmware or payload misplaced")
	}
	if bytes.Count(out[1000:0x30000-0x4000], []byte{0xFF}) != 0x30000-0x4000-1000 {
		t.Fatal("gap is not erased-flash 0xFF")
	}
}

func TestRefusals(t *testing.T) {
	cases := []struct {
		fw, pl               []byte
		base, addr, reg, end uint32
		want                 string
	}{
		{make([]byte, 0x30000), []byte{1}, 0x4000, 0x30000, 0x10000, 0x40000, "inside the casebook region"},
		{[]byte{1}, make([]byte, 0x10001), 0x4000, 0x30000, 0x10000, 0x40000, "region holds"},
		{[]byte{1}, []byte{1}, 0x4000, 0x38000, 0x10000, 0x40000, "does not fit"},
		{[]byte{1}, nil, 0x4000, 0x30000, 0x10000, 0x40000, "empty payload"},
	}
	for _, c := range cases {
		if _, err := WithPayload(c.fw, c.pl, c.base, c.addr, c.reg, c.end); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("got %v, want %q", err, c.want)
		}
	}
}
