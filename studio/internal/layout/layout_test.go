package layout

import (
	"os"
	"regexp"
	"strconv"
	"testing"
)

func TestMatchesFirmwareHeader(t *testing.T) {
	src, err := os.ReadFile("../../../runtime/include/quorum/runtime/layout.hpp")
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]int64{"kPico2BlobOffset": Pico2BlobOffset, "kBlobRegionBytes": BlobRegionBytes, "kSweepLimit": SweepLimit,
		"kUnoR4BlobAddress": UnoR4BlobAddress, "kUnoR4SketchBase": UnoR4SketchBase}
	for name, v := range want {
		m := regexp.MustCompile(name + `\s*=\s*(0x[0-9A-Fa-f]+|\d+)U?;`).FindSubmatch(src)
		if m == nil {
			t.Fatalf("%s not found in layout.hpp", name)
		}
		got, err := strconv.ParseInt(string(m[1]), 0, 64)
		if err != nil || got != v {
			t.Errorf("%s: firmware says %s, studio says %#x", name, m[1], v)
		}
	}
}
