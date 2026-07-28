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
