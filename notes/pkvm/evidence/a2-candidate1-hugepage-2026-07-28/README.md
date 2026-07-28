# a2 candidate #1 — hugepage 2 MiB block donation → new EL2 coverage (N90, 2026-07-28)

**Result: CONFIRMED positive.** Driving the #23 firmware fault as a 2 MiB *block* donation
(`hp_mode=1`) reaches EL2 code the pinned 4 KiB baseline never hits, and the manager saves it.

## What a2 candidate #1 is
The source read (mem_protect/host.rs, guest.rs, transition.rs) predicted that if the firmware
window faults as a **2 MiB block** (512 pages) instead of a single 4 KiB page, the donation runs
the multi-page state loop (`___host_check_page_state_range`, host.rs:1230), the pvmfw multi-page
copy + size-clamp, and block-level stage2_map. `fw_ipa` stays pinned at `0x7fc00000`.

## How it is driven (the durable lesson)
The toggle had to become a **syscall argument**, not an env var or a file flag: the executor runs
each program **chrooted into a fresh tmpfs newroot** (executor/common_linux.h:4343, "Mount tmpfs
and chroot ... in sandbox=none and sandbox=namespace"), so `getenv()` and `/root/...` files never
reach the fault code — every earlier env/file-toggled "candidate" was silently the 4 KiB baseline.
Fix: `syz_kvm_run_fw_fault_gen$arm64(fd, ipa_size, fw_ipa, hp_mode int8[0:1])`; `hp_mode` bit0
selects a 2 MiB-aligned, pre-faulted THP backing in `pkvm_fw_backing()`. This also *is* the
generator-dimension form #24 wanted, so it converges with the plan.

## Verified facts (N90 6.6.30+ pkvm_cov, remote_cover=false, procs=1, CPU 0, ring nr_pages=16)
- **Block map forms:** `drains` 78 → 2 (the 2 MiB block absorbs the pvmfw accesses). The map
  granularity is the out-of-chroot observable; it is not assumed.
- **New coverage:** `C_common − B_union` = 2 PCs, of which **1 new Rust line:
  `mem_protect/host.rs:1230`** — exactly the source-predicted multi-page state loop. Baseline
  (`base16`) = 127 EL2 PCs / 79 `.rs:line`; candidate (`candhp5`) = 93 PCs.
- **Stable / deflake-compatible:** over 5 repeats `host.rs:1230` is present in **5/5** runs
  (deflake keeps signal seen in ≥3/5, job.go); the whole 93-PC set is identical every run
  (B_common == B_union), and `LOST_IN_RING` varies only 3205–3290 (~2.6%). The ring overflow is
  **deterministic** — it drops the same repeated loop PCs each run, losing no *unique* PC — so it
  does NOT turn real signal into flaky signal.
- **Manager acceptance:** a fresh-workdir syz-manager (generator emits `hp_mode`) grew corpus to 26
  / coverage 7052 and **saved corpus whose coverage includes `host.rs:1230`** (manager-rawcover.txt
  → manager-cover-symbolized.txt). Full loop: generate → block path → deflake → corpus.

## Honest caveat (follow-up)
The block-donation loop generates ~15.6k EL2 hits per program; even at the **max 16-page ring**
`LOST_IN_RING ≈ 3258`. So the captured 93-PC candidate set is a **lower bound** — there may be more
new coverage past the ring cap. Because the overflow is deterministic it does not block corpus
growth, but a ring-side dedup (or more frequent drain) would reveal the full block-path coverage.

## Files
- `base4k.union` / `base16.union` — baseline EL2 PC sets (4 KiB path), 127 PCs.
- `candhp5.{union,common,stats,new_pcs,new_lines}` — 5-repeat hp_mode=1 candidate + the confirmed
  new set (`new_lines` = host.rs:1230).
- `manager-rawcover.txt` / `manager-cover-symbolized.txt` — the manager's saved-corpus coverage,
  showing host.rs:1230 (acceptance evidence).

Harness: `scripts/pkvm/a2/` (a2-capture.sh on N90, a2-analyze.sh + a2-run.sh on the host).

## Update (2026-07-28): capacity sweep + line-level tail analysis

**Two independent confirmations the 2 MiB block donation forms** (retires the earlier
`drains`-ambiguity for good): (1) `drains` 78 → 2, and (2) the new line `host.rs:1230`.

**Capacity sweep** (same hp_mode=1 program, `nr_pages` set between lifecycles):

| nr_pages | capacity | unique EL2 PCs | LOST_IN_RING |
|---|---|---|---|
| 4 | 2045 | 85 | 15528 |
| 8 | 4093 | 93 | 11398 |
| 16 | 8189 | 93 | 3258 |

Total hits ≈ 19,600 across `drains=2` ⇒ ~9,800/window vs max 8,189 capacity: **no ring
size reaches the end of a window.** Unique-PC plateau at 93 is loop-dominance, NOT
completeness — the ~512-iteration `check_donation` per-page loop floods the ring with
repeats before the tail runs.

**Line-level bucket of the 93 candidate PCs proves the tail is unseen:** the candidate's
`guest.rs` PCs are 1271/331/335 = `guest_ack_donation` + check helpers — all in
`check_donation` (do_donate step 1). `guest.rs:1247` (block `stage2_map`) is ABSENT and
`pkvm.rs` (pvmfw copy) is 0. So candidate #1 reaches the *check* per-page loop (new:
`host.rs:1230` + `range.rs:764`) but the `__do_donate` tail — `guest_complete_donation`,
`pkvm_load_pvmfw_pages`, block `stage2_map` — runs into an already-full ring.

Also note 93 < baseline's 127: the hp set is a **subset**, not superset (one deep block
window vs 78 shallow per-page faults). New coverage is correctly the set difference. This
is why `hp_mode` must be **mutatable** — the fuzzer needs both modes for the union.

**Next (#26): EL2-side ring dedup** so the loop's repeats don't fill the ring. Falsifiable
prediction: dedup reveals `guest.rs:1247`, `pkvm.rs` (pvmfw copy), `host.rs:1026`
(`host_initiate_donation`). Must be EL2-side (before the ring write) — host-side dedup at
drain doesn't buy ring capacity, so only the EL2 variant reaches the tail.

## Review corrections (2026-07-28)

Three corrections from review, recorded for honesty:

1. **Acceptance is 3 of 4, not "full".** The a2 plan's per-candidate gate (item 2) requires
   `LOST_IN_RING=0`; candidate #1 runs at `LOST_IN_RING≈3258`. So: item 1 (baseline) ✅,
   item 2 (smoke incl. no ring loss) ❌ PENDING, item 3 (`C_common−B_union` non-empty:
   host.rs:1230, stable 5/5) ✅, item 4 (manager corpus grows from the new signal) ✅.
   The finding + commit aca4da48c are sound; "full acceptance" was overstated.

2. **`range.rs:764` is not phase evidence.** It is Rust core `core/src/iter/range.rs`
   (inlined Range iterator), present in loops in BOTH check and complete phases — it cannot
   discriminate phase. Dropped from the tail-unseen argument. The conclusion still holds on
   the sound evidence: `guest.rs:1247` (block stage2_map) ABSENT and `pkvm.rs` = 0.

3. **Fix is a constant, not EL2 dedup.** `PKVM_COV_MAX_PAGES 16→32` (shared header
   asm/kvm_pkvm_cov.h) gives capacity 16381 vs ~9800 hits/window — closes item 2
   (LOST_IN_RING→0) AND reveals the tail, same rebuild, none of the EL2-dedup risk (no hyp
   hot-path logic, no host-can't-clear-hyp-memory gap, no PC→offset OOB-write surface, no
   LOST_IN_RING-semantics break). EL2 seen-bitmap dedup → backlog (only if a future candidate
   needs windows no bounded ring can hold).

Measurement note: the a2 plan cites the plateau as 111 EL2 PCs / 79 lines (corpus-wide, from
l1-rawcover); this session's baseline is 127 PCs / 79 lines (5-repeat pinned #23, base16).
Different measurements — corpus-wide union vs pinned 5-repeat — same 79 source lines.

## Update 2 (2026-07-28): MAX_PAGES 16→32 rebuild — item 2 closed, tail revealed

Rebuilt the kernel with `PKVM_COV_MAX_PAGES 16→32` (cap 8189→16381), redeployed to N90
(Image-only overwrite of vmlinuz-6.6.30+; modules/initramfs unchanged; `/lib` intact),
re-measured at `nr_pages=32`:

- **`LOST_IN_RING = 0`** on all 5 candidate runs (full ~9800-hit window fits) — **plan
  acceptance item 2 now met.** Candidate #1 meets all four gates.
- Captured PCs **93 → 132** (+39); confirmed new lines (`C_common − B_union` vs base32)
  **1 → 7**: `guest.rs:298`, `guest.rs:444`, `host.rs:752`, `host.rs:1230`, `utils.rs:581`,
  `mm.rs:190`, `setup.rs:109`. Reproducible 5/5, instability 0. The 16-page measurement was
  head-biased (missed 39 PCs / 6 lines) — the instrument, not the DUT, had been the limiter.
- **Falsifiable prediction outcome, honest:** the *general* claim held (bigger ring reveals
  hidden tail: 1→7 new lines). The *specific* prediction FAILED — `guest.rs:1247` (block
  stage2_map), `host.rs:1026` (host_initiate_donation), `pkvm.rs` (pvmfw copy) are all still
  0. We do reach `guest_complete_donation` (`guest.rs:1195` now present) but not those exact
  lines. The mechanistic call-chain trace was a hypothesis; the block path exercises a
  different/broader set of new code. Coverage result verified; function-level guess retracted.

base32 baseline = 128/127 (79 lines); candhp32 candidate = 132 PCs. Artifacts: base32.union,
candhp32.{union,common,new_pcs,new_lines,stats}.
