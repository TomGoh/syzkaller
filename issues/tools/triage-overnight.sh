#!/bin/bash
# Morning triage for a campaign that ran unattended.
#
# Answers, in order, the only three questions that matter after an unattended
# run, and answers them in a way that cannot be confused with "everything is
# fine" when the truth is "the board died and stopped reporting":
#
#   1. Is the board still alive, and is the manager still driving it?
#   2. Did anything crash -- and if so, how many DISTINCT signatures?
#   3. Did coverage move, or did the run plateau?
#
# The distinction in (1) matters more than it looks. A wedged board produces a
# manager log that simply stops growing; a healthy plateaued run produces one
# that grows with unchanging numbers. Both look like "no news". This script
# separates them by checking the log's mtime against wall-clock, not just its
# contents. (Same failure shape as the `alarm()`+`_exit(3)` trap recorded in
# issues/README.md: a hang and a pass must never print the same thing.)
#
# Usage: triage-overnight.sh <workdir> <board-ip> [baseline-file]

set -uo pipefail

if [ $# -lt 2 ]; then
	sed -n '2,20p' "$0" >&2
	exit 2
fi

WORKDIR=$1
BOARD=$2
BASELINE=${3:-}
LOG=$WORKDIR/manager.log

ssh_board() { timeout 20 ssh -o BatchMode=yes -o ConnectTimeout=8 "root@$BOARD" "$@"; }

echo "=============== 1. LIVENESS ==============="
date

if pgrep -f 'syz-manager -config' >/dev/null; then
	echo "manager:   RUNNING (pid $(pgrep -f 'syz-manager -config' | head -1))"
else
	echo "manager:   *** NOT RUNNING *** -- check tail of $LOG for EXIT="
	tail -5 "$LOG"
fi

if [ -f "$LOG" ]; then
	AGE=$(( $(date +%s) - $(stat -c %Y "$LOG") ))
	printf 'log mtime: %ss ago' "$AGE"
	# syz-manager prints a stats line every 10s. Anything past ~120s means it
	# has stopped making progress, which a plateau alone does not cause.
	if [ "$AGE" -gt 120 ]; then echo "   *** STALE -- manager is not writing ***"; else echo "   (fresh)"; fi
fi

if ssh_board true 2>/dev/null; then
	echo "board ssh: OK"
	ssh_board 'uptime; echo "D-state tasks: $(ps -eo stat,pid,comm | awk "\$1 ~ /^D/" | wc -l)"; echo "executors:     $(pgrep -c syz-executor)"'
else
	echo "board ssh: *** UNREACHABLE *** -- N90 has no out-of-band channel; this needs"
	echo "           physical access. See task #5."
fi

echo
echo "=============== 2. CRASHES ==============="
CRASHDIR=$WORKDIR/crashes
if [ -d "$CRASHDIR" ] && [ -n "$(ls -A "$CRASHDIR" 2>/dev/null)" ]; then
	echo "distinct signatures: $(ls "$CRASHDIR" | wc -l)"
	echo
	for d in "$CRASHDIR"/*/; do
		[ -f "$d/description" ] || continue
		printf '  [%3d reports]  %s\n' "$(ls "$d" | grep -c '^log')" "$(cat "$d/description")"
	done
	echo
	echo "Reminder: every kernel WARN is a crash in this campaign -- no WARN ignores"
	echo "are compiled in. A high report count on ONE known signature (vmid.c"
	echo "rollover, arch_timer.c) is expected noise, not a new finding. What matters"
	echo "is a signature that is NOT one of those two."
else
	echo "no crashes recorded"
fi

echo
echo "=============== 3. COVERAGE ==============="
tail -1 "$LOG" 2>/dev/null
echo
echo "-- corpus/coverage trajectory (hourly) --"
grep -E 'exec total=' "$LOG" 2>/dev/null | awk 'NR==1 || NR%360==0 || 0' | tail -20

echo
echo "-- EL2 ring --"
ssh_board 'cat /sys/kernel/debug/kvm/pkvm_cov/stats' 2>/dev/null \
	| grep -E 'LOST_IN_RING|LOST_IN_KCOV_AREA|kcov_requested|kcov_accepted|link_dropped' \
	|| echo "(board unreachable)"

if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
	echo
	echo "-- baseline for comparison ($BASELINE) --"
	grep -E 'corpus=|LOST_IN_RING|kcov_accepted' "$BASELINE"
fi

echo
echo "Next: issues/tools/capture-run.sh to freeze the run record before changing"
echo "anything. 'not_observed' may only be filled once the run has ENDED."
