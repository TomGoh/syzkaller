// Probe N90's pKVM firmware state: does pkvm_firmware_mem exist, and what do INFO / SET_FW_IPA return
// pre-run and post-run? Self-contained (literals + inline structs); cross-compile static for arm64.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define KVMIO 0xAE
#define KVM_CREATE_VM        _IO(KVMIO, 0x01)
#define KVM_CREATE_VCPU      _IO(KVMIO, 0x41)
#define KVM_RUN              _IO(KVMIO, 0x80)
#define KVM_GET_VCPU_MMAP_SIZE _IO(KVMIO, 0x04)

struct kvm_enable_cap { uint32_t cap; uint32_t flags; uint64_t args[4]; uint8_t pad[64]; };
#define KVM_ENABLE_CAP       _IOW(KVMIO, 0xa3, struct kvm_enable_cap)
struct kvm_vcpu_init { uint32_t target; uint32_t features[7]; };
#define KVM_ARM_VCPU_INIT    _IOW(KVMIO, 0xae, struct kvm_vcpu_init)

#define CAP_PROTECTED_VM 0xffbadab1
#define FLAG_SET_FW_IPA 0
#define FLAG_INFO 1

int main(void) {
	int kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { printf("open /dev/kvm: %s\n", strerror(errno)); return 1; }

	int vm = ioctl(kvm, KVM_CREATE_VM, 0x80000000UL);
	if (vm < 0) { printf("CREATE_VM(protected): %s\n", strerror(errno)); return 1; }
	printf("CREATE_VM(bit31) = %d\n", vm);

	// INFO
	uint64_t info[8]; memset(info, 0, sizeof(info));
	struct kvm_enable_cap cap; memset(&cap, 0, sizeof(cap));
	cap.cap = CAP_PROTECTED_VM; cap.flags = FLAG_INFO; cap.args[0] = (uint64_t)(uintptr_t)info;
	errno = 0;
	int r = ioctl(vm, KVM_ENABLE_CAP, &cap);
	printf("INFO           ret=%d errno=%d(%s)  firmware_size=%llu\n",
	       r, errno, r ? strerror(errno) : "ok", (unsigned long long)info[0]);

	// SET_FW_IPA pre-run
	memset(&cap, 0, sizeof(cap));
	cap.cap = CAP_PROTECTED_VM; cap.flags = FLAG_SET_FW_IPA; cap.args[0] = 0x40000000UL;
	errno = 0;
	r = ioctl(vm, KVM_ENABLE_CAP, &cap);
	printf("SET_FW_IPA pre = ret=%d errno=%d(%s)\n", r, errno, r ? strerror(errno) : "ok");

	// Run a throwaway vCPU (immediate_exit) to set pkvm.handle, then SET_FW_IPA again -> expect -EBUSY
	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu >= 0) {
		struct kvm_vcpu_init init; memset(&init, 0, sizeof(init)); init.target = 5;
		if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) == 0) {
			int msz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
			volatile unsigned char* run = mmap(0, msz, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu, 0);
			if (run != MAP_FAILED) {
				run[1] = 1; // kvm_run.immediate_exit at offset 1 (after u8 request_interrupt_window)
				errno = 0;
				long rr = ioctl(vcpu, KVM_RUN, 0);
				printf("KVM_RUN(imm)   ret=%ld errno=%d(%s)\n", rr, errno, strerror(errno));
			}
		}
		memset(&cap, 0, sizeof(cap));
		cap.cap = CAP_PROTECTED_VM; cap.flags = FLAG_SET_FW_IPA; cap.args[0] = 0x40000000UL;
		errno = 0;
		r = ioctl(vm, KVM_ENABLE_CAP, &cap);
		printf("SET_FW_IPA post= ret=%d errno=%d(%s)\n", r, errno, r ? strerror(errno) : "ok");
		close(vcpu);
	}
	close(vm); close(kvm);
	return 0;
}
