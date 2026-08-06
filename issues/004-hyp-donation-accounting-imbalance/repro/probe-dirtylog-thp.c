// Diagnostic: is the -E2BIG tied to a 2 MiB block mapping?
//   ./probe2            -> default (THP allowed)
//   ./probe2 nohuge     -> MADV_NOHUGEPAGE before the guest faults anything in
// Also samples kvm->stat.protected_hyp_mem from debugfs at each step.
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

#define GUEST_PHYS 0x40000000UL
#define MEM_SIZE   0x200000UL
#define DATA_OFF   0x10000UL

#define MOVZ(rd, imm16, sh) (0xD2800000u | ((uint32_t)(sh) << 21) | (((uint32_t)(imm16) & 0xffff) << 5) | (rd))
#define STR_X(rt, rn)       (0xF9000000u | ((uint32_t)(rn) << 5) | (rt))
#define B_SELF              (0x14000000u)

static int g_vm;

static void show(const char *step)
{
	char p[160];
	long v = 0;
	FILE *f;

	snprintf(p, sizeof(p), "/sys/kernel/debug/kvm/%d-%d/protected_hyp_mem",
		 getpid(), g_vm);
	f = fopen(p, "r");
	if (!f) { printf("%-30s hyp_mem: <%s>\n", step, strerror(errno)); return; }
	if (fscanf(f, "%ld", &v) != 1) v = -999999;
	fclose(f);
	printf("%-30s hyp_mem = %8ld\n", step, v);
}

static void on_alarm(int s) { (void)s; _exit(3); }

int main(int argc, char **argv)
{
	int nohuge = (argc > 1 && !strcmp(argv[1], "nohuge"));
	int nodirty = (argc > 1 && !strcmp(argv[1], "nodirty"));
	struct kvm_userspace_memory_region region;
	struct kvm_vcpu_init init;
	struct kvm_one_reg reg;
	uint64_t pc = GUEST_PHYS;
	volatile struct kvm_run *run;
	int kvm, vcpu, msz, ret;
	uint32_t *code;
	void *mem;

	signal(SIGALRM, on_alarm);
	alarm(20);

	kvm = open("/dev/kvm", O_RDWR);
	g_vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (g_vm < 0) { perror("KVM_CREATE_VM"); return 1; }

	mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }

	if (nohuge) {
		if (madvise(mem, MEM_SIZE, MADV_NOHUGEPAGE) < 0)
			perror("MADV_NOHUGEPAGE");
		else
			printf("[MADV_NOHUGEPAGE applied]\n");
	}

	code = mem;
	*code++ = MOVZ(0, 0x4001, 1);
	*code++ = MOVZ(2, 0x5000, 1);
	*code++ = MOVZ(1, 1, 0);
	*code++ = STR_X(1, 0);
	*code++ = STR_X(1, 2);
	*code++ = MOVZ(1, 2, 0);
	*code++ = STR_X(1, 0);
	*code++ = STR_X(1, 2);
	*code++ = B_SELF;

	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = GUEST_PHYS;
	region.memory_size = MEM_SIZE;
	region.userspace_addr = (uint64_t)(uintptr_t)mem;
	if (ioctl(g_vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) { perror("memslot"); return 1; }

	vcpu = ioctl(g_vm, KVM_CREATE_VCPU, 0);
	memset(&init, 0, sizeof(init));
	ioctl(g_vm, KVM_ARM_PREFERRED_TARGET, &init);
	memset(init.features, 0, sizeof(init.features));
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0) { perror("VCPU_INIT"); return 1; }

	reg.id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
		 (offsetof(struct kvm_regs, regs.pc) / sizeof(uint32_t));
	reg.addr = (uint64_t)(uintptr_t)&pc;
	ioctl(vcpu, KVM_SET_ONE_REG, &reg);

	msz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);

	ret = ioctl(vcpu, KVM_RUN, 0);
	printf("first  KVM_RUN ret=%d errno=%d exit=%u\n",
	       ret, ret < 0 ? errno : 0, run->exit_reason);
	show("after first KVM_RUN");

	if (!nodirty) {
		region.flags = KVM_MEM_LOG_DIRTY_PAGES;
		if (ioctl(g_vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) { perror("enable dirty log"); return 1; }
		show("after dirty logging on");
	} else {
		printf("[dirty logging NOT enabled]\n");
	}

	ret = ioctl(vcpu, KVM_RUN, 0);
	printf("second KVM_RUN ret=%d errno=%d (%s) exit=%u\n",
	       ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "-", run->exit_reason);
	show("after second KVM_RUN");

	alarm(0);
	return ret < 0 ? 1 : 0;
}
