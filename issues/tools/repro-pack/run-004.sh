#!/bin/bash
# Issue 004 -- the dirty-logging topup into stage2_mc is never accounted, so
#              protected_hyp_mem ends negative and the hyp-donation leak check
#              reports a bogus 18 EB.
#              signature: 'donations to the nVHE hyp are missing'
#
# hazard: none. A counter is wrong; no memory is lost.
#
# Verdict source: a NEW dmesg line from kvm_arch_destroy_vm(). Three wordings
# are matched, because part (b) of the fix rewrites the message:
#
#   unpatched            "<huge>B of donations to the nVHE hyp are missing"
#   patched, positive    "<n>B of donations to the nVHE hyp were never returned"
#   patched, negative    "nVHE hyp donation accounting is <n>B short: ..."
#
# Any of the three means the residual is non-zero, which is this issue. The
# check only prints at all when the residual is non-zero, so zero lines is the
# fixed state.
#
# 'nohuge' is the arm to measure, per 004's ISSUE.md: KVM_RUN succeeds there, so
# issue 003 is out of the way and the message still appears. 'nodirty' is the
# control and must produce none.
#
# The message is printed AFTER the process exits (from kvm_arch_destroy_vm), so
# dmesg is read only after a settle. Reading it synchronously on return misses
# it intermittently, which produced a wrong conclusion twice before.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 004 "the hyp-donation leak check is broken, in both directions"

TIMEOUT=${TIMEOUT_004:-60}
SETTLE=${SETTLE:-5}
PATTERN='donations to the nVHE hyp|nVHE hyp donation accounting'
SRC=$ISSUES_DIR/004-hyp-donation-accounting-imbalance/repro/probe-dirtylog-thp.c

BIN=$(build_repro "$SRC" probe-dirtylog-thp) || exit
build_only_stop

need_native
need_kvm
need_pkvm
need_dmesg
warn_if_campaign

# --- measured arm: dirty logging on, huge pages out of the way ---------------

dmesg_snap before
run_bounded "$TIMEOUT" "$OUTDIR/004-nohuge.log" "$BIN" nohuge
NOHUGE_RC=$RC
sleep "$SETTLE"
dmesg_snap after
dmesg_delta before after
MSGS=$(dmesg_count "$PATTERN")
WRAPPED_A=$DMESG_WRAPPED
grep -E "$PATTERN" "$OUTDIR/dmesg-new.txt" 2>/dev/null | sed 's/^/    /'

# --- control: no dirty logging, must print nothing ---------------------------

echo "--- control: nodirty ---"
dmesg_snap before2
run_bounded "$TIMEOUT" "$OUTDIR/004-nodirty.log" "$BIN" nodirty
NODIRTY_RC=$RC
sleep "$SETTLE"
dmesg_snap after2
dmesg_delta before2 after2
CTRL=$(dmesg_count "$PATTERN")
WRAPPED_B=$DMESG_WRAPPED

printf 'donation messages: nohuge arm=%s (rc=%s)  nodirty control=%s (rc=%s)  wrapped=%s/%s\n' \
	"$MSGS" "$NOHUGE_RC" "$CTRL" "$NODIRTY_RC" "$WRAPPED_A" "$WRAPPED_B"

if [ "$NOHUGE_RC" != 0 ]; then
	verdict INCONCLUSIVE "the nohuge arm exited rc=$NOHUGE_RC (3 = its own alarm, i.e. it hung) -- the VM never completed a clean dirty-logging lifecycle"
fi
if [ "$WRAPPED_A" = 1 ]; then
	verdict INCONCLUSIVE "the dmesg ring buffer wrapped or was cleared during the nohuge arm -- the delta is not trustworthy"
fi
if [ "$MSGS" != 0 ]; then
	W="$MSGS donation-accounting message(s) at VM teardown after dirty logging"
	[ "$CTRL" != 0 ] && W="$W -- but the nodirty control printed $CTRL too, so the trigger is wider than this issue records"
	verdict REPRODUCED "$W"
fi
verdict NOT-REPRODUCED "no donation-accounting message in any wording after a dirty-logging VM lifecycle: the residual is zero"
