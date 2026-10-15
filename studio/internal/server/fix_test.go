package server

import (
	"net/http"
	"testing"
)

// A rulebook nothing can argue with is refused, the check offers the repair, and the repaired text passes.
func TestUnarguableRulebookOffersTheSensorRepair(t *testing.T) {
	s, ln, err := Listen()
	if err != nil {
		t.Fatal(err)
	}
	ln.Close()
	h := s.Handler()
	src := `casebook plain v1
source "a regulation" dated 2026-10-14 by "tests"
observe overspeed "engine overspeed"
observe oil_low "lube oil pressure low"
claim overspeed_fault "overspeed" needs overspeed
claim oil_alarm "low oil pressure" needs oil_low
action keep_running idle
action raise_alarm when oil_alarm
action shut_down safe when overspeed_fault
prefer shut_down over raise_alarm over keep_running
`
	code, c := call(t, h, s, "POST", "/api/check", map[string]any{"source": src, "name": "plain"})
	if code != http.StatusOK || c["ok"] != false || c["fix"] != "sensors" {
		t.Fatalf("check: %d ok=%v fix=%v", code, c["ok"], c["fix"])
	}
	code, f := call(t, h, s, "POST", "/api/fix/sensors", map[string]any{"source": src})
	if code != http.StatusOK {
		t.Fatalf("fix: %d %v", code, f)
	}
	fixed, _ := f["source"].(string)
	code, c = call(t, h, s, "POST", "/api/check", map[string]any{"source": fixed, "name": "plain"})
	if code != http.StatusOK || c["ok"] != true {
		t.Fatalf("repaired rulebook refused: %d\n%v\n%s", code, c["output"], fixed)
	}
	if c["fix"] != nil {
		t.Error("a passing rulebook must not offer the repair")
	}
}
