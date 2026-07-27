#!/usr/bin/env bash
# Attribute every static `bl __sanitizer_cov_trace_pc` call site in a Rust hyp
# object (nvhe_rust.o) to its source module, via the object's own DWARF line
# table. Prints a per-file count so ftrace.rs / trace.rs can be told apart from
# the rest of the crate.
#
#   usage: sancov-attrib.sh <nvhe_rust.o> [label]
set -u
OBJ="$1"; LABEL="${2:-$(basename "$OBJ")}"
A2L=aarch64-linux-gnu-addr2line
OD=aarch64-linux-gnu-objdump
export LC_ALL=C

# Each call site shows up as an R_AARCH64_CALL26 relocation against the callback.
# objdump -dr prints the reloc on its own line, prefixed by the instruction's
# section offset:  "   <hex>: R_AARCH64_CALL26  __sanitizer_cov_trace_pc"
# Collect those offsets.
$OD -dr "$OBJ" 2>/dev/null \
  | awk '/R_AARCH64_CALL26[[:space:]]+__sanitizer_cov_trace_pc([[:space:]]|$)/ {
            gsub(/:/,"",$1); print "0x"$1 }' > /tmp/sancov-offs.txt
n=$(wc -l < /tmp/sancov-offs.txt)

# addr2line the call-site offsets against the object's DWARF. A relocatable .o
# has section-relative addresses, which is exactly what these offsets are.
$A2L -e "$OBJ" -f < /tmp/sancov-offs.txt 2>/dev/null \
  | sed -n '2~2p' \
  | sed 's/:[0-9].*$//; s#.*/rust/src/#rust/src/#; s#.*/library/#stdlib/library/#' \
  | sort | uniq -c | sort -rn > /tmp/sancov-byfile.txt

echo "=== $LABEL: $n static __sanitizer_cov_trace_pc call sites ==="
echo "--- by source file (top 20) ---"
head -20 /tmp/sancov-byfile.txt
echo "--- ftrace.rs / trace.rs total ---"
awk '$2 ~ /(ftrace|trace)\.rs$/ {s+=$1} END{print s+0}' /tmp/sancov-byfile.txt
echo "--- unresolved (no line info) ---"
awk '$2=="??" || $2=="" {s+=$1} END{print s+0}' /tmp/sancov-byfile.txt
cp /tmp/sancov-byfile.txt "/tmp/sancov-byfile-${LABEL}.txt" 2>/dev/null
