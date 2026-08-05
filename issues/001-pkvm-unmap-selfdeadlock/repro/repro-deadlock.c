// SPDX-License-Identifier: GPL-2.0
/*
 * Reproducer: self-deadlock in pkvm_unmap_range() (arch/arm64/kvm/mmu.c).
 *
 * On a pKVM host (kvm-arm.mode=protected) whose CPU lacks ARM64_HAS_STAGE2_FWB,
 * a SECOND KVM_ARM_VCPU_INIT on a vCPU that has already run takes:
 *
 *   kvm_arch_vcpu_ioctl_vcpu_init()      arm.c:1440
 *     -> stage2_unmap_vm()               mmu.c:1157  mmap_read_lock(current->mm)
 *        -> ... -> pkvm_unmap_range()    mmu.c:342
 *           -> account_locked_vm()       mm/util.c:540  mmap_write_lock(mm)   << DEADLOCK
 *
 * A task holding an rwsem for read cannot acquire it for write, so the calling
 * thread blocks forever in D state holding mmap_lock and a kvm->srcu read-side.
 * In practice the whole machine becomes unresponsive and needs a power cycle.
 *
 * Two conditions must hold, both satisfied below:
 *   1. cnt > 0  -- at least one PINNED page must be unmapped. account_locked_vm()
 *      returns immediately when pages == 0, so the guest must actually fault
 *      memory in. We run the vCPU briefly to force a stage-2 fault.
 *   2. !ARM64_HAS_STAGE2_FWB -- otherwise arm.c takes icache_inval_all_pou()
 *      instead and never calls stage2_unmap_vm() here.
 *
 * WARNING: on an affected kernel this HANGS THE MACHINE. Run only on a host you
 * can power-cycle. Expected output on a FIXED kernel: "PASS".
 *
 *   aarch64-linux-gnu-gcc -O2 -static -o repro-deadlock repro-deadlock.c
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
#define MEM_SIZE	0x200000UL	/* 2 MiB */

static void on_alarm(int sig) { (void)sig; }	/* breaks KVM_RUN out with -EINTR */

int main(void)
{
	struct kvm_userspace_memory_region region;
	struct kvm_vcpu_init init;
	struct kvm_one_reg reg;
	uint64_t pc = GUEST_PHYS;
	int kvm, vm, vcpu, ret;
	uint32_t *code;
	void *mem;

	kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { perror("open /dev/kvm"); return 1; }

	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0) { perror("KVM_CREATE_VM"); return 1; }

	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	/* Guest code: "b ." -- spin. The point is only to fault the page in. */
	code = mem;
	code[0] = 0x14000000;

	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GUEST_PHYS;
	region.memory_size = MEM_SIZE;
	region.userspace_addr = (uint64_t)(uintptr_t)mem;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION"); return 1;
	}

	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) { perror("KVM_CREATE_VCPU"); return 1; }

	memset(&init, 0, sizeof(init));
	if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0) {
		perror("KVM_ARM_PREFERRED_TARGET"); return 1;
	}
	/* No feature bits: avoids PMU_V3 and the unrelated arch_timer WARN. */
	memset(init.features, 0, sizeof(init.features));

	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
		perror("KVM_ARM_VCPU_INIT (first)"); return 1;
	}

	/* Point PC at guest RAM so the first instruction fetch faults it in. */
	reg.id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
		 (offsetof(struct kvm_regs, regs.pc) / sizeof(uint32_t));
	reg.addr = (uint64_t)(uintptr_t)&pc;
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0)
		perror("KVM_SET_ONE_REG(pc) (continuing anyway)");

	/*
	 * Run briefly. The stage-2 fault on the instruction fetch goes through
	 * pkvm_mem_abort(), which PINS the page and accounts it -- giving the
	 * cnt > 0 the deadlock requires. SIGALRM breaks us back out.
	 */
	signal(SIGALRM, on_alarm);
	alarm(2);
	ret = ioctl(vcpu, KVM_RUN, 0);
	alarm(0);
	printf("first KVM_RUN returned %d (errno %d) -- pages should now be pinned\n",
	       ret, ret < 0 ? errno : 0);

	/*
	 * THE TRIGGER. vcpu_has_run_once() is now true, so this reaches
	 * stage2_unmap_vm() -> pkvm_unmap_range() -> account_locked_vm().
	 * On an affected kernel this never returns.
	 */
	printf("second KVM_ARM_VCPU_INIT -- hangs here on an affected kernel...\n");
	fflush(stdout);

	ret = ioctl(vcpu, KVM_ARM_VCPU_INIT, &init);

	printf("PASS: second KVM_ARM_VCPU_INIT returned %d (errno %d) -- no deadlock\n",
	       ret, ret < 0 ? errno : 0);
	return 0;
}
