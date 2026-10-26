// Package bench turns cbc's commissioning sheet into the two checks the exhaustive validation cannot make: a printable
// loop-check sheet for the real plant, and a bench test that drives a board over Modbus and compares each answer with
// the sheet.
//
// Exhaustive validation proves what the rules do with every combination of readings. It cannot prove that the wire
// labelled "overspeed" is the overspeed switch, that a threshold is set where the document says, or that the
// controller writes the right bit. On a plant that is done by a loop check: set each input for real and watch the
// response. The sheet says what the response must be.
package bench

import (
	"bufio"
	"errors"
	"fmt"
	"html"
	"strconv"
	"strings"
)

// Row is one reading of the sheet.
type Row struct {
	Bit         int    `json:"bit"`
	Name        string `json:"name"`
	How         string `json:"how"`     // "presence bit 3", or the channel threshold that sets it
	Alone       string `json:"alone"`   // the action with only this reading set
	Outcome     string `json:"outcome"` // Decided, or why the fallback was taken
	With        string `json:"with"`    // the fewest other readings with which this one changes the decision; "alone" or "-"
	Then        string `json:"then"`    // the action with those and this reading
	Without     string `json:"without"` // the action with those only
	Description string `json:"description"`
}

// Sheet is a parsed commissioning sheet.
type Sheet struct {
	Casebook    string   `json:"casebook"`
	Fingerprint uint32   `json:"fingerprint"`
	Actions     []string `json:"actions"` // in the order the device numbers them
	Channels    int      `json:"channels"`
	Rest        string   `json:"rest"` // the action with nothing set
	Rows        []Row    `json:"rows"`
}

// Parse reads the tab-separated output of cbc --commissioning.
func Parse(text string) (Sheet, error) {
	var s Sheet
	sc := bufio.NewScanner(strings.NewReader(text))
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	first := true
	for sc.Scan() {
		line := strings.TrimRight(sc.Text(), "\r")
		if first {
			if line != "quorum-commissioning 1" {
				return s, errors.New("not a quorum commissioning sheet")
			}
			first = false
			continue
		}
		f := strings.Split(line, "\t")
		switch {
		case strings.HasPrefix(line, "casebook "):
			s.Casebook = strings.TrimPrefix(line, "casebook ")
		case strings.HasPrefix(line, "fingerprint "):
			v, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(line, "fingerprint "), "0x"), 16, 32)
			if err != nil {
				return s, fmt.Errorf("fingerprint: %w", err)
			}
			s.Fingerprint = uint32(v)
		case f[0] == "actions":
			s.Actions = f[1:]
		case f[0] == "channels" && len(f) == 2:
			s.Channels, _ = strconv.Atoi(f[1])
		case f[0] == "rest" && len(f) >= 2:
			s.Rest = f[1]
		case f[0] == "row" && len(f) == 10:
			bit, err := strconv.Atoi(f[1])
			if err != nil {
				return s, fmt.Errorf("row %q: %w", line, err)
			}
			s.Rows = append(s.Rows, Row{Bit: bit, Name: f[2], How: f[3], Alone: f[4], Outcome: f[5], With: f[6],
				Then: f[7], Without: f[8], Description: f[9]})
		}
	}
	if first {
		return s, errors.New("empty commissioning sheet")
	}
	if s.Casebook == "" || len(s.Actions) == 0 || s.Rest == "" {
		return s, errors.New("incomplete commissioning sheet")
	}
	return s, sc.Err()
}

// HTML is the printable loop-check sheet: one line per reading, with what must happen and room for who saw it.
// Timed returns how many readings the board works out itself over time (`reading ... when ...` in the rulebook).
func (s Sheet) Timed() int {
	n := 0
	for _, r := range s.Rows {
		if strings.HasPrefix(r.How, "timed:") {
			n++
		}
	}
	return n
}

func (s Sheet) HTML(source string) string {
	e := html.EscapeString
	var b strings.Builder
	b.WriteString(`<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Commissioning sheet: ` + e(s.Casebook) + `</title>
<style>
body{font:13px/1.4 system-ui,sans-serif;margin:24px;color:#0f172a}
h1{font-size:20px;margin:0 0 4px}
p{margin:4px 0 10px;max-width:60em}
table{border-collapse:collapse;width:100%}
th,td{border:1px solid #94a3b8;padding:5px 6px;text-align:left;vertical-align:top}
th{background:#e2e8f0}
td.sign{width:7em}
code{font-family:ui-monospace,monospace}
@media print{body{margin:10mm}}
</style></head><body>
`)
	b.WriteString("<h1>Commissioning sheet: " + e(s.Casebook) + "</h1>\n")
	fmt.Fprintf(&b, "<p>Image fingerprint <code>0x%08x</code>. The board prints this number at start-up (and reports it "+
		"in Modbus registers 2 and 3). If it differs, the board runs a different rulebook and this sheet does not "+
		"apply.</p>\n", s.Fingerprint)
	b.WriteString("<p>quorum has checked what these rules do in every combination of readings. It cannot check the " +
		"plant: that each reading comes from the right instrument, that each threshold is set where the document " +
		"says, and that the controller passes each one on. For each line, make that reading true <b>for real</b> " +
		"(operate the switch, simulate the signal at the transmitter), with the other readings at rest unless the " +
		"line says otherwise, and record the action quorum reports.</p>\n")
	b.WriteString("<p>With nothing set, quorum must report <b>" + e(s.Rest) + "</b>.</p>\n")
	b.WriteString("<table><tr><th>#</th><th>Reading</th><th>How it is set</th><th>Set it alone: expect</th>" +
		"<th>To see it matter</th><th>Seen</th><th class=sign>Initials</th><th class=sign>Date</th></tr>\n")
	for i, r := range s.Rows {
		alone := e(r.Alone)
		if r.Outcome != "Decided" {
			alone += " <small>(" + e(r.Outcome) + ", the fallback)</small>"
		}
		matter := "it changes nothing with up to three other readings: check it by hand"
		switch {
		case r.With == "alone":
			matter = "(the line before shows it)"
		case r.With != "-":
			matter = "with " + e(strings.ReplaceAll(r.With, "+", " + ")) + " set: <b>" + e(r.Then) + "</b>; without it: " +
				e(r.Without)
		}
		fmt.Fprintf(&b, "<tr><td>%d</td><td><b>%s</b><br><small>%s</small></td><td>%s</td><td><b>%s</b></td><td>%s</td>"+
			"<td></td><td></td><td></td></tr>\n", i+1, e(r.Name), e(r.Description), e(r.How), alone, matter)
	}
	b.WriteString("</table>\n<p>Rulebook: " + e(source) + ". Generated by quorum studio from the checked rulebook. " +
		"A line that does not match is a wiring, threshold or controller fault, or a rule that says something " +
		"other than intended; do not put the machine in service until each one is explained.</p>\n</body></html>\n")
	return b.String()
}

// Step is one state the bench test drives the board into.
type Step struct {
	Label  string `json:"label"`
	Mask   uint32 `json:"mask"`
	Expect string `json:"expect"`
	Got    string `json:"got"`
	Pass   bool   `json:"pass"`
}

// Steps lists the states to test: at rest, each reading alone, and each reading with the readings that make it
// matter, both with and without it.
func (s Sheet) Steps() ([]Step, error) {
	bit := map[string]int{}
	for _, r := range s.Rows {
		bit[r.Name] = r.Bit
	}
	steps := []Step{{Label: "nothing set", Mask: 0, Expect: s.Rest}}
	for _, r := range s.Rows {
		steps = append(steps, Step{Label: r.Name + " alone", Mask: 1 << uint(r.Bit), Expect: r.Alone})
		if r.With == "alone" || r.With == "-" {
			continue
		}
		var m uint32
		for _, n := range strings.Split(r.With, "+") {
			b, ok := bit[n]
			if !ok {
				return nil, fmt.Errorf("sheet row %s names an unknown reading %q", r.Name, n)
			}
			m |= 1 << uint(b)
		}
		with := strings.ReplaceAll(r.With, "+", " + ")
		steps = append(steps,
			Step{Label: r.Name + " with " + with, Mask: m | 1<<uint(r.Bit), Expect: r.Then},
			Step{Label: with + " without " + r.Name, Mask: m, Expect: r.Without})
	}
	return steps, nil
}
