package bench

import "syscall"

const (
	ioctlGet = syscall.TCGETS
	ioctlSet = syscall.TCSETS
)

func setSpeed(t *syscall.Termios) {
	t.Cflag = t.Cflag&^0x100f | syscall.B115200 // 0x100f is CBAUD, which package syscall does not export
	t.Ispeed, t.Ospeed = syscall.B115200, syscall.B115200
}
