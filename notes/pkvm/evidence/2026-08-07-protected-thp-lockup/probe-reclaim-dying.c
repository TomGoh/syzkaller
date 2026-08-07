// Differential probe: does a PROTECTED VM's teardown reach
// __pkvm_reclaim_dying_guest_page, where an ordinary VM's does not?
//
// Hypothesis under test (code-read, arch/arm64/kvm/mmu.c:__unmap_stage2_range):
//   kvm_destroy_vm() calls kvm_flush_shadow_all() BEFORE kvm_arch_destroy_vm().
//   For a protected VM (kvm->arch.pkvm.enabled) __unmap_stage2_range returns
//   immediately, so pinned_pages survives into __pkvm_destroy_hyp_vm() and its
//   reclaim loop fires the hypercall once per pinned page.
//   For an ordinary VM the same path runs pkvm_unmap_range(), which drains every
//   pinned page via __pkvm_host_unmap_guest and empties the tree -- so the
//   reclaim loop body executes zero times.
//
// Both arms do the SAME thing except the KVM_CREATE_VM type argument, so any
// difference in the EL2 PC set is attributable to that one bit.
//
// argv[1] = 0 -> ordinary VM (control)   argv[1] = 1 -> protected VM
// argv[2] = output file for the EL2 PC dump
//
// The ring must already be armed (pkvm-cov-arm.sh). No CPU pinning needed on a
// per-CPU-ring kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KCOV_INIT_TRACE _IOR('c', 1, unsigned long)
#define KCOV_ENABLE _IO('c', 100)
#define KCOV_DISABLE _IO('c', 101)
#define KCOV_TRACE_PC 0
#define COVER_SIZE (256 << 10)

#define GPA 0x40000000UL
#define SIZE 0x200000UL // 2 MiB
#define MMIO_GPA 0x50000000UL // deliberately outside every memslot

#define MOVZ_IMM(rd, imm16, sh) (0xD2800000u | ((unsigned)(sh) << 21) | (((unsigned)(imm16) & 0xffff) << 5) | (rd))
#define STR_X_REG(rt, rn) (0xF9000000u | ((unsigned)(rn) << 5) | (rt))

static void on_alarm(int sig)
{
	(void)sig;
}

int main(int argc, char **argv)
{
	int protect = argc > 1 ? atoi(argv[1]) : 0;
	const char *out = argc > 2 ? argv[2] : "/tmp/reclaim-pcs.txt";
	unsigned long vmtype = protect ? 0x80000000UL : 0UL;

	// Non-SA_RESTART alarm so a wedged KVM_RUN returns EINTR instead of hanging.
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);

	int kcov = open("/sys/kernel/debug/kcov", O_RDWR);
	if (kcov < 0) {
		perror("open kcov");
		return 1;
	}
	if (ioctl(kcov, KCOV_INIT_TRACE, COVER_SIZE)) {
		perror("KCOV_INIT_TRACE");
		return 1;
	}
	unsigned long *cover = mmap(NULL, COVER_SIZE * sizeof(unsigned long),
				    PROT_READ | PROT_WRITE, MAP_SHARED, kcov, 0);
	if (cover == MAP_FAILED) {
		perror("mmap kcov");
		return 1;
	}

	int kvmfd = open("/dev/kvm", O_RDWR);
	if (kvmfd < 0) {
		perror("open /dev/kvm");
		return 1;
	}

	void *mem = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
			 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap guest");
		return 1;
	}
	madvise(mem, SIZE, MADV_NOHUGEPAGE); // keep it 4 KiB so pins are per-page

	// Guest: store to an unbacked GPA -> KVM_EXIT_MMIO. Reaching the first
	// instruction already costs one stage-2 fault, which is the pinned page
	// this probe is actually about.
	unsigned *code = (unsigned *)mem;
	int n = 0;
	code[n++] = MOVZ_IMM(1, MMIO_GPA >> 16, 1); // movz x1, #0x5000, lsl #16
	code[n++] = MOVZ_IMM(0, 0x1234, 0); // movz x0, #0x1234
	code[n++] = STR_X_REG(0, 1); // str  x0, [x1]
	code[n++] = STR_X_REG(0, 1); // and again, so a re-entry still exits

	int msz = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);

	if (ioctl(kcov, KCOV_ENABLE, KCOV_TRACE_PC)) {
		perror("KCOV_ENABLE");
		return 1;
	}
	__atomic_store_n(&cover[0], 0, __ATOMIC_RELAXED);

	int vm = ioctl(kvmfd, KVM_CREATE_VM, vmtype);
	if (vm < 0) {
		fprintf(stderr, "KVM_CREATE_VM(0x%lx): %s\n", vmtype, strerror(errno));
		return 1;
	}

	struct kvm_userspace_memory_region region;
	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GPA;
	region.memory_size = SIZE;
	region.userspace_addr = (unsigned long)mem;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region)) {
		perror("KVM_SET_USER_MEMORY_REGION");
		return 1;
	}

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) {
		perror("KVM_CREATE_VCPU");
		return 1;
	}

	struct kvm_vcpu_init init;
	memset(&init, 0, sizeof(init));
	init.target = 5; // KVM_ARM_TARGET_GENERIC_V8, no features -> no PMU/timer WARN
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init)) {
		perror("KVM_ARM_VCPU_INIT");
		return 1;
	}

	// PC must be set BEFORE the first KVM_RUN: pkvm_create_hyp_vm() runs there
	// (arm.c:879) and EL2's pkvm_vcpu_init_psci() copies the host vCPU's PC into
	// reset_state when the VM has no pvmfw (nvhe/pkvm.c:438-443).
	unsigned long pc = GPA;
	struct kvm_one_reg reg;
	memset(&reg, 0, sizeof(reg));
	reg.id = 0x6030000000100040ULL; // ARM64_CORE_REG(regs.pc)
	reg.addr = (unsigned long)&pc;
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg)) {
		perror("KVM_SET_ONE_REG(pc)");
		return 1;
	}

	volatile struct kvm_run *run = mmap(NULL, msz, PROT_READ | PROT_WRITE,
					    MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) {
		perror("mmap kvm_run");
		return 1;
	}

	alarm(3);
	int rr = ioctl(vcpu, KVM_RUN, 0);
	int rerr = errno;
	alarm(0);
	fprintf(stderr, "arm=%s KVM_RUN=%d errno=%s exit_reason=%u\n",
		protect ? "protected" : "ordinary", rr,
		rr ? strerror(rerr) : "-", run->exit_reason);

	// Teardown. The last fput of the vm fd runs kvm_destroy_vm() in THIS task's
	// context (task_work on return from close()), so it lands in this KCOV area.
	munmap((void *)run, msz);
	close(vcpu);
	close(vm);

	ioctl(kcov, KCOV_DISABLE, 0);

	unsigned long n_pc = __atomic_load_n(&cover[0], __ATOMIC_RELAXED);
	FILE *f = fopen(out, "w");
	if (!f) {
		perror("fopen out");
		return 1;
	}
	for (unsigned long i = 0; i < n_pc; i++)
		fprintf(f, "0x%lx\n", cover[i + 1]);
	fclose(f);
	fprintf(stderr, "arm=%s collected %lu PCs -> %s\n",
		protect ? "protected" : "ordinary", n_pc, out);
	return 0;
}
