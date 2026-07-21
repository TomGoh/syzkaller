# Host slice 2 — controlled memslot state machine (N90, 2026-07-21)

Reaches the two DISTINCT pKVM memslot rejects in `kvm_arch_prepare_memory_region`
(`arch/arm64/kvm/mmu.c:2492-2502`), which have different preconditions and are tested separately.

## Design (narrow, reliable v0)
- **Executor-owned backing** — `syz_kvm_register_memslot` mmaps a fixed 1-page RW backing and checks
  the ioctl return; the fuzzer never supplies a userspace pointer (unlike `setup_vm`, which registers
  READONLY/LOG_DIRTY slots and ignores the return).
- **State token** — register yields `kvm_memslot`; `delete`/`move` consume it, so they operate on a
  slot actually registered on the same pVM (without this, generated programs mostly hit non-existent
  slots → generic errors, not the pKVM branch).
- Fixed slot 0, one page at GPA `0x40000000` (well within a ≥32-bit IPA), flags fuzzed only within
  `{0, LOG_DIRTY, READONLY}`. **No guest memory is executed** (pages never reach EL2).

## Functional acceptance — ALL PASS (N90, syz-execprog -debug)
```
A) DELETE after run : register=0x0(ok)  run_immediate=-EINTR  memslot_delete = -1 errno=1 (EPERM)  pcret ret=0x0  BUG1=0
B) MOVE   after run : register=0x0(ok)  run_immediate=-EINTR  memslot_move   = -1 errno=1 (EPERM)  pcret ret=0x0  BUG1=0
C) DIRTY, no run    : register_memslot_flags(LOG_DIRTY) = -1 errno=1 (EPERM)   (no vCPU, no run)   BUG1=0
```
Matches the kernel exactly: normal register succeeds before run; dirty/readonly rejects with only
`pkvm.enabled` (no run); delete/move reject only after the first run sets `pkvm.handle`.

## Coverage — explicable new pKVM/mmu code (acceptance #4)
vs the Step-1 baseline (single-vCPU lifecycle, no memory-region calls):
```
baseline unique PCs: 5177    memslot unique PCs: 5596    NEW in memslot: 468 PCs
```
The +468 include the entire memslot subsystem the baseline never touched (75 unique kvm func:line;
`slice2-memslot-raw/ms_delta_kvm_symbolized.txt`), headed by:
```
kvm_arch_prepare_memory_region (12)   ← the mmu.c function holding the pKVM -EPERM rejects
__kvm_set_memory_region (19)  kvm_set_memslot (5)  kvm_commit_memory_region (7)  kvm_prepare_memory_region (5)
kvm_replace_memslot / kvm_replace_gfn_node / kvm_swap_active_memslots / kvm_activate_memslot / kvm_check_memslot_overlap …
```
This is genuine memslot-subsystem + pKVM-reject coverage, **not** generic MM noise — the real
host-breadth gain (vs Step 1's ~4 infrastructure PCs).

## Artifacts (slice2-memslot-raw/)
`ms_delta_pcs.txt` (raw new PCs), `ms_delta_kvm_symbolized.txt` (kvm func:line), `ms_{delete,move,dirty}.syz`.

## Not done here / next
Short campaign with these calls in the allowlist (confirm they enter corpus under fuzzing) →
then commit slice 1 + slice 2 together. Deeper (real guest page-fault / donation) needs a
protected-guest execution channel (separate subproject), not `run_immediate`.
