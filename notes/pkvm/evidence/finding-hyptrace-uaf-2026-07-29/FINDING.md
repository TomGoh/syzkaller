# Finding: slab-use-after-free reached via the hyp-trace enable path (2026-07-29)

**KASAN slab-use-after-free**, N90 kernel 6.6.30+ #47 (KASAN=y). Real, KASAN-caught (read; box continued).

## Corrected mechanism (an earlier draft of this file was WRONG — see "Retraction")
- **Trigger:** userspace write "1" to `/sys/kernel/tracing/hypervisor/tracing_on` (after a `buffer_size_kb`
  resize), i.e. enabling hyp tracing. Done during MANUAL probing (task `sh`), NOT the KVM-composite
  campaign (which never touches tracing).
- **Where it fires:** a 4-byte READ of freed memory in `rb_allocate_cpu_buffer+0x2bc`, via
  `hyp_tracing_on → hyp_trace_start → hyp_trace_buffer_load → ring_buffer_reader(&writer) →
  __ring_buffer_alloc → alloc_buffer → __cpuhp_state_add_instance → cpuhp_invoke_callback →
  trace_rb_cpu_prepare → rb_allocate_cpu_buffer`.
- **The faulting code is GENERIC ftrace** (`kernel/trace/ring_buffer.c`: `rb_allocate_cpu_buffer` /
  `trace_rb_cpu_prepare`), not pKVM. The hyp-trace path is only the *caller* (`hyp_trace_buffer_load`
  at hyp_trace.c:340 via `ring_buffer_reader(&hyp_buffer->writer)`).
- **The freed object** is a `names_cache` (4096B) pathname buffer from an UNRELATED task (2178:
  `getname` on an `openat`, freed at that syscall's audit exit `putname`/`audit_reset_context`).
  So the per-CPU-buffer prepare reads a dangling pointer into a recycled slab region.
- Full report: `kasan-uaf-dmesg.txt`.

## HVC ordering (why this is NOT "before the EL2 handlers")
`hyp_trace_start` (hyp_trace.c:383): `hyp_clock_start` (:393, plausibly `__pkvm_update_clock_tracing` #42)
→ `hyp_trace_buffer_load` (:395). Inside load: `kvm_call_refill_hyp_nvhe(__pkvm_load_tracing, …)` (:332,
#43) — control only proceeds to `ring_buffer_reader → __ring_buffer_alloc` (:340, the UAF site) **if
that HVC returned 0** (else `goto err_teardown_pages` :335). So **#43 (and likely #42) fired and
SUCCEEDED before the UAF**; only `__pkvm_enable_tracing` (:401, #45) and downstream are cut off. Tracing
DOES reach EL2 handlers — the UAF sits *between* the load and the enable, not in front.

## Empirical checks (2026-07-29, manager stopped, ring armed owner_cpu=0)
- **Tracing reaches EL2 HVCs — confirmed.** A clean `echo 1 > tracing_on` (on CPU 0) incremented
  `begin_calls` by **3** — i.e. 3 host→hyp HVCs fired (plausibly #42 `__pkvm_update_clock_tracing`,
  #43 `__pkvm_load_tracing`, #45 `__pkvm_enable_tracing`). This **falsifies "tracing can't reach EL2"**:
  the census's never-fired status for the tracing handlers is because the *fuzzer doesn't drive them*,
  not unreachability. (Note: measured via `begin_calls`, which counts HVCs regardless of KCOV; the
  ring-fill counters `el2_hits_max` need a KCOV-active run — see Blocker 2 in the target-status note.)
- **The UAF is INTERMITTENT.** That same clean enable did NOT re-fire it (`KASAN_delta=0`; the enable
  fully succeeded — all 3 HVCs). It's a recycled-slab UAF whose trigger depends on allocation timing,
  not every enable.
- **Generic-vs-pKVM: answered by provenance (see next section), not the instance test.** The
  `mkdir /sys/kernel/tracing/instances/probe0` probe didn't reproduce, but the intermittency makes
  that inconclusive on its own. The git history settles it.
- Executor context on N90 = **root** (tracefs is root-only — fine); events group = **`hypervisor`**.

## Upstream provenance — an ACK-backport bug, not a mainline-generic one (all verified in-tree)
The faulting function (`rb_allocate_cpu_buffer`) is generic, but it is only *reached* this way via a
code path that mainline has since rewritten. Confirmed against `common-stage2mvp`:
- **This tree carries the early `writer` API, not the upstream `remote` rewrite.** `struct
  ring_buffer_writer` is present (include/linux/ring_buffer.h + kernel/trace/ring_buffer.c);
  `ring_buffer_reader()` is a macro (ring_buffer.h:336) that calls `__ring_buffer_alloc(0,
  RB_FL_OVERWRITE, &key, writer)` — size 0, hyp-owned pages. `grep -c 'buffer->remote|ring_buffer_remote'`
  = **0**. Upstream renamed `writer→remote` and gave `alloc_buffer` a dedicated `else if (remote)` branch
  (computes per-CPU pages from the remote descriptor) in the merged "Tracefs support for pKVM" series
  (RFC→v7, ~6.13+). This 6.6.30 ACK backport predates that rewrite.
- **The "obvious" cpuhp guard was already tried and reverted upstream.** Present in-tree:
  `912da2c384d5` ("Do not have boot mapped buffers hook to CPU hotplug") → `580bb355bcae` (Revert). The
  revert rationale (Rostedt): the cpuhp theory was a *red herring*; the real fix was refcounting,
  `2cf9733891a4` ("Fix refcount setting of boot mapped buffers") — also in-tree. So re-applying a cpuhp
  guard would re-introduce a known-wrong patch.
- **Conclusion:** a real bug in this ACK/pKVM-tracefs backport (early `writer` path), **not reproducible
  against mainline** (mainline replaced the path). Fix options for the kernel owner: (a) rebase onto the
  merged ring-buffer *remotes* series (the upstream answer); (b) a local guard ONLY after root-causing,
  given 580bb355bcae. Do NOT report it upstream as a generic-ftrace UAF.
- Refs: LKML 912da2c384d5 revert thread; LWN "Tracefs support for pKVM" (Articles/1020290); the v7
  ring-buffer-remotes series.

## Retraction
The first draft of this file said the path "UAFs before reaching the EL2 tracing handlers" and framed it
as a pKVM finding. Both are wrong per the KASAN stack + hyp_trace.c: `__pkvm_load_tracing` fires and
succeeds first, and the faulting frames are generic ring-buffer code. Kept here because this file is
handoff material and the correction must travel with it.

## Implication for a tracing coverage target
Tracing is reachable and its EL2 handlers (#42/#43, and #45 once past this bug) do fire — but enabling it
currently trips this UAF, and syzkaller detects the KASAN report as a crash (observed: the manager tore
down the VM when the report hit its console). A tracing campaign would need this crash triaged/suppressed
first, plus the ring-headroom hazard checked (`el2_hits_max` vs `ring_capacity_pcs`; see campaign notes).
