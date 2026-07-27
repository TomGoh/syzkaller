# pKVM × syzkaller — session record, bigger picture, and next-move plan

## Context

Goal of the effort: make syzkaller coverage-guide the **Rust EL2 pKVM hypervisor** on the physical board **N90** (`root@10.42.27.17`), i.e. get EL2 (protected-KVM, nVHE, Rust) source coverage into KCOV so the fuzzer can evolve a corpus toward pKVM code.

This session picked up after "EL2 coverage reaches corpus" was claimed but unstable, and drove it to a validated, evidence-backed state — fixing two real blockers, correcting two of my own wrong root causes, and ending with the fuzzer finding a genuine kernel warning. Everything below is checked against code / git / logs (an independent verification pass caught and removed two exaggerations before this was written).

All numbers here are re-verified. Where a claim is not directly reproducible from a saved artifact, its provenance is stated.

---

## Part 1 — Faithful record of what we did

### 1.1 Committed + pushed (branch `pkvm-lifecycle-fuzzing`, `/home/jose/syzkaller-pkvm`)

- **`94a29c2a6`** — `report:` don't misdetect the OpenSSH post-quantum banner as a crash. `pkg/report/linux.go` `ctorLinux` appends a `ctx.ignores` regexp for `WARNING: connection is not using a post-quantum key exchange algorithm`; adds `pkg/report/testdata/linux/report/1003` (no-TITLE ⇒ expect-no-crash).
- **`942bf6622`** — `3B:` pin the generateable firmware-fault composite to safe args. `sys/linux/dev_kvm_arm64.txt:123` → `syz_kvm_run_fw_fault_gen$arm64(fd fd_kvm, ipa_size const[0], fw_ipa const[0x7fc00000])`; removed the `kvm_arm_pvm_fw_ipa` flag set.
- **`e09e26bea`** — `docs:` evidence bundle (5 files) under `notes/pkvm/evidence/gen-coverage-diagnosis-2026-07-27/`.

Kernel changes stay **local/uncommitted** in `/home/jose/common-stage2mvp` (per standing constraint). No Claude trailers in commits (per standing constraint).

### 1.2 Chronology — issues met and how they were solved (methods in brackets)

1. **Verified a colleague's status summary** — accurate on architecture/results; flagged 4 staleness points (Steps 0/1 already done; the coverage=0 "transport" framing was superseded). [adversarial verification Workflow, 6 parallel agents]

2. **The "RPC connection issue", board-free half — SOLVED.** The `-debug` RPC `EOF` was a **false crash**: the SSH client's post-quantum banner reached the console stream and the Linux reporter matched its leading `WARNING:` as a kernel oops (`matchOops`), tearing down the VM → the executor saw a peer-closed RPC. Fixed at the reporter layer (`94a29c2a6`). Verified: testdata `1003` fails pre-fix (`found unexpected crash`), passes post-fix; full `pkg/report/...` suite green; `LogLevel=ERROR` does **not** suppress the banner (so the fix must live in the detector). [systematic-debugging; TDD]

3. **Manager `coverage=0` for the gen composite — SOLVED, after two of MY wrong root causes (recorded honestly):**
   - **WRONG #1 — "triage raw cover (~250k PCs) too large, chokes triage."** Disproven: during fuzzing, deflake never ran (no `threaded=0 cover=1` execs, no "found new signal"); the Step-1 experiment of adding `ExecFlagDedupCover` to triage was a no-op; and `pkg/fuzzer/queue/queue.go:497` (`req.ExecOpts.ExecFlags |= do.opts.ExecFlags`) already merges `DedupCover`. My analysis had come from partial greps, not reading the full path.
   - **WRONG #2 — "KCOV-remote handle leaks because `kcov_task_exit` bails on `t->kcov==NULL` for remote-enablers."** Disproven by a colleague and confirmed in code: `kernel/kcov.c:684-685` sets `t->kcov = kcov; kcov->t = t;`, so task-exit does clean up. Again built from an incomplete grep.
   - **CORRECT root cause (verified):** generated programs **hang**. The fuzzer mutated `fw_ipa` to `0x7fd/0x7fe/0x7ff00000`; those pass the in-C window check but drive the real `KVM_RUN` into a non-returning guest fault → executor `killing hanging pid` (~10 s) → program marked **`Hanged`** → `pkg/fuzzer/fuzzer.go:149` `dontTriage` drops the whole program (line 154 gates triage on `!dontTriage`) → `Corpus.Save` never reached → `corpus/coverage=0`, even though earlier calls produced valid new signal. Evidence: 27 `killing hanging pid`, all in the fuzzing phase; hangs correlate with non-`0x7fc` `fw_ipa`. [code-map Workflow + `-debug` board runs + systematic-debugging]
   - **FIX (`942bf6622`) + VALIDATION:** pin `fw_ipa=0x7fc00000`, `ipa_size=0`. Fresh-workdir run: `corpus 0→8`, `coverage 0→6631`, `killing hanging = 0`, exec `~0.2 → 13/sec`; kernel `LOST_IN_RING=0`, `LOST_IN_KCOV=0`, `skip_not_owner=0` (STEP2-OUTCOME.txt).

4. **KCOV-remote `EEXIST` (line-2 investigation) — CONCLUDED: not a normal-operation defect.** Built kprobe identity-tracing (`kcov_remote_add`/ret/`kcov_remote_reset`/`kcov_task_exit`), **no kernel rebuild**. **VERIFIED:** a clean-boot 11,361-exec campaign leaked nothing — every remote-enable was reset on its owner's exit (one lifecycle confirmed to 10 µs: `kcov=0xffff0020e69fe780`, pid 23639, `kadd → ktexit(23639) → kreset`). **STRONGLY-SUPPORTED (not deterministically re-traced):** the earlier `EEXIST`s were `kcov_remote_map` **pollution from abnormal termination** (repeated `pkill -9`/`kill -9` of managers during cleanup, plus the hangs) accumulated on one long boot, cleared by reboot. A/B: clean boot = 0 leaks; prior polluted run = immediate `EEXIST` on `inst=2`. Deterministic genesis repro backlogged. [kprobe-events; systematic-debugging]

5. **Line-1 stability run on the `6.6.30+` `pkvm_cov` kernel, `remote_cover=false`.** Ran to **`exec total=13002`, `corpus=36`, `coverage=6900`**; kernel `drains=5,111,808`, `el2_hits=788,103,836`, `LOST_IN_RING=0`, `skip_not_owner=0`. Corpus coverage re-derived from `l1-rawcover.txt` symbolizes to **111 distinct EL2 Rust PCs → 79 source lines across 20 files / 59 functions**. The bridge captures coverage **only inside the #23 `pkvm_host_map_guest` window** (`mmu.c:1803-1811`): `pkvm_cov_begin` zeroes `ring->count` and arms, EL2 fills the ring during the map, `pkvm_cov_end` drains it into the faulting task — and `count` is zeroed **every window** (`pkvm_cov.c:414`), so there is **no boot/init residue**; every captured PC is EL2 code executed during a map. The set spans the `mem_protect` core (`host.rs`, `guest.rs`, `hyp.rs`, `mem_region.rs`, `transition.rs`, `utils.rs`), allocators/infra reached during the map (`page_alloc.rs`, `early_alloc.rs`, `mm.rs`, `refcount.rs`, `tlb.rs`) and the entry (`hyp_main.rs`, `kvm_host.rs`); a few init-looking files (`setup.rs`, `psci_relay.rs`, `cpufeature.rs`, `iommu.rs`) appear because they are reached from the map path or are symbolization/inlining attributions — **not** boot coverage. At ~13k execs the fuzzer **found a real kernel WARNING** (see 1.4). [bounded campaign + live board `pkvm_cov` stats + dmesg + code read]

### 1.3 Methods used
systematic-debugging skill; brainstorming skill; two adversarial verification Workflows; a code-map Workflow; **kprobe-events identity tracing** (no rebuild); **TDD** for the reporter fix; bounded/short campaigns; **live board `pkvm_cov` stats** (`/sys/kernel/debug/kvm/pkvm_cov/stats`); `addr2line` symbolization against `vmlinux` (KASLR off); git-revision-matched rebuilds with the `buildvcs`/`CGO_ENABLED=0` cross-build workarounds.

### 1.4 Remaining issues / open items (faithful)
- **Kernel finding (handed off, NOT ours):** VMID-rollover WARN — `arch/arm64/include/asm/kvm_host.h:1096` `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` in `kvm_call_hyp_nvhe`, fired at `vmid.c:69` `kvm_call_hyp(__kvm_flush_vm_context)` inside `flush_context()` (reached from `new_vmid()` on VMID-generation rollover). Trigger: rapid pVM create/destroy exhausts VMIDs (~13k execs) → rollover → EL2 returns non-success for the flush HVC. Crash record at `workdir-line1/crashes/4f65423248d87e2de7b665a00bd5da03f59d2351`. Likely NOT single-program reproducible (cumulative state). Evidence dir + memory note written.
- **`LOST_IN_KCOV_AREA=12042`** — re-confirmed live on the board (`kcov_requested=788,103,836` vs `kcov_accepted=788,091,794`). Provenance: board `/sys` stat, **not** in `mgr-line1.log`. Clean runs recorded `0`; this (WARN-hitting) run recorded 12042 and it is bounded. **Not proven to be exclusively within the WARN burst** (no timestamped/segmented counter) — honest wording: "seen only in the run that hit the rollover/WARN; zero in clean runs." Quick check owed (C).
- **Coverage plateau:** with pinned args the corpus reaches **111 EL2 PCs / 79 source lines / 20 files / 59 functions** — all captured inside `pkvm_host_map_guest` windows, so this is *already* the per-map fault-path set (no init to exclude). Whether that fault path can be driven to reach NEW EL2 lines is the hypothesis a2 will test.
- **Untracked evidence (gap to commit):** `docs/.../evidence/finding-vmid-flush-warn-2026-07-27/` (FINDING.txt, dmesg-warning.txt) and, under `gen-coverage-diagnosis-2026-07-27/`, `LINE2-KCOV-REMOTE-FINDINGS.txt` + `kcov-remote-cleanboot-trace.txt.gz` + `mgr-cleanboot-11k-run.log` are on-disk but **not committed/pushed**.
- **Board state:** on `6.6.30+` (`pkvm_cov`), a **non-default** GRUB entry (`6.6.30+ pKVM EL2-cov smoke`) requiring manual selection; ring currently disarmed, `leaked_bytes=0`, no procs. efi-pstore is dead on N90 (firmware can't `SetVariable`) → no unattended recovery yet.

---

## Part 2 — Bigger picture: adapting syzkaller to a Rust-pKVM kernel

syzkaller assumes host-kernel KCOV, but pKVM's security-relevant code runs at **EL2 in an isolated Rust hypervisor** the host KCOV can't see. Adapting the fuzzer is a layered problem; where each layer stands:

1. **Coverage bridge — DONE and proven.** Rust EL2 SanCov → per-CPU host-shared ring → EL1 drain wrapped around a chosen hypercall boundary → runtime-VA→link-address conversion → `kcov_add_pcs` → current task's KCOV → syzkaller signal/corpus → `addr2line` to `rust/src/*.rs:line`. Structural constraints this forces on the fuzzer: the ring is **per-CPU** and EL2 runs on the vCPU's CPU ⇒ executor **taskset-pinned to the owner CPU + `procs=1` serial, non-threaded** (`pkvm_serial`); the drain runs in a **non-sleepable** context (`mmu_lock`, RCU) ⇒ boundary wrapping must respect that; the ring is bounded (4 pages).
2. **Input model — partly done.** pKVM ops have strict **state/order preconditions** (protected-VM lifecycle, firmware window, `KVM_RUN` boundary, fd order) that syzkaller's permissive resource model can't enforce ⇒ **composite pseudo-syscalls** encode the lifecycle in C, and the mutation surface must be provably **non-hanging** (the pinned-`fw_ipa` lesson: an unsafe mutable arg silently defeats the whole loop by making programs `Hanged`).
3. **Boundary coverage — the immediate frontier (a1).** Each host→hyp hypercall is a distinct EL2 entry with its **own** context (lock held / CPU stability / RCU / sleepability). They must be armed/drained **per boundary**, never via one global `kvm_call_hyp_nvhe()` wrap. The VMID finding landed on exactly such an un-instrumented boundary — evidence that more boundaries = more reachable pKVM code and more findings.
4. **Guest→hyp surface — the strategic frontier (B).** The core pKVM threat model is a **compromised guest attacking the hypervisor** (guest HVCs into EL2). Reaching it needs a fuzzer agent **inside a protected guest** plus a transport that survives protected-VM memory isolation — normal SyzOS relies on host-shared memory, which protected VMs sever. This is the richest but hardest surface.
5. **Campaign trustworthiness (C).** The single-CPU/serial constraint caps throughput; unattended runs need serial/netconsole capture (efi-pstore is dead here); and abnormal manager termination pollutes kernel state ⇒ clean-shutdown discipline + reboot-to-clean.

**Guiding principle:** get EL2 coverage into KCOV faithfully (done) → drive **safe, reaching, diverse** inputs (a2) → expand boundaries carefully (a1) → open the guest→hyp surface (B) → make long runs trustworthy (C). Deepen the proven bridge before leaving it for the hardest part.

---

## Part 3 — Next-move plan (colleague-endorsed + independent)

Consensus across three colleague reviews + my own reasoning: **A is the main line, a2 before a1; C in parallel (only the unblockers); B design-only.** The VMID finding validates the approach — one boundary, pinned args, 79 EL2 lines still surfaced a real host→hyp bug at ~13k execs.

### Immediate: **a2 — falsifiable #23 coverage-diversity experiment** (this spec)
Hypothesis (NOT assumed): the #23 path can be driven to *new* EL2 code via safe input variation. `fw_ipa` stays **fixed at `0x7fc00000`** — no reopening IPA/`ipa_size` mutation.
1. **Baseline — precise set algebra over repeated runs (no control-run subtraction).** *Correction to the earlier draft (P0):* the bridge captures coverage **only inside the `pkvm_host_map_guest` window** and zeroes `ring->count` every window (`pkvm_cov.c:414`), so the captured set is *already* fault-attributable — there is no boot/init set mixed in, and a no-fault `openat$kvm`/`close` control captures **nothing** (it never reaches #23, so `pkvm_cov_begin` is never called). Control subtraction is therefore out; use set algebra over repeats. From a clean reboot, run the pinned #23 composite **N times** → sets `S_1..S_N`, and define:
   - `B_union = ∪ S_i` — the baseline's full reach (its entire nondeterministic envelope);
   - `B_common = ∩ S_i` — the stable core it reaches every time.
   Report the baseline instability `|B_union − B_common|`; if it's large, raise N until it settles. **The diff reference for a2 is `B_union`, NOT `B_common`.** Subtracting only the common core would let the baseline's own nondeterministic tail (`B_union − B_common`) masquerade as candidate-new coverage — a false positive. `B_common` is reported only as a stability descriptor.
2. **Derive safe candidates from source** (read `pkvm_mem_abort`/`__pkvm_host_map_guest` + `mem_protect` donation code first): firmware-window memslot partition/layout; mapping/donation prep-state + operation order; safe multi-page / multi-fault; lifecycle-preserving combinations.
3. **Per-candidate bounded smoke gate:** fast return, **no hang / no WARN/BUG / no ring or KCOV loss** — only passers enter the generator.
4. **Measure by SET, not hits — false-positive-safe.** Run each candidate **M times** → `T_1..T_M`; `C_common = ∩ T_j`, `C_union = ∪ T_j`. **Confirmed new coverage = `C_common − B_union`** — PCs stable across the candidate's own repeats (not candidate flicker) AND never seen in any baseline run (genuinely beyond the pinned path). PCs in `(C_union − B_union) − C_common` are logged as *probabilistic* signal only, not counted. `el2_hits` count is explicitly not the metric.
5. **Decide:** `C_common − B_union` non-empty for a candidate ⇒ encode that dimension into the generateable composite, then confirm **the manager saves corpus driven by the new EL2 signal**. Empty across the whole batch ⇒ **valuable negative result → end a2, pivot to a1**.
6. **Discipline — the VMID budget is per-BOOT, not per-campaign.** VMID consumption (`vmid_generation`) accumulates across the whole boot session; stopping/restarting the manager does **not** reset it, so "each campaign under 13k execs" is insufficient. Therefore: (a) start a2 measurement from a **clean reboot**; (b) budget by **cumulative pVM-creation count per boot** — each `syz_kvm_run_fw_fault_gen` exec creates+destroys one pVM ≈ consumes one VMID — and give the **entire a2 batch** a total budget well below the rollover threshold, not each sub-campaign independently under 13k; (c) if a rollover/WARN appears, **stop measuring and reboot** — never use tainted post-rollover state for set comparison.

Files likely touched (a2 impl, later): `sys/linux/dev_kvm_arm64.txt`, `executor/common_kvm_arm64.h` (the `syz_kvm_run_fw_fault_gen` helper), a new `sys/linux/test/` case. Reuse the existing ring-arm / `stats_reset` / `addr2line` tooling already in-tree: `notes/pkvm/campaign-runbook.sh` and the `symbolize.sh` scripts under `notes/pkvm/evidence/composite-campaign-2026-07-21/` and `campaign-2026-07-21/`.

### Next: **a1 — more host→hyp boundaries**
Order `#34/#35` (VM/vCPU create) → `#36-38` (teardown) → `#21` (share). **Each boundary individually audited** — `current`/CPU-pin/lock/RCU-lifecycle/drain-timing — and armed/drained on its own; **no global `kvm_call_hyp_nvhe()` wrap**. Acceptance per boundary: new EL2 unique PCs/lines appear and are attributable, `LOST_*`/`skip_not_owner` stay 0. (Kernel changes in `arch/arm64/kvm/pkvm_cov.c` + the drain sites; stays local/uncommitted.)

### Parallel: **C — only the unblockers**
- Unattended crash recovery via **netconsole or serial capture** (efi-pstore dead) — prerequisite for scaling a2/a1 to long runs.
- Quick **`LOST_IN_KCOV=12042`** check: confirm whether it's the per-exec KCOV-area cap hit by a large EL2 burst; if it only appears with the rollover/WARN, record as finding-noise and do not expand KCOV.
- (KCOV-remote clean-shutdown hardening stays low priority — normal ops are clean; a clean-exit guardrail + reboot-to-clean suffice.)

### Mid-term: **B — design only**
Scope the protected-guest fuzzer transport (agent placement in a protected guest, HVC test-case delivery, coverage extraction under memory isolation). Do **not** implement until a2/a1 mature.

### Housekeeping
Commit the untracked evidence (vmid-finding dir, line-2 findings, cleanboot files). Keep the WARN handed off to the kernel owner.

---

## Verification (how a2 is judged end-to-end)
On `6.6.30+` (`pkvm_cov`; arm ring on CPU 0), with `remote_cover=false`, pinned `fw_ipa`, from a **clean reboot**, within a per-boot pVM budget:
1. **Baseline** = `B_union` over N pinned-#23 repeats (§Immediate.1); baseline instability `|B_union − B_common|` reported.
2. Each candidate: bounded smoke passes (no hang/WARN/BUG, `LOST_IN_RING=0`, `LOST_IN_KCOV=0`).
3. Per candidate: **confirmed new coverage = `C_common − B_union`** over M repeats (the pass/fail signal); probabilistic-only PCs (`(C_union − B_union) − C_common`) logged separately, not counted.
4. Success = `C_common − B_union` non-empty **and** the manager's `corpus`/`coverage` grow from a program carrying that new EL2 signal (dashboard `/cover` shows the new `rust/src/*.rs` lines). Failure/negative = empty for the whole batch → documented, pivot to a1.
5. Measurement stays within the per-boot cumulative-pVM budget below rollover (§Immediate.6); a rollover/WARN **invalidates that boot's comparison** → reboot and re-baseline before continuing. Logs + board `pkvm_cov` counters saved to a dated evidence bundle.
