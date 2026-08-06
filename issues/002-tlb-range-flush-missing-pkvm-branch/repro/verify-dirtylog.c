// SPDX-License-Identifier: GPL-2.0
/*
 * Verifier for issue 002 -- does the range TLB flush actually invalidate?
 *
 * This is NOT the reproducer. repro-tlbflush-warn.c exposes the defect by
 * making the host issue a hypercall EL2 refuses; it deliberately creates no
 * vCPU, which is what makes it cheap. That same minimalism makes it useless as
 * a fix test: with no vCPU there is no hyp VM, so kvm->arch.pkvm.handle is 0,
 * EL2 maps handle 0 to an out-of-range index and returns without flushing and
 * without touching a0 -- which the dispatcher has already set to
 * SMCCC_RET_SUCCESS. After the fix that program stops warning because the call
 * became a no-op, not because the invalidation started happening.
 *
 * So this program runs a vCPU first, which is what creates the hyp VM and maps
 * guest pages through EL2, and then checks the property the flush exists to
 * guarantee: that a guest write issued AFTER dirty logging was enabled is
 * recorded in the dirty bitmap.
 *
 * Sequence:
 *   1. ordinary VM (type 0) + 2 MiB memslot, no dirty logging
 *   2. vCPU created, INIT'd, and RUN -- hyp VM now exists, pages are mapped
 *   3. guest writes the data page once, exits via MMIO
 *   4. KVM_MEM_LOG_DIRTY_PAGES added to the SAME slot (KVM_MR_FLAGS_ONLY):
 *      write-protect, then kvm_arch_flush_remote_tlbs_range() -- the path
 *      under test
 *   5. KVM_GET_DIRTY_LOG once to clear the bitmap and re-protect
 *   6. guest resumes and writes the SAME page again
 *   7. KVM_GET_DIRTY_LOG -- that page MUST be marked dirty
 *
 * If the invalidation did not happen, the guest can still hold a writable
 * stage-2 TLB entry for that page, its write takes no fault, and the bit stays
 * clear.
 *
 * READ THIS BEFORE TRUSTING A PASS ON AN UNPATCHED KERNEL. A stale TLB entry is
 * not guaranteed to survive: the CPU may evict it, re-walk, and take the fault
 * anyway, which sets the bit without the flush ever having run. So:
 *
 *   FAIL  -> conclusive, the flush is not working
 *   PASS  -> the path works end to end on this kernel; on an unpatched kernel
 *            it means only "the stale entry did not survive this time"
 *
 * Run it on both builds. The result that matters is unpatched FAIL -> patched
 * PASS. A patched PASS alone still confirms no regression, which is the
 * minimum bar for the fix.
 *
 * Harmless: no hang, bounded by alarm(). Unlike issue 001's reproducer this is
 * safe on a machine you cannot power-cycle.
 *
 *   aarch64-linux-gnu-gcc -O2 -static -o verify-dirtylog verify-dirtylog.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_PHYS	0x40000000UL
#define MEM_SIZE	0x200000UL		/* 2 MiB */
#define DATA_OFF	0x10000UL		/* page 16 of the slot */
#define DATA_GPA	(GUEST_PHYS + DATA_OFF)
#define MMIO_GPA	0x50000000UL		/* outside the slot -> KVM_EXIT_MMIO */
#define NPAGES		(MEM_SIZE / 0x1000)
#define DATA_PAGE	(DATA_OFF / 0x1000)

/* Just enough A64 to write a page and exit twice. */
#define MOVZ(rd, imm16, sh)	(0xD2800000u | ((uint32_t)(sh) << 21) | \
				 (((uint32_t)(imm16) & 0xffff) << 5) | (rd))
#define STR_X(rt, rn)		(0xF9000000u | ((uint32_t)(rn) << 5) | (rt))
#define B_SELF			(0x14000000u)

static void on_alarm(int sig) { (void)sig; _exit(3); }

static int bit_set(const unsigned long *bm, unsigned long i)
{
	return (bm[i / (8 * sizeof(*bm))] >> (i % (8 * sizeof(*bm)))) & 1;
}

int main(void)
{
	struct kvm_userspace_memory_region region;
	struct kvm_dirty_log log;
	struct kvm_vcpu_init init;
	struct kvm_one_reg reg;
	unsigned long bitmap[NPAGES / (8 * sizeof(unsigned long))];
	uint64_t pc = GUEST_PHYS;
	int kvm, vm, vcpu, msz, ret, rc = 1;
	volatile struct kvm_run *run;
	uint32_t *code;
	void *mem;

	signal(SIGALRM, on_alarm);
	alarm(20);

	kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { perror("open /dev/kvm"); return 1; }

	vm = ioctl(kvm, KVM_CREATE_VM, 0);		/* ordinary VM, not protected */
	if (vm < 0) { perror("KVM_CREATE_VM"); return 1; }

	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	code = mem;
	*code++ = MOVZ(0, 0x4001, 1);		/* x0 = DATA_GPA   */
	*code++ = MOVZ(2, 0x5000, 1);		/* x2 = MMIO_GPA   */
	*code++ = MOVZ(1, 1, 0);		/* x1 = 1          */
	*code++ = STR_X(1, 0);			/* write #1        */
	*code++ = STR_X(1, 2);			/* MMIO exit A     */
	*code++ = MOVZ(1, 2, 0);		/* x1 = 2          */
	*code++ = STR_X(1, 0);			/* write #2 <- must be logged */
	*code++ = STR_X(1, 2);			/* MMIO exit B     */
	*code++ = B_SELF;

	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GUEST_PHYS;
	region.memory_size = MEM_SIZE;
	region.userspace_addr = (uint64_t)(uintptr_t)mem;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION (initial)"); return 1;
	}

	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) { perror("KVM_CREATE_VCPU"); return 1; }

	memset(&init, 0, sizeof(init));
	if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
		perror("KVM_ARM_PREFERRED_TARGET"); return 1;
	}
	memset(init.features, 0, sizeof(init.features));   /* no PMU_V3 */
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
		perror("KVM_ARM_VCPU_INIT"); return 1;
	}

	reg.id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
		 (offsetof(struct kvm_regs, regs.pc) / sizeof(uint32_t));
	reg.addr = (uint64_t)(uintptr_t)&pc;
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0) {
		perror("KVM_SET_ONE_REG(pc)"); return 1;
	}

	msz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (msz < (int)sizeof(*run)) { perror("KVM_GET_VCPU_MMAP_SIZE"); return 1; }
	run = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) { perror("mmap kvm_run"); return 1; }

	/*
	 * Run to the first MMIO exit. This is what creates the hyp VM
	 * (__pkvm_create_hyp_vm -> pkvm.handle != 0) and faults the pages in --
	 * everything the reproducer skips.
	 */
	ret = ioctl(vcpu, KVM_RUN, 0);
	if (ret < 0) { perror("KVM_RUN (first)"); return 1; }
	if (run->exit_reason != KVM_EXIT_MMIO) {
		fprintf(stderr, "FAIL: first run exited %u, expected KVM_EXIT_MMIO(%u)\n",
			run->exit_reason, KVM_EXIT_MMIO);
		return 1;
	}
	printf("guest ran, wrote %s once, exited via MMIO -- hyp VM now exists\n",
	       "the data page");

	/* THE PATH UNDER TEST: flags-only change adding dirty logging. */
	region.flags = KVM_MEM_LOG_DIRTY_PAGES;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION (enable dirty logging)"); return 1;
	}
	printf("dirty logging enabled (write-protect + range TLB flush)\n");

	/* Drain and re-protect, so what follows is only the post-protect write. */
	memset(bitmap, 0, sizeof(bitmap));
	log.slot = 0;
	log.dirty_bitmap = bitmap;
	if (ioctl(vm, KVM_GET_DIRTY_LOG, &log) < 0) {
		perror("KVM_GET_DIRTY_LOG (drain)"); return 1;
	}

	/* Resume: the guest writes the same page again, now write-protected. */
	ret = ioctl(vcpu, KVM_RUN, 0);
	if (ret < 0) { perror("KVM_RUN (second)"); return 1; }
	if (run->exit_reason != KVM_EXIT_MMIO) {
		fprintf(stderr, "FAIL: second run exited %u, expected KVM_EXIT_MMIO(%u)\n",
			run->exit_reason, KVM_EXIT_MMIO);
		return 1;
	}

	memset(bitmap, 0, sizeof(bitmap));
	log.slot = 0;
	log.dirty_bitmap = bitmap;
	if (ioctl(vm, KVM_GET_DIRTY_LOG, &log) < 0) {
		perror("KVM_GET_DIRTY_LOG (check)"); return 1;
	}

	if (bit_set(bitmap, DATA_PAGE)) {
		printf("PASS: guest page %lu (gpa 0x%lx) written after write-protect "
		       "and recorded in the dirty bitmap\n",
		       (unsigned long)DATA_PAGE, (unsigned long)DATA_GPA);
		rc = 0;
	} else {
		unsigned long i, n = 0;

		for (i = 0; i < NPAGES; i++)
			n += bit_set(bitmap, i);
		printf("FAIL: guest wrote gpa 0x%lx after write-protect, but page %lu "
		       "is NOT in the dirty bitmap (%lu other pages set)\n"
		       "      the write went through a stale writable stage-2 entry: "
		       "the TLB invalidation did not happen\n",
		       (unsigned long)DATA_GPA, (unsigned long)DATA_PAGE, n);
		rc = 1;
	}

	/* Value check: the guest really did perform the second write. */
	if (*(volatile uint64_t *)((char *)mem + DATA_OFF) != 2) {
		printf("NOTE: data page holds %llu, expected 2 -- the guest may not "
		       "have executed the second write, so the result above is not "
		       "meaningful\n",
		       (unsigned long long)*(volatile uint64_t *)((char *)mem + DATA_OFF));
		rc = 2;
	}

	alarm(0);
	return rc;
}
