package pipeline

import (
	"bytes"
	"context"
	"os"
	"strings"
	"testing"

	"quorum/studio/internal/layout"
	"quorum/studio/internal/uf2"
)

const root = "../../../casebook/"

func read(t *testing.T, p string) []byte {
	t.Helper()
	b, err := os.ReadFile(root + p)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// The pump image must be byte-identical to what the C++ test suite proved equivalent to the compiled header, and
// the image must land in the UF2 exactly where the firmware looks.
func TestPumpEndToEnd(t *testing.T) {
	c, err := Check(context.Background(), string(read(t, "pump/pump.cb")))
	if err != nil {
		t.Fatal(err)
	}
	if !c.OK {
		t.Fatalf("pump.cb refused:\n%s", c.Output)
	}
	// Recorded from `qblob sweep` and from runtime_blob_pump, which decides every state through both paths.
	// The DEVICE line, then the GUARD line (every state x every proposal, none weaker than the rulebook).
	const want = "DEVICE centrifugal-pump states 16384 asserts 1024 fnv1a 0xe5408b85\n" +
		"GUARD centrifugal-pump pairs 49152 accepted 46326 replaced 2826 weaker 0 fnv1a 0xac18f721"
	if !strings.HasSuffix(c.Sweep, want) {
		t.Fatalf("sweep %q, want suffix %q", c.Sweep, want)
	}
	img, err := Image("pico2", c.Blob)
	if err != nil {
		t.Fatal(err)
	}
	blocks, err := uf2.Parse(img)
	if err != nil {
		t.Fatal(err)
	}
	got, err := uf2.Read(blocks, uf2.FamilyRP2350ArmSecure, layout.Pico2BlobAddress, len(c.Blob))
	if err != nil || !bytes.Equal(got, c.Blob) {
		t.Fatalf("the casebook image is not where the firmware reads it: %v", err)
	}
	fw, _ := os.ReadFile("../tools/bin/firmware/quorum_runtime_pico2.uf2")
	if !bytes.Equal(img[:uf2.BlockSize], fw[:uf2.BlockSize]) {
		t.Fatal("the SDK's leading RP2350-E10 block was not kept first and unchanged")
	}
	if lo, _ := uf2.Extent(blocks, uf2.FamilyRP2350ArmSecure); lo != layout.XIPBase {
		t.Fatalf("image starts at 0x%08X; the firmware must still be at the start of flash", lo)
	}
}

// An imported draft is refused by design, and the importer's safeguard analysis comes through.
func TestHazopDraftIsRefused(t *testing.T) {
	d, err := Import(context.Background(), "hazop", "hazop_worksheet.csv", read(t, "fmea/hazop_worksheet.csv"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(d.Analysis, "UNPROTECTED") || !strings.Contains(d.Source, "casebook hazop_worksheet") {
		t.Fatalf("unexpected import:\n%s\n%s", d.Analysis, d.Source[:200])
	}
	c, err := Check(context.Background(), d.Source)
	if err != nil {
		t.Fatal(err)
	}
	if c.OK || len(c.Blob) != 0 {
		t.Fatal("an unfinished HAZOP draft produced an image")
	}
	if !strings.Contains(c.Output, "needs at least one action") {
		t.Fatalf("refusal does not say why:\n%s", c.Output)
	}
}

// The Uno R4 image: the sketch, erased flash up to the region, then exactly the blob.
func TestUnoR4Image(t *testing.T) {
	c, err := Check(context.Background(), string(read(t, "pump/pump.cb")))
	if err != nil || !c.OK {
		t.Fatalf("pump.cb: %v", err)
	}
	img, err := Image("unor4", c.Blob)
	if err != nil {
		t.Fatal(err)
	}
	fw, _ := os.ReadFile("../tools/bin/firmware/quorum_runtime_unor4.bin")
	off := layout.UnoR4BlobAddress - layout.UnoR4SketchBase
	if !bytes.Equal(img[:len(fw)], fw) || !bytes.Equal(img[off:], c.Blob) {
		t.Fatal("sketch or casebook image misplaced")
	}
}

// Learning from records: the after-the-event columns are flagged, and without them the draft validates.
func TestLearnAI4I(t *testing.T) {
	b, err := os.ReadFile("../../../learn/data/ai4i2020.csv")
	if err != nil {
		t.Fatal(err)
	}
	l, err := Learn(context.Background(), "ai4i2020.csv", b, LearnOptions{Outcome: "Machine failure", Asserts: "failure"})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(l.Report, "TWF is 1") {
		t.Fatalf("leak not flagged:\n%s", l.Report)
	}
	l, err = Learn(context.Background(), "ai4i2020.csv", b, LearnOptions{Outcome: "Machine failure", Asserts: "failure",
		Ignore: "TWF,HDF,PWF,OSF,RNF"})
	if err != nil {
		t.Fatal(err)
	}
	c, err := Check(context.Background(), l.Source)
	if err != nil || !c.OK {
		t.Fatalf("learned casebook refused: %v\n%s", err, c.Output)
	}
	if _, err := Learn(context.Background(), "x.csv", b, LearnOptions{Outcome: "no such column"}); err == nil ||
		!strings.Contains(err.Error(), "no column named") {
		t.Fatalf("bad outcome column not explained: %v", err)
	}
}

// Two casebooks on one chip: the palette image validates, and each member's DEVICE line is its line alone.
func TestPalette(t *testing.T) {
	c, err := Check(context.Background(), string(read(t, "pump/pump.cb")), Extra{Name: "tank.cb", Source: string(read(t, "tank/tank.cb"))})
	if err != nil || !c.OK {
		t.Fatalf("palette refused: %v\n%s", err, c.Output)
	}
	for _, want := range []string{"DEVICE centrifugal-pump states 16384 asserts 1024 fnv1a 0xe5408b85",
		"DEVICE tank-409 states 32768 asserts 7680 fnv1a 0x90bc4385"} {
		if !strings.Contains(c.Sweep, want) {
			t.Fatalf("missing %q in:\n%s", want, c.Sweep)
		}
	}
	if !strings.Contains(c.Info, "2 casebooks on one device") {
		t.Fatalf("info: %s", c.Info)
	}
	for _, b := range []string{"pico2", "unor4"} {
		if _, err := Image(b, c.Blob); err != nil {
			t.Fatalf("%s: %v", b, err)
		}
	}
}

func TestUnknownBoardAndKind(t *testing.T) {
	if _, err := Image("uno", []byte{1}); err == nil {
		t.Error("an unsupported board was accepted")
	}
	if _, err := Import(context.Background(), "pdf", "x.pdf", nil); err == nil {
		t.Error("an unknown worksheet kind was accepted")
	}
}

// A rulebook carrying the draft marker passes every check and still gets no image.
func TestUnapprovedDraftGetsNoImage(t *testing.T) {
	src := strings.Replace(string(read(t, "pump/pump.cb")), "\nobserve ",
		"\nnote \"UNAPPROVED DRAFT: written by a language model\"\nobserve ", 1)
	c, err := Check(context.Background(), src)
	if err != nil {
		t.Fatal(err)
	}
	if c.OK || c.Blob != nil || !strings.Contains(c.Output, "REFUSED: this rulebook is an UNAPPROVED DRAFT") {
		t.Fatalf("an unapproved draft was accepted (OK=%v):\n%s", c.OK, c.Output)
	}
}
