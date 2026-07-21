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
#if SYZ_EXECUTOR || __NR_syz_kvm_memslot_reject_delete || __NR_syz_kvm_memslot_reject_move || __NR_syz_kvm_memslot_reject_flags
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
