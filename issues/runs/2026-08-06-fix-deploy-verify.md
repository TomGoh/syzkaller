---
id: 2026-08-06-fix-deploy-verify
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #13 SMP Thu Aug 6 13:50:30 — klinux pkvm-lifecycle-fuzzing @24714fe308bb'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~40m, four kernels built and booted
result: '002, 003 and 004 verified fixed on hardware; 005 fix withdrawn — writing the page state back livelocks the guest, 39962 aborts in 3s'
ring: n/a
observed: [5]
not_observed: [2, 3, 4]
---

# Run 2026-08-06-fix-deploy-verify

The first deployment of this session's fixes. Four kernels, because the first one broke the guest and the cause had to be bisected.

| build | contents | outcome |
| --- | --- | --- |
| `#4` | baseline, no fixes | the state all four issues were filed on |
| `#10` | 003 + 004 + 005 | **dirty logging livelocks** |
| `#11` | 003 + 004, 005 hunk reverted | everything works — the bisect |
| `#12` | `#10` plus a `relax_perms` fallback on `-EAGAIN` | still livelocks |
| `#13` | 003 + 004, 005 reverted with the reasoning in a comment | the state the board is left in |

## Verified fixed

**003 — the criterion it was filed with.** Default THP mode, five runs on `#13`:

```
second KVM_RUN ret=0 errno=0 (-) exit=6      x5
```

On `#4` this returned `-1/E2BIG` in 22 of 25. The guest now resumes. `nohuge` and `nodirty` also pass.

**004 — the residual is zero.** After five THP dirty-logging runs plus a `probe-dirtylog-twice` run, `dmesg` contains **no** donation message at all, in either the old wording or the new one. Since the check only prints when the residual is non-zero, zero messages means the accounting now balances. On `#4` it printed on every dirty-logging teardown.

**002 — unblocked and verified, after being stuck all session.** Its stated criterion was a program that runs a vCPU first, then enables dirty logging, then confirms a subsequent guest write reaches the dirty bitmap. `probe-dirtylog-twice` is that program, and on `#13`:

```
after round2:          A(page 16)=1  B(page 32)=0
pgtable.c:654 WARN (kvm_tlb_flush_vmid_range): 0
```

The write-protected page's write lands in the bitmap, and the `SMCCC_RET_NOT_SUPPORTED` warning that defined the issue is gone. This could not have been checked before, because reaching it required a guest that survives enabling dirty logging — which is exactly what 003 prevented. **Fixing 003 is what made 002 verifiable**, which is the reverse of the ordering assumed when 002 was filed.

## Not fixed — 005, and the fix withdrawn

`probe-dirtylog-twice nohuge` on `#13` still gives `A=0 B=1`, and the teardown `WARN` still fires (now `mmu.c:461`, moved by the added code from `:456`). That is expected: the fix was reverted.

It was reverted because it hangs the guest. Full account in `../005-unmap-guest-fails-after-dirty-log/evidence/2026-08-06-one-line-fix-livelocks.txt`; the short version is that `kvm_pgtable_stage2_map()` **refuses permissions-only updates to a guest PTE** by design (`pgtable.c:961-975`), returning `-EAGAIN`, which `user_mem_abort()` turns into "retry" — so the guest re-executes the same store forever. Measured: 39962 guest aborts in three seconds, 39961 returning 1.

The uncomfortable consequence: **the dirty-log handler has only ever worked because it corrupts the page state.** The corruption is what makes the new PTE differ from the old by more than permissions, which is the only reason the mapping is ever installed. Repairing the state without replacing the primitive removes the thing that was making it work.

## Method notes worth keeping

**A reproducer that hangs prints nothing.** All three programs use `alarm()` + `_exit(3)`, and `_exit()` does not flush stdio. Piped output is block-buffered, so a hang produces a completely empty result that looks identical to "the check passed with no findings". The first verification run reported all-zeros across every check and was nearly read as success. Check the exit status: `rc=3` is the alarm. `stdbuf -o0` does not help either — nothing had been written yet.

**`/tmp` is tmpfs and is wiped by the reboot.** Re-copy the binaries after every deploy; a stale run reported `No such file or directory` as `rc=127`.

**Deployment shape on this board.** ostree, so the kernel is `/boot/ostree/kylin-<hash>/vmlinuz-6.6.103+`, not `/boot/vmlinuz-*`, and the live menu is `/boot/grub/grub.cfg` (entry 0 is the pKVM kernel; entry 1 is the stock `6.6.0-76-generic` and is the fallback). `/boot/loader/entries/*.conf` are present but describe the stock kernel only and are not what boots. The known-good `#4` image is kept on the board as `vmlinuz-6.6.103+.knowngood-4`.

**Risk that was accepted knowingly:** N90 still has no out-of-band crash channel, so a kernel that failed to boot would have needed physical access. It was judged acceptable because none of the four changes touch the boot path or EL2 init — they run only when a guest with dirty logging takes a write fault. Four boots later that held, but the risk does not shrink for the next change.

## Scope

Says nothing about issue 001.
