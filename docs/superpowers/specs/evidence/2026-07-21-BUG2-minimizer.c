// BUG2 minimization: isolate what triggers WARNING at pgtable.c:639 kvm_tlb_flush_vmid_range.
// One case per invocation (argv[1] = A|B|C), each a single memslot operation.
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define GPA 0x40000000UL
#define GPA2 0x40001000UL

static long PG;
static unsigned long BACK;

static int mreg(int vm, unsigned slot, unsigned flags, unsigned long gpa, unsigned long size, unsigned long backing)
{
	struct kvm_userspace_memory_region m;
	memset(&m, 0, sizeof(m));
	m.slot = slot;
	m.flags = flags;
	m.guest_phys_addr = gpa;
	m.memory_size = size;
	m.userspace_addr = backing;
	return ioctl(vm, KVM_SET_USER_MEMORY_REGION, &m);
}

int main(int argc, char** argv)
{
	PG = sysconf(_SC_PAGESIZE);
	BACK = (unsigned long)mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	int kvm = open("/dev/kvm", O_RDWR);
	char c = argc > 1 ? argv[1][0] : 'A';

	if (c == 'A') { // real protected VM: register normal slot, MOVE before any run
		int vm = ioctl(kvm, KVM_CREATE_VM, 0x80000000);
		int r1 = mreg(vm, 0, 0, GPA, PG, BACK);
		errno = 0;
		int r2 = mreg(vm, 0, 0, GPA2, PG, BACK);
		printf("A pVM(bit31): register=%d  MOVE-pre-run=%d errno=%d(%s)\n", r1, r2, errno, r2 ? strerror(errno) : "ok");
		close(vm);
	} else if (c == 'B') { // real protected VM: register, controlled run, then MOVE
		int vm = ioctl(kvm, KVM_CREATE_VM, 0x80000000);
		mreg(vm, 0, 0, GPA, PG, BACK);
		int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
		struct kvm_vcpu_init init;
		memset(&init, 0, sizeof(init));
		init.target = 5;
		ioctl(vcpu, KVM_ARM_VCPU_INIT, &init);
		struct kvm_run* run = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
		if (run != MAP_FAILED) {
			run->immediate_exit = 1;
			ioctl(vcpu, KVM_RUN, 0);
		}
		errno = 0;
		int r = mreg(vm, 0, 0, GPA2, PG, BACK);
		printf("B pVM(bit31): run then MOVE=%d errno=%d(%s) [expect EPERM=%d]\n", r, errno, r ? strerror(errno) : "ok", EPERM);
		close(vcpu);
		close(vm);
	} else { // NORMAL VM (no bit31): register normal slot, then enable dirty-logging (FLAGS_ONLY)
		int vm = ioctl(kvm, KVM_CREATE_VM, 0);
		int r1 = mreg(vm, 0, 0, GPA, PG, BACK);
		errno = 0;
		int r2 = mreg(vm, 0, KVM_MEM_LOG_DIRTY_PAGES, GPA, PG, BACK);
		printf("C normal-VM: register=%d  enable-dirtylog=%d errno=%d(%s)\n", r1, r2, errno, r2 ? strerror(errno) : "ok");
		close(vm);
	}
	return 0;
}
