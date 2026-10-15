//go:build linux || darwin

package bench

import (
	"errors"
	"strings"
	"syscall"
	"unsafe"
)

type unixPort struct{ fd int }

// openPort opens a serial device in raw mode, 115200 8N1 (a USB CDC port ignores the speed), with reads that return
// what has arrived or nothing after 0.1 s.
func openPort(name string) (Port, error) {
	if !strings.HasPrefix(name, "/dev/") {
		return nil, errors.New("a serial port is a device under /dev, such as /dev/ttyACM0 or /dev/cu.usbmodem1101")
	}
	fd, err := syscall.Open(name, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	var t syscall.Termios
	if err := ioctl(fd, ioctlGet, unsafe.Pointer(&t)); err != nil {
		syscall.Close(fd)
		return nil, err
	}
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP | syscall.INLCR | syscall.IGNCR |
		syscall.ICRNL | syscall.IXON | syscall.IXOFF
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.CSTOPB
	t.Cflag |= syscall.CS8 | syscall.CREAD | syscall.CLOCAL
	t.Cc[syscall.VMIN] = 0
	t.Cc[syscall.VTIME] = 1
	setSpeed(&t)
	if err := ioctl(fd, ioctlSet, unsafe.Pointer(&t)); err != nil {
		syscall.Close(fd)
		return nil, err
	}
	return &unixPort{fd: fd}, nil
}

func ioctl(fd int, req uintptr, arg unsafe.Pointer) error {
	if _, _, e := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(arg)); e != 0 {
		return e
	}
	return nil
}

func (p *unixPort) Read(b []byte) (int, error) {
	for {
		n, err := syscall.Read(p.fd, b)
		if err == syscall.EINTR {
			continue
		}
		if n < 0 {
			n = 0
		}
		return n, err
	}
}

func (p *unixPort) Write(b []byte) (int, error) {
	total := 0
	for total < len(b) {
		n, err := syscall.Write(p.fd, b[total:])
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return total, err
		}
		total += n
	}
	return total, nil
}

func (p *unixPort) Close() error { return syscall.Close(p.fd) }
