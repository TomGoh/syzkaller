#!/bin/bash
# Recompute the pkvm.c attribution from rawcover.txt against a matching vmlinux (sha256 in provenance.txt).
set -euo pipefail
VMLINUX="${1:?usage: symbolize.sh <vmlinux>}"
A2L="${A2L:-aarch64-linux-gnu-addr2line}"
here="$(dirname "$0")"
"$A2L" -e "$VMLINUX" -f -i < "$here/rawcover.txt" | paste - - > "$here/rawcover.sym"
echo "== pkvm.c functions covered (expect 5 lifecycle + 3 config) =="
grep -E 'arch/arm64/kvm/pkvm\.(c|h)' "$here/rawcover.sym" | awk '{print $1}' | sort -u
