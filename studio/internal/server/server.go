// Package server is the studio's local web interface.
//
// It listens on 127.0.0.1 only, on a random port, and every request must carry a random per-launch token: nothing
// else on the machine or the network can drive it. It also checks the Host header, so a web page the user happens to
// visit cannot reach it through DNS rebinding.
package server

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	"quorum/studio/internal/bench"
	"quorum/studio/internal/fix"
	"quorum/studio/internal/pipeline"
)

// Web holds index.html and the bundled example casebooks; set by main from the web package.
var Web embed.FS

type image struct {
	name  string
	board string
	uf2   []byte // the programming file: a UF2 for the Pico 2, a flat .bin for the Uno R4
	sheet string // cbc's commissioning sheet; empty for a palette
	src   string // the rulebook's file name, printed on the sheet
}

// Server is one running studio.
type Server struct {
	token string
	addr  string

	mu     sync.Mutex
	images map[string]image
	drafts drafting
}

func newID() (string, error) {
	b := make([]byte, 12)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// Listen opens a loopback listener on a random port.
func Listen() (*Server, net.Listener, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, nil, err
	}
	t := make([]byte, 24)
	if _, err := rand.Read(t); err != nil {
		return nil, nil, err
	}
	return &Server{token: hex.EncodeToString(t), addr: ln.Addr().String(), images: map[string]image{}}, ln, nil
}

// URL is the address to open, token included.
func (s *Server) URL() string { return "http://" + s.addr + "/?t=" + s.token }

func (s *Server) authorised(r *http.Request) bool {
	if r.Host != s.addr {
		return false
	}
	got := r.Header.Get("X-Quorum-Token")
	if got == "" {
		got = r.URL.Query().Get("t")
	}
	return subtle.ConstantTimeCompare([]byte(got), []byte(s.token)) == 1
}

// Handler routes every request through the token check.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", s.index)
	mux.HandleFunc("GET /api/examples", s.examples)
	mux.HandleFunc("GET /api/examples/{name}", s.example)
	mux.HandleFunc("POST /api/import", s.importWorksheet)
	mux.HandleFunc("POST /api/check", s.check)
	mux.HandleFunc("POST /api/learn", s.learn)
	mux.HandleFunc("POST /api/fix/sensors", s.fixSensors)
	mux.HandleFunc("POST /api/draft", s.draftStart)
	mux.HandleFunc("GET /api/draft/{id}", s.draftStatus)
	mux.HandleFunc("POST /api/draft/{id}/cancel", s.draftCancel)
	mux.HandleFunc("POST /api/approve", s.draftApprove)
	mux.HandleFunc("GET /api/download/{id}", s.download)
	mux.HandleFunc("POST /api/flash/{id}", s.flash)
	mux.HandleFunc("GET /api/sheet/{id}", s.sheet)
	mux.HandleFunc("POST /api/bench/{id}", s.bench)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.authorised(r) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
		r.Body = http.MaxBytesReader(w, r.Body, 16<<20)
		mux.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, code int, err error) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
}

func (s *Server) index(w http.ResponseWriter, r *http.Request) {
	b, err := fs.ReadFile(Web, "index.html")
	if err != nil {
		fail(w, http.StatusInternalServerError, err)
		return
	}
	page := strings.Replace(string(b), "{{TOKEN}}", s.token, 1)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = w.Write([]byte(page))
}

func (s *Server) examples(w http.ResponseWriter, r *http.Request) {
	entries, _ := fs.ReadDir(Web, "examples")
	names := []string{}
	for _, e := range entries {
		names = append(names, e.Name())
	}
	writeJSON(w, names)
}

func (s *Server) example(w http.ResponseWriter, r *http.Request) {
	name := filepath.Base(r.PathValue("name"))
	b, err := fs.ReadFile(Web, "examples/"+name)
	if err != nil {
		fail(w, http.StatusNotFound, err)
		return
	}
	writeJSON(w, map[string]string{"name": name, "source": string(b)})
}

func (s *Server) importWorksheet(w http.ResponseWriter, r *http.Request) {
	var req struct{ Kind, Name, Content string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	d, err := pipeline.Import(r.Context(), req.Kind, req.Name, []byte(req.Content))
	if err != nil {
		fail(w, http.StatusUnprocessableEntity, err)
		return
	}
	writeJSON(w, d)
}

func (s *Server) learn(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name, Content string
		pipeline.LearnOptions
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	l, err := pipeline.Learn(r.Context(), req.Name, []byte(req.Content), req.LearnOptions)
	if err != nil {
		fail(w, http.StatusUnprocessableEntity, err)
		return
	}
	writeJSON(w, l)
}

func (s *Server) check(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Source, Name, Board string
		Extras              []pipeline.Extra
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	if len(req.Extras) > 3 {
		fail(w, http.StatusBadRequest, errors.New("at most 4 casebooks fit on one chip"))
		return
	}
	c, err := pipeline.Check(r.Context(), req.Source, req.Extras...)
	if err != nil {
		fail(w, http.StatusInternalServerError, err)
		return
	}
	resp := map[string]any{"ok": c.OK, "output": c.Output, "info": c.Info, "sweep": c.Sweep}
	if !c.OK && strings.Contains(c.Output, fix.Refusal) && len(req.Extras) == 0 {
		resp["fix"] = "sensors" // the page offers the one-click repair
	}
	if c.OK {
		board := req.Board
		if board == "" {
			board = "pico2"
		}
		u, err := pipeline.Image(board, c.Blob)
		if err != nil {
			fail(w, http.StatusInternalServerError, err)
			return
		}
		id := make([]byte, 8)
		_, _ = rand.Read(id)
		key := hex.EncodeToString(id)
		name := pipeline.CasebookName(req.Name) + "-" + board + pipeline.Extension(board)
		s.mu.Lock()
		s.images[key] = image{name: name, board: board, uf2: u, sheet: c.Sheet, src: req.Name}
		s.mu.Unlock()
		resp["image"] = key
		resp["file"] = name
		resp["bytes"] = len(u)
		resp["sheet"] = c.Sheet != ""
	}
	writeJSON(w, resp)
}

// fixSensors answers cbc's refusal of a rulebook nothing can argue with: it returns the source with a failed-sensor
// check for each reading, every one marked TODO for the engineer. Nothing is saved or checked here; the page puts the
// text in the editor and the person checks it again.
func (s *Server) fixSensors(w http.ResponseWriter, r *http.Request) {
	var req struct{ Source string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	res, err := fix.SensorChecks(req.Source)
	if err != nil {
		fail(w, http.StatusUnprocessableEntity, err)
		return
	}
	writeJSON(w, res)
}

// sheetOf parses the commissioning sheet of a checked image.
func (s *Server) sheetOf(w http.ResponseWriter, id string) (image, bench.Sheet, bool) {
	im, ok := s.lookup(id)
	if !ok {
		fail(w, http.StatusNotFound, errors.New("no such image"))
		return im, bench.Sheet{}, false
	}
	if im.sheet == "" {
		fail(w, http.StatusUnprocessableEntity, errors.New("several casebooks share this chip: check each on its own "+
			"for its commissioning sheet"))
		return im, bench.Sheet{}, false
	}
	sh, err := bench.Parse(im.sheet)
	if err != nil {
		fail(w, http.StatusInternalServerError, err)
		return im, bench.Sheet{}, false
	}
	return im, sh, true
}

// sheet serves the printable loop-check sheet for the real plant.
func (s *Server) sheet(w http.ResponseWriter, r *http.Request) {
	im, sh, ok := s.sheetOf(w, r.PathValue("id"))
	if !ok {
		return
	}
	name := pipeline.CasebookName(im.src) + "-commissioning.html"
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", name))
	_, _ = w.Write([]byte(sh.HTML(filepath.Base(im.src))))
}

// bench drives a board on a serial port through the sheet over Modbus and reports each answer.
func (s *Server) bench(w http.ResponseWriter, r *http.Request) {
	var req struct{ Port string }
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		fail(w, http.StatusBadRequest, err)
		return
	}
	req.Port = strings.TrimSpace(req.Port)
	if req.Port == "" {
		fail(w, http.StatusBadRequest, errors.New("give the board's serial port, for example COM4 or /dev/ttyACM0"))
		return
	}
	_, sh, ok := s.sheetOf(w, r.PathValue("id"))
	if !ok {
		return
	}
	res, err := bench.Run(r.Context(), req.Port, sh)
	if err != nil {
		fail(w, http.StatusUnprocessableEntity, err)
		return
	}
	writeJSON(w, res)
}

func (s *Server) lookup(id string) (image, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	im, ok := s.images[id]
	return im, ok
}

func (s *Server) download(w http.ResponseWriter, r *http.Request) {
	im, ok := s.lookup(r.PathValue("id"))
	if !ok {
		fail(w, http.StatusNotFound, errors.New("no such image"))
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", im.name))
	_, _ = w.Write(im.uf2)
}

// findPico returns the root of a mounted RP2350 bootloader drive, identified by its INFO_UF2.TXT.
func findPico() (string, error) {
	var roots []string
	if runtime.GOOS == "windows" {
		for c := 'D'; c <= 'Z'; c++ {
			roots = append(roots, string(c)+`:\`)
		}
	} else if runtime.GOOS == "darwin" {
		roots, _ = filepath.Glob("/Volumes/*")
	} else {
		for _, base := range []string{"/media", "/run/media", "/mnt"} {
			matches, _ := filepath.Glob(filepath.Join(base, "*", "*"))
			roots = append(roots, matches...)
			more, _ := filepath.Glob(filepath.Join(base, "*"))
			roots = append(roots, more...)
		}
	}
	for _, root := range roots {
		b, err := os.ReadFile(filepath.Join(root, "INFO_UF2.TXT"))
		if err == nil && strings.Contains(string(b), "RP2350") {
			return root, nil
		}
	}
	return "", errors.New("no Pico 2 found. Hold BOOTSEL while plugging it in, so it appears as a drive, then try again")
}

func (s *Server) flash(w http.ResponseWriter, r *http.Request) {
	im, ok := s.lookup(r.PathValue("id"))
	if !ok {
		fail(w, http.StatusNotFound, errors.New("no such image"))
		return
	}
	if im.board == "unor4" {
		out, err := flashUno(r.Context(), im)
		if err != nil {
			fail(w, http.StatusInternalServerError, err)
			return
		}
		writeJSON(w, map[string]string{"drive": "the Uno R4 (DFU)", "log": out})
		return
	}
	root, err := findPico()
	if err != nil {
		fail(w, http.StatusNotFound, err)
		return
	}
	if err := os.WriteFile(filepath.Join(root, im.name), im.uf2, 0o644); err != nil {
		fail(w, http.StatusInternalServerError, err)
		return
	}
	writeJSON(w, map[string]string{"drive": root})
}

// findDfuUtil locates the dfu-util that the Arduino IDE or arduino-cli installs with the Uno R4 board package. The
// studio does not ship dfu-util itself.
func findDfuUtil() (string, error) {
	var roots []string
	if runtime.GOOS == "windows" {
		roots = append(roots, filepath.Join(os.Getenv("LOCALAPPDATA"), "Arduino15"))
	} else if home, err := os.UserHomeDir(); err == nil {
		// The Arduino IDE keeps its packages in ~/Library/Arduino15 on macOS and ~/.arduino15 on Linux.
		roots = append(roots, filepath.Join(home, "Library", "Arduino15"), filepath.Join(home, ".arduino15"))
	}
	name := "dfu-util"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	for _, r := range roots {
		m, _ := filepath.Glob(filepath.Join(r, "packages", "arduino", "tools", "dfu-util", "*", name))
		if len(m) > 0 {
			return m[len(m)-1], nil
		}
	}
	return "", errors.New("dfu-util not found. Install the \"Arduino UNO R4 Boards\" package in the Arduino IDE once, " +
		"or download the image and upload it with arduino-cli")
}

// flashUno writes the image with the same dfu-util command the Arduino board package uses for the Minima. The board
// exposes a DFU runtime interface, so dfu-util switches it into the bootloader itself.
func flashUno(ctx context.Context, im image) (string, error) {
	dfu, err := findDfuUtil()
	if err != nil {
		return "", err
	}
	f, err := os.CreateTemp("", "quorum-*.bin")
	if err != nil {
		return "", err
	}
	defer os.Remove(f.Name())
	if _, err := f.Write(im.uf2); err != nil {
		f.Close()
		return "", err
	}
	f.Close()
	out, err := exec.CommandContext(ctx, dfu, "--device", "0x2341:0x0069,:0x0369", "-D", f.Name(), "-a0", "-Q").CombinedOutput()
	if err != nil {
		return string(out), fmt.Errorf("dfu-util failed: %v\n%s", err, out)
	}
	return string(out), nil
}

// Serve runs until ctx ends.
func (s *Server) Serve(ctx context.Context, ln net.Listener) error {
	srv := &http.Server{Handler: s.Handler()}
	go func() {
		<-ctx.Done()
		_ = srv.Close()
	}()
	err := srv.Serve(ln)
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}
