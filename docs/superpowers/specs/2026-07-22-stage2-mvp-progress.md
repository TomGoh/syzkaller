# Stage-2 EL2-KCOV MVP — progress + delivery spec (2026-07-22)

Scope: the minimal closed loop for one boundary (#23 `__pkvm_host_map_guest`), per the approved plan. All
work in an isolated kernel tree (`/home/jose/common-stage2mvp`); **N90 was not touched**. Evidence +
patch: `evidence/stage2-mvp-2026-07-22/`.

## Increment 1 — PRODUCER: DONE and verified (builds, no board needed)

The Rust hyp is now SanCov-instrumented and its callback lands in a hyp ring, while staying symbolizable.

- **Build flags** (`build_rust.sh`): `-C debuginfo=2` + `-C passes=sancov-module -C llvm-args=-sanitizer-coverage-level=3
  -C llvm-args=-sanitizer-coverage-trace-pc` (+ `CARGO_PROFILE_RELEASE_DEBUG=2`). See `producer.patch`.
- **Callback** (`arch/arm64/kvm/hyp/nvhe/cov.c`, uninstrumented nvhe C): `__sanitizer_cov_trace_pc()` records
  `__builtin_return_address(0)` into a per-CPU ring (`struct pkvm_cov_ring`, `asm/kvm_pkvm_cov.h`), gated by
  an enable flag; plus `pkvm_cov_set_enabled()` / `pkvm_cov_snapshot()`. Added `cov.o` to the nvhe Makefile.
- **Verified in the linked vmlinux:**
  - `nvhe_rust.o` has **7443 `__sanitizer_cov_trace_pc` call sites** (the whole Rust hyp is instrumented);
  - `__kvm_nvhe___sanitizer_cov_trace_pc` is defined and resolves to `cov.c:23` — all call sites link;
  - `pkvm_cov_set_enabled` / `pkvm_cov_snapshot` are present in the hyp image;
  - **debuginfo + SanCov coexist**: `handle___pkvm_host_map_guest` still resolves to `hyp_main.rs:1069`.

So instrumented-Rust → our callback → ring, and the PCs remain symbolizable — the two hard, novel,
board-independent pieces. `evidence/stage2-mvp-2026-07-22/increment1-verification.txt`.

## Increment 2 — DELIVERY: specified (needs the hypercall + host hook + a board deploy to validate)

Design (single hypercall + shared-page flags, to minimise the Rust-dispatch surface):
1. **Shared ring page.** Change the per-CPU ring from static hyp memory to a **host-shared page**
   (`kvm_share_hyp`), and add an `enabled` field to `struct pkvm_cov_ring`. The host toggles `enabled` and
   reads `count`/`pcs[]` directly via the shared mapping — no per-op hypercall.
2. **One hypercall `__pkvm_cov_setup(pfn)`** (register the ring page for this CPU). Concretely:
   - `asm/kvm_asm.h`: add `__KVM_HOST_SMCCC_FUNC___pkvm_cov_setup` (new id, after the current last ~65).
   - `hyp/nvhe/rust/src/hyp_main.rs`: add `handle___pkvm_cov_setup(&mut self)` reading `cpu_reg(1)` = pfn,
     convert pfn→hyp-VA, call a C `pkvm_cov_set_ring(ptr)`; extend the `HOST_HCALL` table (`[…; 66]` → 67)
     at the new id.
   - `cov.c`: `pkvm_cov_set_ring()` stores the per-CPU pointer; `__sanitizer_cov_trace_pc` writes to it.
3. **KCOV core `kcov_add_pcs(u64 *pcs, u32 n)`** (new, `kernel/kcov.c`): if the current task is in
   `KCOV_MODE_TRACE_PC`, append each PC to `t->kcov_area` following the existing protocol
   (`pos = area[0]+1; if (pos < kcov_size) { area[pos]=pc; area[0]=pos; }`).
4. **Hook at #23** (`arch/arm64/kvm/mmu.c`, in `pkvm_mem_abort` around the `__pkvm_host_map_guest` call):
   `ring->count=0; ring->enabled=1;` → do the map → `ring->enabled=0; kcov_add_pcs(ring->pcs, ring->count);`.
   `procs:1` + **CPU-pin** the fuzzer vcpu thread so the per-CPU ring is unambiguous.

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
