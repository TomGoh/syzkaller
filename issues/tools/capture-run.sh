#!/bin/bash
# Snapshot the observability conditions of a fuzzing run.
#
# A run record exists so that "we did not see it" means something. The fields
# this script fills are the mechanical ones -- the ones a human reliably forgets
# or mis-remembers, above all WHICH FILTERS WERE IN FORCE. The reporter's ignore
# list is compiled into syz-manager, so a config saying `"ignores": []` does not
# mean nothing was filtered; we recover the real list from the revision the
# binary embeds.
#
# Usage: capture-run.sh <run-id> <board-ip> <config.cfg> [workdir]
#
# Writes issues/runs/<run-id>.md (skeleton, with judgement fields marked TODO)
# and issues/runs/<run-id>/ (config copy, ring stats, final manager stats).

set -euo pipefail

if [ $# -lt 3 ]; then
	sed -n '2,17p' "$0" >&2
	exit 2
fi

RUN_ID=$1
BOARD=$2
CONFIG=$3
WORKDIR=${4:-$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["workdir"])' "$3")}

REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO/issues/runs/$RUN_ID
MANAGER=$REPO/bin/syz-manager

mkdir -p "$OUT"
cp "$CONFIG" "$OUT/$(basename "$CONFIG")"

ssh_board() { timeout 30 ssh -o BatchMode=yes -o ConnectTimeout=8 "root@$BOARD" "$@"; }

echo "== board ==" >&2
KERNEL=$(ssh_board 'uname -srvm' 2>/dev/null || echo "UNREACHABLE")
CMDLINE=$(ssh_board 'cat /proc/cmdline' 2>/dev/null || echo "UNREACHABLE")
ssh_board 'cat /sys/kernel/debug/kvm/pkvm_cov/stats' > "$OUT/pkvm_cov-stats.txt" 2>/dev/null \
	|| echo "UNREACHABLE" > "$OUT/pkvm_cov-stats.txt"

RING=$(grep -E '^(LOST_IN_RING|LOST_IN_KCOV_AREA|skip_not_owner)' "$OUT/pkvm_cov-stats.txt" \
	| awk '{printf "%s=%s ", $1, $2}' || true)

echo "== manager ==" >&2
# syzkaller embeds its build revision in the binary; this is the only reliable
# way to know which reporter ignores the RUNNING manager had compiled in, since
# bin/syz-manager is often older than the working tree.
MGR_REV=$(strings "$MANAGER" 2>/dev/null | grep -oE '^[0-9a-f]{40}$' | head -1 || true)
MGR_REV=${MGR_REV:-unknown}

REPORTER_IGNORES="(could not derive: manager revision unknown)"
if [ "$MGR_REV" != unknown ] && git -C "$REPO" cat-file -e "$MGR_REV^{commit}" 2>/dev/null; then
	REPORTER_IGNORES=$(git -C "$REPO" show "$MGR_REV:pkg/report/linux.go" \
		| awk '/ctx\.ignores = append/{f=1} f && /regexp\.MustCompile/{print; f=0}' \
		| sed -E 's/.*MustCompile\(`?([^`)]*)`?\).*/    - '"'"'\1'"'"'/')
fi

CONFIG_IGNORES=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
for pat in d.get("ignores", []):
    print("    - %r" % pat)
' "$CONFIG")
[ -n "$CONFIG_IGNORES" ] || CONFIG_IGNORES="    []"

NSYSCALLS=$(python3 -c '
import json, sys
print(len(json.load(open(sys.argv[1])).get("enable_syscalls", [])))
' "$CONFIG")

echo "== workdir ==" >&2
if [ -f "$WORKDIR/manager.log" ]; then
	grep -E 'exec total=' "$WORKDIR/manager.log" | tail -1 > "$OUT/final-stats.txt" || true
	gzip -c "$WORKDIR/manager.log" > "$OUT/manager.log.gz"
fi
FINAL=$(cat "$OUT/final-stats.txt" 2>/dev/null || echo "unknown")

cat > "$REPO/issues/runs/$RUN_ID.md" <<EOF
---
id: $RUN_ID
board: $BOARD
kernel: $KERNEL
cmdline: $CMDLINE
manager_rev: $MGR_REV
config: $RUN_ID/$(basename "$CONFIG")
filters:
  config_ignores:
$CONFIG_IGNORES
  reporter_ignores:
$REPORTER_IGNORES
enabled_syscalls: $NSYSCALLS
duration: TODO
result: $FINAL
ring: $RING
# TODO observed: issue ids this run observed.
observed: []
# TODO not_observed: issue ids this run COULD have observed and did not. A
# signature matched by any filter above, or needing a disabled syscall, does
# not belong here -- that stays 'unknown'.
not_observed: []
---

# Run $RUN_ID

TODO — purpose of this run, and anything that limited what it could observe
(disabled syscalls, filters, early termination).

Artifacts: \`$RUN_ID/\`.
EOF

echo "wrote issues/runs/$RUN_ID.md — fill the TODO fields before committing" >&2
