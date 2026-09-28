# XHyper fuzzing — findings log

Running record of crashes/defects found by the coverage-guided syzkaller campaign against XHyper
(Rust EL2 hypervisor, Gunyah ABI) under QEMU AArch64 TCG. Each entry: how it manifested, the EL1
(host/driver) vs EL2 (XHyper hypervisor) attribution, the trigger, and status. Recorded per the
project rule (log defects; fix only when our commit rewrites that code). Newest first is fine.

Attribution key:
- **EL2 (XHyper)** = root cause in the Rust hypervisor. XHyper marks its own unresolved traps with
  `XHYPER PANIC:` (a hypervisor panic) or `XHYPER_HOST_EXIT_UNRESOLVED` (a host exit it could not
  handle). A pstate showing EL2 in a panic is XHyper.
- **EL1 (host)** = a Linux oops in the HLOS kernel / the gunyah driver (pstate EL1, Linux symbols),
  with XHyper healthy.

---

## F3 — host memory-region donation → unrecoverable host stage-2 external abort (host DoS)
- **Found:** 2026-09-27, S2 campaign (workdir-covfinal), crash id 5bf47ad3a3574e7feae2415e027718b912ee27a3.
- **Attribution:** EL2 root cause, EL1 manifestation. XHyper reported
  `XHYPER_HOST_EXIT_UNRESOLVED detail=host-exit-unhandled vcpu=21 esr=0x93c18046 far=0x20000080
  ipa=0x42000080 hpfar=0x420000` — a host stage-2 data abort XHyper could not resolve — and returned
  a synchronous external abort to EL1. The HLOS then oopsed:
  `Internal error: synchronous external abort 0x96000010` in `strncpy_from_user <- getname <-
  do_sys_openat2 <- __arm64_sys_openat` (a normal openat reading a userspace path).
- **Trigger (from log0):** heavy `ioctl$GH_VM_SET_USER_MEM_REGION` donations whose `userspace_addr`
  points into the executor's own mapped memory (e.g. lines 140 `{0x3,0x3,0x0,0x4000,0x7f0000006000}`,
  152 `{0x2,0x5,0x0,0x1000,0x7f0000003000}`, 159 `{0x3,0x1,0x0,0x4000,0x7f0000007000}`) followed by
  `ioctl$GH_VM_START`. The host lends in-use memory to a guest; XHyper removes it from the host
  stage-2; a later host access to it (or an aliased page) faults unresolvably.
- **Assessment / open question:** under the "host may be compromised" model a host self-donating
  in-use memory could be "expected self-harm", BUT the result is an *unrecoverable* host abort, and
  it may indicate XHyper does not validate/reject donation of in-use or critical host memory the way
  C Gunyah's memextent lend/donate does. Needs a Rust-vs-C comparison of the memextent
  donate/lend validation + XHyper's host stage-2 fault handling. Recorded, not fixed.
- **Reproducibility:** CONFIRMED reproducible (2026-09-27): re-seeding the campaign with the corpus
  that first found it re-triggered the identical crash (same id 5bf47ad3...). syzkaller marks it
  `[corrupted]` (its report parser does not recognise the interleaved `XHYPER_HOST_EXIT_UNRESOLVED`
  lines) and so does not auto-minimise a repro; the trigger call sequence is saved in
  notes/gunyah/repro/F3-trigger-calls.txt and the crash dir's log0. Adding a pkg/report oops rule for
  `XHYPER_HOST_EXIT_UNRESOLVED` would categorise + auto-repro these cleanly.

## F2 — WITHDRAWN (2026-09-28): not an XHyper defect; the raw-HVC test driver's first trampoline did not save LR
- **Found:** 2026-09-26, S2 (raw host HVC injector /dev/xh_raw_hvc), during bring-up.
- **Original claim (wrong):** XHyper's hypercall error return leaves internal values in the guest's
  callee-saved x19..x28 (observed x19=0x20000600 after the hvc), unlike C Gunyah's return path.
- **What actually happened:** the first trampoline saved x19/x20 but not x30, then `blr`ed to the
  `hvc #imm; ret` stub, which set x30 to trampoline+0x20. Pass 1 stored the results correctly and restored
  the caller's x19/x20; its final `ret` jumped back to +0x20, and pass 2 stored through the caller's x19,
  which held the ioctl user pointer 0x20000600 -> PAN permission fault (ESR 0x9600004e). Crash a842ba95
  shows it: pc = lr = xh_raw_hvc_trampoline+0x20/0x38; sp = the caller's sp (x29-0x50), so the trampoline
  frame was already popped; x19 = x20 = 0x20000600; __arm64_sys_ioctl's x21/x22/x23 (cmd/arg/fd) intact;
  x0..x3 = -1/0/0/0, XHyper's answer to hypercall 0x59 (not on the host allow-list); x4..x14 the host's own values.
- **XHyper side:** entry.S saves all of x0..x30 on every exit and restores all of them before eret; no
  host-reachable hypercall path writes x8..x30. The C comparison was also misread: C restores x19..x29
  because its entry path zeroes them (the "EL2 gadgets" comment is about that), and it deliberately
  preserves x8.
- **Status:** withdrawn. The trampoline was fixed on 2026-09-26 (LR and result pointer spilled to the stack);
  its comment, which blamed XHyper, was corrected on 2026-09-28. Full analysis in the xhyper workspace:
  docs/c-to-rust-migration/xhyper-fuzz-findings-triage-20260928.md; the recovered first trampoline and the
  crash dump are in docs/artifacts/xhyper-fuzz-findings-20260928/bug3-hvc-regs-abi/.

## F1a/F1b — EL1 gunyah-driver crashes (earlier campaign, unreproduced leads)
- **Found:** 2026-09-26, S1 campaign (cover:false era).
- **Attribution:** EL1 (host / gunyah driver), XHyper healthy.
- F1a: `Internal error in gh_vm_ioctl` reached via `GH_VM_REMOVE_FUNCTION`.
- F1b: `Internal error in eventfd_release` (null-deref on the eventfd path via /dev/gunyah).
- **Status:** recorded, unreproduced leads; guest-kernel driver side, not XHyper EL2.

## S3 verification (2026-09-27): guest execution CONFIRMED working
An S3-only fuzz (only openat$gunyah, GH_CREATE_VM, syz_gunyah_setup_vm$arm64, syz_gunyah_add_vcpu$arm64, mmap, close enabled) grew corpus 0->1 with coverage 0->3794 — the nested guest boots, runs the SYZOS payload and reaches EL2 (not just VM setup). In the combined S1+S2+S3 campaign, however, coverage only rose 12110->12147: S3's ~3794 blocks overlap heavily with the VM-lifecycle + hypercall-dispatch code S1's driver path already covers, so the NET new coverage is small so far. S3's unique surface (guest SMCCC/sysreg-trap/vGIC-MMIO/stage-2) needs longer fuzzing or richer guest payloads to surface. (Standalone syz-execprog of the identify prog hung with no output — a libc-less-guest tooling issue, NOT S3: the serial showed no XHyper crash and the S3-only syz-manager path works.)
