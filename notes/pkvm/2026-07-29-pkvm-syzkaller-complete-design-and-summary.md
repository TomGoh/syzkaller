# Fuzzing a Rust Hypervisor: The Complete Design and Summary

*pKVM × syzkaller, 2026-07-20 → 2026-07-29. Written for a reader who has never used a fuzzer.*

---

## 1. What this project was

There is a piece of software on this ARM64 machine that runs at a higher privilege level than the operating system itself. It is called **pKVM** — "protected KVM" — and it is a hypervisor written partly in Rust. Its job is to run virtual machines whose memory the host Linux kernel *cannot read*, even though the host is the one that starts them. That is the whole point: a compromised host kernel should not be able to peek inside a protected guest.

Software at that privilege level is worth testing hard, because a bug there defeats the entire security model. This project built the machinery to test it automatically, using a fuzzer called **syzkaller**, and then used that machinery to find real bugs.

Two sentences of vocabulary before we go further:

- A **fuzzer** feeds a program huge numbers of semi-random inputs and watches for crashes. A *coverage-guided* fuzzer additionally measures which lines of code each input reached, and preferentially mutates the inputs that reached new code. That feedback loop is what makes modern fuzzing effective rather than a lottery.
- **syzkaller** is the coverage-guided fuzzer used for the Linux kernel. Its "inputs" are short sequences of system calls — the requests a normal program makes to the kernel, like `open` or `ioctl`.

Everything below follows from one awkward fact: **syzkaller's coverage measurement cannot see pKVM.** The rest of this document is the story of fixing that, and of what happened once it was fixed.

---

## 2. Why the coverage feedback loop was broken

Linux has a built-in coverage mechanism called **KCOV**. When the kernel is compiled with it, the compiler inserts a tiny call at the start of every basic block of code — `__sanitizer_cov_trace_pc()` — which records "I just executed the instruction at this address" into a buffer belonging to the currently-running task. syzkaller reads that buffer after every input and learns exactly which kernel code ran.

pKVM breaks all three assumptions in that sentence.

1. **It runs at a different privilege level (EL2).** The host kernel runs at EL1. Code at EL2 has its own page tables, its own stacks, and no access to the host's per-task data structures. It literally cannot find the "current task's KCOV buffer" — the concept does not exist there.
2. **It is written in Rust, built by a separate build script**, not by the main kernel build. The kernel's KCOV compiler flags never reached it.
3. **The interface between host and hypervisor is a hypercall (HVC)** — a privileged trap instruction, not a function call. Execution vanishes into EL2 and comes back with a return value. From the host's point of view, everything in between is a black box.

So the feedback loop was open. syzkaller could *call into* pKVM all day and learn nothing about which parts of it ran. Fuzzing without coverage feedback is barely better than random.

---

## 3. Part I — Making EL2 visible

The solution has four pieces, and they had to be built in order.

### 3.1 Teach the Rust hypervisor to report

The Rust hypervisor is built by `arch/arm64/kvm/hyp/nvhe/rust/build_rust.sh`. Two flags were added there, gated behind a new kernel option `CONFIG_PKVM_EL2_COV`:

```sh
export RUSTFLAGS="$RUSTFLAGS --cfg=CONFIG_PKVM_EL2_COV -C debuginfo=2 \
  -C passes=sancov-module -C llvm-args=-sanitizer-coverage-level=3 \
  -C llvm-args=-sanitizer-coverage-trace-pc"
```

`-C debuginfo=2` emits DWARF line tables, without which a raw address can never be turned back into `mem_protect/host.rs:1230`. The SanCov flags make LLVM insert the same `__sanitizer_cov_trace_pc()` call the C kernel uses.

The toolchain was also pinned from a floating `nightly` to `nightly-2025-05-05`. A floating nightly means the hypervisor silently changes underneath you; for a project whose entire output is *measurements*, that is unacceptable.

### 3.2 Give that call somewhere to write

The inserted calls need a `__sanitizer_cov_trace_pc()` implementation that works at EL2. That is the new file **`arch/arm64/kvm/hyp/nvhe/cov.c`**. It is deliberately in the C part of the hypervisor build, which is *not* SanCov-instrumented — otherwise the coverage callback would call itself, forever.

It writes into a **ring buffer that the host and the hypervisor share**. The host allocates the pages, hands them to EL2 through a new hypercall (`__pkvm_cov_setup`), and EL2 pins them. The producer is deliberately small:

```c
void notrace __sanitizer_cov_trace_pc(void)
{
	struct pkvm_cov_ring *r = __this_cpu_read(pkvm_cov_ring);
	if (!r || !(smp_load_acquire(&r->flags) & PKVM_COV_FLAG_ENABLED))
		return;
	if (__this_cpu_read(pkvm_cov_in_cb)) { r->nested++; return; }   /* no recursion */
	__this_cpu_write(pkvm_cov_in_cb, true);
	WRITE_ONCE(r->hits, READ_ONCE(r->hits) + 1);   /* count BEFORE the capacity test */
	if (count < capacity) {
		r->pcs[count] = (u64)__builtin_return_address(0);
		smp_store_release(&r->count, count + 1);
	} else {
		r->dropped++;
		smp_store_release(&r->flags, flags | PKVM_COV_FLAG_OVERFLOW);
	}
	__this_cpu_write(pkvm_cov_in_cb, false);
}
```

Two details in there are load-bearing, and both exist because **the host is not trusted by the hypervisor**:

- The producer's capacity bound comes from a *per-CPU value EL2 computed itself* at setup, never from the shared header. If it read the bound from memory the host can write, a malicious host could make EL2 write past the end of the pinned range — a hypervisor memory-corruption primitive handed over for free.
- `nr_pages` arriving from the host is validated (non-zero, power of two, ≤ a compile-time maximum) *before* it is used to size anything.

`hits` is incremented before the capacity check on purpose. That makes `hits - count` the exact number of PCs this window could not deliver — the loss is *measurable* rather than silent. This turns out to matter enormously (§5).

### 3.3 Drain it on the host side, into KCOV

The new file **`arch/arm64/kvm/pkvm_cov.c`** is the host half. Around a hypercall it does:

```c
bool armed = pkvm_cov_begin(&cov);   /* arm the ring, if this is the fuzzer task on the owner CPU */
... the HVC ...
if (armed) pkvm_cov_end(&cov);       /* drain the ring into the task's KCOV area */
```

`pkvm_cov_begin()` self-guards on three conditions: the ring is set up, we are on the designated owner CPU, and the current task is actually collecting KCOV. If any fails it returns false and the hypercall proceeds untouched. That is why the instrumentation can be applied globally without slowing down ordinary KVM users.

The drain needs to *put* the PCs somewhere KCOV understands, so two small functions were added to core KCOV (`kernel/kcov.c`):

```c
int  kcov_add_pcs(const u64 *pcs, u32 n);   /* append external PCs; returns how many fit */
bool kcov_current_trace_pc(void);           /* is this task collecting right now? */
```

`kcov_add_pcs()` deliberately returns a *short count* when the area fills, distinct from `-ENODEV` when the consumer disappeared mid-drain. Both lose coverage, but only one is a capacity problem — and telling them apart later proved decisive.

There is also an address-translation step. EL2 records runtime addresses; the symbolizer needs link-time addresses. The drain converts them using the `__hyp_text` anchor before handing them to KCOV.

### 3.4 Exposed for control and measurement

`pkvm_cov.c` registers a debugfs directory, `/sys/kernel/debug/kvm/pkvm_cov/`:

| file | purpose |
|---|---|
| `enable` | arm/disarm the ring on a CPU |
| `nr_pages` | ring size (power of two, ≤ max) — **so a capacity sweep needs no rebuild** |
| `owner_cpu` | which CPU owns the ring |
| `stats` | every counter below |
| `stats_reset` | zero the counters |
| `fault_inject` | test-only failure injection for the teardown paths |

The `stats` file is the instrument's own conscience. It reports not just what was collected but precisely what was *lost*, and where:

```
LOST_IN_RING        0     # EL2 produced PCs the ring could not hold
LOST_IN_KCOV_AREA   0     # the drain produced PCs the KCOV area could not hold
skip_not_owner      0     # HVCs on the wrong CPU (attribution would be wrong)
el2_hits_max    10199     # worst single-hypercall volume seen
```

Building the loss accounting in from the start is the single best design decision in this project. Three separate times, a result that looked like a fact about pKVM turned out to be a fact about the instrument — and each time, these counters are what revealed it.

### 3.5 The generalization: one macro instead of forty-six

Initially each interesting hypercall site was wrapped by hand. That works but does not scale — there are about 46 host→hypervisor call sites, each needing an edit, and any missed one is invisible coverage.

The final form wraps the **one primitive every hypercall funnels through**, in `asm/kvm_host.h`:

```c
#define KVM_PKVM_COV_HVC(_res, f, ...)					\
	do {								\
		struct pkvm_cov_ctx __cov;				\
		bool __cov_on = pkvm_cov_begin(&__cov);			\
		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f),		\
				  ##__VA_ARGS__, &(_res));		\
		if (__cov_on)						\
			pkvm_cov_end(&__cov);				\
	} while (0)
```

`kvm_call_hyp_nvhe()` and `kvm_call_refill_hyp_nvhe()` now use it. One change covers every boundary. The per-site wraps were reverted, because double-wrapping would corrupt the window.

An earlier project rule said "never globally wrap `kvm_call_hyp_nvhe`", on the grounds that some paths can sleep. That rule was superseded rather than ignored: **only the atomic HVC is inside the window**. The sleeping part of the refill path (`__pkvm_topup_hyp_alloc`) sits outside it, so the ring is never held across a sleep.

**Result, measured on the target:** one fixed input reached **540 EL2 program counters across 298 distinct Rust source lines** — 2.4× what the hand-wrapped version saw — with execution speed unchanged despite ~815 wrapped hypercalls per input, and zero loss on every counter.

---

## 4. Part II — Making pKVM reachable

Visibility is half the problem. The other half is *getting the fuzzer to call in at all*.

syzkaller describes system calls in a small language called **syzlang**. A description tells the fuzzer the shape of each argument — which are file descriptors, which are integers with a valid range, which are pointers to structures — so it can generate inputs that are well-formed enough to get past argument validation, yet still varied.

Creating a **protected** VM cannot be expressed in syzlang. It is a fixed multi-step dance: create the VM with a magic type bit, ask the kernel how large the protected firmware is, register a memory region covering exactly the right guest-physical window, tell the kernel the firmware load address, initialise a vCPU with the right feature bits, and only then run. Get the order or the arguments wrong and you get an error — or worse, a hang. syzlang can describe *one* call's arguments; it cannot describe a protocol.

So the project wrote a **composite pseudo-syscall** in C, inside the executor: `syz_kvm_run_fw_fault_gen$arm64`. It performs the whole dance internally and exposes only a few safe, mutatable knobs to the fuzzer. Everything the fuzzer can vary is re-validated and clamped in C, so a bad value fails cleanly instead of wedging the machine.

### 4.1 The hang lesson

The first version let the fuzzer vary the firmware load address (`fw_ipa`). Almost every generated value produced a **hang**, not an error. syzkaller classifies a hung program as `Hanged`, skips its triage, and never records its coverage — so the campaign reported `corpus=0, coverage=0` while the kernel-side counters showed the hypervisor was being hit thousands of times.

That took a long time to diagnose, and it produced the project's most-repeated rule: **an input that hangs is worse than an input that fails.** The fix was to pin the address to a known-good constant, and only ever re-expose dimensions that are *proven* non-hanging.

### 4.2 A safe dimension that reached new code

With the address pinned, one carefully derived variation was added: `hp_mode`, a single bit choosing whether the guest's firmware window is backed by ordinary 4 KiB pages or by a 2 MiB huge page. The huge-page case makes the hypervisor donate 512 pages at once instead of one, exercising its multi-page loops.

It worked — new hypervisor code was reached, reproducibly. Getting there also produced a durable discovery: **the executor runs each program chrooted into a fresh tmpfs**, so environment variables and files on disk never reach the code under test. The only channel into a running test is a **syscall argument**. Every toggle since has been designed that way.

---

## 5. Part III — Three times, the instrument was the limiter

This is the most transferable lesson in the project, so it gets its own section.

**Episode one: a metric that could not detect what it was measuring.** A count of hypervisor "drains" was used to decide whether the huge-page donation had worked. It stayed constant, which looked like proof the feature never engaged. Reading the *complete* loop that produced the count showed it advanced one 4 KiB page at a time regardless of the mapping size — so the number was mathematically incapable of distinguishing the two cases. The "negative result" was an artifact.

**Episode two: a ring that truncated the interesting half.** The EL2 ring is fill-then-stop: once full, everything afterwards is discarded. The huge-page path runs a ~512-iteration checking loop *first* and does the interesting work *afterwards*, so the ring filled with repetitions of the loop and the tail was systematically invisible. A capacity sweep at 4, 8 and 16 pages showed unique PCs plateauing — which read as "there is nothing more to see", but actually meant "the loop alone exceeds the largest ring we have."

Raising `PKVM_COV_MAX_PAGES` from 16 to 32 (a one-constant change; capacity 8,189 → 16,381 PCs) took `LOST_IN_RING` to zero and turned **1 newly-covered source line into 7**.

Worth recording honestly: the specific functions predicted to appear were **wrong**. The general claim — a bigger ring reveals a hidden tail — held; the guess about *which* code was in that tail did not.

**Episode three: a delivery buffer 85% too small.** After the input surface was widened (§6), `LOST_IN_KCOV_AREA` jumped from 0 to over 600 million. The ring was fine; the *KCOV area* the drain writes into was overflowing. Each program now made far more hypercalls, so per-program volume exceeded syzkaller's fixed buffer:

```c
/* executor/executor.cc */
const int kCoverSize = 4 << 20;    /* was 512 << 10 */
```

| | requested | accepted | lost |
|---|---|---|---|
| before | 707,190,974 | 103,396,256 | **85%** |
| after | 1,663,039 | 1,662,977 | **~0%** |

Structurally this is the *same bug* as episode two, one layer up: a buffer fills with repetitions and drops the unique tail.

**The pattern:** all three looked like discoveries about pKVM. All three were facts about the measuring apparatus. A lossy collector does not degrade gracefully into "less coverage" — it degrades into *systematically the wrong coverage*, which is indistinguishable from a real negative result unless the instrument reports its own losses. That is why §3.4's counters earn their keep.

---

## 6. Part IV — The reversal

By late in the project the conclusion was that the composite approach was near-exhausted: the protected VM had been driven about as hard as a hand-written composite could drive it, and coverage had flattened at roughly 7,863 with 298 hypervisor source lines.

To test that belief with data rather than intuition, a **census** was built (`scripts/pkvm/tools/hvc-census.py`). It takes the campaign's accumulated coverage, symbolizes it, and cross-references it against the hypervisor's own dispatch table — a 66-entry array in `hyp_main.rs` mapping each hypercall number to its handler. The output is a simple, brutal list: which handlers the fuzzer has ever reached, and which it has not.

**14 fired. 49 never fired.**

The initial reading was that most of the 49 were unreachable — belonging to subsystems (IOMMU, module loading, tracing) that no sequence of KVM system calls can drive. That reading was partly right and substantially wrong, and checking the actual *callers* rather than trusting the classification revealed why:

```
$ grep enable_syscalls -A5 campaign.cfg
"openat$kvm", "syz_kvm_run_fw_fault_gen$arm64", "close"
```

**Three.** The campaign was running with a three-word vocabulary. Meanwhile syzkaller has shipped a complete, mature ARM64 KVM description for years — `KVM_CREATE_VM`, `KVM_CREATE_VCPU`, `KVM_CREATE_IRQCHIP`, memory-region calls, register access, and a guest-code framework (`syz_kvm_setup_syzos_vm$arm64`) that runs real instructions inside the guest. All of it sitting unused behind a config list.

The error was a generalization. The *protected* VM genuinely needed a bespoke composite, because its setup protocol is not expressible in syzlang. That was correct work. But "protected VMs need a composite" had quietly become "pKVM needs a composite" — and **ordinary, non-protected VMs are just the standard KVM target**, which syzkaller has always been able to drive, and whose exits still pass through exactly the same Rust hypervisor.

There are three gates on what a fuzzer can run, and only the first was ever really a pKVM problem:

1. **Descriptors** define what is *expressible*. No description, no reach — this was the real constraint for protected VMs.
2. **`enable_syscalls`** selects the *vocabulary*. This was the constraint everywhere else, and it was one JSON edit.
3. **Runtime feature detection** filters what the target actually supports.

Widening the list from 3 → 13 → 41 → 63 entries, with the instruments fixed, moved coverage from the 7,863 plateau to **11,299 in seven minutes**, with the corpus growing from 56 to 208 programs.

---

## 7. Part V — Making it survive itself

Broadening the input surface immediately surfaced kernel warnings. syzkaller treats a warning as a crash, and a crash means "destroy the VM and start fresh". On a normal virtual target that costs a second. Here the target is a *physical board that never reboots*, and the effect was pathological.

**Problem one: the same warning, forever.** syzkaller followed the kernel log with `dmesg -w`, which prints the *entire existing buffer* before following. On a board that never reboots, every reconnect re-read the same old warning, re-detected it as a fresh crash, tore the machine down, reconnected, re-read it… an infinite loop. Upstream never hits this because virtual targets get a clean console on every boot.

```go
/* vm/isolated/isolated.go */
dmesg, err := vmimpl.OpenConsoleByCmd("ssh", append(args,
    "dmesg >> "+inst.cfg.TargetDir+"/dmesg-history.log; dmesg -C; dmesg -w"))
```

Archive the buffer to a file on the target, then clear it, then follow. The loop is broken and nothing is lost.

**Problem two: warnings are not crashes here.** The kernel under test is a heavily modified vendor tree; some of its warnings fire constantly. Suppressing them one at a time meant editing Go source and rebuilding three binaries for each one — an open-ended grind.

syzkaller already had the right mechanism, unused. In the manager's own config file:

```json
"ignores": ["WARNING:"]
```

`ignores` means *do not save and do not reboot*, as opposed to `suppressions` which still reboots. One line, no rebuild, per-campaign, and — because it is data rather than code — trivially adjustable later. This is the correct home for *campaign policy*; compiled-in filters should be reserved for permanent tooling artifacts (there is one: an OpenSSH banner beginning with the word `WARNING:` that is not a kernel message at all).

The trade-off is real and was taken deliberately: with warnings ignored, the fuzzer no longer *automatically* reports new warning-class bugs. So the record is kept another way — a continuous `dmesg --follow` on the target writing to `warn-history.log`, plus the archive above. Nothing is discarded; it is simply mined afterwards instead of interrupting the run.

**Problem three: a 115200-baud serial console.** The board boots with `console=ttyAMA0,115200`. Kernel printing to a serial port is *synchronous* — the CPU blocks until the characters are out. Each warning carries a full stack trace plus a ~1.5 KB module list, about a quarter-second of blocked CPU each. syzkaller's executor explicitly sets the console log level to 7 on every run, so lowering it by hand never stuck:

```c
/* executor/common_linux.h */
{"/proc/sys/kernel/printk", "1 4 1 3"},   /* was "7 4 1 3" */
```

Safe *here* because this backend detects crashes by reading `/dev/kmsg`, not the serial console — so nothing is hidden from the fuzzer. It would be wrong upstream, where other backends parse the serial console, and the comment in the source says so.

---

## 8. What was found

Five real defects, all banked with evidence under `notes/pkvm/evidence/`.

**1. VMID-rollover warning.** The hypervisor returns a non-success status for `__kvm_flush_vm_context` when the VM-identifier generation counter rolls over (roughly every 13,000 VM creations). Found at ~13k executions by the original narrow campaign — proof that even a one-boundary, fixed-input fuzzer can find genuine host↔hypervisor bugs.

**2. Use-after-free in the hypervisor-trace buffer path.** A KASAN-detected read of freed memory, reached by enabling hypervisor tracing. The investigation is the interesting part: the crashing function is *generic* kernel ring-buffer code, but it is only reachable through an early-revision Android backport of a feature mainline later **rewrote** (the interface was renamed `writer` → `remote`). So it is a backport defect, not reproducible against mainline.

The finding also documents a trap. The obvious fix — skip the CPU-hotplug registration for these buffers — was tried upstream (commit `912da2c384d5`), **reverted three weeks later** (`580bb355bcae`) as a "red herring", with the real cause turning out to be reference counting. Anyone fixing this should rebase onto the rewritten series, not re-apply the reverted guard.

**3. TLB-flush warning.** The hypervisor returns non-success for `__kvm_tlb_flush_vmid`, tripping the host's check. Same class as (1) — both are `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` in the hypercall wrapper — and quite possibly the same underlying defect seen through two doors. Provenance was checked: an earlier occurrence exists from 2026-07-21, so it is "first banked today", not "newly discovered".

**4. Timer-interrupt warning.** `WARN_ON` on a failed virtual-interrupt injection. A *different* class: no hypercall is involved, and the hypervisor is not implicated — this is generic ARM64 KVM, reachable now only because the widened input surface drives interrupt-controller configuration the old composite never touched. It is a user-space-reachable warning, which is a robustness concern in its own right. No earlier precedent; genuinely new.

**5. `pkvm_unmap_range()` self-deadlock — fixed and submitted.** The heaviest of the five and the only one carried all the way to a patch. `pkvm_unmap_range()` un-accounts pages with `account_locked_vm()`, which takes `mmap_lock` for write, while its caller `stage2_unmap_vm()` already holds that same `mmap_lock` for read — an unconditional rwsem self-deadlock, not a race. Any process with `/dev/kvm` access wedges the machine via a second `KVM_ARM_VCPU_INIT` on a vCPU that has already run, on any CPU lacking `ARM64_HAS_STAGE2_FWB`. Confirmed on three machines across two builds with a standalone reproducer; fixed by deferring the decrement through a per-VM atomic settled after the locks drop.

Three commits went to the Kylin futlab Gerrit (`2030/bug930`): two upstream `ANDROID:` cherry-picks that had to precede it, then our `KYLIN:` fix. Full record — commit hashes, Change-Ids, compile/checkpatch verification, the upstream-search trap, and what remains open — in `evidence/finding-pkvm-unmap-selfdeadlock-2026-07-29/SUBMISSION.md`. The migration process itself is now a reusable skill at `~/.claude/skills/kylin-futlab-patch-migration/`.

Note the discipline that produced the differences between these five: for each one, *read the complete function*, *check whether it has been seen before*, and *check what upstream already did about it*. Two of the first four changed character entirely under that treatment, and the fifth would have been hand-rolled from scratch had the upstream search stopped at branches — the two prerequisite fixes exist only on a *tag* in this repo.

---

## 9. Complete change inventory

### 9.1 Kernel — `common-stage2mvp` (base `da966ce9a047`)

These remain uncommitted in the kernel tree by design; a patch snapshot is archived in `notes/pkvm/evidence/kernel-instrumentation-2026-07-27/`.

**New files**

| file | what it is |
|---|---|
| `arch/arm64/include/asm/kvm_pkvm_cov.h` | Shared ring layout and ABI; `PKVM_COV_MAX_PAGES` (16 → **32**); `pkvm_cov_capacity_for()`; the host-side `pkvm_cov_ctx` |
| `arch/arm64/kvm/hyp/nvhe/cov.c` | The EL2 producer: `__sanitizer_cov_trace_pc()`, plus `pkvm_cov_setup()` to pin/register the shared ring per CPU |
| `arch/arm64/kvm/pkvm_cov.c` | The host drain: `pkvm_cov_begin()`/`pkvm_cov_end()`, runtime→link address conversion, full loss accounting, the debugfs interface |

**Modified files**

| file | change |
|---|---|
| `asm/kvm_host.h` | `KVM_PKVM_COV_HVC` macro; `kvm_call_hyp_nvhe()` routed through it |
| `asm/kvm_pkvm.h` | Same for `kvm_call_refill_hyp_nvhe()`, with the sleeping refill kept outside the window |
| `asm/kvm_asm.h` | New hypercall id `__pkvm_cov_setup` |
| `asm/kvm_mmu.h` | Declaration for the checked unshare |
| `kvm/hyp/nvhe/rust/src/hyp_main.rs` | Explicit dispatch for `__pkvm_cov_setup` **before** the fixed hypercall table, so no existing id shifts |
| `kernel/kcov.c`, `include/linux/kcov.h` | `kcov_add_pcs()` and `kcov_current_trace_pc()` — the injection point for externally-collected PCs |
| `kvm/mmu.c` | `kvm_unshare_hyp_checked()`: range-complete unshare that attempts every page and returns the first error, so a partial failure leaks safely instead of half-unwinding |
| `kvm/hyp/nvhe/rust/build_rust.sh` | Pinned toolchain; DWARF; SanCov trace-pc |
| `kvm/Kconfig`, `kvm/Makefile`, `kvm/hyp/nvhe/Makefile` | `CONFIG_PKVM_EL2_COV` and its build wiring |
| `configs/gwd3000_lenovox1d3000m_defconfig` | Target configuration for the fuzzing kernel |

### 9.2 syzkaller — `syzkaller-pkvm`, branch `pkvm-lifecycle-fuzzing`

72 commits, pushed to `origin`. Grouped by purpose:

| area | files | what |
|---|---|---|
| **Input surface** | `executor/common_kvm_arm64.h`, `sys/linux/dev_kvm_arm64.txt` | The protected-VM composites (`syz_kvm_run_fw_fault_gen$arm64` and friends), the `hp_mode` huge-page dimension, protected-VM ioctl descriptors |
| **Smoke gates** | `sys/linux/test/arm64-*` (12 files) | One test per composite/dimension, run before anything reaches the fuzzer |
| **Serial execution** | `syz-manager/manager.go`, `pkg/mgrconfig` | `pkvm_serial` mode: single process, CPU-pinned, so ring ownership and coverage attribution stay exact |
| **Coverage capacity** | `executor/executor.cc` | `kCoverSize` 512K → 4M entries |
| **Target throughput** | `executor/common_linux.h` | Console log level 7 → 1 |
| **Crash handling** | `vm/isolated/isolated.go` | Archive-then-clear the kernel log before following it |
| **Report policy** | `pkg/report/linux.go` | Ignore the OpenSSH banner; two now-redundant kernel-warning filters superseded by config policy |
| **Tooling** | `scripts/pkvm/tools/hvc-census.py`, `apply-macro-cov-injection.py` | The hypercall census; scripted application of the coverage injection |
| **Measurement harness** | `scripts/pkvm/a2/*` | Deterministic capture/analyse scripts and fixed `.prog` inputs for repeatable A/B measurement |
| **Operations** | `scripts/pkvm/target/pkvm-cov-arm.{sh,service}` | Re-arm the ring on boot — closing the top unattended silent-failure, and validated in production by a real power outage |
| **Documentation** | `notes/pkvm/**` | Design records, the operator runbook, evidence bundles, four findings |

### 9.3 Campaign configuration

`workdir-macrocov-thru/maxcov.cfg` — **61** enabled system calls, `"ignores": ["WARNING:"]`, single process, CPU-pinned, no target reboot.

One dependency in that list is not obvious and cost a silent loss of two descriptions: `ioctl$KVM_IRQFD` and `ioctl$KVM_IOEVENTFD` both take an eventfd, so with `eventfd2` absent syzkaller *transitively disables* them — logging "missing resource" rather than failing — and the campaign quietly fuzzes 59 calls while the config claims 61. Enabling a description whose argument is produced by another syscall means enabling that syscall too; check the manager's startup log for `transitively disabled` after any `enable_syscalls` edit.

---

## 10. The judgement calls, and why

**Measure the loss, always.** Every layer that can drop data reports how much it dropped. This is what turned three false conclusions into three fixes.

**Config for policy, code for artifacts.** Which warnings to ignore is campaign policy and belongs in a JSON file that can change without a rebuild. Filtering an SSH client banner is a permanent property of the tooling and belongs in source.

**Fail loudly rather than hang.** A failing input costs one execution; a hanging one poisons the coverage record and can stall a campaign for hours.

**Deterministic replay for measurement, the campaign for discovery.** Every number quoted here — the 540 PCs, the 298 lines, the 7-line delta — came from running a fixed `.prog` file repeatedly with `syz-execprog` and intersecting the results. A live campaign's counter is a moving target and cannot support an A/B claim.

**Code reading yields a hypothesis, never a conclusion.** Stated as a project rule after it was violated. Three of this project's most confident wrong answers came from partial `grep` output; each was corrected only by opening the complete function, and two of the four findings changed character entirely once their *callers* rather than their classifications were read.

**No reboot during the final run.** Periodic reboots would bound accumulated kernel state damage and reset the VM-identifier budget — genuinely desirable for a long campaign. It was declined here because a reboot that fails to come back costs the entire remaining run, and because corrupted state degrades *bug* validity rather than *coverage* validity. With coverage as the goal, that is the right trade; with bug-hunting as the goal, it would not be.

---

## 11. Where things stand

**Working and measured:** end-to-end EL2 coverage from a Rust hypervisor into syzkaller, with every loss counter at zero; ~46 hypercall boundaries instrumented by a single macro; a deterministic measurement harness; a hypercall census that converts "which parts are untested?" from an argument into a number.

**Coverage:** 7,863 (plateau, narrow input set) → **13,611**, corpus 56 → 524, zero instrument loss. Final measured point of the 2026-07-29 N90 run (`workdir-macrocov-thru/maxcov9.log`, 17:35:29): `corpus=524 coverage=13611 exec total=25241`. The intermediate 11,299 figure quoted in earlier drafts was a mid-run reading, not the settling point.

**How that run ended — and an unresolved question.** Execution stopped dead at 17:35:29: `exec total` stayed at exactly 25241 and coverage at 13611 for the next eight minutes while the manager kept polling, then `VM 1: running for 13m8s, restarting` and `boot error: repair failed: SSH failed`. A frozen exec counter means the *target* stopped executing, not that the fuzzer ran out of work. N90 runs the **unfixed** kernel, so the deadlock recurring is the obvious candidate — but this is a **hypothesis, not a conclusion**: no console capture was taken for this particular freeze, and SSH-layer failures have produced a similar manager-side signature before (§7). Confirming it needs the serial log from the freeze window, not the manager log. What the record does support is the weaker, still useful claim that a wedged target is indistinguishable from a finished one in the manager's stats line — the tell is `exec total` frozen while the timestamp advances.

**Found:** five defects, each with evidence, provenance, and — where applicable — upstream history. One of them (the `pkvm_unmap_range()` self-deadlock) was carried through to a fix submitted to the futlab Gerrit.

**Unblocked by the deadlock fix: `ioctl$KVM_ARM_VCPU_INIT` goes back in.** Diffing the campaign configs settles what the deadlock actually cost. `stageAB.cfg` enabled 12 calls; the final `maxcov.cfg` enables 61 — and across that expansion exactly **one** call was *removed*: `ioctl$KVM_ARM_VCPU_INIT`. It had to go because a second `KVM_ARM_VCPU_INIT` on a vCPU that has already run is precisely the deadlock trigger, so leaving it enabled wedged the board.

That makes it the highest-value single re-enable available, because it is the sole entry to `stage2_unmap_vm()` → `unmap_stage2_range()` → `pkvm_unmap_range()` — an entire unmap path that currently has *zero* coverage, on both the host and EL2 sides. Restoring it requires the fixed kernel on the board: D3000 already runs `6.6.30-pkvmfix` and its config (`workdir-d3000-overnight/d3000.cfg`, 62 calls) has it re-enabled; N90 still runs the unfixed `6.6.30+ #47` and must be rebuilt before it can take it back.

**It has to be the generic descriptor — `ioctl$KVM_ARM_VCPU_INIT_safe` cannot reach this path.** The obvious-looking substitution is wrong, and the code says so plainly. `__unmap_stage2_range()` (`mmu.c:444`, pristine upstream code, not ours) opens with:

```c
if (is_protected_kvm_enabled() && kvm->arch.pkvm.enabled)
        return;
```

A *protected* VM therefore returns before `___unmap_stage2_range()` is ever called, so `pkvm_unmap_range()` is unreachable from it. The path is reached by an **ordinary** VM on a pKVM host — `is_protected_kvm_enabled()` true, `kvm->arch.pkvm.enabled` false (it is set only at `pkvm.c:467`, on protected-VM creation). `ioctl$KVM_ARM_VCPU_INIT_safe` is typed on `fd_kvmcpu_protected`, i.e. a vCPU of a protected VM, so it lands squarely in the early-return case and contributes nothing here.

This is confirmed empirically, not just by reading: the reproducer that wedged three machines opens its VM with `ioctl(kvm, KVM_CREATE_VM, 0)` — type 0, ordinary, no bit 31 (`evidence/.../repro-deadlock.c:60`). The generic `ioctl$KVM_ARM_VCPU_INIT` on `fd_kvmcpu` is the descriptor that matches that shape.

The cost of using the generic one is its fuzzable feature bitmap, which can request `PMU_V3` — the documented route to the arch-timer `WARN` of finding (4), still unfixed and a different defect from the deadlock. That is tolerable only because `"ignores": ["WARNING:"]` keeps a warning from tearing down the VM; if warnings are ever promoted back to crashes, this re-enable needs revisiting, and `_safe` is *not* the fallback.

**Known unfinished, stated plainly:**

- **8 of the 49 unreached hypercall handlers are unreachable by any runtime fuzzer.** They run during kernel initialisation, before the coverage ring is armed. Capturing them needs the ring armed inside `kvm_arm_init()` — an instrumentation change, not an input change.
- **The guest→hypervisor surface is barely touched.** Unlike the host side, it has no flat dispatch table to census; interrupt-driven hypercalls are handled inside `kvm_hyp_handle_hvc64` (`pkvm.rs:1084`), wired at `switch.rs:630`. Enumerating it means reading the call identifiers inside that handler. Anyone starting there should know the census technique does *not* transfer.
- **Results found while warnings are suppressed are provisional** on one axis: a failed TLB invalidation means TLB maintenance is genuinely not completing, so stale-mapping artifacts cannot be fully excluded from later crash reports.
- **The hypervisor-tracing interface is reachable and bug-rich** but its host-side buffer path faults before the interesting handlers, so it is a bug-finding target rather than a coverage one until the backport is rebased.

---

*Everything above is reproducible from `notes/pkvm/` — the operator runbook, the evidence bundles, and the deterministic `.prog` inputs under `scripts/pkvm/a2/`.*
