#!/bin/bash
# ============================================================================
# Step 1B — same 2x2x2 factorial, on the kernel that carries per-#23 loss
# accounting (kvm/pkvm_cov/stats) and no longer instruments its own drain loop.
# Adds: stats_reset before each run, stats snapshot after -> kstats.tsv.
# (Derived from the Step-1A harness; address buckets updated for the new link.)
#
# Question 1A must answer: WHERE does the task-KCOV area fill up, and WHO fills
# it -- the ordinary EL1 path, the EL2 bridge, or something else entirely?
#
# A first pass over the 2026-07-23 smoke coverfile showed the 524287-entry area
# is NOT dominated by EL2 PCs (1022 raw / 87 unique = 0.19%) nor by the bridge's
# own EL1 drain loop (7173 = 1.4%), but by the pl011 SERIAL CONSOLE driver
# busy-polling the UART TX FIFO (~424k = 81%). This board boots with
# console=ttyAMA0,115200 loglevel=8 -- a debug setting from the deploy session --
# so every printk issued inside the measured call costs ~100k KCOV entries.
# Measured directly: 4 kmsg lines take 55 ms with the console live vs 3 ms
# suppressed.
#
# Two more instruments turned out to be first-order confounds, so both become
# explicit factors rather than fixed background:
#
#   console : syz-executor UNCONDITIONALLY writes "7 4 1 3" to
#             /proc/sys/kernel/printk during sandbox setup
#             (executor/common_linux.h:5579), so simply lowering
#             console_loglevel does not survive into the run. The 'quiet' arm
#             therefore sets the real console_loglevel to 1 and then bind-mounts
#             a plain file over /proc/sys/kernel/printk, so the executor's write
#             lands in the shadow file and the kernel value stays 1. Verified:
#             after the run the shadow file reads "7 4 1 3" while the real value
#             is still "1 4 1 7" and kmsg timing stays at the suppressed 3 ms.
#
#   kprobe  : the pkvm_mem_abort kprobe+kretprobe used to observe #23 is itself
#             very expensive in KCOV terms (kretprobe trampoline + stack
#             unwinding). Arming it is what takes the OFF baseline from ~5.9k to
#             ~264k entries. It is a measurement instrument, not part of a
#             campaign, so it must be varied, not held on.
#
# Design: full 2x2x2 factorial  bridge(off|on) x console(loud|quiet) x kprobe(off|on),
# REPS replicates, arm order rotated per replicate so warm-up/drift cannot
# masquerade as an arm effect. Everything else fixed: same kernel build, same
# binaries, same test program, same CPU, same flags.
# ============================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
CPU="${CPU:-0}"
REPS="${REPS:-3}"
OUT="${OUT:-$DIR/out-1b}"
PROG="$DIR/arm64-syz_kvm_run_fw_fault"
EXECPROG="$DIR/syz-execprog"
EXECUTOR="${EXECUTOR:-$DIR/syz-executor}"
ENABLE=/sys/kernel/debug/kvm/pkvm_cov/enable
STATS=/sys/kernel/debug/kvm/pkvm_cov/stats
STATS_RESET=/sys/kernel/debug/kvm/pkvm_cov/stats_reset
OWNER=/sys/kernel/debug/kvm/pkvm_cov/owner_cpu
PRINTK=/proc/sys/kernel/printk
SHADOW=/tmp/pkvm-1a-printk-shadow

# Address buckets from System.map of THIS build. Every kernel PC prints as
# 0x + 16 lowercase hex digits, so fixed-width string comparison is exactly
# numeric comparison -- this sidesteps 64-bit signedness traps in awk/bash.
HYP_START=0xffff800081d86ed4     # __hyp_text_start
HYP_END=0xffff800081de6000       # __hyp_text_end
BR_START=0xffff8000800cbbc8      # pkvm_cov_runtime_to_link .. pkvm_cov_end
BR_END=0xffff8000800cc168
UART_START=0xffff800081104748    # amba-pl011 non-init text
UART_END=0xffff80008110d428

T=/sys/kernel/tracing
[ -d "$T" ] || T=/sys/kernel/debug/tracing

mkdir -p "$OUT"
cd "$DIR" || exit 1
PRINTK_ORIG="$(cat $PRINTK)"

console_quiet_on() {
	printf '1\t4\t1\t7\n' > "$SHADOW"
	echo "1 4 1 7" > "$PRINTK"
	mount --bind "$SHADOW" "$PRINTK"
}
console_quiet_off() {
	mountpoint -q "$PRINTK" && umount "$PRINTK"
	echo "$PRINTK_ORIG" > "$PRINTK"
}
kprobe_on() {
	echo > "$T/kprobe_events" 2>/dev/null
	echo 'p:fw_abort_in  pkvm_mem_abort ipa=+0(%x1):x64' >> "$T/kprobe_events" || return 1
	echo 'r:fw_abort_ret pkvm_mem_abort ret=$retval:s64'  >> "$T/kprobe_events" || return 1
	echo 1 > "$T/events/kprobes/fw_abort_in/enable"
	echo 1 > "$T/events/kprobes/fw_abort_ret/enable"
}
kprobe_off() {
	echo 0 > "$T/events/kprobes/fw_abort_in/enable"  2>/dev/null
	echo 0 > "$T/events/kprobes/fw_abort_ret/enable" 2>/dev/null
	echo > "$T/kprobe_events" 2>/dev/null
}
cleanup() { kprobe_off; console_quiet_off; echo 0 > "$ENABLE" 2>/dev/null; }
trap cleanup EXIT

# $1 = coverfile -> "total unique hyp_raw hyp_uniq br_raw br_uniq uart_raw uart_uniq bad"
cover_metrics() {
	[ -s "$1" ] || { echo "0 0 0 0 0 0 0 0 0"; return; }
	awk -v hs="$HYP_START" -v he="$HYP_END" -v bs="$BR_START" -v be="$BR_END" \
	    -v us="$UART_START" -v ue="$UART_END" '
		{ n++; u[$1]=1
		  if (length($1) != 18) bad++
		  if ($1 >= hs && $1 < he) { h++; hu[$1]=1 }
		  if ($1 >= bs && $1 < be) { b++; bu[$1]=1 }
		  if ($1 >= us && $1 < ue) { a++; au[$1]=1 } }
		END { print n+0, length(u), h+0, length(hu), b+0, length(bu), a+0, length(au), bad+0 }
	' "$1"
}

run_one() {  # $1=tag $2=off|on(bridge) $3=loud|quiet $4=off|on(kprobe)
	local tag="$1" bridge="$2" console="$3" kp="$4"
	local pfx="/tmp/cov-$tag" attempt=0 rc=1 last=""

	while [ $attempt -lt 3 ]; do
		attempt=$((attempt+1))
		rm -f "${pfx}"_prog*
		echo 0 > "$ENABLE" 2>/dev/null       # disable is idempotent
		console_quiet_off
		kprobe_off
		dmesg -C > /dev/null
		echo 1 > "$STATS_RESET"          # per-run kernel loss accounting

		[ "$kp"      = on    ] && { kprobe_on || { echo "[$tag] kprobe arm FAILED"; return 1; }; : > "$T/trace"; }
		[ "$console" = quiet ] && console_quiet_on
		# the setup hypercall records owner_cpu = the CPU of THIS writer, so it
		# must share affinity with syz-execprog below
		[ "$bridge"  = on    ] && { taskset -c "$CPU" sh -c "echo 1 > $ENABLE" || { echo "[$tag] ENABLE FAILED"; return 1; }; }

		taskset -c "$CPU" "$EXECPROG" -executor="$EXECUTOR" -procs=1 -threaded=0 \
			-cover=1 -slowdown=10 -vv=1 -coverfile="$pfx" "$PROG" \
			> "$OUT/execprog-$tag.log" 2>&1
		rc=$?

		cp "$STATS" "$OUT/stats-$tag.txt" 2>/dev/null   # BEFORE disable, though
		[ "$bridge" = on ] && echo 0 > "$ENABLE"        # disable only prints, never resets
		# capture the trace BEFORE disarming, and read back the real console
		# level BEFORE unmounting the shadow
		[ "$kp" = on ] && cp "$T/trace" "$OUT/trace-$tag.txt" 2>/dev/null
		local shadowed=""
		[ "$console" = quiet ] && shadowed="$(tr -d '\n\t ' < "$SHADOW")"
		console_quiet_off
		local realprintk; realprintk="$(tr -d '\n\t ' < "$PRINTK")"
		kprobe_off

		last=$(ls "${pfx}"_prog*.1 2>/dev/null | tail -1)
		[ -n "$last" ] && [ -s "$last" ] && break
		echo "[$tag] attempt $attempt produced no coverfile (rc=$rc), retrying"
		sleep 3
	done

	dmesg > "$OUT/dmesg-$tag.txt"

	local nres; nres=$(ls "${pfx}"_prog*.1 2>/dev/null | wc -l)
	local m1 m0
	m1="$(cover_metrics "$last")"
	m0="$(cover_metrics "${last%.1}.0")"
	# FULL per-PC histogram (a few thousand lines) -> exact offline attribution
	[ -n "$last" ] && sort "$last" | uniq -c | sort -rn > "$OUT/hist-$tag.txt"

	local faults=NA retnz=NA
	if [ "$kp" = on ]; then
		faults=$(grep -c 'fw_abort_in' "$OUT/trace-$tag.txt" 2>/dev/null)
		retnz=$(grep 'fw_abort_ret' "$OUT/trace-$tag.txt" 2>/dev/null | grep -cv 'ret=0$')
	fi
	local ovf;  ovf=$(grep -c 'ring overflow' "$OUT/dmesg-$tag.txt" 2>/dev/null)
	local skip; skip=$(grep -c 'skip drain'   "$OUT/dmesg-$tag.txt" 2>/dev/null)
	local warn; warn=$(grep -cE 'WARNING|BUG:|unconfirmed' "$OUT/dmesg-$tag.txt" 2>/dev/null)
	local call1; call1=$(grep -oE 'CALL 1: signal [0-9]+, coverage [0-9]+ errno [0-9]+' "$OUT/execprog-$tag.log" | head -1)
	local sig1;   sig1=$(echo "$call1" | grep -oE 'signal [0-9]+'  | grep -oE '[0-9]+')
	local errno1; errno1=$(echo "$call1" | grep -oE 'errno [0-9]+' | grep -oE '[0-9]+')
	[ -n "$sig1" ]   || sig1=NA
	[ -n "$errno1" ] || errno1=NA

	# kernel-side loss accounting for this run
	sv() { awk -v k="$1" '$1==k{print $2; found=1} END{if(!found)print "NA"}' "$OUT/stats-$tag.txt" 2>/dev/null; }
	local k_drains k_hits k_written k_lostring k_ovf k_hitsmax k_wmax k_linkok k_linkdrop k_req k_acc k_lostkcov k_skipown k_skipkcov k_nested
	k_drains=$(sv drains);            k_hits=$(sv el2_hits)
	k_written=$(sv el2_written);      k_lostring=$(sv LOST_IN_RING)
	k_ovf=$(sv el2_overflow_drains);  k_hitsmax=$(sv el2_hits_max)
	k_wmax=$(sv el2_written_max);     k_nested=$(sv el2_nested_dropped)
	k_linkok=$(sv link_ok);           k_linkdrop=$(sv link_dropped)
	k_req=$(sv kcov_requested);       k_acc=$(sv kcov_accepted)
	k_lostkcov=$(sv LOST_IN_KCOV_AREA)
	k_skipown=$(sv skip_not_owner);   k_skipkcov=$(sv skip_no_kcov)
	echo "        KERNEL drains=$k_drains el2_hits=$k_hits written=$k_written LOST_IN_RING=$k_lostring ovf=$k_ovf hits_max=$k_hitsmax nested=$k_nested"
	echo "               link_ok=$k_linkok link_drop=$k_linkdrop kcov=$k_acc/$k_req LOST_IN_KCOV=$k_lostkcov skip(own/kcov)=$k_skipown/$k_skipkcov"

	case "$tag" in r1-*) gzip -c "$last" > "$OUT/coverfile-$tag.gz" 2>/dev/null;; esac
	rm -f "${pfx}"_prog*

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$tag" "$bridge" "$console" "$kp" "$attempt" "$rc" "$nres" "$errno1" "$sig1" \
		"$m1" "$m0" "$faults" "$retnz" "$ovf" "$skip" "$warn" "${shadowed:-NA}/$realprintk" \
		>> "$OUT/raw.tsv"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$tag" "$bridge" "$console" "$kp" "$k_drains" "$k_hits" "$k_written" "$k_lostring" \
		"$k_ovf" "$k_hitsmax" "$k_wmax" "$k_nested" "$k_linkok" "$k_linkdrop" \
		"$k_req" "$k_acc" "$k_lostkcov" "$k_skipown/$k_skipkcov" >> "$OUT/kstats.tsv"
	echo "[$tag] bridge=$bridge console=$console kprobe=$kp try=$attempt errno=$errno1 signal=$sig1"
	echo "        call1 (total uniq hypRAW hypUNQ brRAW brUNQ uartRAW uartUNQ bad) = $m1"
	echo "        faults=$faults retnz=$retnz overflow=$ovf skip=$skip warn=$warn printk(shadow/real)=${shadowed:-NA}/$realprintk"
}

HDR='tag\tbridge\tconsole\tkprobe\tattempt\trc\tnres\terrno1\tsignal1\tc1_total\tc1_uniq\tc1_hypRAW\tc1_hypUNQ\tc1_brRAW\tc1_brUNQ\tc1_uartRAW\tc1_uartUNQ\tc1_bad\tc0_total\tc0_uniq\tc0_hypRAW\tc0_hypUNQ\tc0_brRAW\tc0_brUNQ\tc0_uartRAW\tc0_uartUNQ\tc0_bad\tfaults\tret_nonzero\toverflow\tskip\twarn\tprintk\n'

echo "=== Step 1A 2x2x2 factorial — $(date -Is) ==="
uname -a
echo "CPU=$CPU REPS=$REPS printk_orig='$PRINTK_ORIG'"
echo "hyp=[$HYP_START,$HYP_END) bridgeEL1=[$BR_START,$BR_END) uart=[$UART_START,$UART_END)"

# warm-up: the first exec after boot pages in a 48 MB binary and sets up the
# executor swapfile; it has been observed to lose the RPC handshake. Discarded.
printf "$HDR" > "$OUT/raw.tsv"
printf 'tag\tbridge\tconsole\tkprobe\tdrains\tel2_hits\tel2_written\tLOST_IN_RING\toverflow_drains\thits_max\twritten_max\tnested\tlink_ok\tlink_dropped\tkcov_req\tkcov_acc\tLOST_IN_KCOV\tskip_own/kcov\n' > "$OUT/kstats.tsv"
echo "--- warm-up (discarded) ---"
run_one warmup off loud off >/dev/null 2>&1
printf "$HDR" > "$OUT/raw.tsv"
printf 'tag\tbridge\tconsole\tkprobe\tdrains\tel2_hits\tel2_written\tLOST_IN_RING\toverflow_drains\thits_max\twritten_max\tnested\tlink_ok\tlink_dropped\tkcov_req\tkcov_acc\tLOST_IN_KCOV\tskip_own/kcov\n' > "$OUT/kstats.tsv"

ARMS="off:loud:off on:loud:off off:quiet:off on:quiet:off off:loud:on on:loud:on off:quiet:on on:quiet:on"
for r in $(seq 1 "$REPS"); do
	echo "--- replicate $r ---"
	# rotate the arm order every replicate
	n=$(echo "$ARMS" | wc -w); shift_by=$(( (r-1) % n ))
	rotated="$(echo "$ARMS" | tr ' ' '\n' | tail -n +$((shift_by+1)); echo "$ARMS" | tr ' ' '\n' | head -n "$shift_by")"
	for a in $rotated; do
		b="$(echo "$a" | cut -d: -f1)"; c="$(echo "$a" | cut -d: -f2)"; k="$(echo "$a" | cut -d: -f3)"
		run_one "r$r-$b-$c-kp$k" "$b" "$c" "$k"
	done
done

echo
echo "=== kstats.tsv (kernel-side loss accounting) ==="
cat "$OUT/kstats.tsv"
echo
echo "=== raw.tsv ==="
cat "$OUT/raw.tsv"
echo
echo "=== done $(date -Is) ==="
