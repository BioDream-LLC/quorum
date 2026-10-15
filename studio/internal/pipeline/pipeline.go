// Package pipeline is the studio's whole workflow: worksheet -> draft, casebook -> validated image -> UF2.
package pipeline

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"quorum/studio/internal/binimg"
	"quorum/studio/internal/draft"
	"quorum/studio/internal/layout"
	"quorum/studio/internal/tools"
	"quorum/studio/internal/uf2"
)

// Importers by worksheet kind.
var importers = map[string]string{"hazop": "hazop2cb", "lopa": "lopa2cb", "fmea": "fmea2cb"}

// Draft is what an importer makes of a worksheet. It is a starting point: cbc refuses it until an engineer fills in
// what the worksheet cannot know.
type Draft struct {
	Source   string `json:"source"`   // the drafted casebook
	Analysis string `json:"analysis"` // the importer's findings (stderr), e.g. the safeguard analysis
	Report   string `json:"report"`   // the review report, markdown
}

var nameRe = regexp.MustCompile(`[^a-z0-9_-]+`)

// CasebookName turns a file name into a casebook name the importers accept.
func CasebookName(file string) string {
	n := strings.ToLower(strings.TrimSuffix(filepath.Base(file), filepath.Ext(file)))
	n = strings.Trim(nameRe.ReplaceAllString(n, "_"), "_")
	if n == "" {
		return "imported"
	}
	return n
}

// Import drafts a casebook from a worksheet.
func Import(ctx context.Context, kind, fileName string, content []byte) (Draft, error) {
	tool, ok := importers[kind]
	if !ok {
		return Draft{}, fmt.Errorf("unknown worksheet kind %q (hazop, lopa or fmea)", kind)
	}
	work, err := os.MkdirTemp("", "quorum-import-")
	if err != nil {
		return Draft{}, err
	}
	defer os.RemoveAll(work)
	in := filepath.Join(work, filepath.Base(fileName))
	if filepath.Ext(in) == "" {
		in += ".csv"
	}
	if err := os.WriteFile(in, content, 0o600); err != nil {
		return Draft{}, err
	}
	report := filepath.Join(work, "report.md")
	r, err := tools.Run(ctx, tool, in, CasebookName(fileName), "--report", report)
	if err != nil {
		return Draft{}, err
	}
	if r.ExitCode != 0 {
		return Draft{}, fmt.Errorf("%s could not read this worksheet:\n%s", tool, strings.TrimSpace(r.Stderr))
	}
	rep, _ := os.ReadFile(report)
	// The importer names the file by its temporary path; say what the user actually opened.
	clean := func(s string) string {
		s = strings.ReplaceAll(s, in, filepath.Base(fileName))
		return strings.ReplaceAll(s, report, "the review report")
	}
	return Draft{Source: clean(r.Stdout), Analysis: clean(r.Stderr), Report: clean(string(rep))}, nil
}

// Learned is what learn2cb makes of a table of records.
type Learned struct {
	Source string `json:"source"` // the drafted casebook
	Report string `json:"report"` // the learning report: what was used, what was skipped, cross-validated estimate
}

// LearnOptions mirror learn2cb's flags.
type LearnOptions struct {
	Outcome  string `json:"outcome"`  // column that says what happened
	Positive string `json:"positive"` // the value of that column meaning "it happened"; empty to infer
	Ignore   string `json:"ignore"`   // comma-separated columns to leave out
	K        int    `json:"k"`        // observations to keep, 1..20; 0 for the default
	Asserts  string `json:"asserts"`  // word for the outcome, e.g. "failure"
}

// Learn drafts a casebook from records.
func Learn(ctx context.Context, fileName string, content []byte, o LearnOptions) (Learned, error) {
	if strings.TrimSpace(o.Outcome) == "" {
		return Learned{}, errors.New("choose the column that says what happened")
	}
	work, err := os.MkdirTemp("", "quorum-learn-")
	if err != nil {
		return Learned{}, err
	}
	defer os.RemoveAll(work)
	in := filepath.Join(work, filepath.Base(fileName))
	if err := os.WriteFile(in, content, 0o600); err != nil {
		return Learned{}, err
	}
	args := []string{in, "--outcome", o.Outcome, "--name", CasebookName(fileName)}
	if o.Positive != "" {
		args = append(args, "--positive", o.Positive)
	}
	if o.Ignore != "" {
		args = append(args, "--ignore", o.Ignore)
	}
	if o.K > 0 {
		args = append(args, "--k", fmt.Sprint(o.K))
	}
	if o.Asserts != "" {
		args = append(args, "--asserts", o.Asserts)
	}
	r, err := tools.Run(ctx, "learn2cb", args...)
	if err != nil {
		return Learned{}, err
	}
	clean := func(s string) string { return strings.ReplaceAll(s, in, filepath.Base(fileName)) }
	if r.ExitCode != 0 {
		return Learned{}, fmt.Errorf("%s", strings.TrimSpace(clean(r.Stderr)))
	}
	return Learned{Source: clean(r.Stdout), Report: strings.TrimSpace(clean(r.Stderr))}, nil
}

// Checked is cbc's verdict on a casebook. Blob is set only when cbc validated every state and wrote an image.
type Checked struct {
	OK     bool   `json:"ok"`
	Output string `json:"output"` // cbc's report and diagnostics, verbatim
	Info   string `json:"info"`   // qblob info: the image as the device's loader sees it
	Sweep  string `json:"sweep"`  // qblob sweep: the DEVICE and GUARD lines the board must print
	Blob   []byte `json:"-"`
	// Sheet is cbc's commissioning sheet (cbc --commissioning): the action each reading must cause, for the loop
	// check on the real plant and the bench test of a board. One casebook only; empty for a palette.
	Sheet string `json:"-"`
}

// Extra is another casebook to put on the same chip (a palette).
type Extra struct {
	Name   string `json:"name"`
	Source string `json:"source"`
}

// Check validates a casebook with cbc and, if it passes, loads the image with the device loader. With extras, every
// casebook is validated on its own and the palette is validated for non-interference, as cbc --palette does.
func Check(ctx context.Context, source string, extras ...Extra) (Checked, error) {
	work, err := os.MkdirTemp("", "quorum-check-")
	if err != nil {
		return Checked{}, err
	}
	defer os.RemoveAll(work)
	cb := filepath.Join(work, "casebook.cb")
	blob := filepath.Join(work, "casebook.qcb")
	if err := os.WriteFile(cb, []byte(source), 0o600); err != nil {
		return Checked{}, err
	}
	args := []string{cb}
	rename := map[string]string{cb: "casebook"}
	for i, x := range extras {
		p := filepath.Join(work, fmt.Sprintf("extra-%d.cb", i+1))
		if err := os.WriteFile(p, []byte(x.Source), 0o600); err != nil {
			return Checked{}, err
		}
		args = append(args, p)
		rename[p] = "also on this chip: " + filepath.Base(x.Name)
	}
	if len(extras) > 0 {
		args = append(args, "--palette")
	}
	args = append(args, "--blob", blob)
	sheet := filepath.Join(work, "casebook.sheet")
	if len(extras) == 0 {
		args = append(args, "--commissioning", sheet)
	}
	r, err := tools.Run(ctx, "cbc", args...)
	if err != nil {
		return Checked{}, err
	}
	out := strings.TrimSpace(r.Stdout + "\n" + r.Stderr)
	for p, n := range rename {
		out = strings.ReplaceAll(out, p, n)
	}
	c := Checked{Output: out}
	if r.ExitCode != 0 {
		return c, nil
	}
	b, err := os.ReadFile(blob)
	if err != nil {
		return c, errors.New("cbc reported success but wrote no image")
	}
	info, err := tools.Run(ctx, "qblob", "info", blob)
	if err != nil {
		return c, err
	}
	if info.ExitCode != 0 {
		c.Output += "\n\nThe device loader refused the image:\n" + info.Stdout + info.Stderr
		return c, nil
	}
	sweep, err := tools.Run(ctx, "qblob", "sweep", blob)
	if err != nil {
		return c, err
	}
	if sweep.ExitCode != 0 {
		c.Output += "\n\nThe runtime path failed its own sweep:\n" + sweep.Stdout + sweep.Stderr
		return c, nil
	}
	c.OK, c.Blob, c.Info, c.Sweep = true, b, info.Stdout, strings.TrimSpace(sweep.Stdout)
	if len(extras) == 0 {
		sh, err := os.ReadFile(sheet)
		if err != nil {
			return c, errors.New("cbc reported success but wrote no commissioning sheet")
		}
		c.Sheet = string(sh)
	}
	// A draft written by a language model passes no check by being well-formed. Until a person removes the marker,
	// it may be checked as often as anyone likes, but no chip image is made from it.
	if draft.Unapproved(source) {
		c.OK, c.Blob = false, nil
		c.Output += "\n\nREFUSED: this rulebook is an UNAPPROVED DRAFT written by a language model. It passed every " +
			"check above, which says it is well-formed, not that it is right. Check each rule against the manual " +
			"sentence quoted above it, then approve it (Approve draft in the studio, or delete the UNAPPROVED DRAFT note)."
	}
	return c, nil
}

// Boards the studio can write an image for.
var Boards = map[string]string{"pico2": "Raspberry Pi Pico 2 (RP2350)", "unor4": "Arduino Uno R4 Minima (RA4M1)"}

// Extension is the programming-file type a board takes.
func Extension(board string) string {
	if board == "unor4" {
		return ".bin"
	}
	return ".uf2"
}

// Image combines the board's runtime firmware with a validated casebook image into one programming file: a UF2 for
// the Pico 2, a flat .bin (written by DFU from the sketch base) for the Uno R4.
func Image(board string, blob []byte) ([]byte, error) {
	switch board {
	case "pico2":
		fw, err := tools.Firmware(board, ".uf2")
		if err != nil {
			return nil, err
		}
		return uf2.WithPayload(fw, blob, layout.Pico2BlobAddress, layout.BlobRegionBytes)
	case "unor4":
		fw, err := tools.Firmware(board, ".bin")
		if err != nil {
			return nil, err
		}
		return binimg.WithPayload(fw, blob, layout.UnoR4SketchBase, layout.UnoR4BlobAddress, layout.BlobRegionBytes,
			layout.UnoR4FlashEnd)
	}
	return nil, fmt.Errorf("board %q is not supported", board)
}

// Messages returns the compiler's lines worth sending back to a model drafting a rulebook: errors, conflict advice,
// arguments that never hold, and failed whole-rulebook checks. A draft that cannot be checked yields none.
func Messages(ctx context.Context, source string) []string {
	c, err := Check(ctx, source)
	if err != nil {
		return nil
	}
	var out []string
	for _, l := range strings.Split(c.Output, "\n") {
		// Errors, and the validator's advice on conflicts ("... are BOTH selected ...", "Add `prefer ...`"), which is
		// not an error but leaves states the rulebook cannot decide.
		if strings.Contains(l, "error:") || strings.Contains(l, "BOTH selected") || strings.Contains(l, "Add `prefer") ||
			strings.Contains(l, "NEVER HOLDS") {
			out = append(out, strings.TrimSpace(l))
		}
		// Failed whole-rulebook checks were not sent back, so a draft with an unreachable action came out of every
		// repair round unchanged (seen 2026-10-11, three drafts). An action is unreachable when an action ranked
		// above it applies in every state where it would.
		if t := strings.TrimSpace(l); strings.HasPrefix(t, "FAIL ") {
			msg := t
			if strings.Contains(t, "unreachable") {
				msg += " -- an action is unreachable when an action ranked above it applies in every state where it " +
					"would; give each action only the triggers the manual states for it"
			}
			out = append(out, msg)
		}
	}
	return out
}
