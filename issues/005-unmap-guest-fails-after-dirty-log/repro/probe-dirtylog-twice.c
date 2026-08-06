// Issue 005 confirmation: does a successful __pkvm_host_dirty_log_guest leave the
// page in a state that makes the NEXT write-protect fail?
//
// Predicted chain, if the EL2 handler's bare-RWX kvm_pgtable_stage2_map() really
// zeroes the guest PTE's page-state bits:
//
//   round 2 write to A  -> perm fault -> dirty_log(A) succeeds -> A is RWX, state == OWNED
//   KVM_GET_DIRTY_LOG   -> A reported dirty, then re-write-protected:
//                          pkvm_wp_range -> __pkvm_host_wrprotect_guest
//                            -> __check_host_unshare_guest -> -EPERM   (silently dropped;
//                               kvm_stage2_wp_range() is void)
//   round 3 write to A  -> A is STILL writable -> no fault -> user_mem_abort() never runs
//                          -> mark_page_dirty_in_slot() never called -> WRITE IS LOST
//
// B is the in-run control: same size, same slot, faulted in at the same time, but it is
// never dirty-logged before round 3, so its state is untouched and it must behave.
// KVM_GET_DIRTY_LOG only re-protects pages it reports dirty, so B keeps the protection
// installed when logging was enabled and is not affected by A's failure.
//
// Expected on a correct kernel:  round-3 bitmap = A dirty, B dirty.
// Predicted on #4:               round-3 bitmap = A CLEAN, B dirty.
//
//   ./probe-dirtylog-twice nohuge   <- the mode to use; issue 003 is silent there
//   ./probe-dirtylog-twice          <- THP: issue 003 fires first, run aborts at round 2
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
#define MEM_SIZE   0x200000UL		/* 2 MiB = 512 pages */
#define A_OFF      0x10000UL		/* page 16 — gets dirty-logged in round 2 */
#define B_OFF      0x20000UL		/* page 32 — control, first dirty-logged in round 3 */
#define A_PAGE     (A_OFF / 4096)
#define B_PAGE     (B_OFF / 4096)
#define BITMAP_LEN ((MEM_SIZE / 4096 + 63) / 64)

#define MOVZ(rd, imm16, sh) (0xD2800000u | ((uint32_t)(sh) << 21) | (((uint32_t)(imm16) & 0xffff) << 5) | (rd))
#define STR_X(rt, rn)       (0xF9000000u | ((uint32_t)(rn) << 5) | (rt))
#define B_SELF              (0x14000000u)

static int g_vm;
static int g_mark = -1;

/* Delimit phases in the ftrace buffer so kretprobe hits can be attributed to a
 * round rather than guessed at. Silently inert if tracefs is not mounted. */
static void mark(const char *s)
{
	if (g_mark >= 0)
		(void)!write(g_mark, s, strlen(s));
}

static int bit_at(const uint64_t *bm, unsigned long page)
{
	return !!(bm[page / 64] & (1ULL << (page % 64)));
}

static int get_dirty(const char *tag, uint64_t *bm)
{
	struct kvm_dirty_log dl;

	memset(bm, 0, BITMAP_LEN * sizeof(*bm));
	memset(&dl, 0, sizeof(dl));
	dl.slot = 0;
	dl.dirty_bitmap = bm;

	if (ioctl(g_vm, KVM_GET_DIRTY_LOG, &dl) < 0) {
		printf("%-22s KVM_GET_DIRTY_LOG failed: %s\n", tag, strerror(errno));
		return -1;
	}
	printf("%-22s A(page %lu)=%d  B(page %lu)=%d\n",
	       tag, (unsigned long)A_PAGE, bit_at(bm, A_PAGE),
	       (unsigned long)B_PAGE, bit_at(bm, B_PAGE));
	return 0;
}

static void on_alarm(int s) { (void)s; printf("!! timed out\n"); _exit(3); }

int main(int argc, char **argv)
{
	int nohuge = (argc > 1 && !strcmp(argv[1], "nohuge"));
	struct kvm_userspace_memory_region region;
	uint64_t bm[BITMAP_LEN];
	struct kvm_vcpu_init init;
	struct kvm_one_reg reg;
	uint64_t pc = GUEST_PHYS;
	volatile struct kvm_run *run;
	int kvm, vcpu, msz, ret;
	uint32_t *code;
	void *mem;

	signal(SIGALRM, on_alarm);
	alarm(30);

	g_mark = open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY);

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
	*code++ = MOVZ(0, 0x4001, 1);		/* x0 = A   = 0x40010000 */
	*code++ = MOVZ(3, 0x4002, 1);		/* x3 = B   = 0x40020000 */
	*code++ = MOVZ(2, 0x5000, 1);		/* x2 = MMIO= 0x50000000 */

	*code++ = MOVZ(1, 1, 0);		/* --- round 1: fault both pages in --- */
	*code++ = STR_X(1, 0);
	*code++ = STR_X(1, 3);
	*code++ = STR_X(1, 2);			/* exit 1 */

	*code++ = MOVZ(1, 2, 0);		/* --- round 2: A only, under logging --- */
	*code++ = STR_X(1, 0);
	*code++ = STR_X(1, 2);			/* exit 2 */

	*code++ = MOVZ(1, 3, 0);		/* --- round 3: A then B --- */
	*code++ = STR_X(1, 0);
	*code++ = STR_X(1, 3);
	*code++ = STR_X(1, 2);			/* exit 3 */

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

	/* round 1 — both pages faulted in, no dirty logging yet */
	mark("PHASE round1-run");
	ret = ioctl(vcpu, KVM_RUN, 0);
	printf("round1 KVM_RUN         ret=%d errno=%d exit=%u\n",
	       ret, ret < 0 ? errno : 0, run->exit_reason);
	if (ret < 0) return 1;

	mark("PHASE enable-dirty-logging");
	region.flags = KVM_MEM_LOG_DIRTY_PAGES;
	if (ioctl(g_vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("enable dirty log"); return 1;
	}
	printf("[dirty logging enabled — whole slot write-protected]\n");

	/* round 2 — A takes the permission fault, EL2 dirty-log hypercall runs */
	mark("PHASE round2-run");
	ret = ioctl(vcpu, KVM_RUN, 0);
	printf("round2 KVM_RUN         ret=%d errno=%d (%s) exit=%u\n",
	       ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "-", run->exit_reason);
	if (ret < 0) {
		printf("!! round 2 failed — this is issue 003, not 005. Use nohuge.\n");
		return 1;
	}

	/* reports A dirty, then re-write-protects A. That is the call under test. */
	mark("PHASE getdirty-1-reprotect");
	if (get_dirty("after round2:", bm) < 0) return 1;

	/* round 3 — A again (predicted: no fault, lost), then B (control) */
	mark("PHASE round3-run");
	ret = ioctl(vcpu, KVM_RUN, 0);
	printf("round3 KVM_RUN         ret=%d errno=%d (%s) exit=%u\n",
	       ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "-", run->exit_reason);
	if (ret < 0) return 1;

	mark("PHASE getdirty-2");
	if (get_dirty("after round3:", bm) < 0) return 1;

	printf("\nverdict: A=%d B=%d  -> %s\n",
	       bit_at(bm, A_PAGE), bit_at(bm, B_PAGE),
	       (!bit_at(bm, A_PAGE) && bit_at(bm, B_PAGE))
		       ? "PREDICTED FAILURE seen: A's write was lost, control B survived"
		       : (bit_at(bm, A_PAGE) && bit_at(bm, B_PAGE))
			       ? "both tracked — prediction NOT reproduced"
			       : "unexpected combination, read the log above");

	mark("PHASE teardown");
	alarm(0);
	return 0;
}
