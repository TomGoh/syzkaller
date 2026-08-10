// Where exactly does a THP-backed PROTECTED VM hang -- in the donation
// (KVM_RUN -> pkvm_mem_abort -> __pkvm_host_map_guest with 512 pages) or in the
// teardown reclaim (close(vm) -> __pkvm_destroy_hyp_vm ->
// __pkvm_reclaim_dying_guest_page with order 9)?
//
// Markers go to /dev/kmsg, NOT stderr: when this hangs it takes ssh down with
// it, so anything on stdout/stderr is lost. /dev/kmsg lands in the console
// follower, interleaved with the kernel's own output, and survives.
//
// argv[1] = 1 leaves the mapping THP-eligible (the hang case); 0 asks for
// MADV_NOHUGEPAGE (the control that is known to pass).
//
// KCOV_TRACE_PC is enabled on this task and left on across close(vm), because
// pkvm_cov_begin() arms the EL2 ring ONLY when kcov_current_trace_pc() is true.
// Without it the ring stays flags=0x0 count=0 and the hypervisor's PCs are
// never recorded -- which is exactly how the first attempt at this probe came
// back empty.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define KCOV_INIT_TRACE _IOR('c', 1, unsigned long)
#define KCOV_ENABLE _IO('c', 100)
#define KCOV_TRACE_PC 0
#define COVER_SIZE (256 << 10)

#define GPA 0x40000000UL
#define SIZE 0x200000UL
#define MMIO_GPA 0x50000000UL

#define MOVZ_IMM(rd, imm16, sh) (0xD2800000u | ((unsigned)(sh) << 21) | (((unsigned)(imm16) & 0xffff) << 5) | (rd))
#define STR_X_REG(rt, rn) (0xF9000000u | ((unsigned)(rn) << 5) | (rt))
#define STR_X_OFF(rt, rn, off) (0xF9000000u | (((((unsigned)(off)) >> 3) & 0xfffu) << 10) | ((unsigned)(rn) << 5) | (rt))

static int kmsg = -1;

static void mark(const char *fmt, ...)
{
	char buf[256];
	int n;
	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (kmsg >= 0) {
		ssize_t w = write(kmsg, buf, n);
		(void)w;
	}
	fprintf(stderr, "%s", buf);
	fflush(stderr);
}

int main(int argc, char **argv)
{
	int thp = argc > 1 ? atoi(argv[1]) : 1;

	kmsg = open("/dev/kmsg", O_WRONLY);

	// KCOV is OPTIONAL and its absence is not a failure. Reproducing the hang
	// needs only the VM lifecycle below; KCOV matters solely for the companion
	// kvm/pkvm_cov/ring_dump capture, which arms the EL2 coverage ring for this
	// task (pkvm_cov_begin() bails unless kcov_current_trace_pc() is true) and
	// is how the hypervisor panic was originally located. A kernel built
	// without CONFIG_KCOV -- i.e. most kernels this reproducer will be handed
	// to -- must still be able to run it, so failure here only downgrades the
	// evidence, it does not stop the test.
	//
	// Once armed it is never disabled: the window that matters opens inside
	// close(vm), long after the last KVM ioctl.
	int kcov = open("/sys/kernel/debug/kcov", O_RDWR);
	if (kcov < 0 || ioctl(kcov, KCOV_INIT_TRACE, COVER_SIZE)) {
		mark("RPGPROBE: KCOV unavailable (%s) -- reproducing without it\n",
		     strerror(errno));
	} else {
		void *cover = mmap(NULL, COVER_SIZE * sizeof(unsigned long),
				   PROT_READ | PROT_WRITE, MAP_SHARED, kcov, 0);
		if (cover == MAP_FAILED || ioctl(kcov, KCOV_ENABLE, KCOV_TRACE_PC))
			mark("RPGPROBE: KCOV enable failed (%s) -- reproducing without it\n",
			     strerror(errno));
		else
			mark("RPGPROBE: KCOV armed; ring_dump will show the EL2 tail\n");
	}

	int kvmfd = open("/dev/kvm", O_RDWR);
	if (kvmfd < 0) {
		perror("open /dev/kvm");
		return 1;
	}
	void *mem = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
			 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (!thp)
		madvise(mem, SIZE, MADV_NOHUGEPAGE);

	unsigned *code = (unsigned *)mem;
	int n = 0;
	code[n++] = MOVZ_IMM(0, 0x4000, 1); // x0 = 0x40000000
	code[n++] = MOVZ_IMM(2, 0x5000, 1); // x2 = 0x50000000 (unbacked)
	code[n++] = MOVZ_IMM(1, 0x5a5a, 0);
	code[n++] = STR_X_OFF(1, 0, 0x1000);
	code[n++] = STR_X_OFF(1, 0, 0x2000);
	code[n++] = STR_X_OFF(1, 0, 0x3000);
	code[n++] = STR_X_REG(1, 2);
	code[n++] = STR_X_REG(1, 2);
	code[n++] = STR_X_REG(1, 2);

	mark("RPGPROBE: start thp=%d mem=%p aligned2M=%d\n",
	     thp, mem, ((unsigned long)mem & (SIZE - 1)) == 0);

	int vm = ioctl(kvmfd, KVM_CREATE_VM, 0x80000000UL);
	if (vm < 0) {
		mark("RPGPROBE: CREATE_VM failed %s\n", strerror(errno));
		return 1;
	}
	struct kvm_userspace_memory_region region;
	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GPA;
	region.memory_size = SIZE;
	region.userspace_addr = (unsigned long)mem;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region)) {
		mark("RPGPROBE: SET_MEM failed %s\n", strerror(errno));
		return 1;
	}
	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	struct kvm_vcpu_init init;
	memset(&init, 0, sizeof(init));
	init.target = 5;
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init)) {
		mark("RPGPROBE: VCPU_INIT failed %s\n", strerror(errno));
		return 1;
	}
	unsigned long pc = GPA;
	struct kvm_one_reg reg;
	memset(&reg, 0, sizeof(reg));
	reg.id = 0x6030000000100040ULL;
	reg.addr = (unsigned long)&pc;
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg)) {
		mark("RPGPROBE: SET_ONE_REG failed %s\n", strerror(errno));
		return 1;
	}
	int msz = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
	volatile struct kvm_run *run = mmap(NULL, msz, PROT_READ | PROT_WRITE,
					    MAP_SHARED, vcpu, 0);

	mark("RPGPROBE: A before KVM_RUN\n");
	int rr = ioctl(vcpu, KVM_RUN, 0);
	mark("RPGPROBE: B after KVM_RUN rr=%d errno=%s exit=%u\n",
	     rr, rr ? strerror(errno) : "-", run->exit_reason);

	munmap((void *)run, msz);
	mark("RPGPROBE: C before close(vcpu)\n");
	close(vcpu);
	mark("RPGPROBE: D before close(vm)  <- teardown reclaim starts here\n");
	close(vm);
	mark("RPGPROBE: E after close(vm)\n");
	munmap(mem, SIZE);
	mark("RPGPROBE: F done\n");
	return 0;
}
