#!/usr/bin/env bash
# What fraction of a running campaign's coverage is actually the EL2 hypervisor?
#
#   el2-coverage-breakdown.sh [http_port] [kernel_obj]
#
# The manager's headline `coverage=NNNNN` counts every PC the host kernel
# reports, and on this workload most of it is generic kernel machinery
# (maple_tree, mm/memory.c, vmalloc, kvm_main) exercised incidentally by
# allocation and file I/O — not pKVM. Judging progress on that number means
# judging mostly noise. This prints the numbers worth deciding on:
#
#   * unique EL2 Rust source lines covered
#   * which __pkvm_* HVC handlers are reached, out of how many exist
#   * the top source files, so host-side dominance stays visible
#
# The kernel_obj MUST be the tree that built the running kernel, or addr2line
# silently produces wrong symbols. Verify with:
#   strings -a <kernel_obj>/vmlinux | grep -m1 '^Linux version'   # vs uname -a
set -euo pipefail

PORT="${1:-56752}"
KOBJ="${2:-/home/jose/common-stage2mvp}"
ADDR2LINE="${ADDR2LINE:-aarch64-linux-gnu-addr2line}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

[ -f "$KOBJ/vmlinux" ] || { echo "no vmlinux in $KOBJ" >&2; exit 1; }

curl -s --max-time 120 "http://127.0.0.1:$PORT/rawcover" > "$TMP/raw.txt" || {
    echo "could not reach manager on port $PORT" >&2; exit 1; }
[ -s "$TMP/raw.txt" ] || { echo "manager returned no coverage (campaign just started?)" >&2; exit 1; }

echo "=== source of truth ==="
# `|| true`: grep -m1 exits early and SIGPIPEs strings, which under `pipefail`
# would abort the whole script on a purely informational line.
{ strings -a "$KOBJ/vmlinux" | grep -m1 '^Linux version' | sed 's/^/  kernel_obj: /'; } || true
echo "  raw PCs   : $(wc -l < "$TMP/raw.txt")"

# -i expands inlined frames, which is why the line count exceeds the PC count.
timeout 600 "$ADDR2LINE" -e "$KOBJ/vmlinux" -f -i < "$TMP/raw.txt" 2>/dev/null \
    | grep -oE '^/?[a-zA-Z0-9_./-]+\.(c|rs|h):[0-9]+' > "$TMP/lines.txt" || true
timeout 600 "$ADDR2LINE" -e "$KOBJ/vmlinux" -f    < "$TMP/raw.txt" 2>/dev/null \
    | grep -E '^[a-zA-Z_]' | sort -u > "$TMP/fns.txt" || true

total=$(sort -u "$TMP/lines.txt" | wc -l)
el2=$(grep -E '/rust/src/.*\.rs:' "$TMP/lines.txt" | sort -u | wc -l)

echo
echo "=== EL2 vs everything else ==="
printf '  unique source lines covered : %s\n' "$total"
printf '  of which EL2 Rust (hyp)     : %s  (%.1f%%)\n' "$el2" \
       "$(awk -v a="$el2" -v b="$total" 'BEGIN{print (b?100*a/b:0)}')"

echo
echo "=== EL2 Rust files ==="
grep -E '/rust/src/.*\.rs:' "$TMP/lines.txt" | sort -u | sed 's/:[0-9]*$//' \
    | sed 's|.*/rust/src/||' | sort | uniq -c | sort -rn

echo
echo "=== __pkvm_* HVC handlers reached ==="
grep -oE '__kvm_nvhe___pkvm_[a-z0-9_]+' "$TMP/fns.txt" | sed 's/__kvm_nvhe_//' | sort -u > "$TMP/hit.txt" || true
# Denominator: distinct __pkvm_* symbols the Rust hyp actually defines.
grep -rhoE '\b__pkvm_[a-z0-9_]+' "$KOBJ/arch/arm64/kvm/hyp/nvhe/rust/src/" 2>/dev/null \
    | sort -u > "$TMP/all.txt" || true
printf '  reached: %s / %s known\n\n' "$(wc -l < "$TMP/hit.txt")" "$(wc -l < "$TMP/all.txt")"
sed 's/^/    + /' "$TMP/hit.txt"
echo "  -- not reached --"
comm -13 "$TMP/hit.txt" "$TMP/all.txt" | sed 's/^/    - /'

echo
echo "=== top 12 source files overall (host-side dominance check) ==="
sort -u "$TMP/lines.txt" | sed 's/:[0-9]*$//' | sed "s|$KOBJ/||;s|/home/jose/common/||" \
    | sort | uniq -c | sort -rn | head -12
