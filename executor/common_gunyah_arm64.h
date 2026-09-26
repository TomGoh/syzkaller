// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

// This file is shared between executor and csource package.
//
// Non-protected Gunyah VM that runs the same SYZOS guest_main() as KVM.
// Memory is shared (GH_VM_SET_USER_MEM_REGION), never lent, and firmware
// config is never set, so RM auth stays AUTH_NONE. The driver's GH_VM_START
// installs the DTB parcel as the IMAGE address layout, which makes the
// default PC the DTB itself. GH_VM_SET_BOOT_CONTEXT overrides PC, SP_EL1,
// x0 and x1 before that start. Source of the protocol:
//   klinux drivers/virt/gunyah/vm_mgr.c gunyah_vm_start / gunyah_vm_set_boot_context
//   futlab gunyah-resource-manager vm_firmware_vm_set_boot_context_default

#ifndef EXECUTOR_COMMON_GUNYAH_ARM64_H
#define EXECUTOR_COMMON_GUNYAH_ARM64_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common_kvm.h"
#include "common_kvm_arm64_syzos.h"

#if SYZ_EXECUTOR || __NR_syz_gunyah_setup_vm || __NR_syz_gunyah_add_vcpu

// kylin include/uapi/linux/gunyah.h. _IO('G', nr) and _IOW('G', nr, size).
// Hardcoded because the build machine's linux/gunyah.h is not this driver's UAPI.
#define GH_IOCTL_SET_USER_MEM_REGION 0x40204701u
#define GH_IOCTL_SET_DTB_CONFIG 0x40104702u
#define GH_IOCTL_VM_START 0x4703u
#define GH_IOCTL_ADD_FUNCTION 0x40104704u
#define GH_IOCTL_VCPU_RUN 0x4705u
#define GH_IOCTL_SET_BOOT_CONTEXT 0x4010470au

#define GH_FN_VCPU 1u
// GUNYAH_MEM_ALLOW_READ|WRITE|EXEC. A non-protected writable page is mapped RWX.
#define GH_MEM_RWX 7u

#define GH_REG_SET_X 0u
#define GH_REG_SET_PC 1u
#define GH_REG_SET_SP 2u
#define GH_BOOT_REG(set, idx) ((((set) & 0xff) << 8) | ((idx) & 0xff))

#define GH_VCPU_EXIT_MMIO 1u
#define GH_VCPU_EXIT_STATUS 2u
#define GH_VCPU_EXIT_PAGE_FAULT 3u
#define GH_VCPU_RESUME_FAULT 1u

// DTB GPA. Also the IMAGE base the driver reports, so it must not be executed.
#define GH_SYZOS_DTB_GPA 0x80000000ull
#define GH_SYZOS_DTB_SIZE (2 * KVM_PAGE_SIZE)

// notes/gunyah/s3-identify.prog stores this at the scratch page before hvc #0x6000.
#define GH_IDENTIFY_MARKER 0x1111ull

#define GH_ADRP_OPCODE 0x90000000u
#define GH_ADRP_OPCODE_MASK 0x9f000000u

struct gh_userspace_memory_region {
	uint32 label;
	uint32 flags;
	uint64 guest_phys_addr;
	uint64 memory_size;
	uint64 userspace_addr;
};

struct gh_vm_dtb_config {
	uint64 guest_phys_addr;
	uint64 size;
};

struct gh_vm_boot_context {
	uint32 reg;
	uint32 reserved;
	uint64 value;
};

struct gh_fn_vcpu_arg {
	uint32 id;
};

struct gh_fn_desc {
	uint32 type;
	uint32 arg_size;
	uint64 arg;
};

// Prefix of struct gunyah_vcpu_run, page-fault arm. offsetof checked against the
// kylin UAPI: exit_reason 8, phys_addr 16, attempt 24, resume_action 28.
struct gh_vcpu_run {
	uint8 immediate_exit;
	uint8 padding[7];
	uint32 exit_reason;
	uint64 phys_addr;
	int attempt;
	uint8 resume_action;
};

// Same layout as kvm_text in common_kvm_arm64.h. A second name so this file
// still compiles when the KVM header's copy is not instantiated.
struct gh_text {
	uintptr_t typ;
	const void* text;
	uintptr_t size;
};

struct gh_syz_vm {
	int vmfd;
	int next_cpu_id;
	int started;
	void* user_text;
	void* scratch;
};

struct gh_bump {
	uint8* cur;
	uint8* end;
};

// dtc -I dts -O dtb. Flat blob, totalsize 536, magic d00dfeed.
// /cpus/cpu@0, /gunyah-vm-config image-name=syzos, vm-attrs=no-dtb-patch,
// memory base-address=0x80000000, vcpus affinity=proxy. No /memory node:
// the RM rejects any device_type=memory node whose name is not exactly "memory".
static const uint8 gh_syzos_dtb[] = {
	0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x02, 0x18, 0x00, 0x00, 0x00, 0x38,
	0x00, 0x00, 0x01, 0xb8, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x11,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60,
	0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x17,
	0x00, 0x00, 0x00, 0x00, 0x73, 0x79, 0x7a, 0x6b, 0x61, 0x6c, 0x6c, 0x65,
	0x72, 0x2c, 0x67, 0x75, 0x6e, 0x79, 0x61, 0x68, 0x2d, 0x73, 0x79, 0x7a,
	0x6f, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x63, 0x70, 0x75, 0x73, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0b,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x63, 0x70, 0x75, 0x40, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x26, 0x63, 0x70, 0x75, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00,
	0x61, 0x72, 0x6d, 0x2c, 0x61, 0x72, 0x6d, 0x2d, 0x76, 0x38, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x32,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x67, 0x75, 0x6e, 0x79, 0x61, 0x68, 0x2d, 0x76,
	0x6d, 0x2d, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x36,
	0x73, 0x79, 0x7a, 0x6f, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x41, 0x6e, 0x6f, 0x2d, 0x64,
	0x74, 0x62, 0x2d, 0x70, 0x61, 0x74, 0x63, 0x68, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0b,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
	0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
	0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
	0x76, 0x63, 0x70, 0x75, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x57, 0x70, 0x72, 0x6f, 0x78,
	0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x09, 0x63, 0x6f, 0x6d, 0x70,
	0x61, 0x74, 0x69, 0x62, 0x6c, 0x65, 0x00, 0x23, 0x61, 0x64, 0x64, 0x72,
	0x65, 0x73, 0x73, 0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x23, 0x73,
	0x69, 0x7a, 0x65, 0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x64, 0x65,
	0x76, 0x69, 0x63, 0x65, 0x5f, 0x74, 0x79, 0x70, 0x65, 0x00, 0x72, 0x65,
	0x67, 0x00, 0x69, 0x6d, 0x61, 0x67, 0x65, 0x2d, 0x6e, 0x61, 0x6d, 0x65,
	0x00, 0x76, 0x6d, 0x2d, 0x61, 0x74, 0x74, 0x72, 0x73, 0x00, 0x62, 0x61,
	0x73, 0x65, 0x2d, 0x61, 0x64, 0x64, 0x72, 0x65, 0x73, 0x73, 0x00, 0x61,
	0x66, 0x66, 0x69, 0x6e, 0x69, 0x74, 0x79, 0x00,
};

static void* gh_bump_alloc(struct gh_bump* bump, size_t size)
{
	if (size > (size_t)(bump->end - bump->cur))
		return NULL;
	void* ret = bump->cur;
	memset(ret, 0, size);
	bump->cur += size;
	return ret;
}

static int gh_set_region(int vmfd, uint32 label, uint64 gpa, void* host, uint64 size)
{
	struct gh_userspace_memory_region region;

	memset(&region, 0, sizeof(region));
	region.label = label;
	region.flags = GH_MEM_RWX;
	region.guest_phys_addr = gpa;
	region.memory_size = size;
	region.userspace_addr = (uint64)host;
	return ioctl(vmfd, GH_IOCTL_SET_USER_MEM_REGION, &region);
}

static int gh_set_boot(int vmfd, uint32 reg, uint64 value)
{
	struct gh_vm_boot_context ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.reg = reg;
	ctx.value = value;
	return ioctl(vmfd, GH_IOCTL_SET_BOOT_CONTEXT, &ctx);
}

static void gh_validate_guest_code(void* mem, size_t size)
{
	uint32* insns = (uint32*)mem;
	size_t i;

	for (i = 0; i < size / 4; i++) {
		if ((insns[i] & GH_ADRP_OPCODE_MASK) == GH_ADRP_OPCODE)
			fail("ADRP instruction detected in SYZOS, exiting");
	}
}

static void gh_install_syzos_code(void* host_mem, size_t mem_size)
{
	size_t size = (char*)&__stop_guest - (char*)&__start_guest;

	if (size > mem_size)
		fail("SYZOS size exceeds guest memory");
	memcpy(host_mem, &__start_guest, size);
	gh_validate_guest_code(host_mem, size);
}

static long syz_gunyah_setup_vm(volatile long a0, volatile long a1)
{
	const int vmfd = a0;
	uint8* host_mem = (uint8*)a1;
	struct gh_syz_vm* vm;
	struct gh_bump bump;
	void* dtb_pages;
	void* user_text;
	void* executor;
	void* scratch;
	void* stack;
	uint32 label = 1;
	struct gh_vm_dtb_config dtb;

	if (!host_mem || ((uintptr_t)host_mem & (KVM_PAGE_SIZE - 1))) {
		errno = EINVAL;
		return -1;
	}
	if (sizeof(gh_syzos_dtb) > GH_SYZOS_DTB_SIZE)
		fail("gunyah SYZOS dtb exceeds the donated pages");

	memset(host_mem, 0, KVM_PAGE_SIZE);
	vm = (struct gh_syz_vm*)host_mem;
	bump.cur = host_mem + KVM_PAGE_SIZE;
	bump.end = host_mem + KVM_GUEST_MEM_SIZE;

	dtb_pages = gh_bump_alloc(&bump, GH_SYZOS_DTB_SIZE);
	executor = gh_bump_alloc(&bump, 4 * KVM_PAGE_SIZE);
	user_text = gh_bump_alloc(&bump, KVM_MAX_VCPU * KVM_PAGE_SIZE);
	scratch = gh_bump_alloc(&bump, KVM_PAGE_SIZE);
	stack = gh_bump_alloc(&bump, KVM_PAGE_SIZE);
	if (!dtb_pages || !executor || !user_text || !scratch || !stack) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(dtb_pages, gh_syzos_dtb, sizeof(gh_syzos_dtb));
	gh_install_syzos_code(executor, 4 * KVM_PAGE_SIZE);

	if (gh_set_region(vmfd, label++, GH_SYZOS_DTB_GPA, dtb_pages, GH_SYZOS_DTB_SIZE))
		return -1;
	if (gh_set_region(vmfd, label++, SYZOS_ADDR_EXECUTOR_CODE, executor, 4 * KVM_PAGE_SIZE))
		return -1;
	if (gh_set_region(vmfd, label++, ARM64_ADDR_USER_CODE, user_text, KVM_MAX_VCPU * KVM_PAGE_SIZE))
		return -1;
	if (gh_set_region(vmfd, label++, ARM64_ADDR_SCRATCH_CODE, scratch, KVM_PAGE_SIZE))
		return -1;
	if (gh_set_region(vmfd, label++, ARM64_ADDR_EL1_STACK_BOTTOM, stack, KVM_PAGE_SIZE))
		return -1;

	memset(&dtb, 0, sizeof(dtb));
	dtb.guest_phys_addr = GH_SYZOS_DTB_GPA;
	dtb.size = GH_SYZOS_DTB_SIZE;
	if (ioctl(vmfd, GH_IOCTL_SET_DTB_CONFIG, &dtb))
		return -1;

	vm->vmfd = vmfd;
	vm->next_cpu_id = 0;
	vm->started = 0;
	vm->user_text = user_text;
	vm->scratch = scratch;
	return (long)vm;
}

static void gh_note_identify(struct gh_syz_vm* vm, struct gh_vcpu_run* run)
{
	uint64* scratch;
	uint32 exit_reason = 0;
	uint64 phys = 0;

	if (!vm->scratch)
		return;
	scratch = (uint64*)vm->scratch;
	// Random programs do not store this marker. The identify smoke does.
	if (scratch[0] != GH_IDENTIFY_MARKER)
		return;
	if (run) {
		exit_reason = run->exit_reason;
		phys = run->phys_addr;
	}
	fprintf(stderr, "syz_gunyah identify x0=%#llx marker=%#llx exit=%u phys=%#llx\n",
		(unsigned long long)scratch[1], (unsigned long long)scratch[0],
		exit_reason, (unsigned long long)phys);
	fflush(stderr);
}

static long syz_gunyah_add_vcpu(volatile long a0, volatile long a1)
{
	struct gh_syz_vm* vm = (struct gh_syz_vm*)a0;
	struct gh_text* utext = (struct gh_text*)a1;
	int cpu_id;
	int vcpu;
	struct gh_fn_vcpu_arg varg;
	struct gh_fn_desc desc;
	long pagesz;
	struct gh_vcpu_run* run = NULL;
	int i;
	size_t text_size = 0;
	uintptr_t pc;
	uintptr_t sp;

	if (!vm || !utext || !vm->user_text) {
		errno = EINVAL;
		return -1;
	}
	if (vm->started) {
		errno = EBUSY;
		return -1;
	}
	if (vm->next_cpu_id >= KVM_MAX_VCPU) {
		errno = ENOMEM;
		return -1;
	}
	cpu_id = vm->next_cpu_id;
	if (utext->text && utext->size) {
		text_size = utext->size;
		if (text_size > KVM_PAGE_SIZE)
			text_size = KVM_PAGE_SIZE;
		memcpy((uint8*)vm->user_text + (cpu_id * KVM_PAGE_SIZE), utext->text, text_size);
	}

	// SP index 1 is SP_EL1. The RM rejects index >= 2. EL1h uses SP_EL1.
	pc = executor_fn_guest_addr(guest_main);
	sp = ARM64_ADDR_EL1_STACK_BOTTOM + KVM_PAGE_SIZE - 128;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_PC, 0), pc))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_SP, 1), sp))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 0), text_size))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 1), cpu_id))
		return -1;

	memset(&varg, 0, sizeof(varg));
	varg.id = cpu_id;
	memset(&desc, 0, sizeof(desc));
	desc.type = GH_FN_VCPU;
	desc.arg_size = sizeof(varg);
	desc.arg = (uint64)&varg;
	vcpu = ioctl(vm->vmfd, GH_IOCTL_ADD_FUNCTION, &desc);
	if (vcpu < 0)
		return -1;

	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz > 0) {
		void* mapped = mmap(0, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
		if (mapped != MAP_FAILED)
			run = (struct gh_vcpu_run*)mapped;
	}

	if (ioctl(vm->vmfd, GH_IOCTL_VM_START)) {
		// START failed after the boot context was queued. Don't try again.
		vm->started = 1;
		close(vcpu);
		return -1;
	}
	vm->started = 1;
	vm->next_cpu_id++;

	// Demand-paged faults are retried inside the driver. Stop on the first
	// exit userspace has to answer, and mark a page fault as not handled so
	// a later GH_VCPU_RUN does not pretend the access succeeded.
	for (i = 0; i < 4; i++) {
		if (ioctl(vcpu, GH_IOCTL_VCPU_RUN))
			break;
		if (!run)
			break;
		if (run->exit_reason == GH_VCPU_EXIT_PAGE_FAULT) {
			run->resume_action = GH_VCPU_RESUME_FAULT;
			break;
		}
		if (run->exit_reason == GH_VCPU_EXIT_STATUS || run->exit_reason == GH_VCPU_EXIT_MMIO)
			break;
	}
	gh_note_identify(vm, run);
	return vcpu;
}

#endif

#endif
