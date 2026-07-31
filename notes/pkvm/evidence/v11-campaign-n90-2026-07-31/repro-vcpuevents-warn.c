// SPDX-License-Identifier: GPL-2.0
/*
 * Reproducer: WARNING at arch/arm64/kvm/inject_fault.c:22
 *             pend_sync_exception+0x110/0x188
 *
 * KVM_SET_VCPU_EVENTS with exception.ext_dabt_pending=1 injects a data abort:
 *
 *   kvm_vcpu_ioctl -> kvm_arch_vcpu_ioctl
 *     -> __kvm_arm_vcpu_set_events      guest.c   (ext_dabt_pending)
 *        -> kvm_inject_dabt -> inject_abt64
 *           -> pend_sync_exception      inject_fault.c:18
 *              -> kvm_pend_exception()  inject_fault.c:22
 *                 -> WARN_ON(vcpu_get_flag(v, INCREMENT_PC))  kvm_emulate.h:720
 *
 * The WARN fires when the vCPU still has a PENDING PC INCREMENT at the moment
 * userspace asks for an exception. Injecting an exception and advancing the PC
 * are mutually exclusive -- the exception must be taken at the faulting
 * instruction, not after it.
 *
 * Getting INCREMENT_PC to survive a return to userspace is the whole trick, and
 * arm64 KVM does it deliberately for HVC/SMC. handle_exit.c:76:
 *
 *     We need to advance the PC after the trap, as it would otherwise return
 *     to the same address. Furthermore, PRE-INCREMENTING THE PC BEFORE
 *     POTENTIALLY EXITING TO USERSPACE maintains the same abstraction for both
 *     SMCs and HVCs.
 *
 * So: make the guest issue PSCI SYSTEM_OFF via HVC. KVM sets INCREMENT_PC, then
 * PSCI turns it into KVM_EXIT_SYSTEM_EVENT and returns to userspace with the
 * flag still set. Calling KVM_SET_VCPU_EVENTS right there hits the WARN. No
 * race and no signal timing needed.
 *
 * Non-destructive: emits a kernel WARNING, the machine stays up.
 *
 *   aarch64-linux-gnu-gcc -O2 -static -o repro-vcpuevents repro-vcpuevents-warn.c
 *
 * Expected on an affected kernel, naming THIS process:
 *   WARNING: CPU: n PID: <pid> at arch/arm64/kvm/inject_fault.c:22 ...
 *   CPU: n PID: <pid> Comm: repro-vcpuev ...
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * KVM_RUN must never be entered without an escape hatch. If the guest does not
 * reach the HVC -- wrong encoding, page not executable, vCPU not started --
 * KVM_RUN simply never returns and the process hangs in the kernel. SIGALRM
 * with no SA_RESTART makes the ioctl fail with -EINTR instead.
 */
static void on_alarm(int sig) { (void)sig; }

#define GUEST_PHYS 0x40000000UL
#define MEM_SIZE   0x200000UL /* 2 MiB */

int main(void)
{
	struct kvm_userspace_memory_region region;
	struct kvm_vcpu_events events;
	struct kvm_vcpu_init init;
	struct kvm_one_reg reg;
	uint64_t pc = GUEST_PHYS;
	int kvm, vm, vcpu, ret, mmap_size;
	struct kvm_run *run;
	uint32_t *code;
	void *mem;

	kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { perror("open /dev/kvm"); return 1; }

	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0) { perror("KVM_CREATE_VM"); return 1; }

	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	/*
	 * Guest code:
	 *   movz x0, #0x0008              -- PSCI_0_2_FN_SYSTEM_OFF = 0x84000008
	 *   movk x0, #0x8400, lsl #16
	 *   hvc  #0
	 */
	code = mem;
	code[0] = 0xD2800100;
	code[1] = 0xF2B08000;
	code[2] = 0xD4000002;

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
	memset(init.features, 0, sizeof(init.features));
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) {
		perror("KVM_ARM_VCPU_INIT"); return 1;
	}

	mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) { perror("mmap kvm_run"); return 1; }

	reg.id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
		 (offsetof(struct kvm_regs, regs.pc) / sizeof(uint32_t));
	reg.addr = (uint64_t)(uintptr_t)&pc;
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0) {
		perror("KVM_SET_ONE_REG(pc)"); return 1;
	}

	/*
	 * The guest HVCs immediately. KVM does kvm_incr_pc() at handle_exit.c:76
	 * BEFORE dispatching to PSCI, and PSCI SYSTEM_OFF exits to userspace --
	 * so we come back here with INCREMENT_PC still pending.
	 */
	printf("entering KVM_RUN...\n");
	fflush(stdout);
	signal(SIGALRM, on_alarm);
	alarm(3);
	ret = ioctl(vcpu, KVM_RUN, 0);
	alarm(0);
	printf("KVM_RUN ret=%d errno=%d exit_reason=%u (expect %u = SYSTEM_EVENT)\n",
	       ret, ret < 0 ? errno : 0, run->exit_reason, KVM_EXIT_SYSTEM_EVENT);
	if (ret < 0 && errno == EINTR)
		printf("  NOTE: interrupted by SIGALRM -- the guest never reached the HVC,\n"
		       "        so INCREMENT_PC is NOT pending and the WARN will not fire.\n");

	/* THE TRIGGER: ask for a data abort while INCREMENT_PC is still set. */
	memset(&events, 0, sizeof(events));
	events.exception.ext_dabt_pending = 1;
	ret = ioctl(vcpu, KVM_SET_VCPU_EVENTS, &events);
	printf("KVM_SET_VCPU_EVENTS ret=%d errno=%d\n", ret, ret < 0 ? errno : 0);

	printf("pid=%d done -- check dmesg for inject_fault.c:22 with this pid\n",
	       (int)getpid());
	return 0;
}
