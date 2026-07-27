# Host slice 2 — controlled memslot rejects, COMPOSITE model (N90, 2026-07-21)

Reaches the two DISTINCT pKVM memslot rejects in `kvm_arch_prepare_memory_region`
(`arch/arm64/kvm/mmu.c:2492-2502`), which have different preconditions and are tested separately.

## Why composite (supersedes the earlier fragment/subtype design)

The first cut modelled this as fragments (`syz_kvm_register_memslot` → `kvm_memslot` token →
`syz_kvm_memslot_{delete,move}` consuming it) plus state-carrying resource subtypes
(`fd_kvmvm_pslot`, `fd_kvmvm_pslot_run`). A corpus program `setup_protected_vm(r1); memslot_move(r1)`
exposed the flaw: **syzkaller resource compatibility is permissive** (prefix-based and bidirectional,
`prog/resources.go:109`), so a supertype `fd_kvmvm_protected` resource satisfies a subtype arg — the
type system CANNOT enforce a "this VM has already run" precondition. A fragment `memslot_run` that
returned its input fd on a failed mmap/run also faked success (colleague catch).

Fix: **self-contained composite pseudo-calls**. Each takes only the `/dev/kvm` fd and builds the whole
required kernel state internally, in C, so the fuzzer cannot reorder it or inject a wrong VM/state:
- `syz_kvm_memslot_reject_delete$arm64(fd_kvm)` — create bit-31 pVM → register slot 0 → create vCPU →
  `INIT_safe` → controlled `immediate_exit` run (**required to return -EINTR**, so `pkvm.handle` really
  is set — no faked state) → `DELETE` slot 0.
- `syz_kvm_memslot_reject_move$arm64(fd_kvm)` — same, but `MOVE` slot 0 to an adjacent page.
- `syz_kvm_memslot_reject_flags$arm64(fd_kvm, flags[1:3])` — create bit-31 pVM → register slot 0 with a
  dirty/readonly flag (no vCPU, no run). `flags` is a non-zero range, so it can never degenerate to a
  normal register.

Building **only bit-31 protected VMs** internally also keeps the campaign off BUG2
(`kvm_tlb_flush_vmid_range`, `pgtable.c:639`), which needs a *normal* VM with dirty-logging — see
`2026-07-21-BUG2-kvm_tlb_flush_vmid_range-MINIMIZED.md`. No guest memory is executed.

## Functional acceptance — ALL PASS (N90, syz-execprog -debug, fresh executor)
```
delete   : syz_kvm_memslot_reject_delete$arm64 = -1 errno=1 (EPERM)   cover=115141
move     : syz_kvm_memslot_reject_move$arm64   = -1 errno=1 (EPERM)   cover=118541
dirty    : syz_kvm_memslot_reject_flags(0x1)   = -1 errno=1 (EPERM)   cover=100530
readonly : syz_kvm_memslot_reject_flags(0x2)   = -1 errno=1 (EPERM)   cover=98355
dmesg after all four: 0 WARN / 0 BUG (no arch_timer BUG1, no pgtable.c:639 BUG2)
```
Matches the kernel exactly: dirty/readonly reject with only `pkvm.enabled` (no run, ~98–100k cover for
create+register on a pVM); delete/move reject only after the first run sets `pkvm.handle` (the extra
vCPU INIT + immediate_exit run pushes cover to ~115–118k). Each composite is a single fuzzer-visible
call, so the sequence is guaranteed regardless of how the fuzzer orders the program.

## Corpus/campaign
A fresh-workdir campaign (`workdir-composite/`, no reused corpus.db) enables exactly the 10 lifecycle +
composite calls (`syscalls: 10/8060`); the composites enter the corpus under fuzzing and coverage grows
with 0 crashes. (Details in the implementation record §6.5.)

## Superseded artifacts
The fragment-era raw data (`slice2-memslot-raw/`, `campaign-slice2-2026-07-21/`) predates the composite
rewrite and reflects the old `register/run/delete/move` calls; kept only for history.
