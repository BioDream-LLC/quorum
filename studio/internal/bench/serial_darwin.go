package bench

import "syscall"

const (
	ioctlGet = syscall.TIOCGETA
	ioctlSet = syscall.TIOCSETA
)

func setSpeed(t *syscall.Termios) { t.Ispeed, t.Ospeed = 115200, 115200 }
