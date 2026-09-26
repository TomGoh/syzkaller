// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package rpcserver

import (
	"fmt"
	"os"
	"path/filepath"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// xhMagic is the little-endian uint64 of the ASCII bytes "XHCOV002".
// Same packing as the previous "XHCOV001" constant (0x313030564f434858):
// the first file byte is the least-significant byte.
const (
	xhMagic    = uint64(0x323030564f434858)
	xhVersion  = uint64(2)
	xhHdrBytes = 4096
)

// xhHeader is the defined prefix of the 4096-byte plugin header.
// The writer publishes little-endian fields; the manager host is little-endian,
// so this overlay matches the file layout. The rest of the page is reserved.
type xhHeader struct {
	magic       uint64
	version     uint64
	cpuIndex    uint64
	textStart   uint64
	granularity uint64
	nwords      uint64
}

// xhDrainer drains per-vCPU EL2 coverage bitmaps written by the QEMU
// TCG plugin at /dev/shm/xh<instance>.<vcpu>.
type xhDrainer struct {
	id    int
	vcpus []*xhVcpu
	// live is set once a mapping taken after QEMU's vcpu_init is in place.
	// CreateInstance runs before that open(O_TRUNC), so the first drain drops
	// any earlier mapping and attaches the live files.
	live bool
}

type xhVcpu struct {
	data        []byte
	textStart   uint64
	granularity uint64
	bitmap      []uint64
}

// openXHDrainer maps every valid /dev/shm/xh<id>.* bitmap.
// It returns nil when no file has a matching magic/version and a usable size.
func openXHDrainer(id int) *xhDrainer {
	d := &xhDrainer{id: id}
	if !d.attach() {
		return nil
	}
	return d
}

func (d *xhDrainer) attach() bool {
	matches, err := filepath.Glob(fmt.Sprintf("/dev/shm/xh%d.*", d.id))
	if err != nil || len(matches) == 0 {
		return false
	}
	for _, path := range matches {
		v := openXHVcpu(path)
		if v != nil {
			d.vcpus = append(d.vcpus, v)
		}
	}
	return len(d.vcpus) != 0
}

func openXHVcpu(path string) *xhVcpu {
	// PROT_WRITE|MAP_SHARED requires a writable fd; drain clears bits in place.
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil || fi.Size() < xhHdrBytes {
		return nil
	}
	data, err := syscall.Mmap(int(f.Fd()), 0, int(fi.Size()),
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}
	hdr := (*xhHeader)(unsafe.Pointer(&data[0]))
	if hdr.magic != xhMagic || hdr.version != xhVersion {
		syscall.Munmap(data)
		return nil
	}
	nwords := hdr.nwords
	if (uint64(len(data))-xhHdrBytes)/8 < nwords {
		syscall.Munmap(data)
		return nil
	}
	var bitmap []uint64
	if nwords > 0 {
		// Alias the mmap so atomic.SwapUint64 writes land in the shared file.
		bitmap = unsafe.Slice((*uint64)(unsafe.Pointer(&data[xhHdrBytes])), int(nwords))
	}
	return &xhVcpu{
		data:        data,
		textStart:   hdr.textStart,
		granularity: hdr.granularity,
		bitmap:      bitmap,
	}
}

// drain returns EL2 block-start virtual addresses whose bits were set since
// the previous drain. Each word is swapped to zero, which is the window edge.
func (d *xhDrainer) drain() []uint64 {
	if d == nil {
		return nil
	}
	if !d.live {
		d.detach()
		if !d.attach() {
			return nil
		}
		d.live = true
	}
	var pcs []uint64
	for _, v := range d.vcpus {
		for w := range v.bitmap {
			old := atomic.SwapUint64(&v.bitmap[w], 0)
			if old == 0 {
				continue
			}
			for b := 0; b < 64; b++ {
				if old&(uint64(1)<<uint(b)) == 0 {
					continue
				}
				k := uint64(w)*64 + uint64(b)
				pcs = append(pcs, v.textStart+k*v.granularity)
			}
		}
	}
	return pcs
}

func (d *xhDrainer) close() {
	if d == nil {
		return
	}
	d.detach()
	d.live = false
}

func (d *xhDrainer) detach() {
	for _, v := range d.vcpus {
		if v.data != nil {
			syscall.Munmap(v.data)
			v.data = nil
		}
	}
	d.vcpus = nil
}
