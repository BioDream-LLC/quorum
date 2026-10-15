package fix

import (
	"strings"
	"testing"
)

// A regulation-style rulebook: two shutdown readings, one alarm-only reading, nothing that can argue with any claim.
const plain = `casebook gen v1
source "test" dated 2026-10-14 by "tests"
observe overspeed "engine overspeed"
observe oil_low "lube oil pressure low"
observe diesel "the prime mover is a diesel"
claim overspeed_fault "overspeed" severity 9 needs overspeed
claim oil_alarm "low oil pressure on a diesel" needs oil_low diesel
action keep_running idle
action raise_alarm when oil_alarm
action shut_down safe
  when any overspeed_fault
prefer shut_down over raise_alarm over keep_running
`

func TestSensorChecksFailToTheResponseTheReadingLedTo(t *testing.T) {
	r, err := SensorChecks(plain)
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, a := range r.Added {
		got[a.Reading] = a.Action
	}
	want := map[string]string{"overspeed": "shut_down", "oil_low": "raise_alarm", "diesel": "raise_alarm"}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("%s: response %q, want %q (all: %v)", k, got[k], v, got)
		}
	}
	for _, line := range []string{
		"action raise_alarm when any oil_alarm oil_low_blind diesel_blind",
		"action shut_down safe when any overspeed_fault overspeed_blind",
		`doubt overspeed_untrusted "overspeed is not trusted while its instrument has failed" needs overspeed_failed`,
		"overspeed_untrusted undercuts overspeed_fault",
		"diesel_untrusted undercuts oil_alarm",
		"claim oil_low_blind",
	} {
		if !strings.Contains(r.Source, line) {
			t.Errorf("missing %q in\n%s", line, r.Source)
		}
	}
	if strings.Contains(r.Source, "\n  when any overspeed_fault\n") {
		t.Error("the continuation line of the rewritten action was left behind")
	}
	if n := strings.Count(r.Source, "# TODO"); n != 3 {
		t.Errorf("%d TODO lines, want one per added reading", n)
	}
}

func TestSensorChecksLeaveQuestionedAndAndOnlyReadingsAlone(t *testing.T) {
	src := `casebook gen v1
observe a "a"
observe b "b"
observe a_dead "a sensor dead"
doubt a_doubt "a is dead" needs a_dead undermines a
claim both "a and b" needs a b
action run idle
action stop safe when all both
`
	r, err := SensorChecks(src)
	if err == nil {
		t.Fatalf("expected nothing to be added, got %v", r.Added)
	}
	if len(r.Skipped) != 2 || !strings.HasPrefix(r.Skipped[0], "a:") || !strings.HasPrefix(r.Skipped[1], "b:") {
		t.Errorf("skipped: %v", r.Skipped)
	}
}
