//go:build !windows && !darwin

package tools

import "embed"

// Only this OS's tools are embedded, so the Linux build does not carry the Windows binaries.
//
//go:embed bin/linux bin/firmware
var bin embed.FS
