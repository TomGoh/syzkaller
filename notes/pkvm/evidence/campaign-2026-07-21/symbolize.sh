#!/bin/bash
# Recompute file-level coverage attribution from rawcover.txt against a vmlinux.
# Usage: ./symbolize.sh [vmlinux] [addr2line]
set -u; D="$(cd "$(dirname "$0")" && pwd)"
VMLINUX="${1:-/home/jose/common/vmlinux}"; A2L="${2:-aarch64-linux-gnu-addr2line}"
sort -u "$D/rawcover.txt" | "$A2L" -e "$VMLINUX" -f 2>/dev/null | paste - - > "$D/rawcover.sym"
{ echo "## total unique PCs: $(sort -u "$D/rawcover.txt" | wc -l)"
  echo "## coverage by arch/arm64/kvm file:"; grep -oE 'arch/arm64/kvm/[a-z_/]+\.c' "$D/rawcover.sym" | sort | uniq -c | sort -rn
  echo "## pKVM host-side (EL1) functions covered:"; grep -iE '\bpkvm|__pkvm' "$D/rawcover.sym" | awk '{print $1}' | sort | uniq -c | sort -rn
} > "$D/coverage-attribution.txt"
