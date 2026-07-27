Stage-2 Step 1 — two-layer feedback-completeness measurement (1A / 1A+ / 1B)
===========================================================================
N90, 2026-07-24.  Record + analysis: ../../2026-07-24-pkvm-stage2-step1-feedback-measurement.md

WHAT THIS ANSWERS
  Which layer truncates EL2 coverage first, who actually fills the task KCOV
  area, and which layer to change next.  Short version: the task KCOV area was
  never the bottleneck; the "524287 = area full" seen in the 2026-07-23 smoke
  was the pl011 SERIAL CONSOLE (84% of entries) driven by the bridge's own 4
  ratelimited overflow warnings; the bridge's own KCOV-instrumented drain loop
  was another 42%; the only real remaining loss is the EL2 ring, at 5.02%, with
  a per-#23 raw high-water of 1088 against a 509-PC ring.

EXPERIMENT DESIGN
  One fixed input (sys/linux/test/arm64-syz_kvm_run_fw_fault, no_generate),
  one board, CPU 0, procs=1 threaded=0, 3 replicates, arm order rotated per
  replicate.  Full 2x2x2 factorial because two "background" conditions turned
  out to be first-order effects:
    bridge  = off | on     debugfs kvm/pkvm_cov/enable
    console = loud | quiet real console_loglevel 1 + a bind-mount shield over
                           /proc/sys/kernel/printk, because syz-executor
                           unconditionally writes "7 4 1 3" there
                           (executor/common_linux.h:5579)
    kprobe  = off | on     the pkvm_mem_abort kprobe/kretprobe used to observe
                           #23 costs ~34k KCOV entries by itself

LAYOUT
  1A/          24 runs on the frozen kernel (Build ID 33774811..., no rebuild)
  1Aplus/      24 runs, executor rebuilt with kCoverSize 512K -> 1M
  1B/          24 runs on the kernel with per-#23 loss accounting
                 (Build ID 3b2d0225...), plus kstats.tsv and stats-*.txt
  analysis/    symbolized attribution per arm (addr2line vs the SAME vmlinux),
               and the 45 EL2 PCs that the console flood had been hiding
  scripts/     run-1a.sh (1A and 1A+; EXECUTOR= override), run-1b.sh,
               attribute.sh, attribute-1b.sh
  stage2-el2-kcov.patch        canonical kernel diff, now 13 files / 1033 lines,
                               includes the 1B loss accounting; applies and
                               reverse-applies clean with --whitespace=error
  1aplus-executor-kcovsize.patch  the executor probe diff (NOT committed to the
                               product; kCoverSize 512K->1M AND kMaxOutputCoverage
                               6MiB->13MiB, since the output shmem, capped by
                               flatrpc MaxOutputSize = 14 MiB, binds first)

PER-RUN FILES
  raw.tsv         one row per run: coverfile totals, unique, and the EL2 /
                  bridge-EL1 / pl011 address buckets, plus faults, overflow,
                  skip, warn, and the observed printk values
  kstats.tsv      (1B only) the kernel-side loss accounting per run
  hist-<tag>.txt.gz    FULL per-PC histogram (count PC) -- lossless attribution
  stats-<tag>.txt      (1B only) raw kvm/pkvm_cov/stats snapshot
  execprog-<tag>.log.gz, dmesg-<tag>.txt.gz, trace-<tag>.txt.gz
  coverfile-r1-{off,on}-quiet-kpoff.gz   raw coverfiles kept for the two
                  decisive arms only; the histograms carry the same information
                  for every other run at a fraction of the size

READING THE ADDRESS BUCKETS
  Addresses shift on every relink, so each harness carries the bucket bounds for
  its own build (see the HYP_START/BR_START/UART_START constants at the top of
  run-1a.sh vs run-1b.sh).  1A/1A+ used __hyp_text_start = 0xffff800081d88ed4;
  1B used 0xffff800081d86ed4.  Symbolize only against the matching vmlinux.

BOARD STATE AFTER THE RUN
  ring disabled (owner_cpu -1), kprobes removed, printk restored, bind-mount
  unmounted, 0 WARN/BUG since boot.  The previous 1A/1A+ kernel image is kept on
  the board as /boot/vmlinuz-6.6.30+.1abuild.bak; GRUB default is still the
  known-good 6.6.30-pkvm-fuzz.
