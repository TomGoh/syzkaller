---
id: 2026-08-05-dirtylog-e2big
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #4 SMP Wed Aug 5 11:28:51 — klinux pkvm-unmap-deadlock-fix @348c94763cc6'
cmdline: kvm-arm.mode=protected
manager_rev: n/a (no syz-manager involved)
config: n/a
filters:
  config_ignores: n/a
  reporter_ignores: n/a
enabled_syscalls: n/a
duration: ~4m, 3 modes x 5 runs
result: 'THP+dirty-logging fails KVM_RUN 5/5 with -E2BIG; 4K+dirty-logging 0/5; THP without dirty logging 0/5'
ring: n/a
observed: [2, 3, 4]
not_observed: []
---

# Run 2026-08-05-dirtylog-e2big

The run that found issue 003, and the first time this project executed the sequence *ordinary VM → run a vCPU → enable dirty logging → resume*. It exists because of issue 002: review established that 002's reproducer could not verify 002's fix, since it creates no vCPU and so leaves `pkvm.handle` at 0. Writing a verifier that runs a vCPU first produced this instead.

Kernel under test is `#4`, which carries the issue 001 fix and **not** the issue 002 fix — so nothing here depends on the 002 patch.

## The matrix

`repro/probe-dirtylog-thp.c` from the 003 directory, three modes, five runs each:

| mode | guest memory | dirty logging | `KVM_RUN` failures | `donations…missing` | `pgtable.c:654` warns |
| --- | --- | --- | --- | --- | --- |
| default | 2 MiB THP block | on | **5/5** | 5 | 5 |
| `nohuge` | forced 4 KiB (`MADV_NOHUGEPAGE`) | on | **0/5** | 4–5 | 5 |
| `nodirty` | 2 MiB THP block | off | **0/5** | **0** | **0** |

Only the granularity of the backing memory differs between the first two rows, and only the dirty-logging step between the first and third.

Three separate conclusions come out of it:

- **Issue 003 (`-E2BIG`) needs both huge pages and dirty logging.** Either one alone is fine.
- **Issue 002's warning is keyed to enabling dirty logging alone** — 5/5 in both dirty-logging modes, 0 without, and unaffected by page size or by whether `KVM_RUN` then fails.
- **The `donations … are missing` message is a third, independent thing.** It tracks dirty logging, not huge pages, and appears whether the second `KVM_RUN` succeeds or fails. It is *not* a consequence of the `-E2BIG` failure — it is [issue 004](../004-hyp-donation-accounting-imbalance/ISSUE.md).

## Method trap: the teardown message needs a settle

`kvm_arch_destroy_vm()` prints the donation check after the process has exited, so reading `dmesg` immediately after the program returns misses it **intermittently**. Two earlier passes of this experiment were read that way and produced 0 and then 1 for the same mode, which briefly supported the wrong conclusion that the accounting imbalance was a consequence of the `-E2BIG`.

The counts above come from five runs followed by a three-second `dmesg -w` settle. Even then the last run's message can land after the window — hence 4–5 rather than 5 for `nohuge`. The discriminating cell is `nodirty`, which is a hard 0 across five runs.

Do not read a teardown-time printk synchronously after the process exits.

## Scope

Says nothing about issue 001: no second `KVM_ARM_VCPU_INIT` is issued.
