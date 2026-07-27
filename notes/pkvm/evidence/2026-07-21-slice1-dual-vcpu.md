# Host slice 1 — safe dual-vCPU lifecycle (N90, 2026-07-21)

Test: `sys/linux/test/arm64-syz_kvm_dual_vcpu`. Program: protected VM → **2** `CREATE_VCPU_protected`
(ids 0,1) → both `INIT_safe` (feature=0) → `run_immediate` on vCPU 0 → close both vCPUs → close VM.
No new syzlang — the model already permits ≥2 vCPUs; this pins the "2 inited vCPUs → first run walks
both" ordering and establishes the test→verify→archive discipline.

## Functional acceptance — ALL PASS
```
CREATE_VCPU_protected =0x5, =0x6   (two vCPUs)      INIT_safe =0x0, =0x0
run_immediate = -EINTR (errno 4)   kprobe: pcret <- pkvm_create_hyp_vm ret=0x0   pdestroy fired
dmesg CLEAN (no arch_timer WARN / BUG1 / fault)     all 10 calls executed (explicit closes ran)
```
So 2-vCPU create → first-run hyp-VM build (ret=0) → explicit teardown all work; no BUG1/hang/fd-leak.

## Coverage — honest measurement (dedup matters)
```
single-vCPU unique PCs: 5396      dual-vCPU unique PCs: 5492
NEW in dual (dual - single): 125 PCs      only-in-single (noise): 29 PCs (0 in kvm)
```
Of the +125 new PCs, **only ~4 are in arch/arm64/kvm**: `share_pfn_hyp` (2), `unshare_pfn_hyp` (1),
`find_shared_pfn` (1) — the host→hyp share path for the 2nd vCPU's struct. The other ~121 are
vCPU-management **infrastructure** (`lib/xarray.c` 20, `mm/list_lru.c` 16, `arch/arm64/kernel/fpsimd.c`
11, `signal.c` 7, …), not pKVM-specific.

## Conclusion (validates the earlier refinement)
A 2nd *identical* vCPU adds almost no **pKVM-specific** coverage — KCOV dedups the per-vCPU
`__pkvm_create_hyp_vcpu` body, so multiplicity mostly re-runs the same PCs. Its real worth here is
(a) a **regression test** proving multi-vCPU lifecycle works cleanly, and (b) establishing the
verify/archive discipline. **The host-breadth value is in slice 2 (memslot state machine)**, which
reaches genuinely new pKVM code (the `-EPERM` rejection paths at `mmu.c:2495-2502`) that v1 has no
memory-region calls to touch. Recommend: keep the dual-vCPU regression test; do not spend a dedicated
campaign on it; proceed to the memslot slice.
