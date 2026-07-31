// Faithful transcription of the upstream syzbot C reproducer
// https://syzkaller.appspot.com/bug?id=66ade132c7a982f1fde5e45d7cfc85d802958b75
//
//   openat("/dev/kvm")
//   ioctl(kvm,  KVM_CREATE_VM)
//   ioctl(vm,   KVM_CREATE_VCPU, 2)
//   ioctl(vcpu, KVM_SET_VCPU_EVENTS, {ae 05 09 .. esr=6})
//   ioctl(vcpu, KVM_SET_VCPU_EVENTS, {54 f9 08 .. esr=1})
//
// No KVM_RUN, no memslot, no guest code.
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>

int main(void)
{
	struct kvm_vcpu_events e;
	struct kvm_vcpu_init init;
	int kvm, vm, vcpu, r;

	kvm = open("/dev/kvm", O_RDWR);
	if (kvm < 0) { perror("open"); return 1; }
	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0) { perror("KVM_CREATE_VM"); return 1; }
	vcpu = ioctl(vm, KVM_CREATE_VCPU, 2);
	if (vcpu < 0) { perror("KVM_CREATE_VCPU"); return 1; }

	/* syzbot's program does not VCPU_INIT; try it both ways. */
	memset(&init, 0, sizeof(init));
	if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) == 0) {
		memset(init.features, 0, sizeof(init.features));
		r = ioctl(vcpu, KVM_ARM_VCPU_INIT, &init);
		printf("  KVM_ARM_VCPU_INIT ret=%d\n", r);
	}

	memset(&e, 0, sizeof(e));
	e.exception.serror_pending  = 0xae;
	e.exception.serror_has_esr  = 0x05;
	e.exception.ext_dabt_pending = 0x09;
	e.exception.serror_esr      = 6;
	r = ioctl(vcpu, KVM_SET_VCPU_EVENTS, &e);
	printf("  SET_VCPU_EVENTS #1 ret=%d errno=%d\n", r, r < 0 ? errno : 0);

	memset(&e, 0, sizeof(e));
	e.exception.serror_pending  = 0x54;
	e.exception.serror_has_esr  = 0xf9;
	e.exception.ext_dabt_pending = 0x08;
	e.exception.serror_esr      = 1;
	r = ioctl(vcpu, KVM_SET_VCPU_EVENTS, &e);
	printf("  SET_VCPU_EVENTS #2 ret=%d errno=%d\n", r, r < 0 ? errno : 0);

	printf("  pid=%d\n", (int)getpid());
	return 0;
}
