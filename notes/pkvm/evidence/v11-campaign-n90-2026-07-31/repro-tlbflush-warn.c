// SPDX-License-Identifier: GPL-2.0
/*
 * Reproducer: WARNING at arch/arm64/kvm/hyp/pgtable.c:654
 *             kvm_tlb_flush_vmid_range+0x74/0xe0
 *
 * On a pKVM host (kvm-arm.mode=protected), enabling dirty logging on an
 * existing memslot makes the host issue the __kvm_tlb_flush_vmid hypercall,
 * which the hypervisor refuses after pKVM finalisation:
 *
 *   KVM_SET_USER_MEMORY_REGION (flags |= KVM_MEM_LOG_DIRTY_PAGES)
 *     kvm_vm_ioctl -> __kvm_set_memory_region -> kvm_set_memslot
 *       -> kvm_arch_commit_memory_region          mmu.c:2493
 *          -> kvm_mmu_wp_memory_region  [inlined] mmu.c (wp + flush)
 *             -> kvm_flush_remote_tlbs_memslot
 *                -> kvm_arch_flush_remote_tlbs_range
 *                   -> kvm_tlb_flush_vmid_range   pgtable.c:653
 *                      -> kvm_call_hyp(__kvm_tlb_flush_vmid)   pgtable.c:654
 *                         -> WARN_ON(res.a0 != SMCCC_RET_SUCCESS)  kvm_host.h:1257
 *
 * Two conditions, both satisfied below:
 *   1. the memslot must ALREADY exist and then gain KVM_MEM_LOG_DIRTY_PAGES
 *      (a KVM_MR_FLAGS_ONLY change). Creating a slot with the flag already set
 *      is a different path.
 *   2. KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2 must NOT be enabled, otherwise
 *      kvm_arch_commit_memory_region() returns early and defers to CLEAR ioctls.
 *      We simply never enable it.
 *
 * No vCPU and no guest code are needed -- the flush is issued unconditionally
 * after the write-protect pass.
 *
 * Non-destructive: this only emits a kernel WARNING. The machine stays up.
 *
 *   aarch64-linux-gnu-gcc -O2 -static -o repro-tlbflush-warn repro-tlbflush-warn.c
 *
 * Expected on an affected kernel: one new WARNING per run, whose header names
 * THIS process, e.g.
 *   WARNING: CPU: 0 PID: <pid> at arch/arm64/kvm/hyp/pgtable.c:654 ...
 *   CPU: 0 PID: <pid> Comm: repro-tlbflush ...
 * Checking Comm is what distinguishes it from the fuzzer's own warnings when a
 * campaign is running on the same board.
 */
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_PHYS 0x40000000UL
#define MEM_SIZE   0x200000UL /* 2 MiB */

int main(void)
{
	struct kvm_userspace_memory_region region;
	int kvm, vm;
	void *mem;

	kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) {
		perror("open /dev/kvm");
		return 1;
	}

	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0) {
		perror("KVM_CREATE_VM");
		return 1;
	}

	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.flags = 0;
	region.guest_phys_addr = GUEST_PHYS;
	region.memory_size = MEM_SIZE;
	region.userspace_addr = (uint64_t)mem;

	/* 1. create the slot without dirty logging */
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION (create)");
		return 1;
	}

	/* 2. flags-only change that turns dirty logging ON -- this is the one
	 *    that reaches the write-protect + TLB-flush path.
	 */
	region.flags = KVM_MEM_LOG_DIRTY_PAGES;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION (enable dirty log)");
		return 1;
	}

	printf("pid=%d done -- check dmesg for pgtable.c:654 with this pid\n",
	       (int)getpid());
	return 0;
}
