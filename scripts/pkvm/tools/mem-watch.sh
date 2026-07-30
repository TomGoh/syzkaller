#!/usr/bin/env bash
# Sample both boards' memory while a campaign runs, so a leak is caught as it
# develops rather than reconstructed from an OOM post-mortem.
#
#   mem-watch.sh [interval_sec] [outfile]        # default 120s
#
# Why this exists: on 2026-07-30 D3000 died of 12 global OOMs with 23.9 GB
# present in NO host counter and 37 GB of swap untouched — memory the host no
# longer owned, so nothing was reclaimable. All we had afterwards was the OOM
# report, which shows the end state and not the growth curve. N90 under the same
# workload stayed healthy, so the trigger is still unknown; the onset is the part
# worth capturing.
#
# The signal is NOT MemFree (page cache masks it). It is the gap between
# MemTotal and everything the kernel can account for. When `unaccounted_mb`
# climbs steadily while SwapFree stays flat, memory is leaving host ownership —
# that is the pKVM donate/reclaim path, not normal pressure.
set -uo pipefail

INTERVAL="${1:-120}"
OUT="${2:-/home/jose/syzkaller/mem-watch.csv}"
KEY=/home/jose/.ssh/id_ed25519
declare -A IP=( [n90]=10.42.27.17 [d3000]=10.42.27.18 )

if [[ ! -s "$OUT" ]]; then
    echo "ts,board,uptime_s,mem_total_kb,mem_avail_kb,anon_kb,mapped_kb,slab_unreclaim_kb,pagetables_kb,swap_free_kb,unaccounted_mb,tlb_warns" > "$OUT"
fi

sample() {
    local b=$1 ip=${IP[$b]} raw
    raw=$(timeout 25 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o LogLevel=ERROR \
              -i "$KEY" "root@$ip" '
        awk "/^MemTotal|^MemAvailable|^AnonPages|^Mapped|^SUnreclaim|^PageTables|^SwapFree/{printf \"%s %s \", \$1, \$2}" /proc/meminfo
        printf "%.0f " "$(cut -d" " -f1 /proc/uptime)"
        dmesg 2>/dev/null | grep -c kvm_tlb_flush_vmid_range
    ' 2>/dev/null) || { echo "$(date +%s),$b,UNREACHABLE,,,,,,,,," >> "$OUT"; return; }

    # Parse "Key: value" pairs into locals.
    local total avail anon mapped slabu pt swapfree up warns
    total=$(grep -oP 'MemTotal:\s*\K[0-9]+' <<<"$raw")
    avail=$(grep -oP 'MemAvailable:\s*\K[0-9]+' <<<"$raw")
    anon=$(grep -oP 'AnonPages:\s*\K[0-9]+' <<<"$raw")
    mapped=$(grep -oP 'Mapped:\s*\K[0-9]+' <<<"$raw")
    slabu=$(grep -oP 'SUnreclaim:\s*\K[0-9]+' <<<"$raw")
    pt=$(grep -oP 'PageTables:\s*\K[0-9]+' <<<"$raw")
    swapfree=$(grep -oP 'SwapFree:\s*\K[0-9]+' <<<"$raw")
    up=$(awk '{print $(NF-1)}' <<<"$raw")
    warns=$(awk '{print $NF}' <<<"$raw")
    [[ -n "$total" ]] || { echo "$(date +%s),$b,PARSE_FAIL,,,,,,,,," >> "$OUT"; return; }

    # Everything the host can name, subtracted from what it has. Page cache is
    # deliberately excluded via MemAvailable rather than MemFree.
    local unacc=$(( (total - avail - anon - slabu - pt) / 1024 ))
    echo "$(date +%s),$b,$up,$total,$avail,$anon,$mapped,$slabu,$pt,$swapfree,$unacc,$warns" >> "$OUT"
    printf '  %-6s up=%-7s avail=%6d MB  anon=%6d MB  unaccounted=%6d MB  tlb_warns=%s\n' \
           "$b" "$up" "$((avail/1024))" "$((anon/1024))" "$unacc" "$warns"
}

echo "sampling every ${INTERVAL}s -> $OUT   (Ctrl-C to stop)"
while true; do
    echo "--- $(date '+%m-%d %H:%M:%S') ---"
    for b in n90 d3000; do sample "$b"; done
    sleep "$INTERVAL"
done
