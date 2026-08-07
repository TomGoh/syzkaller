#!/bin/bash
# Issue 003 -- __pkvm_host_dirty_log_guest() assumes a 4 KiB mapping, so KVM_RUN
#              fails with -E2BIG on any huge-page-backed guest once dirty
#              logging is enabled.
#              signature: 'KVM_RUN returns -E2BIG after KVM_MEM_LOG_DIRTY_PAGES'
#
# hazard: none. Only the guest dies; the board is unaffected.
#
# Verdict source: the reproducer's own stdout. No dmesg needed -- the signature
# is a return value, "second KVM_RUN ret=-1 errno=7". errno 7 is E2BIG.
#
# The three modes are the documented matrix (003's evidence/2026-08-05-mode-
# matrix.txt): default = THP + logging (the failing cell), nohuge = 4 KiB +
# logging, nodirty = THP without logging. Both control cells are hard zeros, so
# a control that fails means the run is not measuring what it thinks it is.
#
# It is FREQUENT BUT NOT DETERMINISTIC on a kernel that still has issue 002:
# 22 of 25 on build #4, because reaching the broken path needs the guest's write
# to actually fault, and the write-protect's TLB invalidation is refused. So the
# default mode is repeated REPS times and the count is reported.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 003 "dirty logging is unusable for a huge-page-backed guest (-E2BIG)"

TIMEOUT=${TIMEOUT_003:-60}
REPS=${REPS:-5}
SRC=$ISSUES_DIR/003-dirty-log-guest-no-huge-page-support/repro/probe-dirtylog-thp.c

build_repro "$SRC" probe-dirtylog-thp
BIN=$REPRO_BIN
build_only_stop

need_native
need_kvm
need_pkvm
need_thp_always
warn_if_campaign

E2BIG=0
HUNG=0
REACHED=0
for i in $(seq 1 "$REPS"); do
	run_bounded "$TIMEOUT" "$OUTDIR/003-default-$i.log" "$BIN"
	case $RC in
	3 | 124) HUNG=$((HUNG + 1)) ;;
	esac
	if grep -q '^second KVM_RUN ret=' "$OUTDIR/003-default-$i.log"; then
		REACHED=$((REACHED + 1))
	fi
	if grep -qE '^second KVM_RUN ret=-1 errno=7( |$)' "$OUTDIR/003-default-$i.log"; then
		E2BIG=$((E2BIG + 1))
	fi
done

echo "--- controls ---"
run_bounded "$TIMEOUT" "$OUTDIR/003-nohuge.log" "$BIN" nohuge
NOHUGE_RC=$RC
run_bounded "$TIMEOUT" "$OUTDIR/003-nodirty.log" "$BIN" nodirty
NODIRTY_RC=$RC

printf 'default THP mode: %d/%d runs returned -E2BIG from the second KVM_RUN (%d hung)\n' \
	"$E2BIG" "$REPS" "$HUNG"
printf 'controls: nohuge rc=%s  nodirty rc=%s  (both must be 0)\n' "$NOHUGE_RC" "$NODIRTY_RC"

if [ "$HUNG" != 0 ]; then
	verdict INCONCLUSIVE "$HUNG of $REPS default-mode runs hung (rc=3 is the reproducer's alarm(20)) -- a livelocking guest, not a clean measurement"
fi
if [ "$REACHED" = 0 ]; then
	verdict INCONCLUSIVE "no default-mode run reached its second KVM_RUN -- setup failed before the trigger; see $OUTDIR/003-default-1.log"
fi
if [ "$E2BIG" != 0 ]; then
	verdict REPRODUCED "$E2BIG/$REPS default-THP runs: second KVM_RUN returned -1/errno=7 (E2BIG) after KVM_MEM_LOG_DIRTY_PAGES"
fi
if [ "$NOHUGE_RC" != 0 ] || [ "$NODIRTY_RC" != 0 ]; then
	verdict INCONCLUSIVE "no -E2BIG in $REPS runs, but a control also failed (nohuge rc=$NOHUGE_RC, nodirty rc=$NODIRTY_RC) -- the run is not measuring the huge-page variable"
fi
verdict NOT-REPRODUCED "0/$REPS default-THP runs returned -E2BIG, and both controls passed (on build #4 this was 22/25)"
