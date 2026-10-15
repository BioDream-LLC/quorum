package bench

import (
	"strings"
	"syscall"
	"unsafe"
)

var (
	kernel32           = syscall.NewLazyDLL("kernel32.dll")
	procGetCommState   = kernel32.NewProc("GetCommState")
	procSetCommState   = kernel32.NewProc("SetCommState")
	procSetCommTimeout = kernel32.NewProc("SetCommTimeouts")
)

// dcb is the Win32 DCB structure.
type dcb struct {
	DCBlength  uint32
	BaudRate   uint32
	Flags      uint32
	wReserved  uint16
	XonLim     uint16
	XoffLim    uint16
	ByteSize   byte
	Parity     byte
	StopBits   byte
	XonChar    byte
	XoffChar   byte
	ErrorChar  byte
	EofChar    byte
	EvtChar    byte
	wReserved1 uint16
}

type commTimeouts struct {
	ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant uint32
	WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant                    uint32
}

type winPort struct{ h syscall.Handle }

// openPort opens COMn at 115200 8N1 with DTR and RTS on (a Pico's USB serial only talks to a host that raises DTR),
// and reads that return what has arrived or nothing after 0.1 s.
func openPort(name string) (Port, error) {
	if !strings.HasPrefix(name, `\\.\`) {
		name = `\\.\` + name // needed for COM10 and above, harmless below
	}
	p, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	h, err := syscall.CreateFile(p, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		return nil, err
	}
	var d dcb
	d.DCBlength = uint32(unsafe.Sizeof(d))
	if r, _, e := procGetCommState.Call(uintptr(h), uintptr(unsafe.Pointer(&d))); r == 0 {
		syscall.CloseHandle(h)
		return nil, e
	}
	d.BaudRate = 115200
	d.Flags = 0x0001 | 0x0010 | 0x1000 // fBinary, DTR_CONTROL_ENABLE, RTS_CONTROL_ENABLE; no flow control
	d.ByteSize, d.Parity, d.StopBits = 8, 0, 0
	if r, _, e := procSetCommState.Call(uintptr(h), uintptr(unsafe.Pointer(&d))); r == 0 {
		syscall.CloseHandle(h)
		return nil, e
	}
	t := commTimeouts{ReadIntervalTimeout: 0xFFFFFFFF, ReadTotalTimeoutMultiplier: 0xFFFFFFFF,
		ReadTotalTimeoutConstant: 100, WriteTotalTimeoutConstant: 1000}
	if r, _, e := procSetCommTimeout.Call(uintptr(h), uintptr(unsafe.Pointer(&t))); r == 0 {
		syscall.CloseHandle(h)
		return nil, e
	}
	return &winPort{h: h}, nil
}

func (p *winPort) Read(b []byte) (int, error) {
	var n uint32
	err := syscall.ReadFile(p.h, b, &n, nil)
	return int(n), err
}

func (p *winPort) Write(b []byte) (int, error) {
	var n uint32
	err := syscall.WriteFile(p.h, b, &n, nil)
	return int(n), err
}

func (p *winPort) Close() error { return syscall.CloseHandle(p.h) }
