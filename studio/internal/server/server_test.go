package server

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestOnlyTheTokenHolderGetsIn(t *testing.T) {
	s, ln, err := Listen()
	if err != nil {
		t.Fatal(err)
	}
	ln.Close()
	h := s.Handler()
	req := func(host, path, hdr string) int {
		r := httptest.NewRequest("GET", path, nil)
		r.Host = host
		if hdr != "" {
			r.Header.Set("X-Quorum-Token", hdr)
		}
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w.Code
	}
	if c := req(s.addr, "/api/examples", ""); c != http.StatusForbidden {
		t.Errorf("no token: %d", c)
	}
	if c := req(s.addr, "/api/examples", "wrong"); c != http.StatusForbidden {
		t.Errorf("wrong token: %d", c)
	}
	if c := req("evil.example:80", "/api/examples", s.token); c != http.StatusForbidden {
		t.Errorf("rebound Host header: %d", c)
	}
	if c := req(s.addr, "/api/examples", s.token); c != http.StatusOK {
		t.Errorf("correct token: %d", c)
	}
	if c := req(s.addr, "/api/download/nope?t="+s.token, ""); c != http.StatusNotFound {
		t.Errorf("unknown image: %d", c)
	}
}
