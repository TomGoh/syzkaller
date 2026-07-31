// Settle which KVM_SET_VCPU_EVENTS sequence trips
// WARN_ON(vcpu_get_flag(v, INCREMENT_PC)) in kvm_pend_exception().
// NO KVM_RUN anywhere -- that is the point under test.
//
//   case 1: dabt, then dabt
//   case 2: serror (no esr), then dabt
//   case 3: one call with serror_pending=1 AND ext_dabt_pending=1
//   case 4: serror (with esr), then dabt
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int vcpu_fd;

static int ev(int serror, int has_esr, unsigned long esr, int dabt, const char *what)
{
	struct kvm_vcpu_events e;
	int r;

	memset(&e, 0, sizeof(e));
	e.exception.serror_pending = serror;
	e.exception.serror_has_esr = has_esr;
	e.exception.serror_esr = esr;
	e.exception.ext_dabt_pending = dabt;
	r = ioctl(vcpu_fd, KVM_SET_VCPU_EVENTS, &e);
	printf("    %-34s ret=%d\n", what, r);
	return r;
}

int main(int argc, char **argv)
{
	int which = argc > 1 ? atoi(argv[1]) : 1;
	struct kvm_vcpu_init init;
	int kvm, vm;

	kvm = open("/dev/kvm", O_RDWR);
	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
	memset(&init, 0, sizeof(init));
	ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init);
	memset(init.features, 0, sizeof(init.features));
	if (ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0) {
		perror("KVM_ARM_VCPU_INIT");
		return 1;
	}

	printf("  case %d (pid=%d):\n", which, (int)getpid());
	switch (which) {
	case 1:
		ev(0, 0, 0, 1, "dabt #1");
		ev(0, 0, 0, 1, "dabt #2");
		break;
	case 2:
		ev(1, 0, 0, 0, "serror (no esr)");
		ev(0, 0, 0, 1, "dabt");
		break;
	case 3:
		ev(1, 0, 0, 1, "serror + dabt in ONE call");
		break;
	case 4:
		ev(1, 1, 0x1000000, 0, "serror (with esr)");
		ev(0, 0, 0, 1, "dabt");
		break;
	}
	return 0;
}
