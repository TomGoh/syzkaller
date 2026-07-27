#!/bin/bash
# Recompute the arch/arm64/kvm attribution from rawcover.txt against a matching vmlinux.
# Usage: ./symbolize.sh /path/to/vmlinux   (must match provenance.txt sha256; KASLR off)
set -euo pipefail
VMLINUX="${1:?usage: symbolize.sh <vmlinux>}"
A2L="${A2L:-aarch64-linux-gnu-addr2line}"
here="$(dirname "$0")"
"$A2L" -e "$VMLINUX" -f -i < "$here/rawcover.txt" | paste - - > "$here/rawcover.sym"
echo "== arch/arm64/kvm unique source lines =="
grep -E 'arch/arm64/kvm/' "$here/rawcover.sym" | sed -E 's#.*(arch/arm64/kvm/[^ ]+)#\1#' | sort -u | wc -l
echo "== pkvm.c functions covered (the host-side pKVM driver) =="
grep -E 'arch/arm64/kvm/pkvm\.(c|h)' "$here/rawcover.sym" | awk '{print $1}' | sort -u
