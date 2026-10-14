//go:build windows

package tools

import "embed"

// Only this OS's tools are embedded, so the Windows build does not carry the Linux binaries.
//
//go:embed bin/windows bin/firmware
var bin embed.FS
