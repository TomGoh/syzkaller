# a1 macro auto-injection — result (2026-07-28)

**Claim (verified on hardware):** injecting the EL2-coverage arm/drain **once** into the
host→hyp HVC primitive macros auto-covers every host→hyp boundary the fuzzer drives — no
per-call-site code — at **2.4× the coverage** of the hand-wrapped create kernel and **zero
throughput cost**.

## Method
- Kernel `6.6.30+ #47` (built 2026-07-28 15:36) on N90, Image-only deploy, `/lib` intact, box booted clean.
- `KVM_PKVM_COV_HVC` injected into `kvm_call_hyp_nvhe` (kvm_host.h) + `kvm_call_refill_hyp_nvhe`
  (kvm_pkvm.h); the 5 per-site wraps (mmu.c #23, pkvm.c create/teardown) **reverted** (else double-wrap).
- Capture: `scripts/pkvm/a2/a2-run.sh macrocov 5 ./baseline.prog` — the SAME `baseline.prog`
  (`openat$kvm; syz_kvm_run_fw_fault_gen$arm64(fd, 0, 0x7fc00000, hp_mode=0)`) and SAME 5-repeat
  set-algebra method used for the create-wrap baseline, so the numbers are directly comparable.
- Throughput: fresh-workdir `syz-manager`, ~2.5 min (see `throughput-ringstats.txt`).

## Numbers (all measured, files in this dir)

| kernel | boundaries instrumented | EL2 PCs (baseline.prog ×5 union) | distinct `.rs:line` |
|---|---|---|---|
| map-only (#23) | 1 | ~127 | ~79 |
| create-wrap (per-site) | 3 (map + VM/vCPU create) | 229 (`a1create-before.union`) | ~133 |
| **macro auto-inject** | **all ~46 host→hyp HVCs the lifecycle hits** | **540 (`macrocov.union`)** | **298 (`macrocov.lines`)** |

Per-run: el2_pcs 518–533, drains 255–259, `LOST_IN_RING=0`, `LOST_IN_KCOV=0`, `skip_not_owner=0`,
`dmesg_warn=0`, rc=0 — all 5 runs (`macrocov.log`). Set-algebra: `B_union=540 B_common=512
instability=28` (run-to-run allocator-state variance, same character as the create path).

Throughput (`throughput-ringstats.txt`): **13–14 exec/s, unchanged** from the pre-macro baseline,
despite ~815 HVC wraps/exec (`begin_calls=2,099,738` over 2576 execs). `el2_written==el2_hits`
(510M, zero dropped), `el2_overflow_drains=0`, `LOST_IN_RING=0`, `LOST_IN_KCOV_AREA=0`; corpus 0→35.

## Why safe (per CLAUDE.md: hypothesis was confirmed empirically, not asserted)
The colleague's standing "never global-wrap `kvm_call_hyp_nvhe`" warning is about the **naive**
version. Both of its hazards are handled:
1. **Sleep-in-window RCU-UAF** — the plain macro is a single atomic HVC; the refill macro wraps
   **only** its inner atomic HVC, leaving the sleeping `__pkvm_topup_hyp_alloc` topup outside the
   begin/end window. No sleep between arm and drain.
2. **Wrong context** — `pkvm_cov_begin()`'s existing self-guards (`ring==NULL` / `smp_processor_id()
   != owner_cpu` / `!kcov_current_trace_pc()`) make every non-fuzzer / off-owner-CPU / out-of-KCOV
   HVC a cheap no-op. Measured: `skip_not_owner=0`, `skip_no_kcov=9909` (out-of-window HVCs correctly
   skipped), and throughput is unchanged — the fast path is genuinely cheap.

Residual caveat (unchanged from the accepted create-boundary one): `pkvm_cov_begin` runs in a
preemptible context for some sites, so `pkvm_cov_disable()` (operator-only) must not race a live
campaign — disarm the ring only when idle.

## Reproduce
1. Apply `../kernel-instrumentation-2026-07-27/stage2-el2-kcov.patch` onto common-stage2mvp
   `da966ce9a047`; `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image`.
2. Image-only deploy to N90 (`kylin-v10-kernel-deploy`), boot `6.6.30+ pKVM EL2-cov smoke`.
3. Arm ring: `echo 32 > .../nr_pages; taskset -c 0 sh -c 'echo 1 > .../enable'`.
4. `scripts/pkvm/a2/a2-run.sh macrocov 5 ./baseline.prog` — expect union ≫ 229, `LOST_IN_RING=0`.
