#!/bin/bash
# Issue 001 -- pkvm_unmap_guest() takes mmap_lock for write while stage2_unmap_vm()
#              holds it for read.  signature: 'INFO: task hung in __unmap_stage2_range'
#
#   *** hazard: wedges-target ***
#
# THIS SCRIPT CAN PERMANENTLY WEDGE THE MACHINE IT RUNS ON. The blocked task is
# in D state and unkillable; it holds mmap_lock, and rwsem fairness then queues
# every later reader of that mm behind a writer that can never be granted, so
# anything walking /proc (ps, pgrep, a stock system daemon) freezes in turn.
# Recovery needs sysrq, or physical access. See ../../001-*/ISSUE.md.
#
# How the verdict is reached, and why it is not "did it time out":
#
#   The reproducer's last flushed line is "second KVM_ARM_VCPU_INIT -- hangs
#   here on an affected kernel...". If it never returns, we read the live task
#   state out of /proc -- state=D plus wchan=account_locked_vm is exactly the
#   evidence in 001's evidence/2026-08-05-task-stack.txt. A timeout WITHOUT that
#   corroboration is INCONCLUSIVE, not a reproduction.
#
#   We never read /proc/<pid>/cmdline or /maps for the wedged pid: 001's own
#   blast-radius table records that both block forever. /stat, /wchan and /stack
#   do not take mmap_lock and are safe.
#
# The default timeout is 180s so that the hung-task watchdog (120s) has fired
# and left its own signature in dmesg as a second, independent witness.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 001 "pkvm_unmap_guest() self-deadlock on mmap_lock  [HAZARD: wedges-target]"

TIMEOUT=${TIMEOUT_001:-180}
SRC=$ISSUES_DIR/001-pkvm-unmap-selfdeadlock/repro/repro-deadlock.c

build_repro "$SRC" repro-deadlock
BIN=$REPRO_BIN
build_only_stop

need_native
need_kvm
need_pkvm
warn_if_campaign

note "this issue additionally needs a CPU WITHOUT ARM64_HAS_STAGE2_FWB. There is no userspace way to read that bit, so it is NOT checked here. N90 and D3000 lack FWB; most upstream test hardware has it, and on such a CPU arm.c takes the icache_inval_all_pou() branch and this defect is unreachable -- which looks identical to a fixed kernel. See ../../REACHABILITY.md."

dmesg_snap before

LOG=$OUTDIR/001-repro.log
: > "$LOG"

echo "--- running repro-deadlock (up to ${TIMEOUT}s; it is EXPECTED to hang on a defective kernel) ---"
"$BIN" > "$LOG" 2>&1 &
PID=$!

HUNG=0
ELAPSED=0
while kill -0 "$PID" 2>/dev/null; do
	if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
		HUNG=1
		break
	fi
	sleep 5
	ELAPSED=$((ELAPSED + 5))
done

if [ "$HUNG" = 0 ]; then
	RC=0
	wait "$PID" || RC=$?
else
	RC=timeout
fi

sed 's/^/    /' "$LOG"
echo "--- repro-deadlock rc=$RC pid=$PID ---"

# --- the returned case -------------------------------------------------------

if [ "$HUNG" = 0 ]; then
	if [ "$RC" != 0 ]; then
		verdict INCONCLUSIVE \
			"reproducer exited rc=$RC before reaching the trigger (a setup ioctl failed) -- see $LOG"
	fi
	if ! grep -q '^PASS: second KVM_ARM_VCPU_INIT returned' "$LOG"; then
		verdict INCONCLUSIVE \
			"reproducer exited 0 but printed no PASS line -- it did not reach the trigger; see $LOG"
	fi
	verdict NOT-REPRODUCED \
		"second KVM_ARM_VCPU_INIT returned promptly, no deadlock. NB this is also what an FWB-capable CPU looks like -- see the NOTE above"
fi

# --- the hung case: corroborate before calling it a reproduction --------------

STATE=$(cut -d')' -f2- "/proc/$PID/stat" 2>/dev/null | awk '{print $1}')
WCHAN=$(cat "/proc/$PID/wchan" 2>/dev/null)
STACK=$(cat "/proc/$PID/stack" 2>/dev/null | head -8)

echo "--- live task state of pid $PID ---"
printf '    state=%s\n    wchan=%s\n' "${STATE:-<unreadable>}" "${WCHAN:-<unreadable>}"
[ -n "$STACK" ] && printf '%s\n' "$STACK" | sed 's/^/    /'

dmesg_snap after
dmesg_delta before after
HUNGTASK=$(dmesg_count 'INFO: task .* blocked for more than')
UNMAP=$(dmesg_count '__unmap_stage2_range')
printf '    dmesg (new): hung-task reports=%s  __unmap_stage2_range mentions=%s  wrapped=%s\n' \
	"$HUNGTASK" "$UNMAP" "$DMESG_WRAPPED"

if [ "$STATE" != D ]; then
	verdict INCONCLUSIVE \
		"reproducer did not return within ${TIMEOUT}s but its task state is '${STATE:-unreadable}', not D -- the hang is not the documented uninterruptible one"
fi
if ! grep -q 'second KVM_ARM_VCPU_INIT -- hangs here' "$LOG"; then
	verdict INCONCLUSIVE \
		"task is in D state but the log never reached the trigger line -- it is stuck somewhere else; see $LOG"
fi

WHY="pid $PID stuck in D state inside the second KVM_ARM_VCPU_INIT, wchan=${WCHAN:-unreadable}"
[ "$HUNGTASK" != 0 ] && WHY="$WHY, $HUNGTASK hung-task report(s) in dmesg"

echo
echo "########################################################################"
echo "# THE MACHINE IS NOW WEDGED. pid $PID is unkillable and holds mmap_lock."
echo "# Anything that walks /proc will freeze behind it. Reboot the board:"
echo "#   echo 1 > /proc/sys/kernel/sysrq ; echo b > /proc/sysrq-trigger"
echo "# Do not run any further script from this pack until it has rebooted."
echo "########################################################################"

verdict REPRODUCED "$WHY"
