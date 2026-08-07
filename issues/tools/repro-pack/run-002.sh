#!/bin/bash
# Issue 002 -- kvm_arch_flush_remote_tlbs_range() has no pKVM branch, so EL2
#              rejects the hypercall and the TLB invalidation never runs.
#              signature: 'WARNING in kvm_tlb_flush_vmid_range'
#
# hazard: none. No vCPU, no guest code; the board is unaffected.
#
# Verdict source: a NEW kernel WARNING naming kvm_tlb_flush_vmid_range in the
# dmesg delta around the run. The reproducer itself returns 0 either way, so the
# dmesg delta is the only signal -- if dmesg is unreadable or the ring buffer
# wrapped, that is INCONCLUSIVE.
#
# READ THIS BEFORE BELIEVING A NOT-REPRODUCED. 002's own ISSUE.md is explicit
# that this reproducer CANNOT verify the fix: it creates no vCPU, so
# kvm->arch.pkvm.handle is still 0, and after the fix EL2 maps handle 0 to an
# out-of-range index and returns success without flushing. The warning then
# disappears because the call became a no-op, not because the invalidation
# started happening. NOT-REPRODUCED here means "the warning did not fire",
# nothing stronger.
#
# verify-dirtylog.c is therefore also built and run, as a clearly-labelled
# SUPPLEMENTARY check that does create a vCPU. It is not part of the verdict --
# see the note it prints for why its result is confounded on an unpatched
# kernel.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 002 "range TLB flush asks EL2 for a hypercall EL2 has retired"

TIMEOUT=${TIMEOUT_002:-60}
DIR=$ISSUES_DIR/002-tlb-range-flush-missing-pkvm-branch/repro

build_repro "$DIR/repro-tlbflush-warn.c" repro-tlbflush
BIN=$REPRO_BIN
build_repro "$DIR/verify-dirtylog.c" verify-dirtylog
VBIN=$REPRO_BIN
build_only_stop

need_native
need_kvm
need_pkvm
need_dmesg
warn_if_campaign

dmesg_snap before
run_bounded "$TIMEOUT" "$OUTDIR/002-repro.log" "$BIN"
REPRO_RC=$RC
sleep "${SETTLE:-2}"
dmesg_snap after
dmesg_delta before after

WARNS=$(dmesg_count 'kvm_tlb_flush_vmid_range')
MINE=$(dmesg_count 'Comm: repro-tlbflush')
printf 'dmesg (new): kvm_tlb_flush_vmid_range=%s  Comm:repro-tlbflush=%s  wrapped=%s\n' \
	"$WARNS" "$MINE" "$DMESG_WRAPPED"
grep -E 'kvm_tlb_flush_vmid_range|Comm: repro-tlbflush' "$OUTDIR/dmesg-new.txt" 2>/dev/null | sed 's/^/    /'

# --- supplementary, not part of the verdict ---------------------------------

echo "--- supplementary: verify-dirtylog (runs a vCPU; NOT the verdict) ---"
run_bounded "$TIMEOUT" "$OUTDIR/002-verify.log" "$VBIN"
VRC=$RC
note "verify-dirtylog rc=$VRC. On a kernel that still has issue 003 this program fails for a 003 reason (its 2 MiB mapping is THP-backed and it never calls MADV_NOHUGEPAGE, so the second KVM_RUN returns -E2BIG and it perrors with rc=1). A rc=1 here is therefore NOT evidence about 002. Only 'PASS:' or the explicit 'FAIL: ... TLB invalidation did not happen' line says anything about this issue."

# --- verdict ----------------------------------------------------------------

if [ "$REPRO_RC" = 3 ] || [ "$REPRO_RC" = 124 ]; then
	verdict INCONCLUSIVE "reproducer hung (rc=$REPRO_RC) -- it never completed the ioctl under test"
fi
if [ "$REPRO_RC" != 0 ]; then
	verdict INCONCLUSIVE "reproducer exited rc=$REPRO_RC before reaching the flags-only memslot change -- see $OUTDIR/002-repro.log"
fi
if [ "$DMESG_WRAPPED" = 1 ]; then
	verdict INCONCLUSIVE "the dmesg ring buffer wrapped or was cleared during the run -- the delta is not trustworthy"
fi
if [ "$WARNS" != 0 ]; then
	verdict REPRODUCED "$WARNS new dmesg line(s) naming kvm_tlb_flush_vmid_range, $MINE of the surrounding blocks tagged Comm: repro-tlbflush"
fi
verdict NOT-REPRODUCED "reproducer completed and no new kvm_tlb_flush_vmid_range warning appeared -- but see the header: this program cannot distinguish a fixed flush from a no-op flush"
