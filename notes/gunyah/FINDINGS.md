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
- **Reproducibility:** syzkaller marked it `[corrupted]` (its report parser does not recognise the
  interleaved `XHYPER_HOST_EXIT_UNRESOLVED` lines). Adding a pkg/report oops rule for
  `XHYPER_HOST_EXIT_UNRESOLVED` would categorise these cleanly. Full reproducer in the crash dir's
  log0.

## F2 — hypercall error return does not restore/sanitize guest callee-saved registers
- **Found:** 2026-09-26, S2 (raw host HVC injector /dev/xh_raw_hvc), during bring-up.
- **Attribution:** EL2 (XHyper), ABI deviation with security weight. On a fuzzed unknown/malformed
  hypercall, XHyper's error return leaves internal values in the guest's callee-saved x19..x28
  (observed x19=0x20000600 after the hvc). C Gunyah's return path
  (`vcpu_hypercall_return_sanitize_*` in futlab-xhyper .../vcpu/armv8-64/src/return.S) restores
  x19..x29 to the guest and sanitizes x0/x1/x2/x8 explicitly "to prevent EL1 targeting EL2 gadgets".
  XHyper's error path does neither -> ABI deviation + potential EL2->EL1 register leak.
- **Status:** recorded, not fixed. Detailed in the bring-up record (Stage 3 / Finding section).

## F1a/F1b — EL1 gunyah-driver crashes (earlier campaign, unreproduced leads)
- **Found:** 2026-09-26, S1 campaign (cover:false era).
- **Attribution:** EL1 (host / gunyah driver), XHyper healthy.
- F1a: `Internal error in gh_vm_ioctl` reached via `GH_VM_REMOVE_FUNCTION`.
- F1b: `Internal error in eventfd_release` (null-deref on the eventfd path via /dev/gunyah).
- **Status:** recorded, unreproduced leads; guest-kernel driver side, not XHyper EL2.
