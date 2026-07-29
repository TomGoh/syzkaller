# hyp-tracing fuzz target — status & how to build it (2026-07-29)

Status: **investigated, NOT built.** The census flagged the 10 tracing/event handlers (#42-51) as the
biggest fuzzer-unreached gap. This note records what we verified so a successor can build the target
correctly — including two blockers that must be handled first.

## Reachability — confirmed
The hyp-trace host entry points live in `arch/arm64/kvm/hyp_trace.c` + `hyp_events.c`, exposed under
`/sys/kernel/tracing/hypervisor/` (live on N90; executor runs as **root**, which tracefs requires).
Empirically, a `echo 1 > tracing_on` fired **3 HVCs** (`begin_calls` +3) — tracing genuinely reaches
EL2. The census "never-fired" for tracing means *the current KVM-composite fuzzer doesn't drive it*,
not that it's unreachable.

tracefs → HVC map (9 of 10 handlers reachable; #51 `__pkvm_disable_ftrace` was census SYMBOL-NOT-FOUND):

| path | op | HVCs |
|---|---|---|
| `tracing_on` | write 1 | `__pkvm_update_clock_tracing`(#42), `__pkvm_load_tracing`(#43, uses `kvm_call_refill_hyp_nvhe` → wrapped by the macro), `__pkvm_enable_tracing`(#45) |
| `tracing_on` | write 0 | `__pkvm_enable_tracing(false)` → `__pkvm_teardown_tracing`(#44) |
| `trace` (seq_file — SAFE) | read | `__pkvm_swap_reader_tracing`(#47), `__pkvm_reset_tracing`(#46) |
| `events/hypervisor/<ev>/enable` | write | `__pkvm_enable_event`(#48) |
| `selftest_event` | write | `__pkvm_selftest_event`(#49) |
| (event enable path) | — | `__pkvm_sync_ftrace`(#50) |
| `buffer_size_kb` | write | no HVC directly; sizes the next load's allocation |

## Blocker 1 — the `trace_pipe` variants HANG (must be excluded)
`hyp_trace_pipe_read()` (hyp_trace.c:~623) does `ring_buffer_wait(...)` — an unbounded wait, and
`O_NONBLOCK` is NOT threaded through from `f_flags`. Reading `trace_pipe` / `per_cpu/cpuN/trace_pipe{,_raw}`
with tracing off or idle **blocks forever → syzkaller marks the program `Hanged` → triage skipped** (the
exact coverage=0 failure class from the fw_ipa detour). **Exclude all pipe variants from the descriptors
outright** — O_NONBLOCK won't save you. `trace` (seq_file, `hyp_trace_fops`) is safe and reaches the same
handlers (`swap_reader`/`reset`), so excluding pipes costs no handler coverage.

## Blocker 2 — the target competes with the coverage ring (measure before shipping)
Enabling hyp tracing makes EL2 do extra instrumented work *inside the same HVC windows the pkvm_cov ring
drains*. Baseline `el2_hits_max ≈ 10210` vs `ring_capacity_pcs = 16381` (32 pages) — ~62% full at the
worst window; ~38% headroom is the entire safety budget. If a tracing window pushes past 16381,
`LOST_IN_RING` goes non-zero and **methodology rule 1 is violated for the whole campaign**, silently.
- **`el2_hits_max` must be a smoke-gate OUTPUT**, checked before/after, not read afterward.
- This needs a **KCOV-active run** to measure (a plain `sh` write doesn't arm the ring for capture — no
  KCOV — so it won't move `el2_hits_max`). Measure it in the PoC.
- If it climbs near the cap, raise `PKVM_COV_MAX_PAGES` 32→64 (cap 32765) BEFORE writing descriptors.

## Blocker 3 — the intermittent UAF (finding-hyptrace-uaf-2026-07-29)
Enabling tracing sometimes trips a KASAN slab-use-after-free in generic `rb_allocate_cpu_buffer`
(reached via the hyp writer path). Intermittent (didn't re-fire on a clean enable). syzkaller detects
the KASAN report as a crash (observed: it tore the VM down). A tracing campaign needs this **triaged +
reporter-suppressed** (like the VMID WARN) to make progress, and the generic-vs-pKVM attribution
resolved. See that finding's `FINDING.md`.

## Descriptor sketch (new — `sys/linux/` has only `bpf_trace.txt`, nothing to reuse)
```
resource fd_hyptrace_on[fd]
openat$hyp_tracing_on(fd const[AT_FDCWD], file ptr[in, string["/sys/kernel/tracing/hypervisor/tracing_on"]], flags const[O_RDWR], mode const[0]) fd_hyptrace_on
write$hyp_tracing_on(fd fd_hyptrace_on, buf ptr[in, fmt[dec, int8[0:1]]], len bytesize[buf])
# buffer_size_kb: MUST be bounded — hyp_buffer_size() does val<<10 with no upper clamp (OOM/DoS if unbounded)
write$hyp_buffer_size(fd fd_hyptrace_bufsz, buf ptr[in, fmt[dec, int32[1:4096]]], len bytesize[buf])
# `trace` (seq_file) OK; events/hypervisor/<ev>/enable with real event IDs; selftest_event.
# EXCLUDE trace_pipe / per_cpu/*/trace_pipe{,_raw} (Blocker 1).
```

## Build order for the successor
1. Fix the a2 harness's stale `syz-execprog` on N90 (rebuild manager+executor+execprog at one rev; the
   current N90 execprog is `7828858a`, executor is `df0f201078` → RPC aborts). Needed for any PoC/capture.
2. PoC (KCOV-active) of `tracing_on` write → confirm new `trace.c`/`events` `.rs` lines AND read
   `el2_hits_max` (Blocker 2). Raise the ring if near cap.
3. Write descriptors (no pipes, bounded buffer_size, real event IDs from `events/hypervisor/`).
4. Add reporter suppression for the intermittent UAF (Blocker 3) so the campaign survives it.
5. Smoke-gate each file individually before it enters `enable_syscalls`.
