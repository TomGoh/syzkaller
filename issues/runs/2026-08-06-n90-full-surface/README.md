# Artifacts — campaign `2026-08-06-n90-full-surface`

Full record of the overnight N90 campaign. The narrative and the judgements live in [`../2026-08-06-n90-full-surface.md`](../2026-08-06-n90-full-surface.md); this directory is the raw material behind it.

**Snapshot taken 2026-08-07 08:51 (+08), 17h03m in. The campaign was still running at that moment** — it was deliberately not stopped to take this archive. Anything time-stamped after 08:51 is not in here.

## What syzkaller does and does not save — and the gap it leaves

**Read this first, because the short version is easy to get backwards:** both WARN events are fully recorded and nothing about them was lost. What was lost is the console of the sessions that did *not* crash.

| source | has the two WARNs? | why |
| --- | --- | --- |
| board `dmesg` | **no** | ring buffer holds ~90 min; crash 2 aged out ~20 min after it fired |
| syzkaller `crashes/*/log0`,`log1` | **yes, complete** | full console of each crashing executor session |
| this archive, `crashes/` | **yes** | verbatim copy of the above |
| `console-continuous.log` (from 09:03) | no | started later; its first line is kernel time 60993, crash 2 is at 60674 |

If you are looking at a WARN stack with an untimestamped `SYZFAIL: rpc peer closed connection (EOF)` spliced into the middle of the call trace, you are reading a syzkaller `log<N>` — that line is executor stdout, which `dmesg` cannot produce. That is the merged console+executor stream, and it is the authoritative record here.

syzkaller **does** save console output, and more of it than "a window around the crash": each `crashes/*/log<N>` is the console for the **entire executor session** up to that crash.

```
log0   kernel time 47492 -> 52925   (1h31m)   ending at crash 1
log1   kernel time 53323 -> 60675   (2h02m)   ending at crash 2
```

**But it only keeps sessions that crashed.** The campaign ran four clean 3-hour windows before 05:22 — 18:49, 21:49, 00:49, 03:49 — and syzkaller discarded every one of their consoles, because nothing in them tripped the reporter. Roughly the first 12 hours of this run has no console record and never will.

Nor can the board fill the gap. The kernel ring buffer there was measured on 2026-08-07 09:03:

```
retained span   5315 s = 1.5 hours
size              85 KB
implied rate      58 KB/hour  =  1.3 MB/day
```

**The buffer holds about 90 minutes at this workload.** Crash 2 is at kernel time 60675; the oldest surviving dmesg line was 60993 — it had already been pushed out, ~20 minutes after it happened. That is why `dmesg | grep -c kvm_unshare_hyp` returns 0 on a board where the WARN fired twice.

So for the archived portion, `crashes/*/log*` is the only record, and it is complete for the two sessions that matter.

### Continuous capture, started 2026-08-07 09:03

To stop losing clean sessions from here on, a reconnecting follower now streams the board's console to a file outside this repo:

```
ssh root@10.42.27.17 'dmesg --follow' >> /home/jose/syzkaller/workdir-n90-full/console-continuous.log
```

It is `setsid nohup`'d with a 5 s reconnect loop, so it survives ssh drops and the agent session, and it stamps `[capture] ssh dropped <ts>, reconnecting` into the stream so gaps are visible rather than silent. At 1.3 MB/day it is free.

It is deliberately **not** committed here as a live file — it grows. Gzip a copy into this directory when the campaign is stopped:

```
gzip -c /home/jose/syzkaller/workdir-n90-full/console-continuous.log > console-continuous.log.gz
```

Note what it can and cannot recover: it began at kernel time 60993, so it holds everything from 09:03 onward **plus** the ~90 minutes the buffer still had. It cannot recover the four clean windows — those are gone for good.

## Contents

| path | what it is |
| --- | --- |
| `n90-full.cfg` | the exact manager config, copied verbatim |
| `enabled-syscalls.txt` | the 111 config entries (the manager resolves these onto 91 actual calls) |
| `manager.log.gz` | complete manager log, start to snapshot |
| `final-stats.txt` | last stats line at snapshot time |
| `hourly-trajectory.txt` | one stats line per hour — the corpus/coverage curve |
| `crash-timeline.txt` | every VM restart, crash, and repro attempt, 15 lines |
| `baseline-2026-08-06T1748.txt` | frozen 2h-in baseline, so the morning numbers read as a delta |
| `board-health-final.txt` | uptime, load, D-state, memory, dmesg counts at snapshot |
| `pkvm_cov-stats-final.txt` | EL2 coverage ring counters at snapshot |
| `crashes/` | complete copy of both occurrences: `log0`/`log1` (full session console), `report0`/`report1`, `repro0`/`repro1`, `machineInfo*`, `title-stat` |
| `corpus.db` | the live corpus, 780 programs — the reusable output of 17 h of fuzzing |
| `http-snapshots/stats.html` | manager `/stats` page, every counter it tracks |
| `http-snapshots/syscalls.html` | per-syscall coverage |
| `http-snapshots/crash_id_*.html` | the manager's own crash page |
| `coverage/rawcover-2026-08-07T0535.txt` | 13987 raw PCs |
| `coverage/hypercall-handlers-covered.txt` | the 16 EL2 handlers reached, of 68 |
| `coverage/el2-rs-files.txt` | EL2 `.rs` PC distribution per file, 525 total |

## Headline numbers

```
started    2026-08-06 15:48
snapshot   2026-08-07 08:51        (17h03m, still running)
corpus     780
coverage   14039                   (host+EL2 PCs, syzkaller's counter)
exec total 105751  (103/min)
syscalls   91 enabled, 0 filtered at the config layer
WARN filters: none — all three kernel WARN ignores removed for this run

crash types  1
crashes      2      both "WARNING in kvm_unshare_hyp"  -> issue 006
repro        0      two extraction attempts, both failed

EL2 ring   LOST_IN_RING 0, link_dropped 0
EL2 PCs    525 distinct .rs lines across 20 files
handlers   16 of 68 hypercall handlers reached
```

## The one pattern in the timeline worth noticing

```
2026/08/06 18:49  VM 0: running for 3h0m, restarting     <- clean
2026/08/06 21:49  VM 0: running for 3h0m, restarting     <- clean
2026/08/07 00:49  VM 0: running for 3h0m, restarting     <- clean
2026/08/07 03:49  VM 0: running for 3h0m, restarting     <- clean
2026/08/07 05:22  VM 0: crash: WARNING in kvm_unshare_hyp
2026/08/07 07:30  VM 0: crash: WARNING in kvm_unshare_hyp
```

**Four clean 3-hour windows, then two crashes in the final 3.5 hours.** The board itself never rebooted — these are executor-session restarts on syzkaller's 3 h timer, and board uptime ran continuously to 18:12.

That shape is *consistent with* something accumulating across the run, which is also the leading explanation for why neither occurrence reproduces from its own log (issue 006, evidence §7). It is **not evidence of accumulation** — two events is far too few to distinguish accumulation from chance, and the crashes fell 1h33m and 2h07m into their respective windows, which is no clean pattern at all. Recorded as an observation to test, not a conclusion.

## Reproducing the conditions

```
config:   issues/runs/2026-08-06-n90-full-surface/n90-full.cfg
workdir:  /home/jose/syzkaller/workdir-n90-full
http:     0.0.0.0:56760
manager:  bin/syz-manager @ 9fd30654e444ec028612efafdf1e3b00939ef685+
kernel:   6.6.103+ #16, Source Version cba248683e5c0c72a46a3cfbb3972b53b4162868
cmdline:  kvm-arm.mode=protected
ring:     echo 32 > /sys/kernel/debug/kvm/pkvm_cov/nr_pages
          taskset -c 0 sh -c 'echo 1 > /sys/kernel/debug/kvm/pkvm_cov/enable'
```

Note the kernel's embedded `Source Version` is `cba248683e5c`, while the run record's `kernel:` field names `@a35f0a8c899e` — the working tree had moved on by the time the record was written. **`cba248683e5c` is the authoritative revision for anything read out of this run**; read code with `git show cba248683e5c:<path>`.
