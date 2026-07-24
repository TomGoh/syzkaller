Stage-2 Step 2 (first two items) — multi-page ring lifecycle + 4-page verdict smoke
==================================================================================
N90, 2026-07-24.  Record: ../../2026-07-24-pkvm-stage2-step2-multipage-ring.md

WHAT THIS SETTLES
  Step 1B measured 5.02% raw loss in the EL2 ring but could not say whether any
  of it was UNIQUE coverage. Sweeping the ring over 1 / 2 / 4 pages (509 / 1021 /
  2045 PC slots) answers it: the unique EL2 PC set is 132 at every size and is
  the SAME set -- all pairwise symmetric differences are 0, and
  new_EL2_PC_set MINUS old_132 is empty. The truncation was eating repeats.

  Scope: one fixed no_generate smoke, one boundary (#23), single-CPU serial.

LIFECYCLE FIX (the P0 that had to land before capacity)
  kvm_unshare_hyp_checked() returned on the first failing page, which on a
  multi-page buffer leaves the REMAINING pages shared, EL2-mapped and recorded.
  Now range-complete: every page is attempted, the first error is returned.
  Cleanup covers ATTEMPTED pages, not successful ones, because share_pfn_hyp()
  inserts the host rb-tree record before the EL2 hypercall and does not roll it
  back. If any page is unconfirmed the whole order-N block is leaked, never
  partially freed. EL2 validates the host-supplied nr_pages before using it to
  size the pin range, and derives the producer bound from its own validated
  value rather than the shared header.

FAULT INJECTION (so the leak paths are tested, not just audited)
  kvm/pkvm_cov/fault_inject is a one-shot test-only knob: bit0 EL2 teardown
  fails, bit1 unshare unconfirmed, bit2 enable-time EL2 setup fails. All three
  exercised on N90; see fault-injection-dmesg.txt. bit2 confirms the rollback
  FREES when the unshare is confirmed (leaked_bytes stays 0) and only leaks when
  it is not. 32 KiB was deliberately leaked during the test and cleared by a
  reboot.

LAYOUT
  runs/verdict.tsv        one row per run: geometry, all kernel loss counters,
                          and coverfile total/unique/EL2-unique
  runs/el2set-*.txt       the unique EL2 PC set per run (plain, uncompressed --
                          these are what the set diffs are computed from)
  runs/el2set-union-p{1,2,4}.txt   union across the 3 replicates per ring size
  runs/stats-*.txt        raw kvm/pkvm_cov/stats snapshot per run
  runs/hist-*.txt.gz      full per-PC histogram per run
  runs/execprog-*.log.gz, runs/dmesg-*.txt.gz
  runs/fault-injection-dmesg.txt   the three injected-failure paths
  scripts/run-verdict.sh  the sweep harness
  stage2-el2-kcov.patch   canonical kernel diff, now 13 files / 1269 lines

BUILD
  6.6.30+, Build ID 8371f4a0cac07905b2e11553e142239c3dfd6c05,
  vmlinux bebd01f4..., Image 6dea3850..., System.map 2d390d51...,
  .config b1f010f5... (unchanged).  __hyp_text_start 0xffff800081d87ed4.
  Addresses shift on every relink -- symbolize only against this vmlinux.

BOARD
  Rests on the multi-page kernel, ring disabled, leaked_bytes 0, fault_inject 0,
  no WARN/BUG.  GRUB default is still the known-good 6.6.30-pkvm-fuzz, and the
  1A / 1B kernels are kept as /boot/vmlinuz-6.6.30+.{1abuild,1bbuild}.bak.
  NOTE: the ESP grubenv one-shot next_entry did NOT take on this board (two
  attempts both booted the default); the 6.6.30+ entry was selected by hand at
  the GRUB menu.  Do not rely on grub-editenv next_entry here.
