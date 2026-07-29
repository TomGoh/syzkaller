# Why the same deadlock panics two boards and only warns on a third (2026-07-29)

Companion to `FINDING.md`. The `pkvm_unmap_range()` self-deadlock was reproduced on three machines, but they behaved very differently — two died outright, one stayed usable. This document explains why, because it changes how any future hang on this project should be investigated.

Provenance labels as elsewhere: **[VERIFIED-HW]** read live from the machine, **[VERIFIED-LOCAL]** read from the source tree, **[INFERENCE]** reasoned from those, not directly observed.

---

## 1. The observed difference

| board | kernel | outcome of the identical deadlock |
|---|---|---|
| N90 | `6.6.30+ #47` | machine dead, manual power cycle needed |
| D3000 | `6.6.30-pkvmcov #49` | **`Kernel panic - not syncing: hung_task: blocked tasks`** on serial |
| oct-pc | `6.6.30+ #176 (OE)` | only `INFO: task ... blocked`; **machine stayed up and ssh-able** |

## 2. Cause: one sysctl **[VERIFIED-HW]**

Read live from all three:

| board | `hung_task_panic` | `hung_task_timeout_secs` | `panic` |
|---|---|---|---|
| N90 | **1** | 120 | 0 |
| D3000 | **1** | 120 | 0 |
| oct-pc | **0** | 120 | 0 |

`kernel.hung_task_panic=1` makes the hung-task watchdog call `panic()` once a task has been blocked past the timeout; `=0` makes it only print `INFO: task ... blocked for more than N seconds`. **The deadlock is identical on all three — only the kernel's reaction differs.**

`kernel.panic=0` on all three means the kernel **halts** after panicking instead of rebooting, which is why N90 and D3000 always needed a physical power cycle rather than recovering on their own.

(The differing "blocked for more than 122/362 seconds" figures are just which watchdog pass noticed the task; the timeout is 120s on every board. syzkaller additionally sets `hung_task_check_interval_secs=20` on the boards it drives, so they re-report sooner.)

## 3. It is **not** syzkaller doing this **[VERIFIED-LOCAL]**

The executor writes a fixed list of sysctls in `executor/common_linux.h`. The only hung-task entry is:

```c
	{"/proc/sys/kernel/hung_task_check_interval_secs", "20"},
```

It never touches `hung_task_panic`. Nor does `pkg/instance/` or `vm/isolated/`.

## 4. Where the setting actually comes from — our own config **[VERIFIED-LOCAL]**

```
$ git diff arch/arm64/configs/gwd3000_lenovox1d3000m_defconfig
+CONFIG_DETECT_HUNG_TASK=y
+CONFIG_BOOTPARAM_HUNG_TASK_PANIC=y
```

Both lines appear as **additions in this project's uncommitted instrumentation diff**. `CONFIG_BOOTPARAM_HUNG_TASK_PANIC=y` sets the boot-time default of `kernel.hung_task_panic` to 1, matching what N90 and D3000 report live. oct-pc runs a kernel built without them, so it sits at the upstream default of 0.

So the answer to "why is it 1 on our kernels?" is: **we enabled it deliberately when building the fuzzing kernel.** It is standard practice for a syzkaller target and the upstream syzkaller documentation recommends it.

### Why that was the right call

- **A hang must be attributable.** Panicking freezes the machine at the moment of the hang with the blocked task's stack in the log. Without it the box limps on, other tasks pile into the same wedged lock, and the first victim becomes impossible to identify.
- **A fuzzer must not silently continue on a broken kernel.** After a hung task the kernel is in an undefined state; coverage or crashes found afterwards are untrustworthy.
- **A panic is reliably detected; a lone `INFO:` line is not.** syzkaller notices a dead VM. A warning it was told to ignore can vanish into the noise — exactly what happened with the TLB-flush warnings we later had to suppress.

The irony is worth recording: this setting is a large part of *why the deadlock was expensive today*. On oct-pc the same bug is a nuisance; on our boards it is fatal, and with `panic=0` it halts rather than reboots, so each hit cost a physical power cycle. **We made the bug maximally disruptive on purpose, and it was correct** — it is what turned a vague "the board got slow" into three clean, attributable stack traces.

## 5. The capture-path trap: N90 panicked too, and we never saw it

**[VERIFIED-HW]** N90 has `hung_task_panic=1`, so the watchdog must have panicked. **[VERIFIED-LOCAL]** Yet all four saved N90 reports (`report0`-`report3`) contain **zero** `Kernel panic` lines — only the `INFO: task ... blocked` messages.

**[INFERENCE]** The panic text was lost to the capture path, not absent. syzkaller reads the kernel log over **ssh** (`dmesg -w` → `/dev/kmsg`, see `vm/vmimpl/console.go`). When the kernel panics it stops scheduling, the ssh session dies, and anything not already flushed through the pipe is gone — so the `INFO:` lines printed at t=120s were banked and the panic that followed milliseconds later was not.

D3000's panic was captured **only** because it was being watched on the **serial console** (`picocom -b 115200`, `console=ttyAMA1,115200 earlycon`) — a hardware path that keeps working through a panic.

**Operational rule this establishes:** for hang and panic hunting on these boards, the network log is guaranteed to truncate at exactly the most interesting moment. **Serial capture is not optional.** This retroactively justifies the `earlycon` + `console=ttyAMA1,115200` work done on D3000, and means N90's `console=ttyAMA0,115200 earlycon` should be watched the same way for any future hang.

## 6. Practical implications

- **Best board for *diagnosing* a hang:** oct-pc-style (`hung_task_panic=0`) — the machine stays up and ssh-able, so `/proc/<pid>/stack`, `sysrq-t`, and the full task list can be inspected live. The caveat is that a wedged `mmap_lock` eventually blocks anything touching that mm.
- **Best board for *detecting* a hang during fuzzing:** N90/D3000-style (`hung_task_panic=1`) — attributable, and syzkaller reliably reports it.
- **`panic=0` costs recovery time.** Setting `kernel.panic=30` would auto-reboot 30s after a panic. Now that N90's GRUB reliably defaults to the instrumented kernel (see the GRUB work of 2026-07-29), that would have removed most of today's manual power cycles for an unattended run. The trade-off is that a halted machine preserves post-mortem state a reboot destroys.
- **Serial should be attached and recorded on any board used for hang work**, for the reason in §5.
