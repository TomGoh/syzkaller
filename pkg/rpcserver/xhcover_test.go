// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package rpcserver

import (
	"os"
	"syscall"
	"testing"
	"unsafe"
)

func TestXHDrainerBitmap(t *testing.T) {
	id := 800000 + os.Getpid()
	path := shmPath(id, 0)
	defer os.Remove(path)
	os.Remove(path)

	if openXHDrainer(id) != nil {
		t.Fatal("openXHDrainer found a bitmap before the file existed")
	}

	// Placeholder used when CreateInstance runs before QEMU creates the files.
	d := &xhDrainer{id: id}
	if pcs := d.drain(); len(pcs) != 0 {
		t.Fatalf("drain before the file exists: %v", pcs)
	}

	const (
		textStart   = uint64(0x80000000)
		granularity = uint64(4)
		nwords      = uint64(2)
	)
	// Bit k covers text_start + k*granularity. Include bit 63 and a second word.
	writeXHBitmap(t, path, textStart, granularity, nwords, []uint64{0, 3, 63, 65})

	pcs := d.drain()
	want := []uint64{
		textStart + 0*granularity,
		textStart + 3*granularity,
		textStart + 63*granularity,
		textStart + 65*granularity,
	}
	if !sameU64(pcs, want) {
		t.Fatalf("first drain: got %x want %x", pcs, want)
	}
	if pcs = d.drain(); len(pcs) != 0 {
		t.Fatalf("second drain: got %x", pcs)
	}
	if !bitmapCleared(t, path, nwords) {
		t.Fatal("bitmap bits still set in the shared file")
	}
	d.close()
}

func TestXHDrainerSkipsBadFiles(t *testing.T) {
	id := 810000 + os.Getpid()
	good := shmPath(id, 0)
	badMagic := shmPath(id, 1)
	shortPath := shmPath(id, 2)
	truncated := shmPath(id, 3)
	defer os.Remove(good)
	defer os.Remove(badMagic)
	defer os.Remove(shortPath)
	defer os.Remove(truncated)

	const textStart = uint64(0x1000)
	writeXHBitmap(t, good, textStart, 4, 1, []uint64{0})

	buf := make([]byte, xhHdrBytes+8)
	hdr := (*xhHeader)(unsafe.Pointer(&buf[0]))
	hdr.magic = 0xdead
	hdr.version = xhVersion
	hdr.textStart = textStart
	hdr.granularity = 4
	hdr.nwords = 1
	if err := os.WriteFile(badMagic, buf, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(shortPath, []byte("short"), 0o644); err != nil {
		t.Fatal(err)
	}
	// Magic matches, but the file is smaller than 4096 + nwords*8.
	shortHdr := make([]byte, xhHdrBytes+8)
	sh := (*xhHeader)(unsafe.Pointer(&shortHdr[0]))
	sh.magic = xhMagic
	sh.version = xhVersion
	sh.granularity = 4
	sh.nwords = 100
	if err := os.WriteFile(truncated, shortHdr, 0o644); err != nil {
		t.Fatal(err)
	}

	d := openXHDrainer(id)
	if d == nil {
		t.Fatal("expected the valid bitmap")
	}
	defer d.close()
	pcs := d.drain()
	if !sameU64(pcs, []uint64{textStart}) {
		t.Fatalf("got %x", pcs)
	}
	if pcs = d.drain(); len(pcs) != 0 {
		t.Fatalf("second drain: got %x", pcs)
	}
	if len(d.vcpus) != 1 {
		t.Fatalf("vcpus: %d", len(d.vcpus))
	}
}

func shmPath(id, vcpu int) string {
	return "/dev/shm/xh" + itoa(id) + "." + itoa(vcpu)
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [16]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}

func writeXHBitmap(t *testing.T, path string, textStart, granularity, nwords uint64, bits []uint64) {
	t.Helper()
	data := make([]byte, xhHdrBytes+int(nwords)*8)
	hdr := (*xhHeader)(unsafe.Pointer(&data[0]))
	hdr.magic = xhMagic
	hdr.version = xhVersion
	hdr.textStart = textStart
	hdr.granularity = granularity
	hdr.nwords = nwords
	bm := unsafe.Slice((*uint64)(unsafe.Pointer(&data[xhHdrBytes])), int(nwords))
	for _, k := range bits {
		bm[k/64] |= uint64(1) << (k % 64)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func bitmapCleared(t *testing.T, path string, nwords uint64) bool {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil {
		t.Fatal(err)
	}
	data, err := syscall.Mmap(int(f.Fd()), 0, int(fi.Size()), syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Munmap(data)
	bm := unsafe.Slice((*uint64)(unsafe.Pointer(&data[xhHdrBytes])), int(nwords))
	for _, w := range bm {
		if w != 0 {
			return false
		}
	}
	return true
}

func sameU64(got, want []uint64) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}
