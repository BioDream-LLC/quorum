// Package web embeds the studio's page and example casebooks.
package web

import "embed"

//go:embed index.html examples
var FS embed.FS
