#!/bin/bash
# Issue 006 -- init_pkvm_hyp_vcpu() leaks the EL2 pin on the host vCPU struct
#              when KVM_MP_STATE_SUSPENDED is set before the first KVM_RUN.
#              signature: 'WARNING in kvm_unshare_hyp'
#
# hazard: none for the running system -- but THE LEAK IS PERMANENT. Each
# triggering VM poisons a few more pages of the vCPU slab and nothing recovers
# them short of a reboot. Run this LAST of the harmless issues, and reboot
# before re-running it or before trusting a fix.
#
# Verdict source: NEW dmesg WARNs naming kvm_unshare_hyp around one full run of
# the probe.
#
# Attribution limit, stated plainly. The probe runs its control arm
# (mp_state=RUNNABLE) and its test arm (mp_state=SUSPENDED) in ONE process, so
# both arms share a pid and a Comm and a single dmesg delta cannot tell them
# apart. 006's ISSUE.md measured the split separately on hardware -- control 0
# WARNs, test 3 -- and this script cannot re-derive it. What it can say is
# whether the pair produced any WARN at all, which on a fixed kernel is zero.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 006 "a suspended vCPU leaks its EL2 pin, and the leak is permanent"

TIMEOUT=${TIMEOUT_006:-120}
SETTLE=${SETTLE:-5}
SRC=$ISSUES_DIR/006-unshare-hyp-pfn-missing-on-vcpu-destroy/repro/probe-mpstate-suspended-pin-leak.c

# Named probe-mpstate to match the Comm the ISSUE.md evidence records.
BIN=$(build_repro "$SRC" probe-mpstate) || exit
build_only_stop

need_native
need_kvm
need_pkvm
need_dmesg
warn_if_campaign

note "this probe leaks pins PERMANENTLY. If you re-run it, reboot first, or pre-existing poison will produce failures unrelated to what you are testing."

dmesg_snap before
run_bounded "$TIMEOUT" "$OUTDIR/006-probe.log" "$BIN"
REPRO_RC=$RC
sleep "$SETTLE"
dmesg_snap after
dmesg_delta before after

WARNS=$(dmesg_count 'kvm_unshare_hyp')
MINE=$(dmesg_count 'Comm: probe-mpstate')
printf 'dmesg (new): kvm_unshare_hyp=%s  Comm:probe-mpstate=%s  wrapped=%s\n' \
	"$WARNS" "$MINE" "$DMESG_WRAPPED"
grep -E 'kvm_unshare_hyp|Comm: probe-mpstate' "$OUTDIR/dmesg-new.txt" 2>/dev/null | sed 's/^/    /'

if grep -q 'FATAL: KVM_CREATE_VCPU' "$OUTDIR/006-probe.log" 2>/dev/null; then
	verdict INCONCLUSIVE "KVM_CREATE_VCPU failed outright -- this board already carries poisoned vCPU slab pages from an earlier trigger. That is itself a consequence of 006, but it is not a clean observation: REBOOT and re-run."
fi
if grep -q 'test is vacuous' "$OUTDIR/006-probe.log" 2>/dev/null; then
	verdict INCONCLUSIVE "the host rejected KVM_SET_MP_STATE(SUSPENDED), so EL2 never saw it and the trigger was never issued"
fi
if [ "$REPRO_RC" = 124 ]; then
	verdict INCONCLUSIVE "probe did not finish within ${TIMEOUT}s"
fi
if [ "$REPRO_RC" != 0 ]; then
	verdict INCONCLUSIVE "probe exited rc=$REPRO_RC (2 = a FATAL setup failure) -- see $OUTDIR/006-probe.log"
fi
if ! grep -q '^\[test-SUSPENDED\] ---- done ----' "$OUTDIR/006-probe.log" 2>/dev/null; then
	verdict INCONCLUSIVE "the test-SUSPENDED arm did not complete -- see $OUTDIR/006-probe.log"
fi
if [ "$DMESG_WRAPPED" = 1 ]; then
	verdict INCONCLUSIVE "the dmesg ring buffer wrapped or was cleared during the run -- the delta is not trustworthy"
fi
if [ "$WARNS" != 0 ]; then
	verdict REPRODUCED "$WARNS new WARN(s) naming kvm_unshare_hyp after the control+test pair, $MINE block(s) tagged Comm: probe-mpstate (see the header: this script cannot attribute them to the SUSPENDED arm on its own)"
fi
verdict NOT-REPRODUCED "both arms completed and no kvm_unshare_hyp WARN appeared, so no pin was left behind"
