// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

import (
	"testing"

	"github.com/google/syzkaller/pkg/mgrconfig"
	"github.com/google/syzkaller/sys/targets"
)

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
