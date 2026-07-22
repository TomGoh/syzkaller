# Stage-2 EL2-KCOV MVP — progress + delivery spec (2026-07-22)

Scope: the minimal closed loop for one boundary (#23 `__pkvm_host_map_guest`), per the approved plan. All
work in an isolated kernel tree (`/home/jose/common-stage2mvp`); **N90 was not touched**. Evidence +
patch: `evidence/stage2-mvp-2026-07-22/`.

## Increment 1 — PRODUCER: DONE and verified (builds, no board needed)

The Rust hyp is now SanCov-instrumented and its callback lands in a hyp ring, while staying symbolizable.

- **Build flags** (`build_rust.sh`): `-C debuginfo=2` + `-C passes=sancov-module -C llvm-args=-sanitizer-coverage-level=3
  -C llvm-args=-sanitizer-coverage-trace-pc` (+ `CARGO_PROFILE_RELEASE_DEBUG=2`). See `producer.patch`.
- **Callback** (`arch/arm64/kvm/hyp/nvhe/cov.c`, uninstrumented nvhe C, `notrace` + per-CPU recursion
  guard): `__sanitizer_cov_trace_pc()` records `__builtin_return_address(0)` into a **host-shared** per-CPU
  ring (`struct pkvm_cov_ring{u32 count; u32 flags; u64 pcs[511]}`, `asm/kvm_pkvm_cov.h`), gated by the
  `PKVM_COV_FLAG_ENABLED` bit the host sets, with `smp_store_release(count)`/`smp_load_acquire(flags)`
  ordering. `pkvm_cov_setup(pfn)` pins/registers (or unpins) the ring. Added `cov.o` to the nvhe Makefile.
- **Verified in the linked vmlinux:**
  - `nvhe_rust.o` has **7443 `__sanitizer_cov_trace_pc` call sites** (the whole Rust hyp is instrumented);
  - `__kvm_nvhe___sanitizer_cov_trace_pc` is defined and resolves to `cov.c` — all call sites link;
  - `__kvm_nvhe_pkvm_cov_setup` is present in the hyp image;
  - **debuginfo + SanCov coexist**: `handle___pkvm_host_map_guest` still resolves to `hyp_main.rs:1069`.

So instrumented-Rust → our callback → ring, and the PCs remain symbolizable — the two hard, novel,
board-independent pieces. `evidence/stage2-mvp-2026-07-22/increment1-verification.txt`.

## Increment 2 — DELIVERY: revised spec (review fixes applied; build in the isolated tree first)

Single hypercall + shared-page flags, to minimise the Rust-dispatch surface. **Six review fixes folded in:**

1. **Ring layout must not overflow the page.** `count(u32)+overflow(u32)+pcs[511](u64)` is already exactly
   4096; adding a separate `enabled` field pushes it to 4104. Fix: **one `flags` u32** carries both bits:
   ```c
   struct pkvm_cov_ring { __u32 count; __u32 flags; __u64 pcs[511]; };  /* == 4096 */
   #define PKVM_COV_FLAG_ENABLED  (1u<<0)   /* host-set */
   #define PKVM_COV_FLAG_OVERFLOW (1u<<1)   /* hyp-set  */
   ```
2. **Reuse KCOV's exact write protocol — do NOT paraphrase.** The kernel's `__sanitizer_cov_trace_pc`
   updates the count *before* the PC (to survive re-entrant interrupts) and uses `t->kcov_size`:
   `pos = READ_ONCE(area[0])+1; if (pos < t->kcov_size) { WRITE_ONCE(area[0], pos); barrier(); area[pos]=ip; }`.
   `kcov_add_pcs(const u64 *pcs, u32 n)` (new, `kernel/kcov.c`, `notrace`, under `CONFIG_PKVM_EL2_COV`) loops
   that body per PC, guarded by `check_kcov_mode(KCOV_MODE_TRACE_PC, current)`. It stores the PCs **as
   given** (no `canonicalize_ip` — the drain side hands it already-converted link addresses, see the
   conversion note below). The **hyp** ring uses the count-last-visible discipline via `smp_store_release`.
3. **CPU-pin is an implementation, not a wish.** `procs:1` does NOT stop the executor migrating CPUs across
   the KVM ioctl. The executor thread that drives the KVM lifecycle must `sched_setaffinity()` to a single
   CPU (e.g. CPU 0) so the EL2 producer (per-CPU ring) and the EL1 drain are the same CPU. Alternative if
   pinning is undesirable: tag each ring batch with `{cpu, generation}` and reconcile on drain. MVP = pin.
4. **Shared-ring concurrency protocol.** host↔EL2↔host on one page needs `READ_ONCE`/`WRITE_ONCE` + a
   release/acquire pair: host `WRITE_ONCE(flags |= ENABLED)` before the map; EL2 writes `pcs[]` then
   `smp_store_release(&count, …)`; host `smp_load_acquire(&count)` after the map, then reads `pcs[0..count]`.
   Single-CPU (pinned) makes this a compiler-barrier concern primarily, but keep the ONCE/release-acquire.
5. **Callback marked non-instrumented explicitly.** Declare `void notrace __sanitizer_cov_trace_pc(void)`
   (matching kernel style) even though the nvhe C build already omits SanCov — belt-and-suspenders against
   self-recursion.
6. **Reproducible, config-gated build.** Pin `rust-src` to the **same** `nightly-2025-05-05`
   (`rustup component add rust-src --toolchain nightly-2025-05-05`, not floating `nightly`), and put the
   debuginfo+SanCov flags behind a **`CONFIG_PKVM_EL2_COV`** gate (Stage-2 fuzz kernel only), not
   unconditional.

**The one hypercall `__pkvm_cov_setup(pfn)` — config-gated, NON-disruptive to existing ids.** New
`CONFIG_PKVM_EL2_COV` (Kconfig, `depends on KVM && X1_RMS && KCOV`) gates *everything*: the debuginfo+SanCov
flags (`build_rust.sh`), `cov.o` (`hyp-obj-$(CONFIG_PKVM_EL2_COV)`), the enum entry, and the dispatch.
- `asm/kvm_asm.h`: `__KVM_HOST_SMCCC_FUNC___pkvm_cov_setup` is added **after** the `XCORE_UNIT_TEST` block,
  under `#ifdef CONFIG_PKVM_EL2_COV`, so it **never shifts any existing hypercall id** (incl. unit-test).
- Dispatch (`hyp_main.rs`): a `#[cfg(CONFIG_PKVM_EL2_COV)]` **explicit `id ==` check** *before* the
  `HOST_HCALL` table lookup — so the fixed 66-entry table (and unit-test's slot) is untouched. The id
  constant comes from the (bindgen-regenerated) `__kvm_host_smccc_func` binding.
- The dispatch reads `declare_reg(1)=pfn` and calls the extern-C `pkvm_cov_setup(pfn)` (in `cov.c`), which
  `hyp_phys_to_virt(pfn<<PAGE_SHIFT)` + `hyp_pin_shared_mem()` and registers the per-CPU ring (or unpins on
  pfn==0). Returns via a1 (HVC ABI: a0 always `SMCCC_RET_SUCCESS`).
- Repro (fix #6): `rust-src` pinned to `nightly-2025-05-05`.

**Status (2026-07-22): the config-gated producer + hypercall path is BUILT and validated in the isolated
tree.** Review fixes #1–#6 applied; the 3 must-fix items verified:
- **#1 `--cfg=CONFIG_PKVM_EL2_COV`** added to `build_rust.sh` (else the `#[cfg]` dispatch would be compiled
  out) — the handler *is* present in the config-on build.
- **#2 genuinely config-driven** (not stale artifacts): `CONFIG_PKVM_EL2_COV=y` in `.config` + `autoconf.h`.
  CONFIG ON → `cov.o` linked, 7446 SanCov calls, `kcov_add_pcs`/callback/`pkvm_cov_setup` in vmlinux,
  callback→`cov.c`, bindgen enum const generated. CONFIG OFF → `cov.nvhe.o` NOT in the hyp link list (gate
  idiom identical to `xcore_unit_test`), no SanCov, clean autoconf. Evidence:
  `evidence/stage2-mvp-2026-07-22/config-driven-verification.txt`.
- **#3 real ordering**: `smp_store_release(&count)` / `smp_load_acquire(&flags)` in `cov.c`, not `barrier()`.
- The `__pkvm_cov_setup` handler uses the **hyp_trace lifecycle template** (`__pkvm_host_share_hyp` by the
  host, then `hyp_phys_to_virt` + `hyp_pin_shared_mem` in `cov.c`); id added **after** the XCORE block under
  the gate, dispatched by an explicit id-check — **unit-test ids unchanged, `HOST_HCALL` table untouched**.

**P0 fixes (2026-07-22, review round 2 — all build clean):**
- **HVC return ABI** (was a real bug): the `__pkvm_cov_setup` dispatch now always sets `a0 = SMCCC_RET_SUCCESS`
  and `a1 = ret` (kvm_host.h: the host's `kvm_call_hyp_nvhe` `WARN_ON`s if a0 != SUCCESS and reads a1). It
  previously put the error in a0 → WARN + undefined a1 on failure.
- **Single-switch honesty**: `kcov_add_pcs` (decl + def + export) is now under `#ifdef CONFIG_PKVM_EL2_COV`
  — config-off leaves no new KCOV symbol.
- **Recursion guard**: a per-CPU `pkvm_cov_in_cb` flag drops nested EL2 callbacks (lose a PC, don't corrupt).

**NOT "only host glue" — two tracks remain (per review):**
1. **Consumer (syzkaller, board-independent).** `pkg/cover/backend/elf.go:getTraceCallbackType` now
   recognizes `__kvm_nvhe___sanitizer_cov_trace_pc` as a trace-pc callback (+unit test), so the EL2
   `bl`-sites in the `__kvm_nvhe_` .text range become coverage points. **Symbolization is already
   validated:** syzkaller symbolizes via `addr2line` (`pkg/symbolizer/addr2line.go`, `-afi`), the exact
   tool that resolves `__kvm_nvhe_` link addresses to `rust/src/*.rs:line` (e.g. `hyp_main.rs:1069`) against
   the debuginfo vmlinux. So once syzkaller receives **link addresses**, the report renders Rust source.

   **The runtime PC → link-address conversion — design fixed, hook NOT wired yet** (done KERNEL-side, not in
   syzkaller). The EL2 callback records a hyp runtime VA; the nVHE hyp-VA is a *tagged* `__kern_hyp_va`
   transform (`va_layout.c`), NOT `hyp_physvirt_offset` (physical-only). The runtime hyp-text base must go
   through **`lm_alias()` first** — matching `hyp_events.c:382` — then `kern_hyp_va`; and because
   `__kern_hyp_va` is only a constant offset *within* the contiguous hyp `.text`, a **range check is
   mandatory**:
   ```c
   link_start = (unsigned long)__hyp_text_start;
   hyp_start  = (unsigned long)kern_hyp_va(lm_alias((unsigned long)__hyp_text_start));  // NOT __kern_hyp_va(__hyp_text_start)
   text_size  = (unsigned long)__hyp_text_end - (unsigned long)__hyp_text_start;
   if (pc < hyp_start || pc - hyp_start >= text_size) drop_pc();   // outside hyp .text
   link_pc = link_start + (pc - hyp_start);
   ```
   **Prerequisite:** `CONFIG_RANDOMIZE_BASE=n` (verified in the fuzz `.config`), so `link_pc` equals the
   offline vmlinux link address. With KASLR on, the runtime kimage address would not match vmlinux.

   **Now implemented (host side, isolated tree):** `arch/arm64/kvm/pkvm_cov.c :: pkvm_cov_runtime_to_link()`
   is exactly the formula above (real `kern_hyp_va(lm_alias(__hyp_text_start))` + range check), gated by
   `kvm-$(CONFIG_PKVM_EL2_COV) += pkvm_cov.o`, and **compiles clean** (`CC arch/arm64/kvm/pkvm_cov.o`). The
   #23 hook **will** call it on each drained PC before `kcov_add_pcs(link_pcs, n)` — still NOT wired
   (`pkvm_mem_abort` has only the plain `__pkvm_host_map_guest` call).

   **Synthetic consumer test — WRITTEN + PASSING** (board-free): `pkg/cover/backend/pkvm_cov_test.go ::
   TestPkvmCovSymbolizePipeline` mirrors the C helper (reversal + range check, *not* re-encoding the
   lm_alias step), then runs the recovered link address through syzkaller's **real** symbolizer. Against a
   `CONFIG_PKVM_EL2_COV` debuginfo vmlinux (opt-in via `PKVM_VMLINUX`) it proves end-to-end:
   `EL2 runtime PC 0xffff8e0081db4780 → link 0xffff800081db4780 → pkvm.rs:4014`, and that out-of-range PCs
   (below start, at/after end) are dropped. Skips cleanly with no artifact; the existing
   `TestGetTraceCallbackType` still passes.
2. **Host glue (board-supervised).** Allocate the ring page, `__pkvm_host_share_hyp` it, `__pkvm_cov_setup`,
   CPU-pin the executor thread; the `pkvm_mem_abort` #23 hook must `smp_store_release(flags, ENABLED)` →
   map → `smp_load_acquire(count)` → convert (above) → `kcov_add_pcs`, on **both** success and failure; and
   **teardown in order**: disable → clear the callback-visible ring pointer (`__pkvm_cov_setup(0)`) →
   `__pkvm_host_unshare_hyp` → free_page (never unshare/free while the ring pointer is still live). Count/log
   ring overflow.

**Do NOT deploy yet** — finish P0 (done) + the consumer first; then host glue is the last kernel path, and
only then the non-default-GRUB deploy + firmware-smoke acceptance (≥1 Rust EL2 PC → `.rs:line`).

**Hook at #23** (`arch/arm64/kvm/mmu.c`, in `pkvm_mem_abort` around the `__pkvm_host_map_guest` call, on the
pinned CPU): reset+`WRITE_ONCE(flags, ENABLED)` → map → `WRITE_ONCE(flags, 0)` → `n = smp_load_acquire(&count)`
→ `kcov_add_pcs(ring->pcs, n)`.

## Increment 3 — CONSUMER (syzkaller, host-side, board-independent): specified
- Recognize EL2 PCs by the `__kvm_nvhe_` address range; apply the boot `__hyp_va` offset (Q1, still to be
  end-to-end checked); resolve against the **same** debuginfo vmlinux (`pkg/cover`/`pkg/symbolizer`).
- The `elf.go` callback-name check must accept `__kvm_nvhe___sanitizer_cov_trace_pc`.

## Acceptance (the closed loop — needs the board)
One controlled `KVM_RUN` → #23 → EL2 Rust executes (instrumented) → PCs in the ring → drained into the
current syz call's KCOV → **syzkaller shows ≥1 Rust EL2 PC symbolized to `.rs:line`** (same-build vmlinux).

## Why stop here autonomously
Increments 2–3 are well-specified but the delivery's Rust-dispatch edit and the `pkvm_mem_abort` hook are
only meaningful once **runtime-validated on N90** — which needs a new fuzz-kernel deploy (board-risky) and
iteration against real behavior. Banking the verified producer + a precise spec, to finish the delivery +
deploy under supervision. The isolated tree `/home/jose/common-stage2mvp` is kept for that next step.
