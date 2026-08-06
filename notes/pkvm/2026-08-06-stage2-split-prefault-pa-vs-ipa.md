# `stage2_map_prefault_block()` compares a PA against an IPA, so the skip never fires for a guest

**Status:** latent upstream defect, code-read plus one concrete hardware demonstration. **Not in the issue tracker** — the tracker records only what fuzzing observed (`issues/README.md`, Scope), and this was uncovered while fixing issue 005 rather than by a campaign. Recorded here because anyone reasoning about pKVM block splitting will otherwise reach the same wrong conclusion I did.

## The code

`arch/arm64/kvm/hyp/pgtable.c`, klinux `@cba248683e5c` (identical in ACK `android15-6.6`):

```c
static void stage2_map_prefault_block(struct kvm_pgtable_pte_ops *pte_ops,
				      const struct kvm_pgtable_visit_ctx *ctx,
				      kvm_pte_t *ptep)
{
	kvm_pte_t block_pte = ctx->old;
	...
	pa = kvm_pte_to_phys(block_pte);              /* physical address */
	granule = kvm_granule_size(ctx->level + 1);
	...
	for (i = 0; i < PTRS_PER_PTE; ++i, ++ptep, pa += granule) {
		kvm_pte_t pte = kvm_init_valid_leaf_pte(pa, block_pte, ctx->level + 1);
		/*
		 * Skip ptes in the range being modified by the caller if we're
		 * installing last level entries. ...
		 */
		if ((ctx->level < (KVM_PGTABLE_LAST_LEVEL - 1)) ||
				(pa < ctx->addr) || (pa >= ctx->end)) {
			*ptep = pte;
			...
		}
	}
}
```

`pa` is a **physical** address, taken from the block PTE. `ctx->addr` and `ctx->end` are the walk's **input** addresses — for a guest stage-2 those are IPAs.

## Why it is only coherent for the host

The host's stage-2 is identity-mapped (`KVM_PGTABLE_S2_IDMAP`), so there PA == IPA and the comparison means what it reads as: "skip the entries the caller is about to install itself."

For a **guest** stage-2 the two are unrelated. Whether the skip fires is then decided by an accidental numeric relationship between the guest's IPA layout and wherever the host happened to allocate the backing memory. In practice guest IPAs start low (`0x40000000` in our reproducers) and host physical addresses on this board are far higher, so `pa >= ctx->end` is true for every entry and **the skip never fires**: every split pre-populates the whole child table, target page included.

## The demonstration

Kernel `#14` on N90 was built on the assumption that the skip *does* fire — that after a split the target page's entry is left invalid, so `stage2_map()` would install it cleanly. Every THP guest that enabled dirty logging livelocked instead, because the target entry had in fact been pre-populated as a valid, still-write-protected leaf, making the subsequent leaf visit a permissions-only change that `stage2_pte_needs_update()` declines.

That is a hardware-confirmed consequence of the comparison not doing what it looks like it does. Full account: `issues/005-unmap-guest-fails-after-dirty-log/evidence/2026-08-06-one-line-fix-livelocks.txt`, section 7.

## Is it a bug?

The observable effect is a **performance and TLB** matter, not a correctness one, which is presumably why it has gone unnoticed:

- The entries are pre-populated with valid leaves derived from the block, so no mapping is lost and no state is corrupted.
- The comment itself says installing the caller's range temporarily is the deliberate fallback path for non-last-level splits, and costs "an unnecessary TLBI as we'll presumably re-break the freshly installed block".

So for guests that fallback is taken *always* rather than *never*, and the optimisation the skip exists to provide is simply dead. Nothing breaks — but any code that reasons "after a split my target entry is invalid" is wrong on a guest, and that is exactly the trap `#14` fell into.

## What to do with it

Nothing urgent. Worth reporting upstream as a question rather than a patch, since the intended semantics are ambiguous: the fix could be to compare against the walk's IPA-space equivalent of `pa`, or to drop the skip for non-identity page tables and document that guests always take the fallback.

Before then, the practical rule for this tree: **do not assume a split leaves the target entry invalid on a guest stage-2.** It does not.

## See also

- `issues/005-unmap-guest-fails-after-dirty-log/` — the fix that this shaped
- [kernel-reference-trees](../../../.claude/projects/-home-jose-syzkaller/memory/kernel-reference-trees.md) — which trees may be compared against
