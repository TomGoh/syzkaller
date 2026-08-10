---
id: 008
slug: vgic-its-save-tables-null-collection
title: vgic_its_save_ite() dereferences ite->collection without checking it, so saving the ITS tables while any ITE has an unmapped collection takes a NULL pointer dereference in the kernel — reachable from an unprivileged KVM_SET_DEVICE_ATTR
class: kernel-defect
signature: 'Internal error: Oops — NULL pointer dereference at 0x10 in vgic_its_save_tables_v0'
hazard: none
diagnosis: root-caused
disposition: open
repro: none
observations:
  - target: 'klinux 6.6.103+ #27 @1f52c77ba624'
    state: observed
    run: 2026-08-10-n90-delivery-acceptance
    evidence: evidence/oops-with-fault-address.txt
---

# 008 — saving the ITS tables dereferences a NULL collection pointer

Found by the delivery acceptance run, 11h25m in. Unlike 001–007 this is **not** a pKVM defect and not a Kylin one: the faulting code is identical in mainline and in ACK up to `android17-6.18`.

## Symptom

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000010
Internal error: Oops: 0000000096000006 [#1] SMP
CPU: 4 PID: 2216003 Comm: syz.5.14572
pc : vgic_its_save_tables_v0+0x27c/0x698
Call trace:
 vgic_its_save_tables_v0+0x27c/0x698
 vgic_its_set_attr+0x184/0x3e0
 kvm_device_ioctl+0xe8/0x278
 __arm64_sys_ioctl+0x108/0x128
```

`ESR 0x96000006` decodes as a data abort taken without an EL change, read, translation fault at level 2 — i.e. an ordinary bad-pointer read, not a permission or alignment problem.

Once the fuzzer found the input it reproduced continuously: **213 Oopses**, roughly one per second, until the run was stopped. Each one kills the calling task; the kernel itself stayed up and the board stayed reachable.

## Root cause

`addr2line` on the faulting PC resolves through the inline frames to `vgic_its_save_ite()`:

```
vgic_its_save_ite            arch/arm64/kvm/vgic/vgic-its.c:2225
vgic_its_save_itt            arch/arm64/kvm/vgic/vgic-its.c:2326
vgic_its_save_device_tables  arch/arm64/kvm/vgic/vgic-its.c:2478
vgic_its_save_tables_v0      arch/arm64/kvm/vgic/vgic-its.c:2698
```

and the function has exactly two pointer dereferences:

```c
static int vgic_its_save_ite(struct vgic_its *its, struct its_device *dev,
			      struct its_ite *ite, gpa_t gpa, int ite_esz)
{
	next_offset = compute_next_eventid_offset(&dev->itt_head, ite);
	val = ((u64)next_offset << KVM_ITS_ITE_NEXT_SHIFT) |
	       ((u64)ite->irq->intid << KVM_ITS_ITE_PINTID_SHIFT) |
		ite->collection->collection_id;      /* <- here */
	...
}
```

The fault address picks between them without ambiguity:

```c
struct its_collection {
	struct list_head coll_list;   /* offset 0x00, 16 bytes */
	u32 collection_id;            /* offset 0x10             */
	u32 target_addr;              /* offset 0x14             */
};
```

`collection_id` sits at **offset 0x10**, and the fault address is **0x10**. So `ite->collection` is NULL.

### A NULL collection is a legal state, and the kernel says so itself

This is not a corrupted pointer. `vgic-its.c:137` defines

```c
#define its_is_collection_mapped(coll) ((coll) && \
					(coll)->target_addr != COLLECTION_NOT_MAPPED)
```

— the NULL test is *part of the predicate* — and five sites in the same file consult it before touching `ite->collection`:

| line | context |
| --- | --- |
| 397 | `if (!its_is_collection_mapped(ite->collection))` |
| 693 | `if (!ite \|\| !its_is_collection_mapped(ite->collection))` |
| 864 | `if (ite && its_is_collection_mapped(ite->collection)) {` |
| 901 | `if (!its_is_collection_mapped(ite->collection))` |
| 905 | `if (!its_is_collection_mapped(collection))` |

`vgic_its_save_ite()` is the one place on the save path that does not. An ITE whose collection was never mapped — or whose collection mapping was removed while the ITE survived — is therefore representable, and saving the tables walks straight into it.

**The mechanism above is established from the fault address, the struct layout and the source; the exact guest command sequence that leaves an ITE with a NULL collection has NOT yet been reproduced by hand.** That is the remaining work, see below.

## Trigger

`ioctl$KVM_SET_DEVICE_ATTR` on an ITS device with the `KVM_DEV_ARM_ITS_SAVE_TABLES` attribute (`vgic_its_set_attr` → `vgic_its_save_tables_v0`), while the ITS holds at least one ITE with an unmapped collection.

All three calls needed are ordinary unprivileged `/dev/kvm` operations and all three were enabled in the acceptance config: `ioctl$KVM_CREATE_DEVICE`, `ioctl$KVM_SET_DEVICE_ATTR`, `syz_kvm_vgic_v3_setup`.

## Not a Kylin defect

The faulting function is byte-identical in the reference trees:

| tree | `vgic_its_save_ite` has a NULL check? |
| --- | --- |
| klinux 6.6.103+ | no |
| ACK `android16-6.12` | no |
| ACK `android17-6.18` | no |
| mainline `torvalds/master` | no |

So this is an **upstream-shared defect**, worth reporting upstream rather than patching locally. That also means it is not evidence about the quality of this vendor kernel specifically — a distinction the delivery report should keep.

## Blast radius

Any process that can open `/dev/kvm` can Oops the kernel. It does not take the machine down — each occurrence kills only the calling task, `[#N]` keeps incrementing and the board stayed usable through 213 of them — but it is an unprivileged kernel NULL dereference, and on a kernel built with `panic_on_oops` it would be a denial of service.

## Fix status

`open`. Nothing has been changed; this entry records the diagnosis.

The obvious fix is the same guard the other five sites use, and it needs a decision that is not ours to make alone: skip the ITE, or fail the whole save with an error. Skipping silently would write an ITT that does not describe the ITS state.

## Still to do

1. **A standalone reproducer.** The acceptance run reproduced it continuously, so the input exists in the corpus; extracting or hand-writing it is the next step. Getting an ITE with a NULL collection needs guest-issued ITS commands (`MAPD`, `MAPTI` without a matching `MAPC`, or a `MAPC` that unmaps), which is why it is not a five-line probe.
2. **Report upstream** once the reproducer exists.
3. Add a `run-008.sh` to the reproducer pack and list it in `delivery/README.md`.

## Why syzkaller did not record it

The manager's crash list shows only `WARNING in pend_sync_exception`; this Oops is absent from it despite being far more serious. It was caught by an independent dmesg watcher instead.

That gap matters for the delivery methodology, not just for this issue: the report's "crash count" comes from syzkaller alone, so a fault that its console pipeline misses is invisible in the headline number. `make-report.py` should read the board's own `dmesg-history.log` as a second, independent source and flag any disagreement.
