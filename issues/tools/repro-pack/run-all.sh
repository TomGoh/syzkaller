#!/bin/bash
# Run every reproducer in the pKVM issue tracker and print a summary table.
#
# Order is deliberate:
#
#   002 003 004 005   harmless, and none of them perturbs the next
#   006               harmless to this boot, but it leaks EL2 pins PERMANENTLY
#                     and can make later KVM_CREATE_VCPU fail, so it goes after
#                     everything that needs a working vCPU
#   001               LAST, opt-in only: it wedges the machine
#
# Usage:
#   ./run-all.sh                        002-006
#   ./run-all.sh --include-hazardous    ... then 001, which can wedge the board
#   ./run-all.sh --build-only           cross-build everything, run nothing
#   ./run-all.sh --yes                  skip the countdown before 001
#
# Exit status: 0 if the table was printed. Read the table, not the status.

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SAFE_ISSUES="002 003 004 005 006"
HAZARDOUS_ISSUES="001"

INCLUDE_HAZ=0
ASSUME_YES=0
export BUILD_ONLY=${BUILD_ONLY:-0}

for arg in "$@"; do
	case $arg in
	--include-hazardous) INCLUDE_HAZ=1 ;;
	--build-only)        BUILD_ONLY=1 ;;
	--yes | -y)          ASSUME_YES=1 ;;
	-h | --help)         sed -n '2,20p' "$0"; exit 0 ;;
	*)                   echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

if [ -z "${OUTDIR:-}" ]; then
	OUTDIR=$(mktemp -d "${TMPDIR:-/tmp}/pkvm-repro-pack.XXXXXX") || exit 2
fi
mkdir -p "$OUTDIR" || exit 2
export OUTDIR
echo "artifacts and binaries: $OUTDIR"
echo "host: $(uname -srm)"
[ -r /proc/cmdline ] && echo "cmdline: $(cat /proc/cmdline)"
echo

RESULTS=""

run_one() {	# run_one <issue-id>
	local id=$1 script="$HERE/run-$1.sh" log="$OUTDIR/run-$1.out" rc=0 line=""

	echo "########################################################################"
	if [ ! -x "$script" ]; then
		RESULTS="$RESULTS$id|INCONCLUSIVE|-|missing or non-executable $script"$'\n'
		echo "RESULT $id: INCONCLUSIVE  (missing or non-executable $script)"
		return
	fi
	"$script" 2>&1 | tee "$log"
	rc=${PIPESTATUS[0]}

	# The RESULT line is the contract. A script that died without printing one
	# is INCONCLUSIVE by definition -- silence never reads as a pass.
	line=$(grep -m1 "^RESULT $id: " "$log" 2>/dev/null)
	if [ -z "$line" ]; then
		RESULTS="$RESULTS$id|INCONCLUSIVE|$rc|no RESULT line: the script died before reaching a verdict"$'\n'
		return
	fi
	local state why
	state=$(printf '%s' "$line" | sed -n 's/^RESULT [0-9]*: \([A-Z-]*\).*/\1/p')
	why=$(printf '%s' "$line" | sed -n 's/^RESULT [0-9]*: [A-Z-]*  (\(.*\))$/\1/p')
	RESULTS="$RESULTS$id|$state|$rc|$why"$'\n'
	echo
}

for id in $SAFE_ISSUES; do
	run_one "$id"
done

if [ "$INCLUDE_HAZ" = 1 ]; then
	for id in $HAZARDOUS_ISSUES; do
		echo
		echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
		echo "!!  ISSUE $id HAS hazard: wedges-target."
		echo "!!"
		echo "!!  On an affected kernel this WEDGES THE MACHINE. The blocked task is"
		echo "!!  in D state and unkillable; it holds mmap_lock, and rwsem fairness"
		echo "!!  then queues every later reader of that mm behind a writer that can"
		echo "!!  never be granted -- so ps, pgrep and ordinary system daemons freeze"
		echo "!!  one after another. Recovery needs sysrq over a still-live ssh, or"
		echo "!!  PHYSICAL ACCESS to the board."
		echo "!!"
		echo "!!  Do NOT run this on a machine you cannot power-cycle, and do not run"
		echo "!!  anything else from this pack afterwards until it has rebooted."
		echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
		if [ "$ASSUME_YES" = 0 ] && [ "$BUILD_ONLY" = 0 ]; then
			echo -n "starting in "
			for s in 10 9 8 7 6 5 4 3 2 1; do
				echo -n "$s "
				sleep 1
			done
			echo
		fi
		run_one "$id"
	done
else
	RESULTS="$RESULTS""001|SKIPPED|-|hazard: wedges-target; pass --include-hazardous to run it"$'\n'
fi

# ------------------------------------------------------------- summary -------

echo
echo "========================================================================"
echo "SUMMARY   (exit codes: 10=REPRODUCED 11=NOT-REPRODUCED 12=INCONCLUSIVE)"
echo "========================================================================"
printf '%-6s %-16s %-4s %s\n' ISSUE RESULT RC WHY
printf '%-6s %-16s %-4s %s\n' ------ ---------------- ---- ---
printf '%s' "$RESULTS" | sort | while IFS='|' read -r id state rc why; do
	[ -n "$id" ] || continue
	printf '%-6s %-16s %-4s %s\n' "$id" "$state" "$rc" "$why"
done
echo
echo "Full per-issue output: $OUTDIR/run-*.out"
echo "INCONCLUSIVE is not a pass. A timeout, a build failure or a missing"
echo "precondition all land there deliberately -- see README.md."
