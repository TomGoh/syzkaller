// Issue 006 confirmation: does KVM_MP_STATE_SUSPENDED leak the EL2 pin on the
// host vCPU struct, so that the teardown unshare is refused by EL2?
//
// HYPOTHESIS UNDER TEST (code-read only until this program says otherwise):
//
//   host  KVM_SET_MP_STATE(SUSPENDED)   is ACCEPTED by arm.c and stores
//         vcpu->arch.mp_state.mp_state = KVM_MP_STATE_SUSPENDED (10).
//
//   first KVM_RUN -> kvm_arch_vcpu_run_pid_change -> pkvm_create_hyp_vm
//         -> __pkvm_create_hyp_vcpu -> __pkvm_init_vcpu (hypercall)
//
//   EL2   init_pkvm_hyp_vcpu()  (xhypervisor/src/pkvm.rs)
//           hyp_pin_shared_mem(host_vcpu, host_vcpu + 1)      <-- pin taken
//           self.vcpu.arch.hyp_reqs = ...; pin hyp_reqs
//           mp_state = read_once(host_vcpu->arch.mp_state.mp_state)   == 10
//           if mp_state != RUNNABLE && mp_state != STOPPED {
//                   ret = -EINVAL;
//                   return goto_done(self, ret);      -> unpin_host_vcpu()
//           }
//           self.host_vcpu = host_vcpu;               <-- ASSIGNED ONLY HERE, TOO LATE
//
//         unpin_host_vcpu() unpins via the FIELD self.host_vcpu, which on this
//         path is still NULL, so the host-vCPU pin is NEVER released. hyp_reqs
//         IS released, because its field was assigned before the check.
//
//   teardown  close(vcpu_fd); close(vm_fd)
//         -> kvm_destroy_vcpus -> kvm_arm_vcpu_destroy (reset.c:160)
//         -> kvm_unshare_hyp(vcpu, vcpu + 1) -> unshare_pfn_hyp -> EL2
//         -> host_request_unshare(): is_range_refcounted() == true -> -EINVAL
//         -> WARN_ON at arch/arm64/kvm/mmu.c:728
//
// EXPECTED ON AN AFFECTED KERNEL: the KVM_RUN below fails, and closing the fds
// produces "WARNING in kvm_unshare_hyp" in dmesg, on the vCPU struct range.
//
// EXPECTED IF THE HYPOTHESIS IS WRONG: KVM_RUN may still fail (that part is
// expected either way), but NO WARN appears at teardown.
//
// The RUNNABLE arm is the in-run control: identical sequence, mp_state left at
// a value EL2 accepts, so init_pkvm_hyp_vcpu() takes no error path and the pin
// is released normally. If the control ALSO warns, the mp_state value is not
// what matters and this whole hypothesis is wrong.
//
// Build:  aarch64-linux-gnu-gcc -O1 -static -o probe-mpstate probe-mpstate-suspended-pin-leak.c
// Run:    ./probe-mpstate            (needs /dev/kvm; run on a pKVM host)
//         then check dmesg for kvm_unshare_hyp.
//
// Exit codes are deliberate and distinct so a hang can never be mistaken for a
// pass (this project has been bitten by exactly that): every exit prints first
// and flushes.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <linux/kvm.h>

#ifndef KVM_MP_STATE_SUSPENDED
#define KVM_MP_STATE_SUSPENDED 10
#endif

// KVM_RUN must be bounded. A RUNNABLE vCPU with no memslots and no pvmfw spins
// in the guest-abort loop and NEVER returns -- the first version of this probe
// hung there and had to be killed. We only need KVM_RUN to get as far as
// kvm_arch_vcpu_run_pid_change() -> pkvm_create_hyp_vm(), which happens before
// the guest executes anything, so interrupting it afterwards costs nothing.
//
// The handler must NOT set SA_RESTART, or the ioctl resumes instead of
// returning -EINTR.
static volatile sig_atomic_t got_alarm;
static void on_alarm(int sig) { (void)sig; got_alarm = 1; }

static void install_alarm_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;              /* deliberately no SA_RESTART */
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		perror("sigaction");
		exit(2);
	}
}

static void die(const char *what)
{
	fprintf(stderr, "FATAL: %s: %s\n", what, strerror(errno));
	fflush(NULL);
	exit(2);
}

// One full VM lifecycle. mp_state < 0 means "do not call KVM_SET_MP_STATE".
// Returns the errno from KVM_RUN (0 if it somehow succeeded).
static int cycle(const char *tag, int mp_state)
{
	int kvm, vmfd, vcpufd, ret, run_errno = 0;
	struct kvm_vcpu_init init;
	struct kvm_mp_state mps;

	printf("[%s] ---- start (mp_state=%d) ----\n", tag, mp_state);
	fflush(stdout);

	kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0)
		die("open /dev/kvm");

	vmfd = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vmfd < 0)
		die("KVM_CREATE_VM");

	vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
	if (vcpufd < 0)
		die("KVM_CREATE_VCPU");

	// KVM_ARM_VCPU_INIT is required: kvm_arch_vcpu_run_pid_change() returns
	// -ENOEXEC for an uninitialised vCPU, which would skip pkvm_create_hyp_vm()
	// entirely and make the test vacuous.
	memset(&init, 0, sizeof(init));
	if (ioctl(vmfd, KVM_ARM_PREFERRED_TARGET, &init) < 0)
		die("KVM_ARM_PREFERRED_TARGET");
	if (ioctl(vcpufd, KVM_ARM_VCPU_INIT, &init) < 0)
		die("KVM_ARM_VCPU_INIT");

	if (mp_state >= 0) {
		memset(&mps, 0, sizeof(mps));
		mps.mp_state = mp_state;
		ret = ioctl(vcpufd, KVM_SET_MP_STATE, &mps);
		printf("[%s] KVM_SET_MP_STATE(%d) = %d%s\n", tag, mp_state, ret,
		       ret < 0 ? strerror(errno) : "");
		if (ret < 0) {
			// The host rejected it, so EL2 never sees it and the test
			// cannot run. Report loudly rather than silently passing.
			printf("[%s] host REJECTED this mp_state -- test is vacuous\n", tag);
			fflush(stdout);
			close(vcpufd); close(vmfd); close(kvm);
			return -1;
		}
		// Read it back: this is what EL2 will read out of the shared struct.
		memset(&mps, 0, sizeof(mps));
		if (ioctl(vcpufd, KVM_GET_MP_STATE, &mps) == 0)
			printf("[%s] KVM_GET_MP_STATE -> %u  (EL2 accepts only 0 and 5)\n",
			       tag, mps.mp_state);
	}

	// First KVM_RUN is the trigger: it is what calls pkvm_create_hyp_vm().
	// Bounded by SIGALRM -- see install_alarm_handler(). Reaching the timeout
	// is a NORMAL outcome here, not a failure: it means the vCPU got far
	// enough to start running, which is strictly after the EL2 init we care
	// about.
	got_alarm = 0;
	errno = 0;
	alarm(3);
	ret = ioctl(vcpufd, KVM_RUN, 0);
	run_errno = (ret < 0) ? errno : 0;
	alarm(0);
	printf("[%s] KVM_RUN = %d  errno=%d (%s)%s\n", tag, ret, run_errno,
	       run_errno ? strerror(run_errno) : "-",
	       got_alarm ? "  [interrupted by our 3s alarm]" : "");
	fflush(stdout);

	// Teardown. This is where the WARN is expected.
	printf("[%s] closing fds -- watch for WARNING in kvm_unshare_hyp\n", tag);
	fflush(stdout);
	close(vcpufd);
	close(vmfd);
	close(kvm);

	printf("[%s] ---- done ----\n\n", tag);
	fflush(stdout);
	return run_errno;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	install_alarm_handler();

	printf("issue 006 probe: KVM_MP_STATE_SUSPENDED pin leak\n");
	printf("KVM_MP_STATE_RUNNABLE=%d STOPPED=%d SUSPENDED=%d\n\n",
	       KVM_MP_STATE_RUNNABLE, KVM_MP_STATE_STOPPED, KVM_MP_STATE_SUSPENDED);

	// Control first, so that if the control itself warns we learn that before
	// the experimental arm muddies dmesg.
	cycle("control-RUNNABLE", KVM_MP_STATE_RUNNABLE);

	// The experiment.
	cycle("test-SUSPENDED", KVM_MP_STATE_SUSPENDED);

	printf("both cycles complete. Now:  dmesg | grep -c kvm_unshare_hyp\n");
	printf("PASS (hypothesis confirmed) == WARN appears only after test-SUSPENDED\n");
	fflush(NULL);
	return 0;
}
