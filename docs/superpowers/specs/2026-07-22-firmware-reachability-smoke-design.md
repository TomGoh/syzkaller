# Firmware reachability smoke — design (2026-07-22)

A one-shot, watchdog-protected check that the host→EL2 **firmware donation/load contract** is actually
entered on N90 — not a Phase 5 campaign, and not a fuzzer primitive (yet). Correct scope (per review):
it does **not** prove per-PC EL2 coverage; it proves the contract runs, and it lights up the EL1
`pkvm_mem_abort` path that is currently missing from the rawcover.

## Why it is needed / what it is not
- `SET_FW_IPA` only *records* `pvmfw_load_addr` (pkvm.c:632). The actual load is the Rust EL2
  `pkvm_load_pvmfw_pages` (`hyp/nvhe/rust/src/pkvm.rs:2692` via `mem_protect/guest.rs:1232`), reached
  through `handle___pkvm_host_map_guest` (hypercall #23) on a **guest fault** at the firmware IPA.
- `immediate_exit=1` returns before the guest executes → never faults → never loads. So the smoke needs a
  **real** first `KVM_RUN`.
- The EL2 load body is `__kvm_nvhe_`-mangled and not an EL1 kprobe target; the EL1 `pkvm_host_map_guest`
  wrapper has no independent kallsyms symbol. So the observable is the **EL1 `pkvm_mem_abort`** entry
  (kallsyms `ffff800080087bc8`, `t`, kprobe-able; `CONFIG_KPROBES/KRETPROBES/KALLSYMS_ALL=y`), NOT a
  kretprobe on `__pkvm_host_map_guest`.

## Steps
```
fresh single-vCPU protected VM (bit-31)
 → register a guest memslot over the firmware window 0x7FC00000..0x80000000 (4 MiB, crosvm layout)
 → KVM_ENABLE_CAP INFO: assert firmware_size (978944) <= 0x400000 (fits the window)
 → SET_FW_IPA(0x7FC00000)   [before the first run: returns 0]
 → KVM_ARM_VCPU_INIT_safe (target=GENERIC_V8, feature=0)
 → real (NON-immediate_exit) first KVM_RUN, bounded by a hard timeout + GWDT watchdog
```

## Observation (tracefs kprobe on pkvm_mem_abort)
`pkvm_mem_abort(struct kvm_vcpu *vcpu, phys_addr_t *fault_ipa, memslot, hva, size)` — fault IPA is `*x1`
at entry, return is the retval:
```
# arm before the run:
echo 'p:fw_abort_in  pkvm_mem_abort ipa=+0(%x1):u64' >> /sys/kernel/tracing/kprobe_events
echo 'r:fw_abort_ret pkvm_mem_abort ret=$retval:s64'  >> /sys/kernel/tracing/kprobe_events
echo 1 > /sys/kernel/tracing/events/kprobes/fw_abort_in/enable
echo 1 > /sys/kernel/tracing/events/kprobes/fw_abort_ret/enable
> /sys/kernel/tracing/trace
# ... run ... then read /sys/kernel/tracing/trace
```

## Acceptance (corrected)
1. `fw_abort_in` fires with `ipa` in `[0x7FC00000, 0x80000000)` — the guest faulted at the firmware IPA;
2. the paired `fw_abort_ret` returns **0** — the donation transaction (EL1 → EL2 `__pkvm_host_map_guest`
   → Rust `pkvm_load_pvmfw_pages`) completed successfully.
At this IPA / single-vCPU / first mapping there is no prior mapping to yield a spurious `-EAGAIN`, so a 0
return is sufficient *indirect* proof the firmware page was donated/loaded — not just a host field write.
3. Secondary: the rawcover for this run now contains `pkvm_mem_abort` + `pkvm_host_map_guest` (EL1 PCs
   absent today), i.e. real new host coverage.

## Board safety
- Bounded run: wrap the probe under `timeout <5s>`; the guest will spin in pvmfw after the donation (no
  payload / DICE chain failed at boot), so the run is expected to be killed — the kprobe captures the
  fault *before* the spin. GWDT watchdog is the backstop; verify `uptime`/SSH after.
- Standalone probe first (extend `fw_probe.c`: add the fw-window memslot + real run), NOT a syzlang
  composite. Only promote to a composite (`syz_kvm_fw_reachable$arm64`) after the standalone smoke is
  stable and the board recovers cleanly.

## RESULT — PASSED (N90, 2026-07-22). Evidence: `firmware-smoke-2026-07-22/`
```
memslot @ 0x7fc00000 size 0x400000 OK
INFO firmware_size=978944 fits_window=1
SET_FW_IPA(0x7fc00000) = 0 (ok)
KVM_RUN ret=0 errno=0 exit_reason=6 (MMIO)          <- returned cleanly, did NOT hang (alarm unused)
kprobe pkvm_mem_abort:
  fw_abort_in  ipa=0x7fc00000   <- first fault at EXACTLY the firmware IPA
  fw_abort_ret ret=0            <- donation contract succeeded
  + 9 more faults 0x7fcc4000 / 0x7fe08000 / … all ret=0  (pvmfw executing)
dmesg WARN/BUG: 0 ; board up, no wedge/reset
```
Acceptance met: the real guest fault at the firmware IPA entered `pkvm_mem_abort` and returned 0 — the
host→EL2 `__pkvm_host_map_guest` → Rust `pkvm_load_pvmfw_pages` donation was actually exercised, not just a
host field write. It also **lights up the EL1 `pkvm_mem_abort`/donation path that was absent** from the
lifecycle rawcover. Bonus: pvmfw genuinely ran (10 page faults, then a clean `KVM_EXIT_MMIO`), so the run
self-terminates — no busy-loop, board stayed healthy.

## After it passes
Two independent tracks now unblocked:
- **Stage 2 producer** — use this single #23 crossing as the first validation target
  (`2026-07-22-stage2-el2-boundary-audit.md` §4/§5): the same fault that returned 0 here is where an EL2 PC
  batch would be drained.
- **Optional composite** — promote to `syz_kvm_fw_reachable$arm64` for a host-side campaign to keep lighting
  up the abort/donation EL1 path (only if we want that extra EL1 coverage before Stage 2).
