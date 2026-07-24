#!/usr/bin/env bash
set -u; cd "$(dirname "$0")"
VMLINUX=/home/jose/common-stage2mvp/vmlinux
export LC_ALL=C
mkdir -p 1b-analysis
awk '{print $2}' 1b/hist-r1-*.txt | sort -u > 1b-analysis/all-pcs.txt
aarch64-linux-gnu-addr2line -f -e "$VMLINUX" < 1b-analysis/all-pcs.txt > 1b-analysis/a2l.raw
paste -d'\t' 1b-analysis/all-pcs.txt <(sed -n '1~2p' 1b-analysis/a2l.raw) <(sed -n '2~2p' 1b-analysis/a2l.raw) > 1b-analysis/symtab.tsv
for arm in off-quiet-kpoff on-quiet-kpoff; do
  h=1b/hist-r1-$arm.txt; [ -f "$h" ] || continue
  total=$(awk '{s+=$1} END{print s+0}' "$h")
  echo "=== r1-$arm : $total raw entries, $(wc -l < "$h") unique PCs ==="
  awk -v T="$total" 'NR==FNR{f[$1]=$3; next}
    {file=f[$2]; sub(/:[0-9]+$/,"",file); if(file=="")file="?"; c[file]+=$1}
    END{for(k in c) printf "%9d  %5.1f%%  %s\n", c[k], 100*c[k]/T, k}' \
    1b-analysis/symtab.tsv "$h" | sort -rn | head -8
  echo
done
