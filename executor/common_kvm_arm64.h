#ifndef EXECUTOR_COMMON_KVM_ARM64_H
#define EXECUTOR_COMMON_KVM_ARM64_H

// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

// This file is shared between executor and csource package.

// Implementation of syz_kvm_setup_cpu pseudo-syscall.
#include <sys/mman.h>

#include "common_kvm.h"
#include "kvm.h"

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_cpu || __NR_syz_kvm_add_vcpu || __NR_syz_kvm_setup_syzos_vm
#include "common_kvm_arm64_syzos.h"
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_cpu || __NR_syz_kvm_add_vcpu
// Register encodings from https://docs.kernel.org/virt/kvm/api.html.
#define KVM_ARM64_REGS_X0 0x6030000000100000UL
#define KVM_ARM64_REGS_X1 0x6030000000100002UL
#define KVM_ARM64_REGS_PC 0x6030000000100040UL
#define KVM_ARM64_REGS_SP_EL1 0x6030000000100044UL
#define KVM_ARM64_REGS_TPIDR_EL1 0x603000000013c684

struct kvm_text {
	uintptr_t typ;
	const void* text;
	uintptr_t size;
};

struct kvm_opt {
	uint64 typ;
	uint64 val;
};
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_cpu || __NR_syz_kvm_setup_syzos_vm
struct addr_size {
	void* addr;
	size_t size;
};

static struct addr_size alloc_guest_mem(struct addr_size* free, size_t size)
{
	struct addr_size ret = {.addr = NULL, .size = 0};

	if (free->size < size)
		return ret;
	ret.addr = free->addr;
	ret.size = size;
	free->addr = (void*)((char*)free->addr + size);
	free->size -= size;
	return ret;
}

// Call KVM_SET_USER_MEMORY_REGION for the given pages.
static void vm_set_user_memory_region(int vmfd, uint32 slot, uint32 flags, uint64 guest_phys_addr, uint64 memory_size, uint64 userspace_addr)
{
	struct kvm_userspace_memory_region memreg;
	memreg.slot = slot;
	memreg.flags = flags;
	memreg.guest_phys_addr = guest_phys_addr;
	memreg.memory_size = memory_size;
	memreg.userspace_addr = userspace_addr;
	ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &memreg);
}

#define ADRP_OPCODE 0x90000000
#define ADRP_OPCODE_MASK 0x9f000000

// Code loading SYZOS into guest memory does not handle data relocations (see
// https://github.com/google/syzkaller/issues/5565), so SYZOS will crash soon after encountering an
// ADRP instruction. Detect these instructions to catch regressions early.
// The most common reason for using data relocaions is accessing global variables and constants.
// Sometimes the compiler may choose to emit a read-only constant to zero-initialize a structure
// or to generate a jump table for a switch statement.
static void validate_guest_code(void* mem, size_t size)
{
	uint32* insns = (uint32*)mem;
	for (size_t i = 0; i < size / 4; i++) {
		if ((insns[i] & ADRP_OPCODE_MASK) == ADRP_OPCODE)
			fail("ADRP instruction detected in SYZOS, exiting");
	}
}

static void install_syzos_code(void* host_mem, size_t mem_size)
{
	size_t size = (char*)&__stop_guest - (char*)&__start_guest;
	if (size > mem_size)
		fail("SYZOS size exceeds guest memory");
	memcpy(host_mem, &__start_guest, size);
	validate_guest_code(host_mem, size);
}

static void setup_vm(int vmfd, void* host_mem, void** text_slot)
{
	// Guest physical memory layout (must be in sync with executor/kvm.h):
	// 0x00000000 - unused pages
	// 0x08000000 - GICv3 distributor region (MMIO, no memory allocated)
	// 0x080a0000 - GICv3 redistributor region (MMIO, no memory allocated)
	// 0xdddd0000 - unmapped region to trigger a page faults for uexits etc. (1 page)
	// 0xdddd1000 - writable region with KVM_MEM_LOG_DIRTY_PAGES to fuzz dirty ring (2 pages)
	// 0xeeee0000 - user code (4 pages)
	// 0xeeee8000 - executor guest code (4 pages)
	// 0xeeef0000 - scratch memory for code generated at runtime (1 page)
	// 0xffff1000 - EL1 stack (1 page)
	struct addr_size allocator = {.addr = host_mem, .size = KVM_GUEST_MEM_SIZE};
	int slot = 0; // Slot numbers do not matter, they just have to be different.

	struct addr_size host_text = alloc_guest_mem(&allocator, 4 * KVM_PAGE_SIZE);
	install_syzos_code(host_text.addr, host_text.size);
	vm_set_user_memory_region(vmfd, slot++, KVM_MEM_READONLY, SYZOS_ADDR_EXECUTOR_CODE, host_text.size, (uintptr_t)host_text.addr);

	struct addr_size next = alloc_guest_mem(&allocator, 2 * KVM_PAGE_SIZE);
	vm_set_user_memory_region(vmfd, slot++, KVM_MEM_LOG_DIRTY_PAGES, ARM64_ADDR_DIRTY_PAGES, next.size, (uintptr_t)next.addr);

	next = alloc_guest_mem(&allocator, KVM_MAX_VCPU * KVM_PAGE_SIZE);
	vm_set_user_memory_region(vmfd, slot++, KVM_MEM_READONLY, ARM64_ADDR_USER_CODE, next.size, (uintptr_t)next.addr);
	if (text_slot)
		*text_slot = next.addr;

	next = alloc_guest_mem(&allocator, KVM_PAGE_SIZE);
	vm_set_user_memory_region(vmfd, slot++, 0, ARM64_ADDR_EL1_STACK_BOTTOM, next.size, (uintptr_t)next.addr);

	next = alloc_guest_mem(&allocator, KVM_PAGE_SIZE);
	vm_set_user_memory_region(vmfd, slot++, 0, ARM64_ADDR_SCRATCH_CODE, next.size, (uintptr_t)next.addr);

	// Allocate memory for the ITS tables: 64K for the device table, collection table, command queue, property table,
	// plus 64K * 4 CPUs for the pending tables, and 64K * 16 devices for the ITT tables.
	int its_size = SZ_64K * (4 + 4 + 16);
	next = alloc_guest_mem(&allocator, its_size);
	vm_set_user_memory_region(vmfd, slot++, 0, ARM64_ADDR_ITS_TABLES, next.size, (uintptr_t)next.addr);

	// Map the remaining pages at address 0.
	next = alloc_guest_mem(&allocator, allocator.size);
	vm_set_user_memory_region(vmfd, slot++, 0, 0, next.size, (uintptr_t)next.addr);
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_cpu || __NR_syz_kvm_add_vcpu
// Set the value of the specified register.
static void vcpu_set_reg(int vcpu_fd, uint64 id, uint64 val)
{
	struct kvm_one_reg reg = {.id = id, .addr = (uint64)&val};
	ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg);
}

// Set up CPU registers.
static void reset_cpu_regs(int cpufd, int cpu_id, size_t text_size)
{
	// PC points to the relative offset of guest_main() within the guest code.
	vcpu_set_reg(cpufd, KVM_ARM64_REGS_PC, executor_fn_guest_addr(guest_main));
	vcpu_set_reg(cpufd, KVM_ARM64_REGS_SP_EL1, ARM64_ADDR_EL1_STACK_BOTTOM + KVM_PAGE_SIZE - 128);
	// Store the CPU ID in TPIDR_EL1.
	vcpu_set_reg(cpufd, KVM_ARM64_REGS_TPIDR_EL1, cpu_id);
	// Pass parameters to guest_main().
	vcpu_set_reg(cpufd, KVM_ARM64_REGS_X0, text_size);
	vcpu_set_reg(cpufd, KVM_ARM64_REGS_X1, cpu_id);
}

static void install_user_code(int cpufd, void* user_text_slot, int cpu_id, const void* text, size_t text_size)
{
	if ((cpu_id < 0) || (cpu_id >= KVM_MAX_VCPU))
		return;
	if (!user_text_slot)
		return;
	if (text_size > KVM_PAGE_SIZE)
		text_size = KVM_PAGE_SIZE;
	void* target = (void*)((uint64)user_text_slot + (KVM_PAGE_SIZE * cpu_id));
	memcpy(target, text, text_size);
	reset_cpu_regs(cpufd, cpu_id, text_size);
}

static void setup_cpu_with_opts(int vmfd, int cpufd, const struct kvm_opt* opt, int opt_count)
{
	uint32 features = 0;
	if (opt_count > 1)
		opt_count = 1;
	for (int i = 0; i < opt_count; i++) {
		uint64 typ = opt[i].typ;
		uint64 val = opt[i].val;
		switch (typ) {
		case 1:
			features = val;
			break;
		}
	}

	struct kvm_vcpu_init init;
	// Queries KVM for preferred CPU target type.
	ioctl(vmfd, KVM_ARM_PREFERRED_TARGET, &init);
	init.features[0] = features;
	// Use the modified struct kvm_vcpu_init to initialize the virtual CPU.
	ioctl(cpufd, KVM_ARM_VCPU_INIT, &init);
}

#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_cpu
// syz_kvm_setup_cpu(fd fd_kvmvm, cpufd fd_kvmcpu, usermem vma[24], text ptr[in, array[kvm_text, 1]], ntext len[text], flags flags[kvm_setup_flags], opts ptr[in, array[kvm_setup_opt, 0:2]], nopt len[opts])
static volatile long syz_kvm_setup_cpu(volatile long a0, volatile long a1, volatile long a2, volatile long a3, volatile long a4, volatile long a5, volatile long a6, volatile long a7)
{
	const int vmfd = a0;
	const int cpufd = a1;
	void* const host_mem = (void*)a2;
	const struct kvm_text* const text_array_ptr = (struct kvm_text*)a3;
	const uintptr_t text_count = a4;
	const uintptr_t flags = a5;
	const struct kvm_opt* const opt_array_ptr = (struct kvm_opt*)a6;
	uintptr_t opt_count = a7;

	(void)flags;
	(void)opt_count;

	(void)text_count; // fuzzer can spoof count and we need just 1 text, so ignore text_count
	int text_type = text_array_ptr[0].typ;
	const void* text = text_array_ptr[0].text;
	size_t text_size = text_array_ptr[0].size;
	(void)text_type;

	void* user_text_slot = NULL;
	setup_vm(vmfd, host_mem, &user_text_slot);
	setup_cpu_with_opts(vmfd, cpufd, opt_array_ptr, opt_count);

	// Assume CPU is 0.
	install_user_code(cpufd, user_text_slot, 0, text, text_size);
	return 0;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_syzos_vm || __NR_syz_kvm_add_vcpu
struct kvm_syz_vm {
	int vmfd;
	int next_cpu_id;
	void* user_text;
};
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_syzos_vm

static long syz_kvm_setup_syzos_vm(volatile long a0, volatile long a1)
{
	const int vmfd = a0;
	void* host_mem = (void*)a1;

	void* user_text_slot = NULL;
	struct kvm_syz_vm* ret = (struct kvm_syz_vm*)host_mem;
	host_mem = (void*)((uint64)host_mem + KVM_PAGE_SIZE);
	setup_vm(vmfd, host_mem, &user_text_slot);
	ret->vmfd = vmfd;
	ret->next_cpu_id = 0;
	ret->user_text = user_text_slot;
	return (long)ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_add_vcpu
static long syz_kvm_add_vcpu(volatile long a0, volatile long a1, volatile long a2, volatile long a3)
{
	struct kvm_syz_vm* vm = (struct kvm_syz_vm*)a0;
	struct kvm_text* utext = (struct kvm_text*)a1;
	const void* text = utext->text;
	size_t text_size = utext->size;
	const struct kvm_opt* const opt_array_ptr = (struct kvm_opt*)a2;
	uintptr_t opt_count = a3;

	if (!vm) {
		errno = EINVAL;
		return -1;
	}
	if (vm->next_cpu_id == KVM_MAX_VCPU) {
		errno = ENOMEM;
		return -1;
	}
	int cpu_id = vm->next_cpu_id;
	int cpufd = ioctl(vm->vmfd, KVM_CREATE_VCPU, cpu_id);
	if (cpufd == -1)
		return -1;
	// Only increment next_cpu_id if CPU creation succeeded.
	vm->next_cpu_id++;
	setup_cpu_with_opts(vm->vmfd, cpufd, opt_array_ptr, opt_count);
	install_user_code(cpufd, vm->user_text, cpu_id, text, text_size);
	return cpufd;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_setup_protected_vm
// Create-only helper for a protected (pKVM) VM. a0 = fd from /dev/kvm, a1 = IPA size.
// KVM_VM_TYPE_ARM_PROTECTED is bit 31 (downstream ABI); the low 8 bits carry the IPA size
// (0 => default 40-bit). Forcing bit 31 here guarantees the returned resource is truthfully
// protected. This deliberately does NOT set up guest memory (a protected VM rejects the
// READONLY/LOG_DIRTY memslots that setup_vm() builds, with -EPERM) and does NOT load pvmfw
// (no SET_FW_IPA => pvmfw-less lifecycle). Returns the new vmfd, or -1 (a failed resource).
static long syz_kvm_setup_protected_vm(volatile long a0, volatile long a1)
{
	return ioctl(a0, KVM_CREATE_VM, 0x80000000 | (a1 & 0xff));
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_vcpu_run_immediate
// Controlled first KVM_RUN for a vCPU. a0 = vCPU fd. Maps the shared kvm_run struct,
// sets immediate_exit=1, then issues KVM_RUN. On the vCPU's *first* run this still executes
// kvm_arch_vcpu_run_pid_change() -> pkvm_create_hyp_vm() (arch/arm64/kvm/arm.c:717,757) — so the
// hyp VM object is built and its host-side KCOV is collected — but the immediate_exit check at
// the top of kvm_arch_vcpu_ioctl_run() (arm.c:1043) returns -EINTR before entering the guest.
// This crosses the host->hyp first-run boundary WITHOUT the pvmfw-less guest-abort busy loop that
// a raw KVM_RUN spins in (and which otherwise floods KCOV / costs the run's coverage). Pair with a
// PMU_V3-free VCPU_INIT to also avoid the arch_timer WARN. immediate_exit is at offset 1 of
// kvm_run, so the first mmap'd page always covers it. 0x1000 assumes a 4K page (true on N90 /
// the current config); on a 16K/64K-page arm64 target this should use the runtime page size.
static long syz_kvm_vcpu_run_immediate(volatile long a0)
{
	volatile struct kvm_run* run = (volatile struct kvm_run*)mmap(
	    NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, a0, 0);
	if (run == MAP_FAILED)
		return -1;
	run->immediate_exit = 1;
	long ret = ioctl(a0, KVM_RUN, 0);
	munmap((void*)run, 0x1000);
	return ret;
}
#endif

// --- Slice 2: controlled memslot rejects on a protected VM (composite) ------------------------
// Each reject is a SELF-CONTAINED composite: it builds its own bit-31 protected VM from the /dev/kvm
// fd, runs the whole sequence internally, returns the target op's result, and cleans up. Starting
// from fd_kvm (not an externally supplied VM fd) means the fuzzer cannot inject a wrong VM or an
// out-of-order state -- and because ONLY bit-31 protected VMs are ever created here, the
// dirty-log-on-a-NORMAL-VM WARN (BUG2, pgtable.c:639, confirmed by minimization) is never reached.
// syzkaller's resource compatibility is too permissive to enforce a state precondition via subtypes
// (a supertype resource can satisfy a subtype arg), so the sequence is enforced in C. Reaches the
// pKVM rejects in kvm_arch_prepare_memory_region (mmu.c:2492-2502): DELETE/MOVE after the first run
// -> -EPERM (pkvm.handle); dirty/readonly register -> -EPERM (only pkvm.enabled, no run). Executor-
// owned page-bounded RW backing; no guest memory is ever executed.
#if SYZ_EXECUTOR || __NR_syz_kvm_memslot_reject_delete || __NR_syz_kvm_memslot_reject_move || __NR_syz_kvm_memslot_reject_flags || __NR_syz_kvm_set_fw_ipa_busy || __NR_syz_kvm_run_fw_fault || __NR_syz_kvm_run_fw_fault_gen
#define PKVM_MEMSLOT_SLOT 0
#define PKVM_MEMSLOT_GPA 0x40000000UL

static uint64 pkvm_page(void)
{
	long p = sysconf(_SC_PAGESIZE);
	return p > 0 ? (uint64)p : 0x1000UL;
}

// One shared, executor-owned RW backing page (allocated once; reused, never freed -- bounded).
static uint64 pkvm_memslot_backing(void)
{
	static void* backing = NULL;
	if (!backing) {
		backing = mmap(NULL, pkvm_page(), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (backing == MAP_FAILED)
			backing = NULL;
	}
	return (uint64)(uintptr_t)backing;
}

// Checked KVM_SET_USER_MEMORY_REGION (unlike vm_set_user_memory_region, which ignores the return).
static long pkvm_memslot_ioctl(long vmfd, uint32 slot, uint32 flags, uint64 gpa, uint64 size, uint64 backing)
{
	struct kvm_userspace_memory_region m;
	memset(&m, 0, sizeof(m));
	m.slot = slot;
	m.flags = flags;
	m.guest_phys_addr = gpa;
	m.memory_size = size;
	m.userspace_addr = backing;
	return ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &m);
}

// Build a bit-31 protected VM with slot 0 registered. If do_run, also create + INIT_safe + controlled
// (immediate_exit) run a throwaway vCPU, REQUIRING the run to actually return -EINTR (so pkvm.handle
// really is set -- no faked state). Returns the VM fd (>=0) or -1 (nothing left open on -1).
static int pkvm_build_slotted_vm(long kvm_fd, int do_run)
{
	int vm = ioctl(kvm_fd, KVM_CREATE_VM, 0x80000000);
	if (vm < 0)
		return -1;
	uint64 b = pkvm_memslot_backing();
	if (!b || pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, 0, PKVM_MEMSLOT_GPA, pkvm_page(), b) != 0) {
		close(vm);
		return -1;
	}
	if (do_run) {
		struct kvm_vcpu_init init;
		memset(&init, 0, sizeof(init));
		init.target = 5; // KVM_ARM_TARGET_GENERIC_V8
		int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
		if (vcpu < 0) {
			close(vm);
			return -1;
		}
		if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) != 0) {
			close(vcpu);
			close(vm);
			return -1;
		}
		uint64 pg = pkvm_page();
		volatile struct kvm_run* run = (volatile struct kvm_run*)mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
		if (run == MAP_FAILED) {
			close(vcpu);
			close(vm);
			return -1;
		}
		run->immediate_exit = 1;
		errno = 0;
		long rr = ioctl(vcpu, KVM_RUN, 0);
		int run_errno = errno; // save before munmap/close can clobber errno (this gates the EINTR check)
		munmap((void*)run, pg);
		close(vcpu);
		if (!(rr == -1 && run_errno == EINTR)) { // the controlled run must actually have happened
			close(vm);
			return -1;
		}
	}
	return vm;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_memslot_reject_delete
// register -> run -> DELETE slot 0 -> -EPERM (pkvm.handle). Returns the DELETE result. a0 = fd_kvm.
static long syz_kvm_memslot_reject_delete(volatile long a0)
{
	int vm = pkvm_build_slotted_vm(a0, 1);
	if (vm < 0)
		return -1;
	long ret = pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, 0, 0, 0, 0);
	int e = errno; // preserve the reject errno (EPERM) across close(vm)
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_memslot_reject_move
// register -> run -> MOVE slot 0 to an adjacent page -> -EPERM. Returns the MOVE result. a0 = fd_kvm.
static long syz_kvm_memslot_reject_move(volatile long a0)
{
	int vm = pkvm_build_slotted_vm(a0, 1);
	if (vm < 0)
		return -1;
	uint64 b = pkvm_memslot_backing();
	long ret = pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, 0, PKVM_MEMSLOT_GPA + pkvm_page(), pkvm_page(), b);
	int e = errno; // preserve the reject errno (EPERM) across close(vm)
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_memslot_reject_flags
// register a slot with a dirty/readonly flag on a PROTECTED VM -> -EPERM (only pkvm.enabled, no run).
// a1 = flags (1=LOG_DIRTY, 2=READONLY, 3=both; syzlang keeps it non-zero). Only bit-31 protected VMs
// are created, so this never becomes the BUG2 dirty-log-on-a-normal-VM case. Returns the ioctl result.
static long syz_kvm_memslot_reject_flags(volatile long a0, volatile long a1)
{
	int vm = ioctl(a0, KVM_CREATE_VM, 0x80000000);
	if (vm < 0)
		return -1;
	uint64 b = pkvm_memslot_backing();
	long ret = -1;
	if (b)
		ret = pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, (uint32)a1, PKVM_MEMSLOT_GPA, pkvm_page(), b);
	int e = errno; // preserve the reject errno (EPERM) across close(vm)
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_pvm_info || __NR_syz_kvm_set_fw_ipa || __NR_syz_kvm_set_fw_ipa_busy || __NR_syz_kvm_run_fw_fault || __NR_syz_kvm_run_fw_fault_gen
// Slice 3a: protected-VM config paths via KVM_ENABLE_CAP(KVM_CAP_ARM_PROTECTED_VM). Composite from
// fd_kvm: each builds the bit-31 protected VM in C, then issues the cap (no raw KVM_ENABLE_CAP fuzz).
// The ARM-specific constant/flags are literals (not in the x86 cross-build's <linux/kvm.h>), matching
// the 0x80000000 create idiom. args[1..3] stay 0 (memset), required by pkvm_vm_ioctl_enable_cap
// (pkvm.c:655), which also requires a protected VM (pkvm.c:652).
#define PKVM_CAP_PROTECTED_VM 0xffbadab1 // KVM_CAP_ARM_PROTECTED_VM
#define PKVM_CAP_FLAGS_SET_FW_IPA 0
#define PKVM_CAP_FLAGS_INFO 1
// crosvm's arm64 pvmfw IPA window start: AARCH64_PHYS_MEM_START(0x80000000) - 4 MiB. Page-aligned and
// distinct from the Slice-2 memslot GPA (0x40000000), so it is forward-compatible with a real firmware
// handoff. SET_FW_IPA only records this (pkvm.c:632, no range check), so the value is not yet exercised
// as a guest load address here -- that is Phase 5.
#define PKVM_FW_IPA 0x7FC00000UL
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_pvm_info
// create bit-31 pVM -> KVM_ENABLE_CAP(INFO). INFO always succeeds (ret 0) on a valid info buffer; the
// firmware_size it writes reflects the GLOBAL pkvm_firmware_mem (non-zero on a board with a preinstalled
// pvmfw -- ~956 KB on N90), not this VM (pkvm.c:642), so we return the ioctl result and do NOT assert the
// size. a0 = fd_kvm.
static long syz_kvm_pvm_info(volatile long a0)
{
	int vm = ioctl(a0, KVM_CREATE_VM, 0x80000000);
	if (vm < 0)
		return -1;
	uint64 info[8]; // struct kvm_protected_vm_info: u64 firmware_size + u64 __reserved[7]
	memset(info, 0, sizeof(info));
	struct kvm_enable_cap cap;
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_INFO;
	cap.args[0] = (uint64)(uintptr_t)info;
	long ret = ioctl(vm, KVM_ENABLE_CAP, &cap);
	int e = errno;
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_set_fw_ipa
// create bit-31 pVM -> KVM_ENABLE_CAP(SET_FW_IPA) on a NOT-yet-run VM. With a firmware region present
// (pkvm_firmware_mem != NULL, as on N90) and no pkvm.handle yet, this succeeds and writes pvmfw_load_addr
// (pkvm.c:632). (On a board with no firmware it would instead be -EINVAL at pkvm.c:623.) a0 = fd_kvm.
static long syz_kvm_set_fw_ipa(volatile long a0)
{
	int vm = ioctl(a0, KVM_CREATE_VM, 0x80000000);
	if (vm < 0)
		return -1;
	struct kvm_enable_cap cap;
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_SET_FW_IPA;
	cap.args[0] = PKVM_FW_IPA;
	long ret = ioctl(vm, KVM_ENABLE_CAP, &cap);
	int e = errno;
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_set_fw_ipa_busy
// SET_FW_IPA AFTER the first run -> -EBUSY (pkvm.c:627-629). pkvm_build_slotted_vm(a0, 1) builds a bit-31
// pVM and runs a throwaway vCPU (controlled immediate_exit, required to return -EINTR), so pkvm.handle is
// really set; its extra memslot is irrelevant to SET_FW_IPA. a0 = fd_kvm.
static long syz_kvm_set_fw_ipa_busy(volatile long a0)
{
	int vm = pkvm_build_slotted_vm(a0, 1);
	if (vm < 0)
		return -1;
	struct kvm_enable_cap cap;
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_SET_FW_IPA;
	cap.args[0] = PKVM_FW_IPA;
	long ret = ioctl(vm, KVM_ENABLE_CAP, &cap);
	int e = errno;
	close(vm);
	errno = e;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_run_fw_fault || __NR_syz_kvm_run_fw_fault_gen
// Stage-2 EL2-coverage SMOKE ONLY (no_generate): reproduce the N90-verified pvmfw -> #23 path THROUGH the
// executor so its KCOV can capture EL2 coverage (the standalone fw_smoke.c proved #23 reachability but its
// coverage never reached syzkaller). Build a bit-31 protected VM, register a 4 MiB RW memslot over the
// crosvm pvmfw window [0x7FC00000, 0x80000000) (flags=0: protected VMs reject READONLY/LOG_DIRTY), confirm
// a usable firmware via INFO (0 < firmware_size <= 4 MiB), SET_FW_IPA, PMU-free vCPU INIT, then a REAL
// (immediate_exit=0) KVM_RUN. The guest fetches at the firmware IPA -> stage-2 fault -> pkvm_mem_abort ->
// #23 donation, handled in-kernel; the run returns to userspace only at the first MMIO exit.
//
// Success is STRICTLY ret == 0 && exit_reason == KVM_EXIT_MMIO (not "the ioctl didn't error"). No alarm()
// here: the executor's own watchdog plus an outer `timeout`/GWDT are the safety net; if the run ever hangs
// and the worker is killed, this run's coverage never returns -- that is a smoke FAILURE, never faked as
// success. a0 = fd_kvm is caller-owned and is NEVER closed here; the helper frees only what it created.
#define PKVM_FW_WINDOW 0x400000UL // 4 MiB, [PKVM_FW_IPA, 0x80000000)

// 4 MiB RW backing for the firmware window (allocated once, reused, never freed -- bounded).
static uint64 pkvm_fw_backing(void)
{
	static void* backing = NULL;
	if (!backing) {
		backing = mmap(NULL, PKVM_FW_WINDOW, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
		if (backing == MAP_FAILED)
			backing = NULL;
	}
	return (uint64)(uintptr_t)backing;
}

static long syz_kvm_run_fw_fault(volatile long a0)
{
	// All state declared up front so every failure can `goto out` (no jump over an
	// initializer) and cleanup can preserve the failing errno.
	int vm = -1, vcpu = -1, msz = 0;
	volatile struct kvm_run* run = NULL;
	struct kvm_enable_cap cap;
	struct kvm_vcpu_init init;
	uint64 info[8]; // struct kvm_protected_vm_info: u64 firmware_size + u64 __reserved[7]
	uint64 b;
	long rr, ret = -1;
	int err = EINVAL;
	uint32 exit_reason;

	vm = ioctl(a0, KVM_CREATE_VM, 0x80000000);
	if (vm < 0) {
		err = errno;
		goto out;
	}

	b = pkvm_fw_backing();
	if (!b) {
		err = ENOMEM;
		goto out;
	}
	if (pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, 0, PKVM_FW_IPA, PKVM_FW_WINDOW, b) != 0) {
		err = errno;
		goto out;
	}

	// INFO gate: only proceed on a board with a usable pvmfw (fits the window).
	memset(info, 0, sizeof(info));
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_INFO;
	cap.args[0] = (uint64)(uintptr_t)info;
	if (ioctl(vm, KVM_ENABLE_CAP, &cap) != 0) {
		err = errno;
		goto out;
	}
	if (info[0] == 0 || info[0] > PKVM_FW_WINDOW) {
		err = EINVAL; // no firmware, or it does not fit the window: precondition not met
		goto out;
	}

	// SET_FW_IPA before the first run: records pvmfw_load_addr; must succeed.
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_SET_FW_IPA;
	cap.args[0] = PKVM_FW_IPA;
	if (ioctl(vm, KVM_ENABLE_CAP, &cap) != 0) {
		err = errno;
		goto out;
	}

	// PMU-free vCPU (GENERIC_V8, no features) avoids the arch_timer/PMU_V3 WARN.
	memset(&init, 0, sizeof(init));
	init.target = 5; // KVM_ARM_TARGET_GENERIC_V8
	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) {
		err = errno;
		vcpu = -1;
		goto out;
	}
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) != 0) {
		err = errno;
		goto out;
	}

	msz = ioctl(a0, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (msz < 0) {
		err = errno; // do NOT mask the ioctl failure by enlarging msz
		goto out;
	}
	if (msz < (int)sizeof(struct kvm_run)) {
		err = EINVAL; // ABI anomaly: the run area cannot be smaller than the struct
		goto out;
	}
	run = (volatile struct kvm_run*)mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) {
		err = errno;
		run = NULL;
		goto out;
	}

	// REAL run: immediate_exit stays 0. #23 faults are serviced in-kernel; the run returns at first MMIO.
	run->immediate_exit = 0;
	errno = 0;
	rr = ioctl(vcpu, KVM_RUN, 0);
	err = errno; // save immediately, before munmap/close can clobber it
	exit_reason = run->exit_reason;
	if (rr == 0 && exit_reason == KVM_EXIT_MMIO) {
		ret = 0;
		err = 0;
	} else if (err == 0) {
		err = EIO; // the ioctl reported success but the exit was not a clean MMIO
	}

out:
	if (run)
		munmap((void*)run, msz);
	if (vcpu >= 0)
		close(vcpu);
	if (vm >= 0)
		close(vm);
	errno = err;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_run_fw_fault_gen
// Stage-2 EL2-coverage GENERATEABLE composite (3B). SAME protected-VM lifecycle, firmware window,
// KVM_RUN boundary and fd cleanup as the fixed no_generate syz_kvm_run_fw_fault smoke -- but it exposes
// a SMALL, type-constrained mutation surface so the fuzzer can vary the Host KVM config and get a
// different #23/EL2 donation footprint WITHOUT ever being handed the raw ioctl sequence. The fixed smoke
// stays the regression; this is the mutable surface. Two dimensions, both re-validated in C (the flags
// sets carry only valid members, but the C re-checks defensively so a stray value fails cleanly instead
// of wedging or building a half-state):
//   a1 = ipa_size : low bits of the protected VM type (bit 31 always forced) -> different stage-2 IPA width.
//   a2 = fw_ipa   : page-aligned firmware load IPA inside the 4 MiB window, with room for the firmware
//                   below 0x80000000. The memslot still covers the FULL window, so any valid fw_ipa is
//                   backed; SET_FW_IPA records it and the guest faults there -> #23 donation.
// a0 = fd_kvm is caller-owned and is NEVER closed here. Success is STRICTLY ret == 0 && KVM_EXIT_MMIO.
static long syz_kvm_run_fw_fault_gen(volatile long a0, volatile long a1, volatile long a2)
{
	int vm = -1, vcpu = -1, msz = 0;
	volatile struct kvm_run* run = NULL;
	struct kvm_enable_cap cap;
	struct kvm_vcpu_init init;
	uint64 info[8]; // struct kvm_protected_vm_info: u64 firmware_size + u64 __reserved[7]
	uint64 b, fw_ipa, page;
	long rr, ret = -1;
	int err = EINVAL;
	uint32 exit_reason;

	// Protected VM, opening ONLY the IPA-size low bits; bit 31 (protected) is always forced.
	vm = ioctl(a0, KVM_CREATE_VM, 0x80000000 | ((uint64)a1 & 0xff));
	if (vm < 0) {
		err = errno;
		goto out;
	}

	b = pkvm_fw_backing();
	if (!b) {
		err = ENOMEM;
		goto out;
	}
	// Memslot covers the FULL firmware window regardless of fw_ipa, so any valid fw_ipa stays backed.
	if (pkvm_memslot_ioctl(vm, PKVM_MEMSLOT_SLOT, 0, PKVM_FW_IPA, PKVM_FW_WINDOW, b) != 0) {
		err = errno;
		goto out;
	}

	// INFO gate: only proceed on a board with a usable pvmfw that fits the window.
	memset(info, 0, sizeof(info));
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_INFO;
	cap.args[0] = (uint64)(uintptr_t)info;
	if (ioctl(vm, KVM_ENABLE_CAP, &cap) != 0) {
		err = errno;
		goto out;
	}
	if (info[0] == 0 || info[0] > PKVM_FW_WINDOW) {
		err = EINVAL;
		goto out;
	}

	// C-side firmware-window enforcement (the state-machine constraint that must NOT be relaxed even
	// though the flags set already carries only valid members): fw_ipa must be page-aligned, at or above
	// the window base, and leave room for the firmware below the window top.
	fw_ipa = (uint64)a2;
	page = pkvm_page();
	if (page == 0 || (fw_ipa & (page - 1)) != 0 ||
	    fw_ipa < PKVM_FW_IPA ||
	    fw_ipa + info[0] > PKVM_FW_IPA + PKVM_FW_WINDOW) {
		err = EINVAL;
		goto out;
	}

	// SET_FW_IPA before the first run: records pvmfw_load_addr; must succeed.
	memset(&cap, 0, sizeof(cap));
	cap.cap = PKVM_CAP_PROTECTED_VM;
	cap.flags = PKVM_CAP_FLAGS_SET_FW_IPA;
	cap.args[0] = fw_ipa;
	if (ioctl(vm, KVM_ENABLE_CAP, &cap) != 0) {
		err = errno;
		goto out;
	}

	// PMU-free vCPU (GENERIC_V8, no features) avoids the arch_timer/PMU_V3 WARN.
	memset(&init, 0, sizeof(init));
	init.target = 5; // KVM_ARM_TARGET_GENERIC_V8
	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) {
		err = errno;
		vcpu = -1;
		goto out;
	}
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) != 0) {
		err = errno;
		goto out;
	}

	msz = ioctl(a0, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (msz < 0) {
		err = errno;
		goto out;
	}
	if (msz < (int)sizeof(struct kvm_run)) {
		err = EINVAL;
		goto out;
	}
	run = (volatile struct kvm_run*)mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) {
		err = errno;
		run = NULL;
		goto out;
	}

	// REAL run: immediate_exit stays 0. #23 faults are serviced in-kernel; the run returns at first MMIO.
	run->immediate_exit = 0;
	errno = 0;
	rr = ioctl(vcpu, KVM_RUN, 0);
	err = errno; // save immediately, before munmap/close can clobber it
	exit_reason = run->exit_reason;
	if (rr == 0 && exit_reason == KVM_EXIT_MMIO) {
		ret = 0;
		err = 0;
	} else if (err == 0) {
		err = EIO;
	}

out:
	if (run)
		munmap((void*)run, msz);
	if (vcpu >= 0)
		close(vcpu);
	if (vm >= 0)
		close(vm);
	errno = err;
	return ret;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_vgic_v3_setup
static int kvm_set_device_attr(int dev_fd, uint32 group, uint64 attr, void* val)
{
	struct kvm_device_attr kvmattr = {
	    .flags = 0,
	    .group = group,
	    .attr = attr,
	    .addr = (uintptr_t)val,
	};

	return ioctl(dev_fd, KVM_SET_DEVICE_ATTR, &kvmattr);
}

static int kvm_create_device(int vm_fd, int type)
{
	struct kvm_create_device create_dev = {
	    .type = (uint32)type,
	    .fd = (uint32)-1,
	    .flags = 0,
	};

	if (ioctl(vm_fd, KVM_CREATE_DEVICE, &create_dev) != -1)
		return create_dev.fd;
	else
		return -1;
}

#define REDIST_REGION_ATTR_ADDR(count, base, flags, index) \
	(((uint64)(count) << 52) |                         \
	 ((uint64)((base) >> 16) << 16) |                  \
	 ((uint64)(flags) << 12) |                         \
	 index)

// Set up the VGICv3 interrupt controller.
// syz_kvm_vgic_v3_setup(fd fd_kvmvm, ncpus flags[kvm_num_cpus], nirqs flags[kvm_num_irqs])
static long syz_kvm_vgic_v3_setup(volatile long a0, volatile long a1, volatile long a2)
{
	const int vm_fd = a0;
	const int nr_vcpus = a1;
	const int want_nr_irq = a2;

	int vgic_fd = kvm_create_device(vm_fd, KVM_DEV_TYPE_ARM_VGIC_V3);
	if (vgic_fd == -1)
		return -1;

	uint32 nr_irq = want_nr_irq;
	int ret = kvm_set_device_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0, &nr_irq);
	if (ret == -1) {
		close(vgic_fd);
		return -1;
	}

	uint64 gicd_base_gpa = ARM64_ADDR_GICD_BASE;
	ret = kvm_set_device_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_DIST, &gicd_base_gpa);
	if (ret == -1) {
		close(vgic_fd);
		return -1;
	}
	uint64 redist_attr = REDIST_REGION_ATTR_ADDR(nr_vcpus, ARM64_ADDR_GICR_BASE, 0, 0);
	ret = kvm_set_device_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_REDIST_REGION, &redist_attr);
	if (ret == -1) {
		close(vgic_fd);
		return -1;
	}

	ret = kvm_set_device_attr(vgic_fd, KVM_DEV_ARM_VGIC_GRP_CTRL, KVM_DEV_ARM_VGIC_CTRL_INIT, NULL);
	if (ret == -1) {
		close(vgic_fd);
		return -1;
	}

	return vgic_fd;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_assert_syzos_uexit
static long syz_kvm_assert_syzos_uexit(volatile long a0, volatile long a1,
				       volatile long a2)
{
#if !SYZ_EXECUTOR
	int cpufd = (int)a0;
#endif
	struct kvm_run* run = (struct kvm_run*)a1;
	uint64 expect = a2;

	if (!run || (run->exit_reason != KVM_EXIT_MMIO) ||
	    (run->mmio.phys_addr != ARM64_ADDR_UEXIT)) {
#if !SYZ_EXECUTOR
		fprintf(stderr, "[SYZOS-DEBUG] Assertion Triggered on VCPU %d\n", cpufd);
#endif
		errno = EINVAL;
		return -1;
	}

	uint64 actual_code = ((uint64*)(run->mmio.data))[0];
	if (actual_code != expect) {
#if !SYZ_EXECUTOR
		fprintf(stderr, "[SYZOS-DEBUG] Exit Code Mismatch on VCPU %d\n", cpufd);
		fprintf(stderr, "   Expected: 0x%lx\n", (unsigned long)expect);
		fprintf(stderr, "   Actual:   0x%lx\n",
			(unsigned long)actual_code);
#endif
		errno = EDOM;
		return -1;
	}
	return 0;
}
#endif

#if SYZ_EXECUTOR || __NR_syz_kvm_assert_reg
static long syz_kvm_assert_reg(volatile long a0, volatile long a1, volatile long a2)
{
	int vcpu_fd = (int)a0;
	uint64 id = (uint64)a1;
	uint64 expect = a2, val = 0;

	struct kvm_one_reg reg = {.id = id, .addr = (uint64)&val};
	int ret = ioctl(vcpu_fd, KVM_GET_ONE_REG, &reg);
	if (ret)
		return ret;
	if (val != expect) {
		errno = EDOM;
		return -1;
	}
	return 0;
}
#endif

#endif // EXECUTOR_COMMON_KVM_ARM64_H
