// SPDX-License-Identifier: GPL-2.0
/*
 * XHyper raw HVC injector for coverage-guided fuzzing.
 *
 * XHyper encodes the hypercall number in the HVC immediate
 * (HV_HVC_BASE 0x6000 + number), not in x0. arm_smccc_1_1_hvc() emits
 * hvc #0 and cannot select that immediate, so each immediate in
 * [0x6000, 0x60ff] is a fixed 8-byte stub: hvc #imm; ret.
 */

#include <linux/build_bug.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/uaccess.h>

struct xh_raw_hvc {
	__u16 imm;		/* HVC immediate; window [0x6000, 0x60ff] */
	__u16 pad[3];
	__u64 x[8];		/* x0..x7 in = args, out = result registers */
};

#define XH_RAW_HVC _IOWR('H', 0x01, struct xh_raw_hvc)
/* Same register block. imm must be 0: the stub is `smc #0; ret`. */
#define XH_RAW_SMC _IOWR('H', 0x02, struct xh_raw_hvc)

#define XH_HVC_IMM_MIN	0x6000
#define XH_HVC_IMM_MAX	0x60ff

static_assert(sizeof(struct xh_raw_hvc) == 72);
static_assert(_IOC_SIZE(XH_RAW_HVC) == 72);
static_assert(XH_RAW_HVC == 0xC0484801);
static_assert(_IOC_SIZE(XH_RAW_SMC) == 72);
static_assert(XH_RAW_SMC == 0xC0484802);

/* r0 = struct xh_raw_hvc.x pointer (u64[8]), r1 = stub address */
extern void xh_raw_hvc_trampoline(u64 *x, unsigned long stub);
extern char xh_raw_hvc_stub_table[];
extern char xh_raw_smc_stub[];

asm(
".pushsection .text\n"
".balign 4\n"
".globl xh_raw_hvc_trampoline\n"
".type xh_raw_hvc_trampoline, %function\n"
"xh_raw_hvc_trampoline:\n"
".cfi_startproc\n"
/*
 * The stub is reached with blr, which overwrites x30, so the caller's LR
 * must be saved before the call. The first version of this driver did not
 * save it: the stub's ret came back correctly, but the trampoline's own
 * final ret then jumped to the instruction after blr again and stored the
 * results through the caller's x19, which held the ioctl user pointer
 * (a PAN fault). The hypervisor was not involved: across hvc #imm it
 * returns results in x0..x7, may clobber x8..x17, and preserves x18..x30
 * and SP. LR and the result pointer are spilled to the EL1 stack so the
 * trampoline does not depend on which callee-saved registers the C caller
 * keeps live.
 */
"stp x30, x0, [sp, #-16]!\n"	/* [sp]=LR to C caller, [sp,#8]=struct ptr */
".cfi_def_cfa_offset 16\n"
"mov x16, x1\n"			/* stub address in a scratch reg, used pre-hvc */
"ldp x2, x3, [x0, #16]\n"
"ldp x4, x5, [x0, #32]\n"
"ldp x6, x7, [x0, #48]\n"
"ldp x0, x1, [x0, #0]\n"		/* load x0,x1 last: overwrites the ptr in x0 */
"blr x16\n"			/* runs hvc #imm; ret -- overwrites x30 */
"ldr x16, [sp, #8]\n"		/* reload struct ptr from the stack */
"stp x0, x1, [x16, #0]\n"
"stp x2, x3, [x16, #16]\n"
"stp x4, x5, [x16, #32]\n"
"stp x6, x7, [x16, #48]\n"
"ldr x30, [sp, #0]\n"		/* restore LR to the C caller */
"add sp, sp, #16\n"
".cfi_def_cfa_offset 0\n"
"ret\n"
".cfi_endproc\n"
".size xh_raw_hvc_trampoline, .-xh_raw_hvc_trampoline\n"
".balign 8\n"
".globl xh_raw_hvc_stub_table\n"
".type xh_raw_hvc_stub_table, %object\n"
"xh_raw_hvc_stub_table:\n"
".set i, 0x6000\n"
".rept 256\n"
"hvc i\n"
"ret\n"
".set i, i+1\n"
".endr\n"
".size xh_raw_hvc_stub_table, .-xh_raw_hvc_stub_table\n"
".balign 4\n"
".globl xh_raw_smc_stub\n"
".type xh_raw_smc_stub, %function\n"
"xh_raw_smc_stub:\n"
"smc #0\n"
"ret\n"
".size xh_raw_smc_stub, .-xh_raw_smc_stub\n"
".popsection\n"
);

static long xh_raw_hvc_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct xh_raw_hvc req;
	unsigned long stub;

	if (cmd != XH_RAW_HVC && cmd != XH_RAW_SMC)
		return -ENOTTY;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (cmd == XH_RAW_SMC) {
		/* Host SMC mediation matches imm16==0 only. */
		if (req.imm != 0)
			return -EINVAL;
		stub = (unsigned long)xh_raw_smc_stub;
	} else {
		if (req.imm < XH_HVC_IMM_MIN || req.imm > XH_HVC_IMM_MAX)
			return -EINVAL;
		/* Each stub is hvc #imm; ret, 8 bytes, indexed from 0x6000. */
		stub = (unsigned long)xh_raw_hvc_stub_table +
		       (req.imm - XH_HVC_IMM_MIN) * 8;
	}

	xh_raw_hvc_trampoline(req.x, stub);

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static const struct file_operations xh_raw_hvc_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= xh_raw_hvc_ioctl,
};

static struct miscdevice xh_raw_hvc_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "xh_raw_hvc",
	.fops	= &xh_raw_hvc_fops,
	.mode	= 0600,
};

static int __init xh_raw_hvc_init(void)
{
	return misc_register(&xh_raw_hvc_miscdev);
}
module_init(xh_raw_hvc_init);

MODULE_DESCRIPTION("XHyper raw HVC fuzzing injector");
MODULE_LICENSE("GPL");
