// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

import (
	"strings"
	"testing"

	"github.com/google/syzkaller/pkg/mgrconfig"
	"github.com/google/syzkaller/sys/targets"
)

func newXHyperReporter(t *testing.T) *Reporter {
	cfg := &mgrconfig.Config{
		Derived: mgrconfig.Derived{
			TargetOS:   targets.Linux,
			TargetArch: targets.AMD64,
		},
	}
	reporter, err := NewReporter(cfg)
	if err != nil {
		t.Fatal(err)
	}
	return reporter
}

// TestXHyperFatalMarkers covers the two-line Rust panic format and the
// Manager/HLOS/EL2 fatal markers, and guards that routine per-abort diagnostics
// and the Manager's self-test probe lines are NOT reported as crashes.
func TestXHyperFatalMarkers(t *testing.T) {
	reporter := newXHyperReporter(t)

	// Crash markers: (input, substring the title must contain).
	crashes := []struct {
		log  string
		want string
	}{
		// Two-line Rust 1.95 panic: the message is on the second line.
		{"XHYPER PANIC: panicked at arch/kcpu/src/aarch64/excp.rs:88:9:\n" +
			"Unhandled EL1 Page Fault far=0x20000280\n", "Unhandled EL1 Page Fault"},
		// The title must keep kind= and owner= so distinct Manager fatals do
		// not dedup into one crash.
		{"MANAGER_FATAL reason=domain kind=ResetCleanup owner=Memparcel\n",
			"MANAGER_FATAL reason=domain kind=ResetCleanup owner=Memparcel"},
		{"MANAGER_FATAL reason=domain kind=ResetCleanup owner=Configuration\n",
			"MANAGER_FATAL reason=domain kind=ResetCleanup owner=Configuration"},
		{"ROOTVM_FATAL reason=ManagerDomain\n", "ROOTVM_FATAL reason=ManagerDomain"},
		{"ROOTVM_FATAL reason=HlosStart\n", "ROOTVM_FATAL reason=HlosStart"},
		{"HLOS_VIC_CLEANUP_FATAL error=9\n", "HLOS_VIC_CLEANUP_FATAL"},
		{"HLOS_VGIC_ITS_ALLOC_FATAL\n", "HLOS_VGIC_ITS_ALLOC_FATAL"},
		{"XHYPER_EL2_PAGE_FAULT far=0x0 elr=0x0 esr=0x0 access=0x0\n",
			"XHYPER EL2 page fault"},
	}
	for _, tc := range crashes {
		log := []byte(tc.log)
		if !reporter.ContainsCrash(log) {
			t.Errorf("not recognized as a crash: %q", tc.log)
			continue
		}
		rep := reporter.Parse(log)
		if rep == nil {
			t.Errorf("Parse returned nil for %q", tc.log)
			continue
		}
		if !strings.Contains(rep.Title, tc.want) {
			t.Errorf("title %q does not contain %q (input %q)", rep.Title, tc.want, tc.log)
		}
		if rep.Corrupted {
			t.Errorf("marked corrupted: %s (input %q)", rep.CorruptedReason, tc.log)
		}
	}

	// Routine diagnostics and self-test probe lines are NOT crashes. Reporting
	// any of these would turn expected/benign output into systematic false
	// findings (see the tool assessment, item 5).
	notCrashes := []string{
		"XHYPER_RESOURCE_REJECT kind=memextent reason=denied\n",
		"XHYPER_HOST_EXIT_UNRESOLVED detail=host-exit-unhandled vcpu=15 pc=0x0 esr=0x0\n",
		"ROOTVM_FATAL_PROBE data-abort\n",
		"ROOTVM_FATAL_PROBE relro-write\n",
		"ROOTVM_FATAL_PROBE unsupported-relocation\n",
		"HLOS_MSI_DEVICE name=foo base=0x0\n",
		"HLOS_UART_HANDOFF ok\n",
		"HLOS_DTB_OVERLAY_FAIL\n",
		"HLOS_ACPI_DEVICE_FAILED\n",
	}
	for _, log := range notCrashes {
		if reporter.ContainsCrash([]byte(log)) {
			t.Errorf("treated as a crash but should not be: %q", log)
		}
	}
}

func TestXHyperCrashTitles(t *testing.T) {
	cfg := &mgrconfig.Config{
		Derived: mgrconfig.Derived{
			TargetOS:   targets.Linux,
			TargetArch: targets.AMD64,
		},
	}
	reporter, err := NewReporter(cfg)
	if err != nil {
		t.Fatal(err)
	}

	panicLog := []byte("XHYPER PANIC: assert failed in memdb\n")
	if !reporter.ContainsCrash(panicLog) {
		t.Fatal("XHYPER PANIC was not recognized")
	}
	rep := reporter.Parse(panicLog)
	if rep == nil {
		t.Fatal("Parse returned nil for XHYPER PANIC")
	}
	if rep.Title != "XHYPER PANIC: assert failed in memdb" {
		t.Fatalf("panic title: %q", rep.Title)
	}
	if rep.Corrupted {
		t.Fatalf("panic marked corrupted: %s", rep.CorruptedReason)
	}

	bootLog := []byte("XHYPER_HOST_BOOT_ERROR stage=dt error=denied\n")
	if !reporter.ContainsCrash(bootLog) {
		t.Fatal("XHYPER_HOST_BOOT_ERROR was not recognized")
	}
	rep = reporter.Parse(bootLog)
	if rep == nil {
		t.Fatal("Parse returned nil for host boot error")
	}
	if rep.Title != "XHYPER host boot error" {
		t.Fatalf("boot title: %q", rep.Title)
	}
	if rep.Corrupted {
		t.Fatalf("boot error marked corrupted: %s", rep.CorruptedReason)
	}

	// A normal boot line shares the prefix up to BOOT_ but is not a crash.
	okLog := []byte("XHYPER_HOST_BOOT_VCPU thread=1:0 affinity=Cpu(0)\n")
	if reporter.ContainsCrash(okLog) {
		t.Fatal("XHYPER_HOST_BOOT_VCPU was treated as a crash")
	}
}
