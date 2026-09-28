// One-shot reproducer for the S3 milestone. Same layout as
// syz_gunyah_setup_vm$arm64 / syz_gunyah_add_vcpu$arm64: donate the SYZOS
// guest section, point the boot PC at guest_main, and run one SYZOS_API_CODE
// blob that does hvc #0x6000 (HYPERVISOR_IDENTIFY).
//
//   gcc -static -O2 -o s3-identify-repro s3-identify-repro.c
//   ./s3-identify-repro syzos-guest.bin syzos.dtb
//
// guest_main offset 0xd2c is for the executor built with this tree
// (guest section VA 0x5a1000, guest_main 0x5a1d2c).

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GH_CREATE_VM 0x4700u
#define GH_SET_USER_MEM_REGION 0x40204701u
#define GH_SET_DTB_CONFIG 0x40104702u
#define GH_VM_START 0x4703u
#define GH_ADD_FUNCTION 0x40104704u
#define GH_VCPU_RUN 0x4705u
#define GH_SET_BOOT_CONTEXT 0x4010470au
#define GH_FN_VCPU 1u
#define GH_MEM_RWX 7u
#define GH_BOOT_REG(set, idx) ((((set) & 0xff) << 8) | ((idx) & 0xff))

#define PAGE 4096u
#define DTB_GPA 0x80000000ull
#define EXEC_GPA 0xeeee8000ull
#define USER_GPA 0xeeee0000ull
#define SCRATCH_GPA 0xeeef0000ull
#define STACK_GPA 0xffff1000ull
#define GUEST_MAIN_OFF 0xd2cull
#define MARKER 0x1111ull

struct region {
	uint32_t label;
	uint32_t flags;
	uint64_t gpa;
	uint64_t size;
	uint64_t userspace;
};

struct dtb_cfg {
	uint64_t gpa;
	uint64_t size;
};

struct boot {
	uint32_t reg;
	uint32_t reserved;
	uint64_t value;
};

struct fn_vcpu {
	uint32_t id;
};

struct fn_desc {
	uint32_t type;
	uint32_t arg_size;
	uint64_t arg;
};

struct vcpu_run {
	uint8_t immediate_exit;
	uint8_t padding[7];
	uint32_t exit_reason;
	uint64_t phys_addr;
	int attempt;
	uint8_t resume_action;
};

static void die(const char* what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static void* load_file(const char* path, size_t* out)
{
	FILE* f = fopen(path, "rb");
	long n;
	void* buf;

	if (!f)
		die(path);
	if (fseek(f, 0, SEEK_END))
		die("fseek");
	n = ftell(f);
	if (n < 0)
		die("ftell");
	rewind(f);
	buf = calloc(1, n);
	if (!buf)
		die("calloc");
	if (fread(buf, 1, n, f) != (size_t)n)
		die("fread");
	fclose(f);
	*out = n;
	return buf;
}

static int set_region(int vm, uint32_t label, uint64_t gpa, void* host, uint64_t size)
{
	struct region r;

	memset(&r, 0, sizeof(r));
	r.label = label;
	r.flags = GH_MEM_RWX;
	r.gpa = gpa;
	r.size = size;
	r.userspace = (uint64_t)(uintptr_t)host;
	if (ioctl(vm, GH_SET_USER_MEM_REGION, &r)) {
		fprintf(stderr, "SET_USER_MEM_REGION label=%u gpa=%#llx: %s\n",
			label, (unsigned long long)gpa, strerror(errno));
		return -1;
	}
	return 0;
}

static int set_boot(int vm, uint32_t reg, uint64_t value)
{
	struct boot b;

	memset(&b, 0, sizeof(b));
	b.reg = reg;
	b.value = value;
	if (ioctl(vm, GH_SET_BOOT_CONTEXT, &b)) {
		fprintf(stderr, "SET_BOOT_CONTEXT reg=%#x val=%#llx: %s\n",
			reg, (unsigned long long)value, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char** argv)
{
	size_t guest_sz = 0, dtb_sz = 0;
	uint8_t* guest;
	uint8_t* dtb;
	uint8_t* mem;
	uint8_t *dtb_pages, *executor, *user, *scratch, *stack;
	int dev, vm, vcpu, i;
	struct dtb_cfg cfg;
	struct fn_vcpu varg;
	struct fn_desc desc;
	struct vcpu_run* run;
	uint64_t* words;
	long pagesz;
	// call=10, size=0x2c, six insns, then ret. Matches notes/gunyah/s3-identify.prog.
	struct {
		uint64_t call;
		uint64_t size;
		uint32_t insns[7];
	} code = {
		10,
		0x2c,
		{
			0xd2800001, // movz x1, #0
			0xf2bddde1, // movk x1, #0xeeef, lsl #16
			0xd2822222, // movz x2, #0x1111
			0xf9000022, // str  x2, [x1]
			0xd40c0002, // hvc  #0x6000
			0xf9000420, // str  x0, [x1, #8]
			0xd65f03c0, // ret
		},
	};

	if (argc != 3) {
		fprintf(stderr, "usage: %s syzos-guest.bin syzos.dtb\n", argv[0]);
		return 2;
	}
	guest = load_file(argv[1], &guest_sz);
	dtb = load_file(argv[2], &dtb_sz);
	if (guest_sz > 4 * PAGE || dtb_sz > 2 * PAGE) {
		fprintf(stderr, "guest %zu or dtb %zu too big\n", guest_sz, dtb_sz);
		return 1;
	}
	if (GUEST_MAIN_OFF >= guest_sz) {
		fprintf(stderr, "guest_main offset past blob\n");
		return 1;
	}

	dev = open("/dev/gunyah", O_RDWR);
	if (dev < 0)
		die("open /dev/gunyah");
	vm = ioctl(dev, GH_CREATE_VM);
	if (vm < 0)
		die("GH_CREATE_VM");
	fprintf(stderr, "vm fd %d\n", vm);

	mem = mmap(NULL, 16 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED)
		die("mmap");
	dtb_pages = mem;
	executor = mem + 2 * PAGE;
	user = mem + 6 * PAGE;
	scratch = mem + 10 * PAGE;
	stack = mem + 11 * PAGE;
	memcpy(dtb_pages, dtb, dtb_sz);
	memcpy(executor, guest, guest_sz);
	memcpy(user, &code, sizeof(code));

	if (set_region(vm, 1, DTB_GPA, dtb_pages, 2 * PAGE))
		return 1;
	if (set_region(vm, 2, EXEC_GPA, executor, 4 * PAGE))
		return 1;
	if (set_region(vm, 3, USER_GPA, user, 4 * PAGE))
		return 1;
	if (set_region(vm, 4, SCRATCH_GPA, scratch, PAGE))
		return 1;
	if (set_region(vm, 5, STACK_GPA, stack, PAGE))
		return 1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.gpa = DTB_GPA;
	cfg.size = 2 * PAGE;
	if (ioctl(vm, GH_SET_DTB_CONFIG, &cfg))
		die("GH_VM_SET_DTB_CONFIG");

	if (set_boot(vm, GH_BOOT_REG(1, 0), EXEC_GPA + GUEST_MAIN_OFF))
		return 1;
	if (set_boot(vm, GH_BOOT_REG(2, 1), STACK_GPA + PAGE - 128))
		return 1;
	if (set_boot(vm, GH_BOOT_REG(0, 0), sizeof(code)))
		return 1;
	if (set_boot(vm, GH_BOOT_REG(0, 1), 0))
		return 1;

	memset(&varg, 0, sizeof(varg));
	memset(&desc, 0, sizeof(desc));
	desc.type = GH_FN_VCPU;
	desc.arg_size = sizeof(varg);
	desc.arg = (uint64_t)(uintptr_t)&varg;
	vcpu = ioctl(vm, GH_ADD_FUNCTION, &desc);
	if (vcpu < 0)
		die("GH_VM_ADD_FUNCTION");
	fprintf(stderr, "vcpu fd %d\n", vcpu);

	pagesz = sysconf(_SC_PAGESIZE);
	run = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED)
		die("mmap vcpu");

	if (ioctl(vm, GH_VM_START))
		die("GH_VM_START");
	fprintf(stderr, "VM started, pc=%#llx\n", (unsigned long long)(EXEC_GPA + GUEST_MAIN_OFF));

	alarm(20);
	for (i = 0; i < 4; i++) {
		if (ioctl(vcpu, GH_VCPU_RUN)) {
			fprintf(stderr, "GH_VCPU_RUN[%d]: %s\n", i, strerror(errno));
			break;
		}
		fprintf(stderr, "exit[%d] reason=%u phys=%#llx\n", i, run->exit_reason,
			(unsigned long long)run->phys_addr);
		if (run->exit_reason == 3) {
			run->resume_action = 1;
			break;
		}
		if (run->exit_reason == 1 || run->exit_reason == 2)
			break;
	}
	words = (uint64_t*)scratch;
	fprintf(stderr, "syz_gunyah identify x0=%#llx marker=%#llx\n",
		(unsigned long long)words[1], (unsigned long long)words[0]);
	return words[0] == MARKER ? 0 : 1;
}
