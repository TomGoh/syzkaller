#!/bin/sh
# a2 EL2-coverage capture — RUNS ON N90 (the DUT).
#
# Arms the pKVM EL2 coverage ring on the owner CPU, runs a syz program N times
# under strict serialization (procs=1, threaded=0, executor pinned to the owner
# CPU), and for each run saves the EL2 PC set + pKVM_cov stats + a dmesg-warning
# count. Set algebra + symbolization happen on the HOST (a2-analyze.sh), which
# has vmlinux; this side only needs a POSIX awk.
#
# EL2 PCs are the coverfile PCs that fall in the hyp .text link range
# [lo,hi) (derived on the host from vmlinux __hyp_text_start/__hyp_text_end and
# passed in). Filtering is a fixed-width lowercase-hex STRING compare — 64-bit
# addresses exceed 2^53 so a float/strtonum compare would be WRONG.
#
# Usage: a2-capture.sh <label> <N> <owner_cpu> <progfile> <lo16hex> <hi16hex> [outdir]
#   e.g. a2-capture.sh baseline 5 0 /root/syzkaller-fuzz/genprog.txt \
#          ffff800081d86ed4 ffff800081de4000
set -u
LABEL="${1:?label}"; N="${2:?N}"; CPU="${3:?owner_cpu}"; PROG="${4:?progfile}"
LO="${5:?lo16hex}"; HI="${6:?hi16hex}"
DIR=/root/syzkaller-fuzz
OUT="${7:-$DIR/a2-out/$LABEL}"
COV=/sys/kernel/debug/kvm/pkvm_cov
cd "$DIR" || exit 1
[ -e "$COV/enable" ] || { echo "FATAL: pkvm_cov absent (wrong kernel?)"; exit 1; }
[ -f "$PROG" ]       || { echo "FATAL: program $PROG missing"; exit 1; }
mkdir -p "$OUT"; : > "$OUT/$LABEL.stats"
echo "== a2 capture: label=$LABEL N=$N cpu=$CPU prog=$PROG el2=[0x$LO,0x$HI) ==" | tee "$OUT/$LABEL.log"

for i in $(seq 1 "$N"); do
  dmesg -C 2>/dev/null
  echo 1 > "$COV/stats_reset" 2>/dev/null
  # arm on the owner CPU: pkvm_cov_enable() records whatever CPU the write runs on
  taskset -c "$CPU" sh -c "echo 1 > $COV/enable" 2>>"$OUT/$LABEL.log"
  rm -f "$OUT/$LABEL.$i.raw"*
  # executor pinned to the same CPU so its KVM_RUN #23 faults land on the owner.
  # A2_EXEC_ENV lets the driver inject an executor env toggle (e.g. PKVM_FW_HP=1
  # for the a2 hugepage candidate) WITHOUT changing default behaviour.
  env ${A2_EXEC_ENV:-} taskset -c "$CPU" "$DIR/syz-execprog" -executor="$DIR/syz-executor" \
    -procs=1 -threaded=0 -cover=1 -slowdown=10 -repeat=1 \
    -coverfile="$OUT/$LABEL.$i.raw" "$PROG" >>"$OUT/$LABEL.log" 2>&1
  rc=$?
  echo 0 > "$COV/enable" 2>>"$OUT/$LABEL.log"

  # EL2 PC set for this run: coverfile PCs normalized to 16-hex lowercase, in [LO,HI)
  cat "$OUT/$LABEL.$i.raw"_prog* 2>/dev/null | grep -oiE '0x[0-9a-f]+' | awk -v lo="$LO" -v hi="$HI" '
    { s=tolower($1); sub(/^0x/,"",s); while (length(s)<16) s="0" s;
      if (s>=lo && s<hi) print "0x" s }' | sort -u > "$OUT/$LABEL.$i.el2"

  # smoke-gate inputs from pKVM_cov stats + dmesg
  st="$COV/stats"
  D=$(awk '/^drains/{print $2}' "$st" 2>/dev/null)
  OWN=$(awk '/^owner_cpu/{print $2}' "$st" 2>/dev/null)
  RING=$(awk '/^LOST_IN_RING/{print $2}' "$st" 2>/dev/null)
  KCOV=$(awk '/LOST_IN_KCOV_AREA/{print $2}' "$st" 2>/dev/null)
  SKIP=$(awk '/^skip_not_owner/{print $2}' "$st" 2>/dev/null)
  WARN=$(dmesg 2>/dev/null | grep -icE 'WARNING:|BUG:|Call trace:|Kernel panic')
  echo "run=$i rc=$rc el2_pcs=$(wc -l < "$OUT/$LABEL.$i.el2") drains=$D owner_cpu=$OWN LOST_IN_RING=$RING LOST_IN_KCOV=$KCOV skip_not_owner=$SKIP dmesg_warn=$WARN" \
    | tee -a "$OUT/$LABEL.stats"
  [ "${WARN:-0}" -gt 0 ] && dmesg 2>/dev/null | grep -iE 'WARNING:|BUG:|Call trace:' | head -5 | sed 's/^/    dmesg: /' | tee -a "$OUT/$LABEL.stats"
done
echo "== done: $OUT/$LABEL.{1..$N}.el2 + $LABEL.stats ; pull to host + a2-analyze.sh crunch ==" | tee -a "$OUT/$LABEL.log"
