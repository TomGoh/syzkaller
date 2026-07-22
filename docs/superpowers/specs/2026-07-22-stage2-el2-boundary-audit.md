# Stage 2 — EL1→EL2 boundary + symbolization static audit (2026-07-22)

Read-only prep for Stage 2 (EL2 in-hyp KCOV). Nothing here injects KCOV into the Rust hyp; it pins down
*where* the boundary is, *which* hypercalls the fuzzed host calls reach, and *why* EL2 is currently dark.
All against `common@` (X1_RMS Rust hyp) and the deployed `6.6.30-pkvm-fuzz` vmlinux
(sha256 `c3d0c24e…`, build-id `1e36e2ee…`).

## 1. The boundary: the host→hyp hypercall interface

EL1 crosses to EL2 via `kvm_call_hyp_nvhe(<id>, …)` / `kvm_call_refill_hyp_nvhe(…)`. The EL2 side
dispatches through a **66-entry function-pointer table** `HOST_HCALL: [Option<HcallFn>; 66]`
(`hyp/nvhe/rust/src/hyp_main.rs`), id → Rust handler `handle_<id>`. 40 distinct `__pkvm_*` targets are
called from `arch/arm64/kvm/`. The ids that matter for the fuzzed lifecycle (see §2):

| id | hypercall | Rust handler | triggered by (host call) |
|----|-----------|--------------|--------------------------|
| 2  | `__pkvm_init` | `handle___pkvm_init` | boot (not fuzzed) |
| 21 | `__pkvm_host_share_hyp` | `handle___pkvm_host_share_hyp` | CREATE_VM / CREATE_VCPU (share structs) |
| 22 | `__pkvm_host_unshare_hyp` | `handle___pkvm_host_unshare_hyp` | teardown |
| 23 | `__pkvm_host_map_guest` | `handle___pkvm_host_map_guest` | **guest fault (real run) — firmware smoke** |
| 24 | `__pkvm_host_unmap_guest` | `handle___pkvm_host_unmap_guest` | guest unmap |
| 25 | `__pkvm_relax_perms` | `handle___pkvm_relax_perms` | guest fault (perm) |
| 26 | `__pkvm_wrprotect` | `handle___pkvm_wrprotect` | dirty-log wrprotect |
| 28 | `__pkvm_tlb_flush_vmid` | `handle___pkvm_tlb_flush_vmid` | mapping changes |
| 30 | `__kvm_vcpu_run` | `handle___kvm_vcpu_run` | KVM_RUN (guest entry) |
| 34 | `__pkvm_init_vm` | `handle___pkvm_init_vm` | first KVM_RUN (`pkvm_create_hyp_vm`, pkvm.c:405) |
| 35 | `__pkvm_init_vcpu` | `handle___pkvm_init_vcpu` | first KVM_RUN (per-vCPU) |
| 36 | `__pkvm_start_teardown_vm` | `handle___pkvm_start_teardown_vm` | VM close |
| 37 | `__pkvm_finalize_teardown_vm` | `handle___pkvm_finalize_teardown_vm` | VM close |
| 38 | `__pkvm_reclaim_dying_guest_page` | `handle___pkvm_reclaim_dying_guest_page` | VM close |

Ids 42–63 are tracing / iommu / module / hyp-alloc / stage2-snapshot; 64–65 are XCore stats/selftest —
**not reachable** from the current syzlang surface.

## 2. Reachability from the fuzzed host calls (what the campaign actually crosses)

- **Covered today (immediate_exit lifecycle + memslot rejects + ENABLE_CAP):** the EL1 *drivers*
  `pkvm_create_hyp_vm`/`__pkvm_create_hyp_vcpu`/`pkvm_init_host_vm` + the boundary calls #21/#34/#35 (VM +
  vCPU create/share) and #36–#38 (teardown). These are the 8 host-side `pkvm.c` functions the rawcover
  shows. The EL2 *handlers* behind them run but are invisible (§3).
- **NOT crossed today:** #23 `__pkvm_host_map_guest` and the guest-fault path — because `immediate_exit`
  returns before the guest executes. The EL1 entry `pkvm_mem_abort` (kallsyms `ffff800080087bc8`,
  kprobe-able) is **absent from the current rawcover**. The firmware reachability smoke
  (`2026-07-22-firmware-reachability-smoke-design.md`) is what first lights up #23 and `pkvm_mem_abort`.

## 3. Why EL2 is dark, and the symbolization reality

Counted against the deployed vmlinux:
- **Host (EL1):** 5 `__sanitizer_cov_*` callbacks — KCOV instruments EL1 normally.
- **EL2 (`__kvm_nvhe_`-prefixed image):** **0** `__sanitizer_cov_*`. The nVHE hyp object is built without
  coverage instrumentation, so every EL2 handler above executes with **no KCOV signal** — Stage 2's core
  gap, confirmed empirically (not just from the Makefile).
- The EL2 image has **2942 `__kvm_nvhe_` symbols**, of which **655 are Rust-mangled**
  (`__kvm_nvhe__ZN…` — the `__kvm_nvhe_` prefix stacked on Rust `_ZN…`). The per-hypercall handlers
  (`handle___pkvm_init_vm`, …) do **not** appear as clean `__kvm_nvhe_handle___pkvm_*` symbols (0 found) —
  inlined into the dispatch and/or Rust-mangled.

**Two-object symbolization approach (for when EL2 PCs exist):** an EL2 PC must be (a) recognized by its
address range as belonging to the `__kvm_nvhe_` image, (b) prefix-stripped, and (c) Rust-demangled
(`_ZN…` → `rustc-demangle`), then mapped via DWARF (`debug=true` is set). syzkaller's `elf.go:50-59` does
not recognize the `__kvm_nvhe___sanitizer_cov_trace_pc` callback name today (design-spec §6 blocker), and
`module_obj` does not apply (`.ko`-only). So Stage 2 needs both a **producer** (sancov in the EL2 crate)
and a **consumer** (a two-object symbolizer that strips + demangles).

## 4. Stage 2 prototype direction (NOT started — this is the map)

1. **Producer:** build the EL2 Rust crate with SanCov (`-Cinstrument-coverage`/`-Zsanitizer` equivalent for
   `aarch64-unknown-none`), providing a hyp-local `__sanitizer_cov_trace_pc` writing a hyp-owned ring.
2. **Delivery:** `kcov_add_pcs()` at the verified boundary sites (§1) to append EL2 PCs to the current
   task's main KCOV area — **NOT** `kcov_remote_start` (WARN-bails; guard `kcov.c:860`), per the design
   memory.
3. **Consumer:** the two-object symbolizer (§3).
4. **Validate** against the firmware-smoke path (#23) first — a single, well-understood boundary crossing.

## Next
- Firmware reachability smoke (`2026-07-22-firmware-reachability-smoke-design.md`) — proves #23 is reached
  and lights up `pkvm_mem_abort` (host-side), independent of Stage 2.
- Then prototype the producer on the smallest handler set (#34/#35 create, #23 map).
