// Firmware reachability smoke: prove the host->EL2 pvmfw donation contract (#23) is actually entered.
// Register a memslot over the firmware window, SET_FW_IPA before the first run, then a REAL (bounded)
// KVM_RUN so the guest fetches at the firmware IPA -> stage-2 fault -> pkvm_mem_abort -> donation.
// A wrapper arms a kprobe on pkvm_mem_abort; this probe just drives the sequence and self-bounds via
// alarm() (the run is expected to spin in pvmfw with no payload). Self-contained (arm64 static).
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define KVMIO 0xAE
#define KVM_CREATE_VM          _IO(KVMIO, 0x01)
#define KVM_CREATE_VCPU        _IO(KVMIO, 0x41)
#define KVM_RUN                _IO(KVMIO, 0x80)
#define KVM_GET_VCPU_MMAP_SIZE _IO(KVMIO, 0x04)
struct kvm_userspace_memory_region { uint32_t slot, flags; uint64_t guest_phys_addr, memory_size, userspace_addr; };
#define KVM_SET_USER_MEMORY_REGION _IOW(KVMIO, 0x46, struct kvm_userspace_memory_region)
struct kvm_enable_cap { uint32_t cap, flags; uint64_t args[4]; uint8_t pad[64]; };
#define KVM_ENABLE_CAP         _IOW(KVMIO, 0xa3, struct kvm_enable_cap)
struct kvm_vcpu_init { uint32_t target; uint32_t features[7]; };
#define KVM_ARM_VCPU_INIT      _IOW(KVMIO, 0xae, struct kvm_vcpu_init)

#define CAP_PROTECTED_VM 0xffbadab1
#define FLAG_SET_FW_IPA 0
#define FLAG_INFO 1
#define FW_IPA  0x7FC00000UL      // crosvm arm64 pvmfw window start
#define FW_SIZE 0x400000UL        // 4 MiB window

static void on_alarm(int s) { (void)s; }

int main(void) {
	int kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { printf("open /dev/kvm: %s\n", strerror(errno)); return 1; }
	int vm = ioctl(kvm, KVM_CREATE_VM, 0x80000000UL);
	if (vm < 0) { printf("CREATE_VM: %s\n", strerror(errno)); return 1; }

	void *mem = mmap(0, FW_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
	if (mem == MAP_FAILED) { printf("mmap: %s\n", strerror(errno)); return 1; }
	struct kvm_userspace_memory_region region = { .slot=0, .flags=0,
		.guest_phys_addr=FW_IPA, .memory_size=FW_SIZE, .userspace_addr=(uint64_t)(uintptr_t)mem };
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) != 0) {
		printf("SET_USER_MEMORY_REGION: %s\n", strerror(errno)); return 1; }
	printf("memslot @ 0x%lx size 0x%lx OK\n", FW_IPA, FW_SIZE);

	uint64_t info[8]; memset(info, 0, sizeof(info));
	struct kvm_enable_cap cap; memset(&cap, 0, sizeof(cap));
	cap.cap=CAP_PROTECTED_VM; cap.flags=FLAG_INFO; cap.args[0]=(uint64_t)(uintptr_t)info;
	ioctl(vm, KVM_ENABLE_CAP, &cap);
	printf("INFO firmware_size=%llu fits_window=%d\n", (unsigned long long)info[0], info[0] <= FW_SIZE);

	memset(&cap, 0, sizeof(cap));
	cap.cap=CAP_PROTECTED_VM; cap.flags=FLAG_SET_FW_IPA; cap.args[0]=FW_IPA;
	int r = ioctl(vm, KVM_ENABLE_CAP, &cap);
	printf("SET_FW_IPA(0x%lx) = %d (%s)\n", FW_IPA, r, r ? strerror(errno) : "ok");
	if (r != 0) { printf("SET_FW_IPA failed, aborting run\n"); return 1; }

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	struct kvm_vcpu_init init; memset(&init, 0, sizeof(init)); init.target = 5;
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) != 0) { printf("VCPU_INIT: %s\n", strerror(errno)); return 1; }
	int msz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	volatile unsigned char* run = mmap(0, msz, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) { printf("run mmap: %s\n", strerror(errno)); return 1; }
	// real run: immediate_exit stays 0

	struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_alarm; // no SA_RESTART -> EINTR
	sigaction(SIGALRM, &sa, 0);
	printf("KVM_RUN (real, bounded 3s) ...\n"); fflush(stdout);
	alarm(3);
	errno = 0;
	long rr = ioctl(vcpu, KVM_RUN, 0);
	int e = errno;
	alarm(0);
	printf("KVM_RUN ret=%ld errno=%d(%s) exit_reason=%u\n", rr, e, strerror(e), *(volatile uint32_t*)(run+8));
	close(vcpu); close(vm); close(kvm);
	return 0;
}
