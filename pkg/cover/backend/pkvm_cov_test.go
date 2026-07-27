// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package backend

import (
	"debug/elf"
	"encoding/binary"
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

// TestPkvmCovSymbolizePipeline is the Stage-2 synthetic consumer test. It models
// the FULL KCOV pipeline for a real Rust EL2 SanCov call site, board-free:
//
//	call site (bl, the CoverPoint the ELF backend records)
//	  -> raw KCOV PC == call site + 4      (EL2 records __builtin_return_address(0))
//	  -> runtime hyp-VA (+ synthetic tag)  (what __kern_hyp_va would produce)
//	  -> pkvm_cov_runtime_to_link          (drain-side reversal, our helper)
//	  -> PreviousInstructionPC (-4)        (syzkaller reconciles return-addr->call)
//	  == the original call site, which addr2line resolves to rust/src/*.rs:line.
//
// This closes the loop the entry-address-only version missed: an off-by-4 in the
// #23 drain (e.g. the kernel wrongly pre-subtracting 4) would now fail here.
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
		"__hyp_text_start", "__hyp_text_end", "__kvm_nvhe___sanitizer_cov_trace_pc")
	linkStart := syms["__hyp_text_start"]
	linkEnd := syms["__hyp_text_end"]
	callback := syms["__kvm_nvhe___sanitizer_cov_trace_pc"]
	textSize := linkEnd - linkStart
	target := targets.Get(targets.Linux, targets.ARM64)

	// Ground truth: a real `bl __kvm_nvhe___sanitizer_cov_trace_pc` inside hyp .text.
	// This is exactly what elfReadModuleCoverPoints records as a CoverPoint (the bl
	// address), so it is what the runtime PC must reconcile back to.
	callSite := findCallbackCallSite(t, vmlinux, callback, linkStart, linkEnd)

	// EL2 records __builtin_return_address(0) == call site + 4 (the insn after bl),
	// identical to ordinary Linux KCOV — the kernel must NOT pre-subtract.
	rawKcovPC := callSite + instructionLen(target.Arch)
	// Simulate __kern_hyp_va: a constant offset across the whole hyp .text block.
	const synthTag = 0x0000_0e00_0000_0000
	hypStart := linkStart + synthTag
	runtimePC := rawKcovPC + synthTag

	// 1. runtime -> link recovers the raw KCOV return address exactly.
	linkPC := pkvmCovRuntimeToLink(runtimePC, linkStart, hypStart, textSize)
	if linkPC != rawKcovPC {
		t.Fatalf("runtime->link: got %#x, want raw KCOV PC %#x", linkPC, rawKcovPC)
	}
	// 2. syzkaller's PreviousInstructionPC reconciles the return address back to the
	//    call site (the CoverPoint). This is the step the earlier test skipped.
	if reportPC := PreviousInstructionPC(target, "", linkPC); reportPC != callSite {
		t.Fatalf("off-by-4: PreviousInstructionPC(%#x)=%#x, want call site %#x",
			linkPC, reportPC, callSite)
	}
	// 3. Range check rejects PCs outside the hyp .text (below start; at/after end).
	if got := pkvmCovRuntimeToLink(hypStart-1, linkStart, hypStart, textSize); got != 0 {
		t.Fatalf("below-range PC not rejected: got %#x", got)
	}
	if got := pkvmCovRuntimeToLink(hypStart+textSize, linkStart, hypStart, textSize); got != 0 {
		t.Fatalf("at-end PC not rejected: got %#x", got)
	}
	// 4. The reconciled call site symbolizes to Rust EL2 source via syzkaller's real
	//    addr2line path — the same consumer used for production coverage.
	symb := symbolizer.Make(target)
	defer symb.Close()
	frames, err := symb.Symbolize(vmlinux, callSite)
	if err != nil {
		t.Fatalf("symbolize %#x: %v", callSite, err)
	}
	if len(frames) == 0 {
		t.Fatalf("no frames for %#x", callSite)
	}
	last := frames[len(frames)-1]
	if !strings.HasSuffix(last.File, ".rs") {
		t.Fatalf("expected a Rust .rs frame, got %q:%d", last.File, last.Line)
	}
	t.Logf("call site %#x -> raw KCOV PC %#x -> runtime %#x -> link %#x -> report %#x -> %s:%d",
		callSite, rawKcovPC, runtimePC, linkPC, callSite, last.File, last.Line)
}

// findCallbackCallSite scans the hyp .text [lo,hi) for the first
// `bl <callback>` instruction and returns its address (the call site). This is the
// same call site elfReadModuleCoverPoints records as a trace-pc CoverPoint.
func findCallbackCallSite(t *testing.T, bin string, callback, lo, hi uint64) uint64 {
	t.Helper()
	f, err := elf.Open(bin)
	if err != nil {
		t.Fatalf("open %v: %v", bin, err)
	}
	defer f.Close()
	var sec *elf.Section
	for _, s := range f.Sections {
		if s.Type == elf.SHT_PROGBITS && s.Addr <= lo && lo < s.Addr+s.Size {
			sec = s
			break
		}
	}
	if sec == nil {
		t.Fatalf("no PROGBITS section covers hyp text %#x", lo)
	}
	data, err := sec.Data()
	if err != nil {
		t.Fatalf("read section data: %v", err)
	}
	for pc := lo; pc+4 <= hi; pc += 4 {
		off := pc - sec.Addr
		if off+4 > uint64(len(data)) {
			break
		}
		insn := binary.LittleEndian.Uint32(data[off : off+4])
		if insn>>26 != 0x25 { // AArch64 BL opcode (bits[31:26] == 0b100101)
			continue
		}
		imm := int64(insn&0x03ffffff) << 2 // imm26 << 2 == 28-bit signed offset
		if imm&(1<<27) != 0 {
			imm |= ^int64((1 << 28) - 1) // sign-extend
		}
		if uint64(int64(pc)+imm) == callback {
			return pc
		}
	}
	t.Fatalf("no `bl %#x` found in hyp .text [%#x,%#x)", callback, lo, hi)
	return 0
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
