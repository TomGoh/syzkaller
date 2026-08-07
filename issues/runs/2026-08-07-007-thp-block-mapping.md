---
id: 2026-08-07-007-thp-block-mapping
kind: reproduction
board: N90 (10.42.27.17)
kernel: '6.6.103+ #25 — klinux pkvm-tlb-flush-range-fix @a89de9c463bc, plus an out-of-tree debug patch adding kvm/pkvm_cov/ring_dump'
cmdline: kvm-arm.mode=protected
config: unchanged from the 2026-08-06 campaign build (CONFIG_X1_RMS=y, CONFIG_PKVM_EL2_COV=y)
filters:
  config_ignores: 'n/a — targeted reproduction, no manager involved'
  reporter_ignores: 'n/a'
enabled_syscalls: 'n/a — one syzkaller program and one standalone C probe'
duration: 2026-08-07 17:10 first isolation, root cause established by 18:10; five power cycles
result: 'issue 007 reproduced deterministically and root-caused: a 2 MiB block mapping in a protected guest makes guest_get_page_state() return PKVM_PAGE_RESTRICTED_PROT alone, no match arm in __pkvm_host_reclaim_page() accepts it, bug_on!() panics, and the Rust EL2 panic handler is loop {} — the CPU never returns and the board must be power cycled'
ring: 'armed on 8 CPUs for the ring_dump capture; also reproduced with the ring fully disarmed (armed_cpus 0)'
observed: [7]
not_observed: []
---

# Run 2026-08-07-007-thp-block-mapping

Targeted reproduction and localisation of issue 007. Not a fuzzing campaign: one syzkaller program (`syz_kvm_run_protected_guest$arm64`, newly added) and one standalone C probe, run under three instruments in turn.

## Why the kernel carries a debug patch

The hang is a hypervisor-level infinite loop. The stuck CPU cannot print, cannot be interrupted, cannot be offlined, and does not answer sysrq — EL2 has no channel to the host once it stops returning. The only surviving record of what EL2 executed is its per-CPU coverage ring in host memory, which `pkvm_cov_end()` never drains because the HVC never returns.

`kvm/pkvm_cov/ring_dump` was added for this run: a read-only debugfs export of the raw, undrained ring, deliberately taking no lock against the producer, so a healthy CPU can read a wedged CPU's ring. Patch archived at `issues/007-protected-block-mapping-el2-panic/repro/ring-dump.patch`. It changes nothing else — the kernel is otherwise byte-identical in source to the `#25` the board already ran, on the same branch (`pkvm-tlb-flush-range-fix`, not `pkvm-el2-kcov`; building on the wrong branch would have silently dropped the 003/004/005/006 fixes and changed more than one variable).

## Isolation

One boot, one sandbox, three programs, one variable:

```
syz_kvm_dirty_log_cycle(hp_mode=0)        ordinary VM              ~1 s   cov 84318   clean
syz_kvm_run_protected_guest(hp_mode=0)    protected, NOHUGEPAGE    ~1 s   cov 69566   clean
syz_kvm_run_protected_guest(hp_mode=1)    protected, THP eligible   46 s  cov     0   WEDGED
```

Rows 2 and 3 differ by one `madvise(MADV_NOHUGEPAGE)`. 46 s is the executor timeout, i.e. the call never returned.

Ruled out by row 1: the syzkaller sandbox's netns churn, and kernel `#25` itself. Ruled out by row 2: running a protected guest as such. Independently, the board had been up 62 minutes and was `Not tainted` when the first soft lockup printed.

## Instruments, in the order they narrowed the problem

1. **Phase markers to `/dev/kmsg`.** Not stderr: ssh dies with the board, so stdout is lost, while the already established `dmesg --follow` keeps streaming. Answer: `KVM_RUN` returns `KVM_EXIT_MMIO` normally; `close(vm)` never returns. Donation is fine, teardown is not.

2. **`function_graph` on `pkvm_destroy_hyp_vm`,** filtered to the probe's pid via `set_ftrace_pid`, `trace_pipe` streamed line by line into `/dev/kmsg` (a whole-buffer write would be truncated to one record). Answer: the **first** `__reclaim_dying_guest_page_call` never returns. The control arm returns from the same call in 12.5 µs.

3. **`ring_dump`,** read by a `setsid`-detached script started *before* the trigger — after the wedge no new ssh login can complete, because sshd's seccomp setup calls `kick_all_cpus_sync()`. Answer, identical twenty seconds apart:

   ```
   cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381
   ```

   `flags=0x1` is a window `pkvm_cov_begin()` armed and `pkvm_cov_end()` never closed. A frozen `count` rules out an instrumented loop, which would have driven `hits` to the 16381 cap and set `OVERFLOW`. EL2 is stopped, not spinning — which redirected the investigation away from the loop hunt it had been on.

4. **`addr2line` on the last PC**, `__pkvm_reclaim_dying_guest_page+0x55c`, resolving through the inline frames to `bindings/mod.rs:300` — the `panic!` inside `bug_on!` — reached from `__pkvm_host_reclaim_page`, whose only `bug_on!` is the `_ =>` arm of the page-state match.

## A false start worth recording

The first `ring_dump` capture came back `flags=0x0 count=0` on every CPU. The probe did not enable KCOV, and `pkvm_cov_begin()` arms the ring only when `kcov_current_trace_pc()` is true, so no window was ever opened. The instrument was reporting an empty ring, not an idle hypervisor.

This also weakened, retroactively, the first attempt at excluding the coverage bridge: with KCOV off on both arms, "ring disarmed" and "ring armed" were running the *same* uninstrumented EL2 path, so the comparison proved less than it appeared to. The exclusion was redone with `probe-thp-kcov.c`, whose KCOV stays enabled across `close(vm)` — validated first on the control arm (`skip_no_kcov 0`, `drains 37`, `el2_hits 5514`, no overflow) before being trusted on the hang.

## Board cost

Five power cycles. Each hit leaves the machine answering ping and refusing ssh, with `cleanup_net` holding `rtnl_lock` forever inside `synchronize_rcu()`. There is no software recovery: the failure is below the host.
