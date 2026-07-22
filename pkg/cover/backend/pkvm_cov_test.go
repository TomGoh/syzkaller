// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package backend

import (
	"debug/elf"
	"os"
	"strings"
	"testing"

	"github.com/google/syzkaller/pkg/symbolizer"
	"github.com/google/syzkaller/sys/targets"
)

// pkvmCovRuntimeToLink mirrors the kernel host helper pkvm_cov_runtime_to_link()
// (arch/arm64/kvm/pkvm_cov.c). It reverses the nVHE hyp-VA transform for a PC
// inside the contiguous hyp .text, given the runtime base (hypStart) and the link
// anchors. Kept in lockstep with the C helper: the kernel computes
//
//	hypStart = kern_hyp_va(lm_alias(__hyp_text_start))
//
// at boot; this test supplies a synthetic hypStart, so it exercises the reversal
// arithmetic and the range check without a running EL2. It deliberately does NOT
// re-encode the lm_alias() step (that is the kernel's job and is validated by
// matching hyp_events.c) — only the anchor reversal the consumer depends on.
func pkvmCovRuntimeToLink(pc, linkStart, hypStart, textSize uint64) uint64 {
	if pc < hypStart || pc-hypStart >= textSize {
		return 0 // outside hyp .text — caller drops it
	}
	return linkStart + (pc - hypStart)
}

// TestPkvmCovSymbolizePipeline is the Stage-2 synthetic consumer test. It feeds a
// known Rust EL2 __kvm_nvhe_ PC through the runtime->link reversal and syzkaller's
// real symbolizer, expecting a rust/src/.../*.rs:line frame — closing the loop
// from "elf.go recognizes the callback name" to "a real EL2 PC becomes a Rust
// source line" with no board.
//
// Opt-in: set PKVM_VMLINUX to a debuginfo vmlinux built with CONFIG_PKVM_EL2_COV
// (Rust hyp compiled -C debuginfo=2), else skipped — the ~480MB artifact is not
// shipped with the tree.
func TestPkvmCovSymbolizePipeline(t *testing.T) {
	vmlinux := os.Getenv("PKVM_VMLINUX")
	if vmlinux == "" {
		t.Skip("set PKVM_VMLINUX to a CONFIG_PKVM_EL2_COV debuginfo vmlinux")
	}
	syms := readElfSyms(t, vmlinux,
		"__hyp_text_start", "__hyp_text_end", "__kvm_nvhe___pkvm_init_vm")
	linkStart := syms["__hyp_text_start"]
	linkEnd := syms["__hyp_text_end"]
	target := syms["__kvm_nvhe___pkvm_init_vm"] // a Rust EL2 handler inside hyp .text
	textSize := linkEnd - linkStart

	if target < linkStart || target >= linkEnd {
		t.Fatalf("target %#x not inside hyp .text [%#x,%#x)", target, linkStart, linkEnd)
	}

	// Simulate __kern_hyp_va: a constant offset applied to the whole hyp .text
	// block. Any constant works — the reversal must be exact and its inverse.
	const synthTag = 0x0000_0e00_0000_0000
	hypStart := linkStart + synthTag
	runtimePC := hypStart + (target - linkStart)

	// 1. Reversal recovers the exact link address.
	if got := pkvmCovRuntimeToLink(runtimePC, linkStart, hypStart, textSize); got != target {
		t.Fatalf("reversal: got %#x, want %#x", got, target)
	}
	// 2. Range check rejects PCs outside the hyp .text (below start; at/after end).
	if got := pkvmCovRuntimeToLink(hypStart-1, linkStart, hypStart, textSize); got != 0 {
		t.Fatalf("below-range PC not rejected: got %#x", got)
	}
	if got := pkvmCovRuntimeToLink(hypStart+textSize, linkStart, hypStart, textSize); got != 0 {
		t.Fatalf("at-end PC not rejected: got %#x", got)
	}
	// 3. The recovered link address symbolizes to Rust EL2 source via syzkaller's
	//    real addr2line path — the same consumer used for production coverage.
	symb := symbolizer.Make(targets.Get(targets.Linux, targets.ARM64))
	defer symb.Close()
	frames, err := symb.Symbolize(vmlinux, target)
	if err != nil {
		t.Fatalf("symbolize %#x: %v", target, err)
	}
	if len(frames) == 0 {
		t.Fatalf("no frames for %#x", target)
	}
	last := frames[len(frames)-1]
	if !strings.HasSuffix(last.File, ".rs") {
		t.Fatalf("expected a Rust .rs frame, got %q:%d", last.File, last.Line)
	}
	t.Logf("EL2 runtime PC %#x -> link %#x -> %s:%d", runtimePC, target, last.File, last.Line)
}

// readElfSyms returns the values of the named symbols from an ELF file, failing
// the test if any is missing.
func readElfSyms(t *testing.T, bin string, names ...string) map[string]uint64 {
	t.Helper()
	f, err := elf.Open(bin)
	if err != nil {
		t.Fatalf("open %v: %v", bin, err)
	}
	defer f.Close()
	all, err := f.Symbols()
	if err != nil {
		t.Fatalf("read symbols: %v", err)
	}
	want := make(map[string]bool, len(names))
	for _, n := range names {
		want[n] = true
	}
	out := make(map[string]uint64, len(names))
	for _, s := range all {
		if want[s.Name] {
			out[s.Name] = s.Value
		}
	}
	for _, n := range names {
		if _, ok := out[n]; !ok {
			t.Fatalf("symbol %q not found in %v", n, bin)
		}
	}
	return out
}
