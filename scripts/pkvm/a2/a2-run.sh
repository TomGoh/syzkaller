#!/bin/bash
# a2 campaign driver — RUNS ON THE HOST. Orchestrates one capture+analyze cycle
# against N90: derive the EL2 range from vmlinux, ship the capture script + the
# program, run N serialized repeats on the owner CPU, pull the EL2 sets back,
# and crunch the set algebra.
#
# PRECONDITION (plan §Immediate.6): N90 must be freshly rebooted onto 6.6.30+
# (pkvm_cov) so VMID generation is clean; the whole a2 batch must stay well under
# the ~13k-exec VMID-rollover budget. This driver does NOT reboot — do that first.
#
# Usage:
#   a2-run.sh <label> <N> <local_progfile> [baseline_union_file]
#     baseline:  a2-run.sh baseline 5 ./baseline.prog
#     candidate: a2-run.sh cand-hugepage 5 ./cand-hugepage.prog ./out/baseline.union
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
LABEL="${1:?label}"; N="${2:?N}"; PROG="${3:?local_progfile}"; BASE="${4:-}"
CPU="${OWNER_CPU:-0}"
N90="${N90:-root@10.42.27.17}"
VM="${VMLINUX:-/home/jose/common-stage2mvp/vmlinux}"
NM="${NM:-aarch64-linux-gnu-nm}"
SSH="ssh -o BatchMode=yes -o LogLevel=ERROR $N90"
OUT="$HERE/out"; mkdir -p "$OUT"
[ -f "$PROG" ] || { echo "FATAL: local program $PROG missing"; exit 1; }

echo "== derive EL2 hyp .text range from vmlinux =="
LO=$("$NM" "$VM" | awk '/ __hyp_text_start$/{print $1}')
HI=$("$NM" "$VM" | awk '/ __hyp_text_end$/{print $1}')
[ -n "$LO" ] && [ -n "$HI" ] || { echo "FATAL: could not read __hyp_text_start/end"; exit 1; }
echo "   EL2 = [0x$LO, 0x$HI)"

echo "== ship capture script + program to N90 =="
$SSH 'mkdir -p /root/syzkaller-fuzz/a2'
scp -q -o LogLevel=ERROR "$HERE/a2-capture.sh" "$N90:/root/syzkaller-fuzz/a2/a2-capture.sh"
scp -q -o LogLevel=ERROR "$PROG" "$N90:/root/syzkaller-fuzz/a2/$LABEL.prog"

echo "== run capture: $N serialized repeats on CPU $CPU  (exec-env: '${A2_EXEC_ENV:-<none>}') =="
$SSH "A2_EXEC_ENV='${A2_EXEC_ENV:-}' sh /root/syzkaller-fuzz/a2/a2-capture.sh $LABEL $N $CPU /root/syzkaller-fuzz/a2/$LABEL.prog $LO $HI" 2>&1 \
  | grep -v 'post-quantum\|store now\|openssh\|upgraded\|^[[:space:]]*\*\*\|Kylin'

echo "== pull EL2 sets + stats =="
bash "$HERE/a2-analyze.sh" pull "/root/syzkaller-fuzz/a2-out/$LABEL" "$OUT"

echo "== per-run smoke stats (gate: rc=0, LOST_IN_RING=0, LOST_IN_KCOV=0, skip_not_owner=0, dmesg_warn=0) =="
sed 's/^/   /' "$OUT/$LABEL.stats" 2>/dev/null || echo "   (no stats pulled)"

echo "== set algebra =="
if [ -n "$BASE" ]; then bash "$HERE/a2-analyze.sh" crunch "$LABEL" "$N" "$OUT" "$BASE";
else                    bash "$HERE/a2-analyze.sh" crunch "$LABEL" "$N" "$OUT"; fi
echo "== artifacts in $OUT/ ($LABEL.union is the baseline diff-reference for candidates) =="
