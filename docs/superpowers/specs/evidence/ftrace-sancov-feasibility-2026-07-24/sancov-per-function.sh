#!/usr/bin/env bash
# Per-FUNCTION __sanitizer_cov_trace_pc attribution for a Rust hyp object, and
# the ftrace=y-vs-ftrace=n diff that backs the "10 lost / 0 unrelated" claim.
#
# WHY per-function and not per-file: SanCov call sites in inlined ftrace code get
# attributed by DWARF to their inlined-from locations, so a per-FILE count is
# misleading (ftrace.rs/trace.rs read 29 in BOTH builds). Enclosing-symbol
# attribution is the correct measure.
#
# Two modes:
#   generate: sancov-per-function.sh <nvhe_rust.ftraceON.o> <nvhe_rust.ftraceOFF.o>
#             -> writes per-function tables, then runs the verify below.
#             (rebuild the two .o per RESULT.txt: base da966ce9a047 + the canonical
#              stage2-el2-kcov.patch + stage2.config, toggling ONLY
#              CONFIG_PROTECTED_NVHE_FTRACE, and copy each nvhe_rust.o out.)
#   verify:   sancov-per-function.sh <ftraceON.txt> <ftraceOFF.txt>
#             -> runs directly on the committed per-function tables (no rebuild).
#
# Requires an aarch64 toolchain on PATH (objdump, c++filt) for generate mode;
# verify mode needs only c++filt (falls back to raw names if absent).
set -u
OBJDUMP="${OBJDUMP:-aarch64-linux-gnu-objdump}"
CXXFILT="${CXXFILT:-aarch64-linux-gnu-c++filt}"
export LC_ALL=C

[ $# -eq 2 ] || { echo "usage: $0 <ON.o|ON.txt> <OFF.o|OFF.txt>" >&2; exit 2; }
ON="$1"; OFF="$2"

# Attribute every `bl __sanitizer_cov_trace_pc` (an R_AARCH64_CALL26 relocation)
# to the symbol whose disassembly block it falls in. Emits "<count> <mangled_fn>".
attribute() {  # <obj> -> stdout table
	"$OBJDUMP" -dr "$1" 2>/dev/null | awk '
		/^[0-9a-f]+ <.*>:/ { fn=$2; gsub(/[<>:]/,"",fn) }
		/R_AARCH64_CALL26[[:space:]]+__sanitizer_cov_trace_pc([[:space:]]|$)/ { print fn }
	' | sort | uniq -c | sort -k2
}

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
case "$ON" in
	*.o) attribute "$ON"  > "$tmp/on.txt";  attribute "$OFF" > "$tmp/off.txt"
	     echo "generated per-function tables from objects";;
	*)   sort -k2 "$ON"  > "$tmp/on.txt";   sort -k2 "$OFF" > "$tmp/off.txt"
	     echo "using pre-generated per-function tables";;
esac

sites() { awk '{s+=$1} END{print s+0}' "$1"; }
demangle() { if command -v "$CXXFILT" >/dev/null 2>&1; then "$CXXFILT"; else cat; fi; }
# a function belongs to the ftrace FEATURE if its name (demangled) mentions ftrace
# or the trace hooks — this is a name test, so it also catches handle___pkvm_disable_ftrace
# in hyp_main.rs, which is ftrace-feature but NOT in ftrace.rs/trace.rs.
FTRACE_RE='ftrace|__hyp_ftrace|hyp_ftrace|trace_func|_ZN9nvhe_rust5trace|_ZN9nvhe_rust6ftrace'

echo "total __sanitizer_cov_trace_pc call sites:  ON=$(sites "$tmp/on.txt")  OFF=$(sites "$tmp/off.txt")"

# functions present (with >=1 site) in ON but absent in OFF == fully lost instrumentation
comm -23 <(awk '{print $2}' "$tmp/on.txt" | sort -u) \
         <(awk '{print $2}' "$tmp/off.txt" | sort -u) > "$tmp/lost.txt"
nlost=$(wc -l < "$tmp/lost.txt")
echo
echo "=== functions that lost SanCov entirely ($nlost) — demangled, with ON site count ==="
while read -r fn; do
	c=$(awk -v f="$fn" '$2==f{print $1}' "$tmp/on.txt")
	printf '%4s  %s\n' "$c" "$(echo "$fn" | demangle)"
done < "$tmp/lost.txt" | sort -rn

# the verification the colleague asked to be reproducible: none of the lost
# functions may be unrelated to the ftrace feature.
unrelated=$(demangle < "$tmp/lost.txt" | grep -Eiv "$FTRACE_RE" | grep -cve '^[[:space:]]*$')
echo
if [ "$unrelated" -eq 0 ]; then
	echo "VERDICT: all $nlost lost functions are ftrace-feature-related; 0 unrelated. PASS"
else
	echo "VERDICT: $unrelated lost function(s) are UNRELATED to ftrace — FAIL:"
	demangle < "$tmp/lost.txt" | grep -Eiv "$FTRACE_RE" | grep -ve '^[[:space:]]*$' | sed 's/^/    /'
	exit 1
fi
