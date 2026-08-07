#!/bin/bash
# Issue 005 -- a successful dirty-logging hypercall wipes the page's EL2
#              shared-state, so every later write-protect fails, dirty writes
#              are silently lost, and the teardown unmap WARNs.
#              signature: 'WARNING in __unmap_stage2_range'
#
# hazard: none. No hang, no Oops; the process exits normally.
#
# This issue has two observable halves and the pack checks both:
#
#   (1) THE SILENT ONE, and the one 005's ISSUE.md names as the regression test:
#       probe-dirtylog-twice writes page A under dirty logging in round 2, has
#       it reported and re-write-protected, then writes it again in round 3.
#       A=0 in round 3 means the write was LOST -- the re-write-protect failed
#       with -EPERM, the error was discarded (kvm_stage2_wp_range() is void),
#       the page stayed writable, and nothing recorded the write. This half has
#       no kernel message at all.
#
#       Per ISSUE.md: the criterion is A, and B must NOT be part of it -- the
#       control page is tracked only 3/5 today for an unrelated, unexplained
#       reason, so requiring B=1 produces false failures.
#
#   (2) the teardown WARN in __unmap_stage2_range, emitted from exit_mmap after
#       the process is gone -- hence the settle before reading dmesg. The line
#       number moves between builds (:456 on #4, :461 on #13), so the match is
#       on the function name, not the line.
#
#   Either half firing is a reproduction. 'nohuge' is the mode to use: under THP
#   issue 003 aborts the dirty-log hypercall before EL2 mutates anything, and
#   005 is masked.
#
# Careful: 001's hung task also names __unmap_stage2_range. The grep here
# requires WARNING and __unmap_stage2_range on the SAME line, which is the
# WARNING header format and not the hung-task format.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 005 "the guest unmap fails at teardown, but only after dirty logging worked"

TIMEOUT=${TIMEOUT_005:-120}
SETTLE=${SETTLE:-5}
SRC=$ISSUES_DIR/005-unmap-guest-fails-after-dirty-log/repro/probe-dirtylog-twice.c

build_repro "$SRC" probe-dirtylog-twice
BIN=$REPRO_BIN
build_only_stop

need_native
need_kvm
need_pkvm
warn_if_campaign

HAVE_DMESG=1
read_dmesg >/dev/null 2>&1 || HAVE_DMESG=0
[ "$HAVE_DMESG" = 0 ] && note "dmesg is unreadable; half (2) of this check is unavailable and the verdict rests on the dirty-bitmap half alone"

[ "$HAVE_DMESG" = 1 ] && dmesg_snap before
run_bounded "$TIMEOUT" "$OUTDIR/005-twice.log" "$BIN" nohuge
REPRO_RC=$RC
sleep "$SETTLE"

WARNS=0
if [ "$HAVE_DMESG" = 1 ]; then
	dmesg_snap after
	dmesg_delta before after
	WARNS=$(dmesg_count 'WARNING.*__unmap_stage2_range')
	grep -E 'WARNING.*__unmap_stage2_range' "$OUTDIR/dmesg-new.txt" 2>/dev/null | sed 's/^/    /'
fi

ROUND3=$(grep '^after round3:' "$OUTDIR/005-twice.log" 2>/dev/null | head -1)
A=$(printf '%s' "$ROUND3" | sed -n 's/.*A(page [0-9]*)=\([01]\).*/\1/p')
B=$(printf '%s' "$ROUND3" | sed -n 's/.*B(page [0-9]*)=\([01]\).*/\1/p')

printf 'round3 bitmap: A=%s B=%s   teardown WARNs in __unmap_stage2_range: %s   wrapped=%s\n' \
	"${A:-?}" "${B:-?}" "$WARNS" "$DMESG_WRAPPED"

if [ "$REPRO_RC" = 3 ] || [ "$REPRO_RC" = 124 ]; then
	verdict INCONCLUSIVE "reproducer hung (rc=$REPRO_RC, its own alarm(30)) -- three earlier fix attempts for this issue livelocked the guest exactly like this"
fi
if grep -q 'this is issue 003, not 005' "$OUTDIR/005-twice.log" 2>/dev/null; then
	verdict INCONCLUSIVE "round 2 failed with -E2BIG: issue 003 fired even in nohuge mode, so 005 is masked and cannot be observed"
fi
if [ -z "$A" ]; then
	verdict INCONCLUSIVE "reproducer exited rc=$REPRO_RC without printing an 'after round3:' line -- see $OUTDIR/005-twice.log"
fi

if [ "$A" = 0 ]; then
	W="page A's round-3 write under dirty logging is absent from the bitmap (A=0, B=${B:-?})"
	[ "$WARNS" != 0 ] && W="$W, and $WARNS teardown WARN(s) in __unmap_stage2_range"
	verdict REPRODUCED "$W"
fi
if [ "$WARNS" != 0 ]; then
	verdict REPRODUCED "$WARNS teardown WARN(s) in __unmap_stage2_range, although the dirty bitmap half passed (A=1)"
fi
if [ "$HAVE_DMESG" = 1 ] && [ "$DMESG_WRAPPED" = 1 ]; then
	verdict INCONCLUSIVE "A=1 (the bitmap half passed) but the dmesg ring buffer wrapped, so the teardown-WARN half could not be checked"
fi
verdict NOT-REPRODUCED "A=1: the re-write-protected page's next write was recorded, and no teardown WARN appeared"
