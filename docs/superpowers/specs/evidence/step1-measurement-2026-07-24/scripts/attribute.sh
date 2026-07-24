#!/usr/bin/env bash
# Symbolize every unique PC seen in any 1A histogram against the SAME vmlinux the
# board is running, then aggregate each arm's raw KCOV entries by source file and
# by function. Answers "who filled the KCOV area" per arm, with counts.
set -u
cd "$(dirname "$0")" || exit 1
VMLINUX=/home/jose/common-stage2mvp/vmlinux
A2L=aarch64-linux-gnu-addr2line
export LC_ALL=C

mkdir -p 1a-analysis

# 1. union of all unique PCs across every histogram
awk '{print $2}' 1a/hist-*.txt | sort -u > 1a-analysis/all-pcs.txt
echo "unique PCs across all arms: $(wc -l < 1a-analysis/all-pcs.txt)"

# 2. one batched addr2line pass: "func" then "file:line" per address
#    (-f without -i => exactly two output lines per input address, so the
#    paste-with-input below stays aligned)
$A2L -f -e "$VMLINUX" < 1a-analysis/all-pcs.txt > 1a-analysis/a2l.raw
paste -d'\t' 1a-analysis/all-pcs.txt \
      <(sed -n '1~2p' 1a-analysis/a2l.raw) \
      <(sed -n '2~2p' 1a-analysis/a2l.raw) > 1a-analysis/symtab.tsv
echo "symtab rows: $(wc -l < 1a-analysis/symtab.tsv)"

# 3. per-arm aggregation
for h in 1a/hist-r1-*.txt; do
	arm=$(basename "$h" .txt); arm=${arm#hist-}
	out=1a-analysis/attr-$arm.txt
	{
		echo "=== $arm ==="
		total=$(awk '{s+=$1} END{print s+0}' "$h")
		echo "raw entries: $total   unique PCs: $(wc -l < "$h")"
		echo
		echo "--- by source file (top 12, raw entries) ---"
		awk 'NR==FNR{f[$1]=$3; fn[$1]=$2; next}
		     {file=f[$2]; sub(/:[0-9]+$/,"",file); if(file=="")file="?";
		      c[file]+=$1; u[file]++}
		     END{for(k in c) printf "%10d  %6d  %5.1f%%  %s\n", c[k], u[k], 100*c[k]/T, k}' \
		    T="$total" 1a-analysis/symtab.tsv "$h" 2>/dev/null | sort -rn | head -12
		echo
		echo "--- by function (top 12, raw entries) ---"
		awk -v T="$total" 'NR==FNR{fn[$1]=$2; next}
		     {f=fn[$2]; if(f=="")f="?"; c[f]+=$1; u[f]++}
		     END{for(k in c) printf "%10d  %6d  %5.1f%%  %s\n", c[k], u[k], 100*c[k]/T, k}' \
		    1a-analysis/symtab.tsv "$h" | sort -rn | head -12
	} > "$out"
	cat "$out"; echo
done
