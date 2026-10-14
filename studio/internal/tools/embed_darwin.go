//go:build darwin

package tools

import "embed"

// Only this OS's tools are embedded. The macOS tools are universal (arm64 and x86_64), built on a Mac by
// build-macos.sh, so one set serves both Go architectures.
//
//go:embed bin/darwin bin/firmware
var bin embed.FS
