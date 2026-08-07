#!/bin/bash
# Where does EL2 coverage actually get lost?  Adapted from
# ../../step1-measurement-2026-07-24/scripts/run-1a.sh -- same discipline
# (console shield, arm/execprog affinity coupling, address buckets, cleanup
# trap), different factors.
#
# The 07-24 harness held the owner CPU fixed at 0 and varied bridge/console/
# kprobe.  That is exactly why it never saw the gate this run is about: with
# everything pinned to CPU 0, `skip_not_owner` is 0 by construction.  The
# production campaign does NOT pin, and its stats show 97.2% of HVC windows
# dropped by that gate.  So here the CPU is the factor and console is held at
# 'quiet' (07-24 already established it as first-order: one printk inside a
# measured call costs ~100k KCOV entries on a 115200 console).
#
# ARMS
#   pin<N>    arm the ring on CPU N and pin syz-execprog to CPU N
#   nopin     arm the ring on CPU 0, let syz-execprog float (campaign conditions)
#
# Usage:  run-pin.sh <outdir> <arm> [<arm> ...]      e.g. run-pin.sh /tmp/o pin0 nopin pin0 nopin
#
# Reads PROG (program file) and EXECUTOR/EXECPROG from the environment or the
# defaults below.

set -u

OUT="${1:?usage: run-pin.sh <outdir> <arm>...}"; shift
[ $# -gt 0 ] || { echo "no arms given" >&2; exit 2; }
mkdir -p "$OUT"

PROGS="${PROGS:-/tmp/w1-1-dirtylog-nohuge.prog /tmp/w1-2-dirtylog-thp.prog /tmp/w1-3-syzos-guest.prog /tmp/w1-4-protected-reject.prog /tmp/w1-5-teardown.prog}"
EXECUTOR="${EXECUTOR:-/tmp/syz-executor}"
EXECPROG="${EXECPROG:-/tmp/syz-execprog}"
NR_PAGES="${NR_PAGES:-32}"

COV=/sys/kernel/debug/kvm/pkvm_cov
ENABLE=$COV/enable
PRINTK=/proc/sys/kernel/printk
SHADOW=/tmp/pkvm-pin-printk-shadow

# Address buckets for kernel #18 @684f834c7ca5, BuildID 0886c85b017d48d3...
# THESE MOVE ON EVERY RELINK.  Re-derive with nm before reusing this script:
#   nm -n vmlinux | grep -E ' (__hyp_text_start|__hyp_text_end)$'
#   nm -n vmlinux | grep -E ' pkvm_cov_runtime_to_link$| pkvm_cov_end$'
#   nm -n vmlinux | grep -E ' [tT] pl011_' | awk '$1 < "ffff800081000000"' | sed -n '1p;$p'
HYP_START=0xffff800081439f00
HYP_END=0xffff80008148b000
BR_START=0xffff8000800ad750
BR_END=0xffff8000800ad8b0
UART_START=0xffff800080c43e80
UART_END=0xffff800080c4ab28

# ---- console shield -------------------------------------------------------
# syz-executor unconditionally writes "7 4 1 3" to /proc/sys/kernel/printk
# during sandbox setup, so console_loglevel does not survive into the run.
# Bind-mounting a plain file over it makes that write land somewhere harmless.
console_quiet_on() {
	echo "1 4 1 3" > "$SHADOW"
	mount --bind "$SHADOW" "$PRINTK" 2>/dev/null
	echo 1 > /proc/sys/kernel/printk_devkmsg 2>/dev/null
	dmesg -n 1 2>/dev/null
}
console_quiet_off() {
	mountpoint -q "$PRINTK" && umount "$PRINTK" 2>/dev/null
	return 0
}

cleanup() { echo 0 > "$ENABLE" 2>/dev/null; console_quiet_off; }
trap cleanup EXIT INT TERM

# ---- helpers --------------------------------------------------------------
stat_of() { awk -v k="$1" '$1==k {print $2; found=1} END{if(!found) print "NA"}' "$2"; }

# coverfile -> "raw uniq hyp_raw hyp_uniq br_raw br_uniq uart_raw uart_uniq"
# -coverfile DISABLES dedup (tools/syz-execprog/execprog.go:138-140), so raw
# and uniq differ and only uniq means anything for reachability.
cover_metrics() {
	[ -s "$1" ] || { echo "0 0 0 0 0 0 0 0"; return; }
	awk -v hs="$HYP_START" -v he="$HYP_END" -v bs="$BR_START" -v be="$BR_END" \
	    -v us="$UART_START" -v ue="$UART_END" '
		{ n++; u[$1]=1
		  if ($1 >= hs && $1 < he) { h++; hu[$1]=1 }
		  if ($1 >= bs && $1 < be) { b++; bu[$1]=1 }
		  if ($1 >= us && $1 < ue) { a++; au[$1]=1 } }
		END { print n+0, length(u), h+0, length(hu), b+0, length(bu), a+0, length(au) }
	' "$1"
}

run_one() {  # $1=tag  $2=arm (pinN | nopin)
	local tag="$1" arm="$2" cpu pinned pfx="/tmp/cov-$1"

	case "$arm" in
	pin*)  cpu="${arm#pin}"; pinned=1 ;;
	nopin) cpu=0;            pinned=0 ;;
	*)     echo "[$tag] unknown arm '$arm'" >&2; return 1 ;;
	esac

	rm -f "${pfx}"_prog*
	echo 0 > "$ENABLE" 2>/dev/null
	console_quiet_off

	# nr_pages is only writable while no ring is live (pkvm_cov.c:636-637)
	echo "$NR_PAGES" > "$COV/nr_pages" 2>/dev/null

	console_quiet_on

	# The setup hypercall records owner_cpu = the CPU of THIS writer
	# (pkvm_cov.c:283-290); there is no way to name a target CPU, so pin the
	# writer and then VERIFY -- get_cpu() guarantees nothing.
	taskset -c "$cpu" sh -c "echo 1 > $ENABLE" || { echo "[$tag] ENABLE FAILED"; console_quiet_off; return 1; }
	local owner; owner=$(cat "$COV/owner_cpu")
	if [ "$owner" != "$cpu" ]; then
		echo "[$tag] owner_cpu=$owner, wanted $cpu -- discarding"
		echo 0 > "$ENABLE"; console_quiet_off; return 1
	fi

	echo 1 > "$COV/stats_reset"

	if [ "$pinned" = 1 ]; then
		taskset -c "$cpu" "$EXECPROG" -executor="$EXECUTOR" -procs=1 -threaded=0 \
			-cover=1 -slowdown=10 -coverfile="$pfx" $PROGS > "$OUT/execprog-$tag.log" 2>&1
	else
		"$EXECPROG" -executor="$EXECUTOR" -procs=1 -threaded=0 \
			-cover=1 -slowdown=10 -coverfile="$pfx" $PROGS > "$OUT/execprog-$tag.log" 2>&1
	fi
	local rc=$?

	cat "$COV/stats" > "$OUT/stats-$tag.txt"
	local shadowed; shadowed="$(tr -d '\n\t ' < "$SHADOW" 2>/dev/null)"
	echo 0 > "$ENABLE"
	console_quiet_off

	# Every coverfile for this run, concatenated: the per-call split does not
	# matter for a reachability question, and a call with zero PCs writes no
	# file at all (execprog.go:347-349).
	cat "${pfx}"_prog* > "$OUT/cover-$tag.txt" 2>/dev/null
	local ncalls; ncalls=$(ls "${pfx}"_prog* 2>/dev/null | wc -l)

	local m; m=$(cover_metrics "$OUT/cover-$tag.txt")
	local leaked; leaked=$(stat_of leaked_bytes "$OUT/stats-$tag.txt")
	local begin;  begin=$(stat_of begin_calls "$OUT/stats-$tag.txt")
	local skipo;  skipo=$(stat_of skip_not_owner "$OUT/stats-$tag.txt")
	local skipk;  skipk=$(stat_of skip_no_kcov "$OUT/stats-$tag.txt")
	local drains; drains=$(stat_of drains "$OUT/stats-$tag.txt")
	local lring;  lring=$(stat_of LOST_IN_RING "$OUT/stats-$tag.txt")
	local lkcov;  lkcov=$(stat_of LOST_IN_KCOV_AREA "$OUT/stats-$tag.txt")

	# Self-consistency: drains == begin_calls - skip_not_owner - skip_no_kcov.
	# A mismatch means the snapshot was polluted by concurrent HVCs from
	# another task; the row is not usable.
	local ok=yes
	if [ "$begin" != NA ]; then
		[ "$drains" -eq $((begin - skipo - skipk)) ] || ok=NO
	fi

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$tag" "$arm" "$owner" "$rc" "$ncalls" "$m" "$begin" "$skipo" "$skipk" "$drains" "$lring" "$lkcov" "$ok" \
		| tr -s ' ' '\t' >> "$OUT/raw.tsv"

	echo "[$tag] arm=$arm owner=$owner rc=$rc files=$ncalls hyp_uniq=$(echo "$m"|cut -d' ' -f4) skip_not_owner=$skipo consistent=$ok leaked=$leaked printk_shadow='$shadowed'"
	[ "$leaked" = 0 ] || { echo "[$tag] !! leaked_bytes=$leaked -- STOPPING (pkvm_cov.c:350-355 leaks the block on purpose)"; return 2; }
	return 0
}

[ -f "$OUT/raw.tsv" ] || printf 'tag\tarm\towner\trc\tfiles\traw\tuniq\thyp_raw\thyp_uniq\tbr_raw\tbr_uniq\tuart_raw\tuart_uniq\tbegin_calls\tskip_not_owner\tskip_no_kcov\tdrains\tLOST_IN_RING\tLOST_IN_KCOV_AREA\tconsistent\n' > "$OUT/raw.tsv"

i=0
for arm in "$@"; do
	i=$((i+1))
	run_one "r$i-$arm" "$arm" || { echo "aborting at r$i-$arm"; exit 1; }
done

echo
echo "=== $OUT/raw.tsv ==="
column -t -s "$(printf '\t')" "$OUT/raw.tsv"
