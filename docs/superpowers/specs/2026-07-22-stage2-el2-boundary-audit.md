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

## 3. Symbolization audit — the two HARD BLOCKERS (empirically verified)

The colleague's three questions, answered by testing `addr2line` against the real objects. **Both must be
fixed before EL2 KCOV is worth writing** — otherwise collected EL2 PCs are un-symbolizable.

**Q3a — Producer (no instrumentation):** host (EL1) vmlinux has **5** `__sanitizer_cov_*` callbacks; the
`__kvm_nvhe_` image has **0**. So EL2 handlers run with no KCOV signal at all. The symbol space: **2942
`__kvm_nvhe_` symbols, 655 Rust-mangled** (`__kvm_nvhe__ZN…`); the per-hypercall handlers do appear as
symbols, e.g. `__kvm_nvhe__ZN9nvhe_rust8hyp_main…handle___pkvm_host_map_guest…` @ `ffff800081d9c8a0`, and
`c++filt`/`rustc-demangle` recovers `nvhe_rust::hyp_main::…::handle___pkvm_host_map_guest`. So **names**
demangle fine.

**Q2 — DWARF line info does NOT resolve for EL2 (the blocker):** `addr2line` on any `__kvm_nvhe_` address —
C *or* Rust — against **vmlinux** returns `??:?` (while EL1 `pkvm_mem_abort` → `mmu.c:1672` works). Tracing
it to the source:
- **C nvhe:** the per-file objects **do** resolve (`switch.nvhe.o` → `hyp/fault.h:48`, `hyp-main.nvhe.o` →
  `hyp-main.c:660`), but the **linked** `kvm_nvhe.o` / vmlinux lose it. ⇒ the nvhe link (`--prefix-symbols`
  + `hyp/nvhe/hyp.lds`) does not preserve/relocate `.debug_line`. Recoverable by symbolizing against the
  per-file `.nvhe.o` objects (address-mapped) or fixing the link.
- **Rust nvhe (the real target, X1_RMS):** resolves to `nvhe_rust.<hash>-cgu.0:?` — no per-address line —
  at **every** level including the per-file `nvhe_rust.nvhe.o`. Root cause: `build_rust.sh` RUSTFLAGS has
  **no `-C debuginfo`** (`-C relocation-model=static -C code-model=small -C opt-level=3 -C panic=abort
  -C force-unwind-tables=no -C target-feature=-neon`), so the crate ships with no usable DWARF line tables.
  ⇒ **fix: add `-C debuginfo=2` (and reckon with `opt-level=3` inlining) to the Rust hyp build**, else EL2
  Rust PCs symbolize to `cgu.0:?` forever.

**Q1 — runtime VA → link addr:** `__kvm_nvhe_` symbols sit at kernel **link** addresses (`ffff8000…`) in
vmlinux; EL2 executes them at a **hyp VA** (the `__hyp_va`/`__kern_hyp_va` offset, fixed at boot, KASLR
off). A collected EL2 PC is a hyp-VA; Stage 2 must subtract the boot-time hyp-VA offset to get the link
address before symbolizing. (Mechanism exists and is deterministic; not a blocker, but a required step.)

**Consumer gap:** syzkaller's `pkg/cover` (`elf.go:50-59`) does not recognize the linker-prefixed
`__kvm_nvhe___sanitizer_cov_trace_pc` callback, and `module_obj` is `.ko`-only. So Stage 2 needs a
**two-object symbolizer**: recognize the EL2 address range, apply the hyp-VA offset (Q1), strip the
`__kvm_nvhe_` prefix, and resolve against a **DWARF-bearing object** — which today means the per-file
`.nvhe.o` set for C, and a **rebuilt-with-debuginfo** Rust crate.

## 4. Attribution & harvest discipline (do NOT wrap the boundary globally)

EL2 PCs must be attributed to **the one syz call that crossed** — not harvested at every ioctl return, and
not by globally wrapping `kvm_call_hyp_nvhe()` (that folds in boot init, other threads, and non-fuzz
contexts → wrong attribution). Per-crossing rules for the reachable set:

| boundary | harvest EL2 PCs at | attribute to | risk |
|----------|--------------------|--------------|------|
| #21 share (CREATE_VM/VCPU) | return of the `kvm_share_hyp` EL1 call site | the CREATE_VM / CREATE_VCPU call | low; single-threaded ioctl |
| #34/#35 init_vm/init_vcpu | return of `pkvm_create_hyp_vm` (first `KVM_RUN`) | the vcpu-run call | the run may be `immediate_exit` — still crosses |
| #23 host_map_guest | return of the EL1 `pkvm_mem_abort`/map path | the vcpu-run call that faulted | must not leak into a *later* call |
| #30 `__kvm_vcpu_run` | around the run hypercall | the vcpu-run call | **CPU migration**: the vcpu can move CPUs; harvest on the same CPU/thread that issued the run |
| #36–38 teardown | at the fd-close destroy path | **special** — fd close is not an ioctl | **teardown attribution**: `close()` runs after per-call output (`executor.cc:1158`); needs an explicit `close$kvmvm_protected` attribution point or an end-of-program flush, else orphaned |

So the harvest points are **specific EL1 return sites** (success *and* error), each fenced to the issuing
thread/CPU, with teardown handled as its own attribution channel.

## 5. Stage 2 prototype direction (NOT started — this is the blueprint)

**Prerequisites (from §3 — do these FIRST, they are the hard blockers):**
- **P0. Rust debuginfo:** add `-C debuginfo=2` to `build_rust.sh` RUSTFLAGS (and account for `opt-level=3`
  inlining) so EL2 Rust PCs resolve to `.rs:line` at all. Verify with `addr2line` on `nvhe_rust.nvhe.o`.
- **P0. nvhe-link DWARF:** make `.debug_line` resolvable in the linked image, or wire the symbolizer to the
  per-file `.nvhe.o` set (C already resolves there).

**Then:**
1. **Producer:** build the EL2 Rust crate with SanCov for `aarch64-unknown-none`, hyp-local
   `__sanitizer_cov_trace_pc` writing a hyp-owned ring.
2. **Delivery:** `kcov_add_pcs()` at the specific boundary return sites (§4) — **NOT** `kcov_remote_start`
   (WARN-bails; guard `kcov.c:860`) and **NOT** a global `kvm_call_hyp_nvhe()` wrap.
3. **Consumer:** two-object symbolizer (§3) — hyp-VA offset + prefix-strip + demangle + DWARF-bearing object.
4. **Validate** on the single #23 crossing (the firmware smoke path) first.

## Next
- Firmware reachability smoke (`2026-07-22-firmware-reachability-smoke-design.md`) — proves #23 is reached
  and lights up `pkvm_mem_abort` (host-side), independent of Stage 2.
- P0 debuginfo fix (small, verifiable with `addr2line`) before any producer work.
