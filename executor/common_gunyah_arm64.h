// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

// This file is shared between executor and csource package.
//
// Non-protected (share) Gunyah VM running the SYZOS guest so the fuzzer drives
// real guest behaviour. syz_gunyah_setup_vm maps the SYZOS parcels (see the
// layout block below) via GH_VM_SET_USER_MEM_REGION and points SET_DTB at the
// DTB parcel; syz_gunyah_add_vcpu copies the fuzzer's api_call list into the
// user parcel and boots GH_VM_SET_BOOT_CONTEXT's PC to guest_main at the exec
// parcel (0xeeee8000), x0 = list byte count, x1 = cpu. RM auth stays AUTH_NONE
// (no GH_VM_ANDROID_SET_FW_CONFIG). Probe-verified 2026-09-28 that a share VM
// accepts the high-IPA parcels and executes from them; only the lend/protected
// path hits the RM sibling-donate DENIED (VM_START error 9), so
// syz_gunyah_setup_vm_lend maps the same layout as a lend and fails cleanly at
// VM_START (kept as the protected-path reverse test, not a running guest).
// The DTB's vdevices (doorbell, message-queue) are realized by the Manager
// at VM_INIT; its interrupt-controller node makes teardown clean. Protocol:
//   klinux drivers/virt/gunyah/vm_mgr.c gunyah_vm_start / gunyah_vm_set_boot_context
//   klinux include/uapi/linux/gunyah.h
//   futlab gunyah-resource-manager vm_firmware_vm_set_boot_context_default

#ifndef EXECUTOR_COMMON_GUNYAH_ARM64_H
#define EXECUTOR_COMMON_GUNYAH_ARM64_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common_kvm.h"
#include "common_kvm_arm64_syzos.h"

#if SYZ_EXECUTOR || __NR_syz_gunyah_setup_vm || __NR_syz_gunyah_setup_vm_lend || __NR_syz_gunyah_add_vcpu

// kylin include/uapi/linux/gunyah.h. _IO('G', nr) and _IOW('G', nr, size).
// Hardcoded because the build machine's linux/gunyah.h is not this driver's UAPI.
#define GH_IOCTL_SET_USER_MEM_REGION 0x40204701u
#define GH_IOCTL_SET_DTB_CONFIG 0x40104702u
#define GH_IOCTL_VM_START 0x4703u
#define GH_IOCTL_ADD_FUNCTION 0x40104704u
#define GH_IOCTL_VCPU_RUN 0x4705u
#define GH_IOCTL_SET_BOOT_CONTEXT 0x4010470au
// _IOW('A', 0x11, struct gunyah_userspace_memory_region) and
// _IOW('A', 0x12, struct gunyah_vm_firmware_config).
// kylin include/uapi/linux/gunyah.h (klinux-s2fuzz). Lending the image
// parcel selects MEM_LEND and marks the VM protected. SET_FW_CONFIG is
// not issued by these helpers: it switches auth to ANDROID_PVM, and the
// driver then ignores GH_VM_SET_BOOT_CONTEXT.
#define GH_IOCTL_LEND_USER_MEM 0x40204111u
#define GH_IOCTL_SET_FW_CONFIG 0x40104112u

#define GH_FN_VCPU 1u
#define GH_FN_IRQFD 2u
#define GH_IRQFD_LEVEL 1u
// Matches the doorbell-source qcom,label in gh_syzos_dtb.
#define GH_DB_SRC_LABEL 7u
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

// One image parcel. GPA 0 matches the unset fw.config.guest_phys_addr, so
// this binding is excluded from demand paging. The DTB occupies page 0 (built
// at runtime by gh_build_dtb from the fuzzer's vdevice list). The fixed payload
// occupies page 1 and is the boot PC.
//
// SYZOS guest execution (running the fuzzer's kvm_text_arm64 op list through
// guest_main at the high SYZOS IPAs) was implemented and then PARKED: a guest
// that produces no trap/exit wedges the vcpu unrecoverably under QEMU TCG proxy
// scheduling. See the finding docs/artifacts/xhyper-fuzz-findings-20260928/
// guest-compute-loop-wedges-vcpu/ and the parked source under
// .fuzz-wip/parked-syzos-guest/ for how to revive it.
#define GH_SYZOS_DTB_GPA 0x0ull
#define GH_SYZOS_DTB_SIZE (2 * KVM_PAGE_SIZE)
#define GH_PLANE_PC 0x1000ull
#define GH_PLANE_SP 0x1f00ull

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

struct gh_fn_irqfd_arg {
	uint32 fd;
	uint32 label;
	uint32 flags;
	uint32 padding;
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

// dtc -I dts -O dtb. Flat blob, magic d00dfeed. The SYZOS nodes (cpu@0,
// gunyah-vm-config image-name=syzos, vm-attrs=no-dtb-patch, memory
// base-address=0x80000000, vcpus affinity=proxy) and the vdevices the Manager
// realizes at VM_INIT (doorbell-source label 7 = BELL_TX for irqfd, doorbell
// label 4, message-queue send label 8, receive label 9, pair label 10; all
// peer-default = the HLOS), PLUS a root interrupt controller:
//   interrupt-controller@12000000 arm,gic-v3, GICD 0x12000000/0x20000,
//   GICR 0x12040000/0x100000, redistributor-stride 0x20000, #interrupt-cells 3,
//   phandle 1, root interrupt-parent = <1>.
// Without this node the Manager's ResetCleanup deletes a capability that was
// never created (MANAGER_FATAL reason=domain kind=ResetCleanup +
// ROOTVM_FATAL), killing the root VM on every teardown, so any program that
// actually booted a guest crashed at close(fd) and never reached the corpus --
// mutation had no foothold. The node is taken verbatim from a real running
// gunyah guest (T11206 host-fdt.dts interrupt-controller@12000000) and verified
// on QEMU to tear down cleanly (RM survives, VMs stay re-creatable). Evidence:
// docs/artifacts/xhyper-fuzz-findings-20260928/manager-resetcleanup-no-gic-dtb/
// (contrast-gic.dtb sha256 134ac558...; .dts = grok-s1/syzos-gic.dtb decompiled).
// The no-interrupt-controller variant is kept as that finding's reverse test.
static const uint8 gh_syzos_dtb[] = {
	0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x05, 0x91, 0x00, 0x00, 0x00, 0x38,
	0x00, 0x00, 0x04, 0x68, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x11,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x29,
	0x00, 0x00, 0x04, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x17,
	0x00, 0x00, 0x00, 0x00, 0x73, 0x79, 0x7a, 0x6b, 0x61, 0x6c, 0x6c, 0x65,
	0x72, 0x2c, 0x67, 0x75, 0x6e, 0x79, 0x61, 0x68, 0x2d, 0x73, 0x79, 0x7a,
	0x6f, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2b,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x63, 0x70, 0x75, 0x73,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x01, 0x63, 0x70, 0x75, 0x40, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x37,
	0x63, 0x70, 0x75, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b,
	0x00, 0x00, 0x00, 0x00, 0x61, 0x72, 0x6d, 0x2c, 0x61, 0x72, 0x6d, 0x2d,
	0x76, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x67, 0x75, 0x6e, 0x79,
	0x61, 0x68, 0x2d, 0x76, 0x6d, 0x2d, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06,
	0x00, 0x00, 0x00, 0x47, 0x73, 0x79, 0x7a, 0x6f, 0x73, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x52,
	0x6e, 0x6f, 0x2d, 0x64, 0x74, 0x62, 0x2d, 0x70, 0x61, 0x74, 0x63, 0x68,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x5b,
	0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x76, 0x63, 0x70, 0x75, 0x73, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x68,
	0x70, 0x72, 0x6f, 0x78, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x76, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x73,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x62, 0x2d, 0x73,
	0x72, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x00, 0x00, 0x71, 0x64, 0x6f, 0x6f, 0x72, 0x62, 0x65, 0x6c, 0x6c,
	0x2d, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x07,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x64, 0x62, 0x2d, 0x64,
	0x73, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x09,
	0x00, 0x00, 0x00, 0x71, 0x64, 0x6f, 0x6f, 0x72, 0x62, 0x65, 0x6c, 0x6c,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x6d, 0x71, 0x2d, 0x74, 0x78, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x71,
	0x6d, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x2d, 0x71, 0x75, 0x65, 0x75,
	0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa7, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x6d, 0x71, 0x2d, 0x72, 0x78, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x71,
	0x6d, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x2d, 0x71, 0x75, 0x65, 0x75,
	0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb1, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x6d, 0x71, 0x2d, 0x70, 0x61, 0x69, 0x72, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x71,
	0x6d, 0x65, 0x73, 0x73, 0x61, 0x67, 0x65, 0x2d, 0x71, 0x75, 0x65, 0x75,
	0x65, 0x2d, 0x70, 0x61, 0x69, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x0a,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x01, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x72, 0x75, 0x70,
	0x74, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x6c, 0x65, 0x72,
	0x40, 0x31, 0x32, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00,
	0x61, 0x72, 0x6d, 0x2c, 0x67, 0x69, 0x63, 0x2d, 0x76, 0x33, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xbd,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xce,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xd5,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xea, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x43,
	0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x04, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x01, 0x16, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03,
	0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x21, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x09,
	0x63, 0x6f, 0x6d, 0x70, 0x61, 0x74, 0x69, 0x62, 0x6c, 0x65, 0x00, 0x69,
	0x6e, 0x74, 0x65, 0x72, 0x72, 0x75, 0x70, 0x74, 0x2d, 0x70, 0x61, 0x72,
	0x65, 0x6e, 0x74, 0x00, 0x23, 0x61, 0x64, 0x64, 0x72, 0x65, 0x73, 0x73,
	0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x23, 0x73, 0x69, 0x7a, 0x65,
	0x2d, 0x63, 0x65, 0x6c, 0x6c, 0x73, 0x00, 0x64, 0x65, 0x76, 0x69, 0x63,
	0x65, 0x5f, 0x74, 0x79, 0x70, 0x65, 0x00, 0x72, 0x65, 0x67, 0x00, 0x69,
	0x6d, 0x61, 0x67, 0x65, 0x2d, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x76, 0x6d,
	0x2d, 0x61, 0x74, 0x74, 0x72, 0x73, 0x00, 0x62, 0x61, 0x73, 0x65, 0x2d,
	0x61, 0x64, 0x64, 0x72, 0x65, 0x73, 0x73, 0x00, 0x61, 0x66, 0x66, 0x69,
	0x6e, 0x69, 0x74, 0x79, 0x00, 0x76, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65,
	0x2d, 0x74, 0x79, 0x70, 0x65, 0x00, 0x71, 0x63, 0x6f, 0x6d, 0x2c, 0x6c,
	0x61, 0x62, 0x65, 0x6c, 0x00, 0x70, 0x65, 0x65, 0x72, 0x2d, 0x64, 0x65,
	0x66, 0x61, 0x75, 0x6c, 0x74, 0x00, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65,
	0x2d, 0x63, 0x61, 0x6e, 0x2d, 0x63, 0x6c, 0x65, 0x61, 0x72, 0x00, 0x69,
	0x73, 0x2d, 0x73, 0x65, 0x6e, 0x64, 0x65, 0x72, 0x00, 0x69, 0x73, 0x2d,
	0x72, 0x65, 0x63, 0x65, 0x69, 0x76, 0x65, 0x72, 0x00, 0x23, 0x69, 0x6e,
	0x74, 0x65, 0x72, 0x72, 0x75, 0x70, 0x74, 0x2d, 0x63, 0x65, 0x6c, 0x6c,
	0x73, 0x00, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x73, 0x00, 0x72, 0x65, 0x64,
	0x69, 0x73, 0x74, 0x72, 0x69, 0x62, 0x75, 0x74, 0x6f, 0x72, 0x2d, 0x73,
	0x74, 0x72, 0x69, 0x64, 0x65, 0x00, 0x23, 0x72, 0x65, 0x64, 0x69, 0x73,
	0x74, 0x72, 0x69, 0x62, 0x75, 0x74, 0x6f, 0x72, 0x2d, 0x72, 0x65, 0x67,
	0x69, 0x6f, 0x6e, 0x73, 0x00, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x72, 0x75,
	0x70, 0x74, 0x2d, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x6f, 0x6c, 0x6c, 0x65,
	0x72, 0x00, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x72, 0x75, 0x70, 0x74, 0x73,
	0x00, 0x70, 0x68, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x00,
};

// Runs at IPA GH_PLANE_PC with the MMU off. hvc #(0x6000+n) is the direct
// Gunyah hypercall. Caps 1..23 are tried; a miss returns inside the handler.
// The final load of IPA 0x4000 is unmapped and makes GH_VCPU_RUN return, so
// this guest always terminates -- unlike fuzzer-supplied guest code, which can
// wedge the vcpu (see the parked SYZOS path in the layout comment above).
static const uint8 gh_plane_payload[] = {
	0x33, 0x00, 0x80, 0xd2, 0xe0, 0x03, 0x13, 0xaa, 0x21, 0x00, 0x80, 0xd2,
	0xe2, 0x03, 0x1f, 0xaa, 0x62, 0x02, 0x0c, 0xd4, 0xe0, 0x03, 0x13, 0xaa,
	0x21, 0x00, 0x80, 0xd2, 0xe2, 0x03, 0x1f, 0xaa, 0x42, 0x02, 0x0c, 0xd4,
	0xe0, 0x03, 0x13, 0xaa, 0x81, 0x00, 0x80, 0xd2, 0x02, 0x00, 0x83, 0xd2,
	0x23, 0x00, 0x80, 0xd2, 0xe4, 0x03, 0x1f, 0xaa, 0x62, 0x03, 0x0c, 0xd4,
	0xe0, 0x03, 0x13, 0xaa, 0x01, 0x00, 0x83, 0xd2, 0x02, 0x08, 0x80, 0xd2,
	0xe3, 0x03, 0x1f, 0xaa, 0x82, 0x03, 0x0c, 0xd4, 0x73, 0x06, 0x00, 0x91,
	0x7f, 0x62, 0x00, 0xf1, 0x63, 0xfd, 0xff, 0x54, 0x20, 0x22, 0x82, 0xd2,
	0x01, 0x80, 0x83, 0xd2, 0x20, 0x00, 0x00, 0xf9, 0x01, 0x00, 0x88, 0xd2,
	0x20, 0x00, 0x40, 0xf9, 0x00, 0x00, 0x00, 0x14
};

// ---- Runtime device-tree builder ----------------------------------------
// The guest DTB is built here rather than embedded as a fixed blob, so the
// fuzzer controls the vdevice list. The Manager realizes every vdevice at
// VM_INIT (object create / configure / activate / bind); those paths are
// otherwise only reached once, during boot. Flattened DT v17: header, an empty
// memory-reservation block, a structure block of big-endian tokens, and a
// strings block holding the property names.
#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_END 9u
#define GH_FDT_STRUCT_CAP 2560
#define GH_FDT_STRINGS_CAP 768
#define GH_MAX_VDEVICES 8

// The vdevice types the Manager decodes (xhyper-fdt secondary.rs
// decode_vdevice_type). Index order must match gh_vdevice_type in
// sys/linux/dev_gunyah_arm64.txt.
static const char* const gh_vdevice_types[] = {
    "doorbell-source", "doorbell", "message-queue", "message-queue-pair",
    "shm", "shm-doorbell", "watchdog", "vrtc-pl031", "iomem",
    "virtio-mmio", "rm-rpc", "virtio-pci", "virtio-iommu", "vsmmu-v2", "pci",
};
// The types a document may carry. virtio-mmio (9), virtio-pci (11) and pci (14)
// are absent because each hangs GH_VM_START unrecoverably; keeping the list here
// rather than only in the descriptions is what makes that exclusion hold, since
// a description-level set can be escaped (see the fold in gh_build_dtb).
static const uint8 gh_vd_allowed[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 13};

#define GH_VD_TYPE_DOORBELL_SRC 0
#define GH_VD_TYPE_DOORBELL 1
#define GH_VD_TYPE_MSGQ 2
#define GH_VD_TYPE_MSGQ_PAIR 3
#define GH_VD_TYPE_SHM 4
#define GH_VD_TYPE_SHM_DOORBELL 5
#define GH_VD_TYPE_WATCHDOG 6
#define GH_VD_TYPE_VRTC 7
#define GH_VD_TYPE_IOMEM 8
#define GH_VD_TYPE_VIRTIO_MMIO 9
#define GH_VD_TYPE_RM_RPC 10
#define GH_VD_TYPE_VIRTIO_PCI 11
#define GH_VD_TYPE_VIRTIO_IOMMU 12
#define GH_VD_TYPE_VSMMU_V2 13
#define GH_VD_TYPE_PCI 14

#define GH_VD_PEER_DEFAULT (1u << 0)
#define GH_VD_SOURCE_CAN_CLEAR (1u << 1)
#define GH_VD_IS_SENDER (1u << 2)
#define GH_VD_IS_RECEIVER (1u << 3)
#define GH_VD_GENERATE (1u << 4)
#define GH_VD_DMA_COHERENT (1u << 5)
#define GH_VD_ALLOCATE_BASE (1u << 6)

// Properties that live on the config node itself rather than on a vdevice.
// They are a second mutable surface: the Manager parses them in
// parse_secondary_configuration before it ever looks at the vdevice list, and
// nothing in the vdevice array can reach them. Mirrors gh_vmconfig in
// sys/linux/dev_gunyah_arm64.txt; the layout is naturally aligned at 112 bytes.
// vm-attrs entries. "crash-fatal" is deliberately NOT here: the Manager
// rejects it outright on an untrusted configuration (configuration.rs:2854),
// and a fuzzed secondary VM is always untrusted, so every document carrying it
// fails at Init and the VM never starts -- a whole program spent for nothing.
// The rest are the conditionally-emitting attributes, each of which reaches a
// parser branch that is otherwise dead.
#define GH_CFG_ATTR_CONTEXT_DUMP (1u << 0)
// Dropping "no-dtb-patch" is what opens the Manager's device-tree PROJECTION
// path. That attribute maps to preserve_dtb (xhyper-fdt secondary.rs:829): with
// it the Manager leaves the document alone, so rewrite_secondary_projection and
// the whole write side -- write_pci_interrupts, write_pci_msi, write_peer,
// write_queue_pair_properties, append_interrupt, append_number_cells,
// ensure_path -- never run. Measured 2026-09-29: none of them appears in the
// Manager's cumulative bitmap, because this builder emitted the attribute
// unconditionally. It is now the fuzzer's choice, so both the preserve path and
// the rewrite path are reachable; the rewrite path writes into a caller-supplied
// buffer with lengths derived from the document, which is where a bounds error
// would be a memory-safety problem rather than a rejected input.
#define GH_CFG_ATTR_NO_DTB_PATCH (1u << 1)
#define GH_CFG_ATTR_GUEST_RAM_DUMP (1u << 2)
#define GH_CFG_ATTR_NOSVE (1u << 3)
#define GH_CFG_ATTR_VPM_VIRQ (1u << 4)
#define GH_CFG_ATTR_NO_VPM_AGG (1u << 5)
#define GH_CFG_MEM_LIMITS (1u << 0)
#define GH_CFG_FIRMWARE (1u << 1)
#define GH_CFG_CONSOLE (1u << 2)
#define GH_CFG_MAX_IRQ_PAIRS 4
#define GH_CFG_MAX_VMMIO 2
#define GH_CFG_MAX_IOMEM 2
#define GH_CFG_MAX_TIMER_SPEC 6

struct gh_vmconfig {
	uint32 attrs;
	uint32 present;
	uint32 nirq;
	uint32 nvmmio;
	uint32 irq[GH_CFG_MAX_IRQ_PAIRS * 2];
	uint64 vmmio[GH_CFG_MAX_VMMIO * 2];
	uint64 size_min;
	uint64 size_max;
	uint64 fw_addr;
	uint64 fw_size_max;
	uint32 niomem;
	uint32 ntimer;
	uint32 timer[GH_CFG_MAX_TIMER_SPEC];
	uint32 iomem_access[GH_CFG_MAX_IOMEM];
	uint64 iomem[GH_CFG_MAX_IOMEM * 3];
};

// Mirrors gh_vdevice in sys/linux/dev_gunyah_arm64.txt.
struct gh_vdevice {
	uint32 typ;
	uint32 label;
	uint32 flags;
	uint32 reserved;
	uint64 arg;
};

struct gh_fdt {
	uint8 st[GH_FDT_STRUCT_CAP];
	size_t st_len;
	char str[GH_FDT_STRINGS_CAP];
	size_t str_len;
	int bad;
};

static uint32 gh_be32(uint32 v)
{
	return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
}

static void gh_fdt_put(struct gh_fdt* f, const void* p, size_t n)
{
	if (f->st_len + n > sizeof(f->st)) {
		f->bad = 1;
		return;
	}
	memcpy(f->st + f->st_len, p, n);
	f->st_len += n;
}

static void gh_fdt_u32(struct gh_fdt* f, uint32 v)
{
	uint32 be = gh_be32(v);
	gh_fdt_put(f, &be, sizeof(be));
}

static void gh_fdt_align(struct gh_fdt* f)
{
	static const uint8 zero[4] = {0, 0, 0, 0};
	size_t rem = f->st_len & 3;
	if (rem)
		gh_fdt_put(f, zero, 4 - rem);
}

// Property names live once in the shared strings block.
static uint32 gh_fdt_intern(struct gh_fdt* f, const char* name)
{
	size_t n = strlen(name) + 1;
	size_t i = 0;
	while (i < f->str_len) {
		if (strcmp(f->str + i, name) == 0)
			return (uint32)i;
		i += strlen(f->str + i) + 1;
	}
	if (f->str_len + n > sizeof(f->str)) {
		f->bad = 1;
		return 0;
	}
	memcpy(f->str + f->str_len, name, n);
	i = f->str_len;
	f->str_len += n;
	return (uint32)i;
}

static void gh_node(struct gh_fdt* f, const char* name)
{
	gh_fdt_u32(f, FDT_BEGIN_NODE);
	gh_fdt_put(f, name, strlen(name) + 1);
	gh_fdt_align(f);
}

static void gh_node_end(struct gh_fdt* f)
{
	gh_fdt_u32(f, FDT_END_NODE);
}

static void gh_prop(struct gh_fdt* f, const char* name, const void* val, size_t len)
{
	gh_fdt_u32(f, FDT_PROP);
	gh_fdt_u32(f, (uint32)len);
	gh_fdt_u32(f, gh_fdt_intern(f, name));
	if (len) {
		gh_fdt_put(f, val, len);
		gh_fdt_align(f);
	}
}

static void gh_prop_empty(struct gh_fdt* f, const char* name)
{
	gh_prop(f, name, NULL, 0);
}

static void gh_prop_str(struct gh_fdt* f, const char* name, const char* s)
{
	gh_prop(f, name, s, strlen(s) + 1);
}

static void gh_prop_u32(struct gh_fdt* f, const char* name, uint32 v)
{
	uint32 be = gh_be32(v);
	gh_prop(f, name, &be, sizeof(be));
}

// Two cells, matching #address-cells/#size-cells = 2.
static void gh_prop_u64(struct gh_fdt* f, const char* name, uint64 v)
{
	uint32 be[2] = {gh_be32((uint32)(v >> 32)), gh_be32((uint32)v)};
	gh_prop(f, name, be, sizeof(be));
}

static size_t gh_fdt_emit(struct gh_fdt* f, uint8* out, size_t cap)
{
	const size_t hdr = 40;
	const size_t rsv = 16; // a single terminating (0, 0) entry
	size_t off_struct;
	size_t off_strings;
	size_t total;
	uint32* h;

	gh_fdt_u32(f, FDT_END);
	if (f->bad)
		return 0;
	off_struct = hdr + rsv;
	off_strings = off_struct + f->st_len;
	total = off_strings + f->str_len;
	if (total > cap)
		return 0;
	memset(out, 0, total);
	h = (uint32*)out;
	h[0] = gh_be32(FDT_MAGIC);
	h[1] = gh_be32((uint32)total);
	h[2] = gh_be32((uint32)off_struct);
	h[3] = gh_be32((uint32)off_strings);
	h[4] = gh_be32((uint32)hdr);
	h[5] = gh_be32(17);
	h[6] = gh_be32(16);
	h[7] = 0;
	h[8] = gh_be32((uint32)f->str_len);
	h[9] = gh_be32((uint32)f->st_len);
	memcpy(out + off_struct, f->st, f->st_len);
	memcpy(out + off_strings, f->str, f->str_len);
	return total;
}

// Build the guest DTB: a fixed skeleton plus the fuzzer's vdevice list. The
// skeleton reproduces the blob this file used to embed -- including the root
// interrupt-controller node, without which the Manager's ResetCleanup deletes a
// capability that was never created and kills the root VM on every teardown
// (values taken verbatim from a real running gunyah guest, T11206 host-fdt).
static size_t gh_build_dtb(uint8* out, size_t cap, const struct gh_vdevice* vd, size_t nvd,
			   const struct gh_vmconfig* cfg)
{
	struct gh_fdt f;
	size_t i;

	memset(&f, 0, sizeof(f));
	gh_node(&f, "");
	gh_prop_str(&f, "compatible", "syzkaller,gunyah-syzos");
	gh_prop_u32(&f, "interrupt-parent", 1);
	gh_prop_u32(&f, "#address-cells", 2);
	gh_prop_u32(&f, "#size-cells", 2);

	gh_node(&f, "cpus");
	gh_prop_u32(&f, "#address-cells", 1);
	gh_prop_u32(&f, "#size-cells", 0);
	gh_node(&f, "cpu@0");
	gh_prop_str(&f, "device_type", "cpu");
	gh_prop_str(&f, "compatible", "arm,arm-v8");
	gh_prop_u32(&f, "reg", 0);
	gh_node_end(&f);
	gh_node_end(&f);

	gh_node(&f, "gunyah-vm-config");
	gh_prop_str(&f, "image-name", "syzos");
	// vm-attrs is a string list, so attributes are appended rather than
	// substituted. Each one is the fuzzer's choice, including no-dtb-patch
	// (see its definition: leaving it out is what reaches the projection
	// writer). An empty list is still emitted as an empty property so the
	// parser takes the same branch either way.
	{
		// A description-level flags set is escapable (one value in a hundred
		// is fully random), so the two attributes that get a document refused
		// outright are cleared here as well -- removing them from the set is
		// necessary but not sufficient, the same reason the vdevice type fold
		// lives in the executor.
		uint32 attrbits = cfg ? (cfg->attrs & ~(GH_CFG_ATTR_CONTEXT_DUMP | GH_CFG_ATTR_GUEST_RAM_DUMP)) : 0;
		static const struct {
			uint32 bit;
			const char* name;
		} vm_attrs[] = {
		    {GH_CFG_ATTR_NO_DTB_PATCH, "no-dtb-patch"},
		    {GH_CFG_ATTR_CONTEXT_DUMP, "context-dump"},
		    {GH_CFG_ATTR_GUEST_RAM_DUMP, "guest-ram-dump"},
		    {GH_CFG_ATTR_NOSVE, "nosve"},
		    {GH_CFG_ATTR_VPM_VIRQ, "vpm-virq"},
		    {GH_CFG_ATTR_NO_VPM_AGG, "no-vpm-aggregation"},
		};
		char attrs[128];
		size_t n = 0, k;
		for (k = 0; cfg && k < sizeof(vm_attrs) / sizeof(vm_attrs[0]); k++) {
			size_t len;
			if (!(attrbits & vm_attrs[k].bit))
				continue;
			len = strlen(vm_attrs[k].name) + 1;
			if (n + len > sizeof(attrs))
				break;
			memcpy(attrs + n, vm_attrs[k].name, len);
			n += len;
		}
		if (n)
			gh_prop(&f, "vm-attrs", attrs, n);
		else
			gh_prop_empty(&f, "vm-attrs");
	}
	gh_prop_u32(&f, "#address-cells", 2);
	gh_prop_u32(&f, "#size-cells", 2);
	if (cfg && cfg->nirq) {
		// Pairs of 32-bit cells; a length that is not a multiple of eight
		// bytes is rejected and takes the whole document with it, so the
		// count is a pair count and the buffer is filled two cells at a time.
		uint32 be[GH_CFG_MAX_IRQ_PAIRS * 2];
		uint32 npair = cfg->nirq > GH_CFG_MAX_IRQ_PAIRS ? GH_CFG_MAX_IRQ_PAIRS : cfg->nirq;
		for (i = 0; i < npair * 2; i++)
			be[i] = gh_be32(cfg->irq[i]);
		gh_prop(&f, "gic-irq-ranges", be, npair * 2 * sizeof(uint32));
	}
	if (cfg && cfg->nvmmio) {
		// Address plus size, each two cells, matching #address-cells = 2.
		uint32 be[GH_CFG_MAX_VMMIO * 4];
		uint32 n = cfg->nvmmio > GH_CFG_MAX_VMMIO ? GH_CFG_MAX_VMMIO : cfg->nvmmio;
		for (i = 0; i < n * 2; i++) {
			uint64 v = cfg->vmmio[i];
			if (i % 2 == 1) {
				// The size half: the adapter rejects an empty range and one
				// whose address plus size overflows, so keep it non-zero and
				// bounded rather than passing the raw value through.
				v = (v & 0xffffffffull) | 1;
			} else {
				v &= 0xffffffffull;
			}
			be[i * 2] = gh_be32((uint32)(v >> 32));
			be[i * 2 + 1] = gh_be32((uint32)v);
		}
		gh_prop(&f, "vmmio-ranges", be, n * 4 * sizeof(uint32));
	}
	if (cfg && cfg->niomem) {
		// One record is physical address + guest address + size (each two
		// cells here, because the root node declares two) plus a 32-bit
		// access code, so 28 bytes; a length that is not a multiple of that
		// is rejected. The count is therefore a RECORD count, which makes an
		// illegal length unrepresentable.
		uint32 be[GH_CFG_MAX_IOMEM * 7];
		uint32 n = cfg->niomem > GH_CFG_MAX_IOMEM ? GH_CFG_MAX_IOMEM : cfg->niomem;
		for (i = 0; i < n; i++) {
			uint32* r = &be[i * 7];
			size_t k;
			for (k = 0; k < 3; k++) {
				r[k * 2] = gh_be32((uint32)(cfg->iomem[i * 3 + k] >> 32));
				r[k * 2 + 1] = gh_be32((uint32)cfg->iomem[i * 3 + k]);
			}
			r[6] = gh_be32(cfg->iomem_access[i]);
		}
		gh_prop(&f, "iomemory-ranges", be, n * 7 * sizeof(uint32));
	}
	gh_node(&f, "memory");
	gh_prop_u64(&f, "base-address", 0x80000000ull);
	if (cfg && (cfg->present & GH_CFG_MEM_LIMITS)) {
		// The parser requires a whole number of 4 KiB pages, and a raw
		// int64 is a multiple of 4096 about one time in ten, so passing one
		// through rejects the document nine times out of ten. Align instead:
		// the value still mutates, it just lands on a page boundary.
		gh_prop_u64(&f, "size-min", cfg->size_min & ~0xfffull);
		gh_prop_u64(&f, "size-max", cfg->size_max & ~0xfffull);
	}
	if (cfg && (cfg->present & GH_CFG_FIRMWARE)) {
		// These two are checked as a PAIR: the parser refuses the document
		// when base + size overflows, and two independent random 64-bit
		// values overflow about half the time. Each field is individually
		// legal, which is why this kind of constraint is the easiest to
		// miss -- only the combination is rejected. Fold the size into what
		// the address leaves room for; it still mutates freely below that.
		uint64 fwbase = cfg->fw_addr & ~0xfffull;
		uint64 fwsize = cfg->fw_size_max;
		if (fwbase != 0)
			fwsize %= (0xffffffffffffffffull - fwbase) + 1;
		gh_prop_u64(&f, "firmware-address", fwbase);
		gh_prop_u64(&f, "firmware-size-max", fwsize & ~0xfffull);
	}
	gh_node_end(&f);
	gh_node(&f, "vcpus");
	gh_prop_str(&f, "affinity", "proxy");
	gh_node_end(&f);

	gh_node(&f, "vdevices");
	int seen_vrtc = 0;
	for (i = 0; i < nvd; i++) {
		char name[8];
		uint32 t = vd[i].typ;

		// A description-level `flags` set is NOT a hard constraint: the
		// generator returns a fully random 64-bit value once in a hundred
		// (prog/rand.go randGen.flags), and randRangeInt does the same for an
		// int range -- only `const` is absolute. So a value outside
		// gh_vdevice_type does arrive here, and folding it over the WHOLE type
		// table would hand back exactly the three types that hang GH_VM_START
		// and leave the machine unrecoverable (virtio-mmio, virtio-pci, pci).
		// One such document poisons every later program in that boot and
		// records no crash, so the exclusion has to hold in the executor, where
		// it can be absolute. Allowed values keep their identity; everything
		// else folds into the allowed set.
		if (t >= sizeof(gh_vdevice_types) / sizeof(gh_vdevice_types[0]) ||
		    t == GH_VD_TYPE_VIRTIO_MMIO || t == GH_VD_TYPE_VIRTIO_PCI || t == GH_VD_TYPE_PCI)
			t = gh_vd_allowed[t % (sizeof(gh_vd_allowed) / sizeof(gh_vd_allowed[0]))];
		// A virtual RTC only becomes published when a projection is
		// installed, and start refuses to power on a VM that still has an
		// unpublished one. A document asking for its device tree to be
		// preserved never installs a projection, so the two together always
		// fail to start -- measured: the same document starts without the
		// RTC and fails with it, nothing else changed. Each part is legal on
		// its own; only the combination is not, the same shape as the
		// firmware address/size pair but one level up, between a document
		// attribute and a vdevice rather than between two fields.
		if (t == GH_VD_TYPE_VRTC && cfg && (cfg->attrs & GH_CFG_ATTR_NO_DTB_PATCH))
			t = GH_VD_TYPE_DOORBELL;
		// The virtual RTC is a singleton: the parser keeps a "seen" state for
		// it and rejects the whole document on the second one. That cannot be
		// said in the description language, which generates array elements
		// independently, so enforce it here. A duplicate becomes a doorbell,
		// a type measured as repeatable, rather than being dropped: the
		// device count the fuzzer chose still gets exercised. The PCI bus is
		// a singleton only while it carries no config property, and this
		// builder always emits one, so it needs no such state.
		if (t == GH_VD_TYPE_VRTC) {
			if (seen_vrtc)
				t = GH_VD_TYPE_DOORBELL;
			else
				seen_vrtc = 1;
		}
		// Node names must be unique within the parent or the document is
		// rejected, so number them instead of naming them after the type.
		name[0] = 'v';
		name[1] = 'd';
		name[2] = (char)('0' + (i / 10));
		name[3] = (char)('0' + (i % 10));
		name[4] = 0;
		gh_node(&f, name);
		gh_prop_str(&f, "vdevice-type", gh_vdevice_types[t]);
		gh_prop_u32(&f, "qcom,label", vd[i].label);
		// Each type has a skeleton of properties the Manager's parser
		// requires; omit one and the whole document is rejected before any
		// VM object is built, which costs a full program for nothing. So the
		// TYPE forces its required properties and only their VALUES stay
		// mutable -- the parser's error branches live in the values, not in
		// the absence of a property. The conditions below were measured
		// against parse_secondary_configuration, not read off the source.
		//
		// Bare (type name alone): watchdog, rm-rpc.
		if (t == GH_VD_TYPE_DOORBELL_SRC || t == GH_VD_TYPE_DOORBELL ||
		    t == GH_VD_TYPE_MSGQ || t == GH_VD_TYPE_MSGQ_PAIR ||
		    t == GH_VD_TYPE_VIRTIO_MMIO || t == GH_VD_TYPE_VIRTIO_PCI ||
		    t == GH_VD_TYPE_IOMEM || t == GH_VD_TYPE_PCI ||
		    t == GH_VD_TYPE_SHM || t == GH_VD_TYPE_SHM_DOORBELL)
			gh_prop_empty(&f, "peer-default");
		else if (vd[i].flags & GH_VD_PEER_DEFAULT)
			gh_prop_empty(&f, "peer-default");
		if (vd[i].flags & GH_VD_SOURCE_CAN_CLEAR)
			gh_prop_empty(&f, "source-can-clear");
		if (t == GH_VD_TYPE_MSGQ) {
			// Exactly one direction is mandatory; neither is a reject. Let
			// the flag bit choose which, so the fuzzer still picks.
			if (vd[i].flags & GH_VD_IS_SENDER)
				gh_prop_empty(&f, "is-sender");
			else
				gh_prop_empty(&f, "is-receiver");
			// Both land in a non-zero 16-bit field: zero is rejected and
			// takes the whole document with it, and the fuzzer reaches for
			// zero constantly. Fold arg into 1..65535 rather than passing it
			// through, and take the two halves from adjacent 16-bit slices so
			// one mutation can move them independently.
			uint32 msgsize = (uint32)(vd[i].arg & 0xffff);
			uint32 qdepth = (uint32)((vd[i].arg >> 16) & 0xffff);
			gh_prop_u32(&f, "message-size", msgsize ? msgsize : 1);
			gh_prop_u32(&f, "queue-depth", qdepth ? qdepth : 1);
		} else {
			if (vd[i].flags & GH_VD_IS_SENDER)
				gh_prop_empty(&f, "is-sender");
			if (vd[i].flags & GH_VD_IS_RECEIVER)
				gh_prop_empty(&f, "is-receiver");
		}
		if (vd[i].flags & GH_VD_DMA_COHERENT)
			gh_prop_empty(&f, "dma-coherent");
		if (vd[i].flags & GH_VD_GENERATE)
			gh_prop_str(&f, "generate", "/hypervisor");
		// Type-specific cell, only where the Manager looks for one: an MMIO
		// base for the memory-mapped kinds and a queue count for the virtio
		// kinds.
		// The virtual RTC wants exactly one of base and allocate-base, and a
		// page-aligned base; virtio-mmio is not aligned-checked but shares
		// the emission.
		if (t == GH_VD_TYPE_VRTC)
			gh_prop_u64(&f, "base", vd[i].arg & ~0xfffull);
		else if (t == GH_VD_TYPE_VIRTIO_MMIO)
			gh_prop_u64(&f, "base", vd[i].arg);
		if (t == GH_VD_TYPE_VIRTIO_MMIO || t == GH_VD_TYPE_VIRTIO_PCI)
			gh_prop_u32(&f, "vqs-num", (uint32)vd[i].arg);
		if (t == GH_VD_TYPE_VIRTIO_IOMMU) {
			// All three are mandatory and none of them is the label. Note
			// the handle is 64 bits here but 32 bits on vsmmu-v2 below: the
			// same property name, a different width, and a wrong width is
			// rejected with the same error as a missing property.
			gh_prop_u64(&f, "smmu-handle", vd[i].arg);
			gh_prop_u32(&f, "max-streams", (uint32)vd[i].arg);
			gh_prop_u32(&f, "pci-slot-index", (uint32)(vd[i].arg >> 32));
		}
		if (t == GH_VD_TYPE_VSMMU_V2) {
			gh_prop_u32(&f, "smmu-handle", (uint32)vd[i].arg);
			gh_prop_str(&f, "patch", "/hypervisor");
			// Both narrow to 8 bits, so anything above 255 is rejected.
			gh_prop_u32(&f, "num-cbs", (uint32)((vd[i].arg >> 32) & 0xff));
			gh_prop_u32(&f, "num-smrs", vd[i].label & 0xff);
		}
		if (t == GH_VD_TYPE_PCI) {
			// npmem-size is accepted only as a non-zero power of two, so
			// derive one from arg instead of passing it through: a document
			// rejected here never reaches the value checks this property
			// exists to exercise. k spans 0..63, which includes the sizes
			// the parser lets through but no allocator could honour.
			gh_prop_u64(&f, "npmem-size", 1ull << (vd[i].arg & 63));
			// A pci bus without config is a singleton -- a second one is
			// rejected and takes the whole document with it. The parser only
			// tests whether config is present, not its value, but it must be
			// 4 or 8 bytes wide because it is read as a number. Emitting one
			// unconditionally lifts the singleton rule.
			gh_prop_u32(&f, "config", 0);
		}
		if (t == GH_VD_TYPE_SHM || t == GH_VD_TYPE_SHM_DOORBELL) {
			// The shared-memory kinds carry their placement in a "memory"
			// CHILD node, not on the node itself, and need at least one of
			// base / allocate-base there. Flat properties are rejected.
			gh_node(&f, "memory");
			if (vd[i].flags & GH_VD_ALLOCATE_BASE)
				gh_prop_empty(&f, "allocate-base");
			else
				gh_prop_u64(&f, "base", vd[i].arg);
			if (t == GH_VD_TYPE_SHM)
				gh_prop_u64(&f, "size", 0x1000);
			gh_node_end(&f);
		} else if (t == GH_VD_TYPE_IOMEM) {
			// iomem also keeps its label in a "memory" child, and that
			// label must be 32 bits wide -- a 64-bit one is rejected with
			// the same error as a missing property, so width is not
			// something the fuzzer can discover on its own.
			gh_node(&f, "memory");
			gh_prop_u32(&f, "qcom,label", vd[i].label);
			gh_node_end(&f);
		} else if ((vd[i].flags & GH_VD_ALLOCATE_BASE) && t != GH_VD_TYPE_VRTC) {
			// Not on the virtual RTC: it already emitted a base above, and
			// the parser rejects a node carrying both.
			gh_prop_empty(&f, "allocate-base");
		}
		gh_node_end(&f);
	}
	gh_node_end(&f); // vdevices
	gh_node_end(&f); // gunyah-vm-config

	gh_node(&f, "interrupt-controller@12000000");
	gh_prop_str(&f, "compatible", "arm,gic-v3");
	gh_prop_u32(&f, "#interrupt-cells", 3);
	gh_prop_u32(&f, "#address-cells", 2);
	gh_prop_u32(&f, "#size-cells", 2);
	gh_prop_empty(&f, "ranges");
	gh_prop_u64(&f, "redistributor-stride", 0x20000ull);
	gh_prop_u32(&f, "#redistributor-regions", 1);
	gh_prop_empty(&f, "interrupt-controller");
	{
		// reg = <0 0x12000000 0 0x20000  0 0x12040000 0 0x100000>
		uint32 be[8] = {0, gh_be32(0x12000000), 0, gh_be32(0x20000),
				0, gh_be32(0x12040000), 0, gh_be32(0x100000)};
		gh_prop(&f, "reg", be, sizeof(be));
	}
	{
		// interrupts = <1 9 4>
		uint32 be[3] = {gh_be32(1), gh_be32(9), gh_be32(4)};
		gh_prop(&f, "interrupts", be, sizeof(be));
	}
	gh_prop_u32(&f, "phandle", 1);
	gh_node_end(&f);

	// Two more root-level nodes the Manager looks for. They are siblings of
	// the interrupt controller, not children of the config node.
	if (cfg && (cfg->present & GH_CFG_CONSOLE)) {
		// The console window is only recognised at exactly this base, so the
		// address is fixed and only its presence is fuzzed.
		uint32 reg[4] = {gh_be32(0), gh_be32(0x09000000), gh_be32(0), gh_be32(0x1000)};
		uint32 irq[3] = {gh_be32(0), gh_be32(1), gh_be32(4)};
		gh_node(&f, "serial@9000000");
		gh_prop_str(&f, "compatible", "arm,pl011");
		gh_prop(&f, "reg", reg, sizeof(reg));
		gh_prop(&f, "interrupts", irq, sizeof(irq));
		gh_node_end(&f);
	}
	if (cfg && cfg->ntimer) {
		// The specifier COUNT selects the branch: fewer than four is the
		// malformed path, four or more the vcpu-timer path. Both are worth
		// reaching, so the count is left free and the VALUES are kept legal,
		// because an out-of-range number would force the malformed path from
		// the other side and make the count meaningless.
		uint32 be[GH_CFG_MAX_TIMER_SPEC * 3];
		uint32 n = cfg->ntimer > GH_CFG_MAX_TIMER_SPEC ? GH_CFG_MAX_TIMER_SPEC : cfg->ntimer;
		static const uint32 flagvals[3] = {1, 4, 8};
		for (i = 0; i < n; i++) {
			uint32 v = cfg->timer[i];
			uint32 typ = v & 3;
			uint32 span, num;
			// The parser computes virq = number + offset and requires
			// virq < limit, so the legal NUMBER runs 0..(limit-offset-1).
			// This used to emit the final virq instead, which the parser
			// then offset a second time -- every type but 1 was out of range
			// (type 1 only looked right because its offset and limit make the
			// two ranges coincide). Nothing rejected the document: the error
			// is swallowed and the node is just marked malformed, so the
			// normal timer path was taken about 6% of the time while looking
			// entirely healthy.
			switch (typ) {
			case 1:
				span = 32 - 16;
				break;
			case 2:
				span = 5120 - 4096;
				break;
			case 3:
				span = 1120 - 1056;
				break;
			default:
				span = 1020 - 32;
				break;
			}
			num = (v >> 8) % span;
			// Keep a slice out of range on purpose: the malformed-timer path
			// is a real branch, and making every specifier legal would make it
			// unreachable -- the same trade-off as drawing labels from a small
			// domain rather than fixing one.
			//
			// The predicate must not test for ZERO and must not overlap typ.
			// Measured over the 381 timer values in the live corpus on
			// 2026-09-29 (unpacked with syz-db; 207 of them are literally 0 and
			// the whole corpus holds only 28 distinct values):
			//
			//   (v & 0xf) == 0        fired 255/381 = 66.9%, ALL of them typ 0
			//   ((v >> 2) & 7) == 3   fires    9/381 =  2.4%, typ 0 and 3
			//
			// Two separate faults in the old form. It tested bits 0..3 for zero,
			// and bits 0..1 are typ, so an out-of-range specifier could only ever
			// be type 0 -- types 1..3 never reached the malformed path at all.
			// And because the values are overwhelmingly zero, testing any field
			// for zero makes the malformed path the COMMON case: the "one in ten"
			// was two in three, so the normal timer path this comment says it is
			// protecting stayed rare. That is the same failure the paragraph above
			// records being fixed once already, reintroduced by moving the test to
			// the low bits without checking what the low bits actually contain.
			//
			// With a distribution this degenerate there is no ~10% predicate to be
			// had from bit tests: a predicate either fires on zero, and so on 54%
			// at once, or it does not, and lands at 1-3%. 2.4% is the right side of
			// that choice -- the normal path is the valuable one and the malformed
			// branch still gets reached often across a run.
			//
			// Bit 4 is shared with the flags index below. That is deliberate:
			// flags do not select a branch, and narrowing them to their own window
			// ((v >> 5) & 7) % 3 was measured to make their spread worse
			// (338/20/23 becomes 325/54/2).
			if (((v >> 2) & 7) == 3)
				num += span;
			be[i * 3] = gh_be32(typ);
			be[i * 3 + 1] = gh_be32(num);
			be[i * 3 + 2] = gh_be32(flagvals[(v >> 4) % 3]);
		}
		gh_node(&f, "timer");
		gh_prop_str(&f, "compatible", "arm,armv8-timer");
		gh_prop(&f, "interrupts", be, n * 3 * sizeof(uint32));
		gh_node_end(&f);
	}

	gh_node_end(&f); // root
	return gh_fdt_emit(&f, out, cap);
}

static void* gh_bump_alloc(struct gh_bump* bump, size_t size)
{
	if (size > (size_t)(bump->end - bump->cur))
		return NULL;
	void* ret = bump->cur;
	memset(ret, 0, size);
	bump->cur += size;
	return ret;
}

static int gh_set_region(int vmfd, uint32 label, uint64 gpa, void* host, uint64 size, int lend)
{
	struct gh_userspace_memory_region region;

	memset(&region, 0, sizeof(region));
	region.label = label;
	region.flags = GH_MEM_RWX;
	region.guest_phys_addr = gpa;
	region.memory_size = size;
	region.userspace_addr = (uint64)host;
	return ioctl(vmfd, lend ? GH_IOCTL_LEND_USER_MEM : GH_IOCTL_SET_USER_MEM_REGION, &region);
}

// Register irqfd against the DTB doorbell-source before GH_VM_START so the
// driver's BELL_TX ticket is populated when hyp resources arrive. The eventfd
// stays quiet until gh_fire_irqfd, which is what runs bell_send.
static int gh_arm_irqfd(int vmfd)
{
	int efd;
	struct gh_fn_irqfd_arg iarg;
	struct gh_fn_desc desc;

	efd = eventfd(0, 0);
	if (efd < 0)
		return -1;
	memset(&iarg, 0, sizeof(iarg));
	iarg.fd = efd;
	iarg.label = GH_DB_SRC_LABEL;
	iarg.flags = GH_IRQFD_LEVEL;
	memset(&desc, 0, sizeof(desc));
	desc.type = GH_FN_IRQFD;
	desc.arg_size = sizeof(iarg);
	desc.arg = (uint64)&iarg;
	if (ioctl(vmfd, GH_IOCTL_ADD_FUNCTION, &desc) < 0) {
		close(efd);
		return -1;
	}
	return efd;
}

static void gh_fire_irqfd(int efd)
{
	uint64 one = 1;

	if (efd < 0)
		return;
	if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
		close(efd);
		return;
	}
	close(efd);
}

static int gh_set_boot(int vmfd, uint32 reg, uint64 value)
{
	struct gh_vm_boot_context ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.reg = reg;
	ctx.value = value;
	return ioctl(vmfd, GH_IOCTL_SET_BOOT_CONTEXT, &ctx);
}

// A small reused pool of guest-memory backing windows. Each window sits
// OUTSIDE the executor's [0x20000000, 0x21000000) data region (mmap(NULL) lands
// in the high mmap area), has a guard page on each side, and is marked
// MADV_NOHUGEPAGE. This keeps the host gunyah driver's 2 MiB huge-page parcel
// sharing off the executor's own syscall arguments (the F3/F4 host
// synchronous-external-abort family) and gives the parcel real host-owned pages
// to be donated from, which the older path (backing the guest with the fuzzer's
// vma inside the data region) could not. The pool is reused round-robin so the
// executor -- which runs many programs before a VM restart -- does not leak a
// 4 MiB mapping per VM setup, while still giving a handful of concurrent VMs
// distinct backing.
#define GH_MEM_POOL 8
static uint8* gh_guest_mem_pool[GH_MEM_POOL];
static unsigned gh_guest_mem_next;

static uint8* gh_map_guest_mem(void)
{
	unsigned slot = gh_guest_mem_next++ % GH_MEM_POOL;
	if (!gh_guest_mem_pool[slot]) {
		const size_t guard = KVM_PAGE_SIZE;
		const size_t total = guard + KVM_GUEST_MEM_SIZE + guard;
		uint8* base = (uint8*)mmap(NULL, total, PROT_NONE,
					   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (base == MAP_FAILED)
			return NULL;
		uint8* win = base + guard;
		if (mprotect(win, KVM_GUEST_MEM_SIZE, PROT_READ | PROT_WRITE)) {
			munmap(base, total);
			return NULL;
		}
		madvise(win, KVM_GUEST_MEM_SIZE, MADV_NOHUGEPAGE);
		// Never overlap the syzkaller data/input region.
		if ((uintptr_t)win < 0x21000000ull &&
		    (uintptr_t)win + KVM_GUEST_MEM_SIZE > 0x20000000ull) {
			munmap(base, total);
			return NULL;
		}
		gh_guest_mem_pool[slot] = win;
	}
	memset(gh_guest_mem_pool[slot], 0, KVM_GUEST_MEM_SIZE);
	return gh_guest_mem_pool[slot];
}

static long gh_setup_vm(volatile long a0, volatile long a1, volatile long a2,
			volatile long a3, volatile long a4, int lend)
{
	const int vmfd = a0;
	const struct gh_vmconfig* cfg = (const struct gh_vmconfig*)a4;
	// Guest backing is our own guarded, no-huge-page, host-owned window (see
	// gh_map_guest_mem), not the fuzzer vma. a1 stays in the signature for the
	// raw SET_USER_MEM_REGION reverse-test path.
	uint8* host_mem = gh_map_guest_mem();
	const struct gh_vdevice* vdev = (const struct gh_vdevice*)a2;
	size_t nvdev = (size_t)a3;
	struct gh_syz_vm* vm;
	struct gh_bump bump;
	uint8* dtb_pages;
	size_t dtb_len;
	struct gh_vm_dtb_config dtb;

	(void)a1;
	if (!host_mem) {
		errno = ENOMEM;
		return -1;
	}
	if (nvdev > GH_MAX_VDEVICES)
		nvdev = GH_MAX_VDEVICES;
	if (!vdev)
		nvdev = 0;

	// Page 0 of the window is host-only bookkeeping (never mapped to the guest).
	memset(host_mem, 0, KVM_PAGE_SIZE);
	vm = (struct gh_syz_vm*)host_mem;
	bump.cur = host_mem + KVM_PAGE_SIZE;
	bump.end = host_mem + KVM_GUEST_MEM_SIZE;

	// One image parcel at GPA 0: the DTB in page 0, the fixed payload in page 1.
	dtb_pages = (uint8*)gh_bump_alloc(&bump, GH_SYZOS_DTB_SIZE);
	if (!dtb_pages) {
		errno = ENOMEM;
		return -1;
	}

	// Build the DTB from the fuzzer's vdevice list. Fall back to the embedded
	// blob if the build overflows, so a VM is still started either way.
	dtb_len = gh_build_dtb(dtb_pages, KVM_PAGE_SIZE, vdev, nvdev, cfg);
	if (dtb_len == 0) {
		if (sizeof(gh_syzos_dtb) > KVM_PAGE_SIZE)
			fail("gunyah fallback dtb exceeds a page");
		memcpy(dtb_pages, gh_syzos_dtb, sizeof(gh_syzos_dtb));
	}
	if (sizeof(gh_plane_payload) > 0x800) {
		errno = EINVAL;
		return -1;
	}
	memcpy(dtb_pages + GH_PLANE_PC, gh_plane_payload, sizeof(gh_plane_payload));
	// Message buffer the payload addresses at IPA 0x1800.
	*(uint32*)(dtb_pages + 0x1800) = 0x706c616e;

	if (gh_set_region(vmfd, 1, GH_SYZOS_DTB_GPA, dtb_pages, GH_SYZOS_DTB_SIZE, lend))
		return -1;

	memset(&dtb, 0, sizeof(dtb));
	dtb.guest_phys_addr = GH_SYZOS_DTB_GPA;
	dtb.size = GH_SYZOS_DTB_SIZE;
	if (ioctl(vmfd, GH_IOCTL_SET_DTB_CONFIG, &dtb))
		return -1;

	vm->vmfd = vmfd;
	vm->next_cpu_id = 0;
	vm->started = 0;
	vm->user_text = dtb_pages + GH_PLANE_PC;
	vm->scratch = dtb_pages + 0x1c00;
	return (long)vm;
}

static long syz_gunyah_setup_vm(volatile long a0, volatile long a1, volatile long a2, volatile long a3,
				volatile long a4)
{
	return gh_setup_vm(a0, a1, a2, a3, a4, 0);
}

static long syz_gunyah_setup_vm_lend(volatile long a0, volatile long a1, volatile long a2, volatile long a3,
				     volatile long a4)
{
	return gh_setup_vm(a0, a1, a2, a3, a4, 1);
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
	int bell;
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
	// The fuzzer's kvm_text is NOT run as guest code. Guest execution of
	// fuzzer-supplied ops is PARKED: a guest that produces no trap/exit wedges
	// the vcpu unrecoverably (see the layout comment and the finding). The fixed
	// payload installed at setup time is what GH_VCPU_RUN executes, and it always
	// terminates. utext stays in the signature so the model keeps the argument
	// (and the descriptions keep no_squash) for when the path is revived.
	(void)text_size;
	(void)utext;

	// SP index 1 is SP_EL1. The RM rejects index >= 2. EL1h uses SP_EL1.
	pc = GH_PLANE_PC;
	sp = GH_PLANE_SP;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_PC, 0), pc))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_SP, 1), sp))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 0), text_size))
		return -1;
	if (gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 1), cpu_id))
		return -1;
	// Further valid indices. A rejection must not discard PC/SP/x0/x1.
	(void)gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 2), 0);
	(void)gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_X, 3), cpu_id);
	(void)gh_set_boot(vm->vmfd, GH_BOOT_REG(GH_REG_SET_SP, 0), sp);

	memset(&varg, 0, sizeof(varg));
	varg.id = cpu_id;
	memset(&desc, 0, sizeof(desc));
	desc.type = GH_FN_VCPU;
	desc.arg_size = sizeof(varg);
	desc.arg = (uint64)&varg;
	vcpu = ioctl(vm->vmfd, GH_IOCTL_ADD_FUNCTION, &desc);
	if (vcpu < 0)
		return -1;

	bell = gh_arm_irqfd(vm->vmfd);

	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz > 0) {
		void* mapped = mmap(0, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
		if (mapped != MAP_FAILED)
			run = (struct gh_vcpu_run*)mapped;
	}

	if (ioctl(vm->vmfd, GH_IOCTL_VM_START)) {
		// START failed after the boot context was queued. Don't try again.
		vm->started = 1;
		if (bell >= 0)
			close(bell);
		close(vcpu);
		return -1;
	}
	vm->started = 1;
	vm->next_cpu_id++;
	gh_fire_irqfd(bell);

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
