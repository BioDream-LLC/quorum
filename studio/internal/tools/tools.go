// Package tools carries quorum's own command-line programs inside the studio binary and runs them.
//
// The studio does no reasoning of its own. Importing, validating and loading a casebook are done by the same cbc,
// hazop2cb, lopa2cb, fmea2cb and qblob that the C++ test suite exercises, so a casebook the studio accepts is one the
// tested toolchain accepted. The binaries are embedded at build time (see build.sh) and extracted once per version
// into the user's cache directory.
package tools

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"time"
)

// Result of one tool run.
type Result struct {
	Stdout, Stderr string
	ExitCode       int
}

var (
	once    sync.Once
	dir     string
	initErr error
)

func exe(name string) string {
	if runtime.GOOS == "windows" {
		return name + ".exe"
	}
	return name
}

// extract copies the embedded binaries for this OS into a directory named by their hash, so an upgrade never runs a
// stale tool and two versions never share one.
func extract() {
	sub, err := fs.Sub(bin, "bin/"+runtime.GOOS)
	if err != nil {
		initErr = err
		return
	}
	h := sha256.New()
	entries, err := fs.ReadDir(sub, ".")
	if err != nil || len(entries) == 0 {
		initErr = fmt.Errorf("tools: no embedded tools for %s; rebuild with build.sh", runtime.GOOS)
		return
	}
	files := map[string][]byte{}
	for _, e := range entries {
		b, err := fs.ReadFile(sub, e.Name())
		if err != nil {
			initErr = err
			return
		}
		files[e.Name()] = b
		h.Write([]byte(e.Name()))
		h.Write(b)
	}
	cache, err := os.UserCacheDir()
	if err != nil {
		cache = os.TempDir()
	}
	dir = filepath.Join(cache, "quorum-studio", hex.EncodeToString(h.Sum(nil))[:16])
	if err := os.MkdirAll(dir, 0o755); err != nil {
		initErr = err
		return
	}
	for name, b := range files {
		p := filepath.Join(dir, name)
		if old, err := os.ReadFile(p); err == nil && bytes.Equal(old, b) {
			continue
		}
		// A temporary file of its own, then an atomic rename: two studios (or two test binaries) starting at once
		// on a fresh machine used to write the same ".tmp" file, and one could run the other's half-written tool.
		f, err := os.CreateTemp(dir, name+".*.tmp")
		if err != nil {
			initErr = err
			return
		}
		_, werr := f.Write(b)
		cerr := f.Close()
		if werr == nil && cerr == nil {
			werr = os.Chmod(f.Name(), 0o755)
		}
		if werr == nil && cerr == nil {
			werr = os.Rename(f.Name(), p)
		}
		if werr != nil || cerr != nil {
			os.Remove(f.Name())
			if werr == nil {
				werr = cerr
			}
			initErr = werr
			return
		}
	}
}

// Run executes an embedded tool with a time limit. A non-zero exit is reported in Result, not as an error: cbc
// refusing a casebook is a normal, informative outcome.
func Run(ctx context.Context, name string, args ...string) (Result, error) {
	once.Do(extract)
	if initErr != nil {
		return Result{}, initErr
	}
	// Generous: checking a 30-reading rulebook takes about 2 minutes on 16 threads, and much longer on a small laptop.
	// The page can always cancel, because the request's context is the parent.
	ctx, cancel := context.WithTimeout(ctx, 2*time.Hour)
	defer cancel()
	cmd := exec.CommandContext(ctx, filepath.Join(dir, exe(name)), args...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	err := cmd.Run()
	r := Result{Stdout: out.String(), Stderr: errb.String()}
	if ee, ok := err.(*exec.ExitError); ok {
		r.ExitCode = ee.ExitCode()
		return r, nil
	}
	return r, err
}

// Firmware returns the embedded prebuilt runtime firmware for a board.
func Firmware(board, ext string) ([]byte, error) {
	b, err := fs.ReadFile(bin, "bin/firmware/quorum_runtime_"+board+ext)
	if err != nil {
		return nil, fmt.Errorf("tools: no runtime firmware for board %q", board)
	}
	return b, nil
}
