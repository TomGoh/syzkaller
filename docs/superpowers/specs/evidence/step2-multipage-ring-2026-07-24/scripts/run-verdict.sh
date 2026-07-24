#!/bin/bash
# ============================================================================
# Step 2 — multi-page ring VERDICT smoke (N90)
#
# Same fixed input as Step 1, but sweeping the ring size so the answer is a
# curve rather than a single point. Step 1B measured a per-#23 raw high-water of
# 1088 against a 509-PC ring and 5.02% loss; the open question it could NOT
# answer is whether that 5% cost any *unique* EL2 PCs. Only a ring that stops
# truncating can settle it, so this runs 1 / 2 / 4 pages (509 / 1021 / 2045 PC
# slots) and diffs the resulting unique EL2 PC sets.
#
# Everything else is held at the campaign-realistic setting: bridge on, kprobes
# off, no hot-path printk (the bridge counts instead of printing since 1B),
# CPU 0, procs=1, threaded=0.
# ============================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
CPU="${CPU:-0}"
REPS="${REPS:-3}"
PAGES="${PAGES:-1 2 4}"
OUT="${OUT:-$DIR/out-verdict}"
PROG="$DIR/arm64-syz_kvm_run_fw_fault"
EXECPROG="$DIR/syz-execprog"
EXECUTOR="${EXECUTOR:-$DIR/syz-executor}"
D=/sys/kernel/debug/kvm/pkvm_cov

# hyp .text bounds for THIS build (System.map). Fixed-width lowercase hex, so
# string comparison is exactly numeric comparison.
HYP_START=0xffff800081d87ed4
HYP_END=0xffff800081de7000

mkdir -p "$OUT"; cd "$DIR" || exit 1
trap 'echo 0 > $D/enable 2>/dev/null' EXIT

run_one() {  # $1=tag  $2=nr_pages
	local tag="$1" np="$2"
	local pfx="/tmp/cov-$tag" attempt=0 last=""

	while [ $attempt -lt 3 ]; do
		attempt=$((attempt+1))
		rm -f "${pfx}"_prog*
		echo 0 > "$D/enable" 2>/dev/null
		echo "$np" > "$D/nr_pages" || { echo "[$tag] nr_pages=$np REJECTED"; return 1; }
		echo 1 > "$D/stats_reset"
		dmesg -C > /dev/null
		# the setup hypercall records owner_cpu = the CPU of THIS writer
		taskset -c "$CPU" sh -c "echo 1 > $D/enable" || { echo "[$tag] ENABLE FAILED"; return 1; }

		taskset -c "$CPU" "$EXECPROG" -executor="$EXECUTOR" -procs=1 -threaded=0 \
			-cover=1 -slowdown=10 -vv=1 -coverfile="$pfx" "$PROG" \
			> "$OUT/execprog-$tag.log" 2>&1

		cp "$D/stats" "$OUT/stats-$tag.txt"
		echo 0 > "$D/enable"
		last=$(ls "${pfx}"_prog*.1 2>/dev/null | tail -1)
		[ -n "$last" ] && [ -s "$last" ] && break
		echo "[$tag] attempt $attempt produced no coverfile, retrying"; sleep 3
	done
	dmesg > "$OUT/dmesg-$tag.txt"

	# unique EL2 PCs delivered this run -> the set we diff across ring sizes
	awk -v hs="$HYP_START" -v he="$HYP_END" '$1>=hs && $1<he' "$last" | sort -u \
		> "$OUT/el2set-$tag.txt"
	local total uniq el2u
	total=$(wc -l < "$last"); uniq=$(sort -u "$last" | wc -l)
	el2u=$(wc -l < "$OUT/el2set-$tag.txt")
	sort "$last" | uniq -c | sort -rn > "$OUT/hist-$tag.txt"
	rm -f "${pfx}"_prog*

	sv() { awk -v k="$1" '$1==k{print $2}' "$OUT/stats-$tag.txt"; }
	local cap lost hmax ovf drains hits written linkdrop req acc lostk skipo skipk nested leak
	cap=$(sv ring_capacity_pcs); lost=$(sv LOST_IN_RING); hmax=$(sv el2_hits_max)
	ovf=$(sv el2_overflow_drains); drains=$(sv drains); hits=$(sv el2_hits)
	written=$(sv el2_written); linkdrop=$(sv link_dropped); req=$(sv kcov_requested)
	acc=$(sv kcov_accepted); lostk=$(sv LOST_IN_KCOV_AREA); skipo=$(sv skip_not_owner)
	skipk=$(sv skip_no_kcov); nested=$(sv el2_nested_dropped); leak=$(sv leaked_bytes)
	local warn; warn=$(grep -cE "WARNING: CPU|Call trace:|BUG:|unconfirmed" "$OUT/dmesg-$tag.txt")

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$tag" "$np" "$cap" "$drains" "$hits" "$written" "$lost" "$ovf" "$hmax" \
		"$nested" "$linkdrop" "$req" "$acc" "$lostk" "$skipo/$skipk" "$leak" \
		"$total/$uniq/$el2u" >> "$OUT/verdict.tsv"
	echo "[$tag] pages=$np cap=$cap | drains=$drains hits=$hits written=$written LOST_IN_RING=$lost ovf=$ovf hits_max=$hmax"
	echo "        link_drop=$linkdrop kcov=$acc/$req LOST_IN_KCOV=$lostk skip=$skipo/$skipk leak=$leak warn=$warn"
	echo "        coverfile total/uniq/EL2uniq = $total/$uniq/$el2u"
}

echo "=== multi-page ring verdict sweep — $(date -Is) ==="
uname -a; echo "CPU=$CPU REPS=$REPS PAGES='$PAGES'"
printf 'tag\tpages\tcapacity\tdrains\tel2_hits\tel2_written\tLOST_IN_RING\toverflow\thits_max\tnested\tlink_drop\tkcov_req\tkcov_acc\tLOST_IN_KCOV\tskip_own/kcov\tleaked\ttotal/uniq/EL2uniq\n' > "$OUT/verdict.tsv"

echo "--- warm-up (discarded) ---"; run_one warmup 4 > "$OUT/warmup.log" 2>&1; echo "warm-up rc=$? (log: $OUT/warmup.log)"
printf 'tag\tpages\tcapacity\tdrains\tel2_hits\tel2_written\tLOST_IN_RING\toverflow\thits_max\tnested\tlink_drop\tkcov_req\tkcov_acc\tLOST_IN_KCOV\tskip_own/kcov\tleaked\ttotal/uniq/EL2uniq\n' > "$OUT/verdict.tsv"

for r in $(seq 1 "$REPS"); do
	echo "--- replicate $r ---"
	for np in $PAGES; do run_one "r$r-p$np" "$np"; done
done

echo; echo "=== verdict.tsv ==="; cat "$OUT/verdict.tsv"
echo; echo "=== unique EL2 PC set sizes per ring size ==="
for np in $PAGES; do
	cat "$OUT"/el2set-r*-p$np.txt | sort -u > "$OUT/el2set-union-p$np.txt"
	echo "  ${np} page(s): per-run $(for f in "$OUT"/el2set-r*-p$np.txt; do wc -l < "$f" | tr -d ' '; done | tr '\n' ' ') | union $(wc -l < "$OUT/el2set-union-p$np.txt")"
done
echo; echo "=== done $(date -Is) ==="
