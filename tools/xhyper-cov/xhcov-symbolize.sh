#!/bin/sh
# Turn libxhcov cumulative bitmaps into a ranked covered-function report,
# plus a coverage percentage and the largest uncovered .text functions.
#
# Usage: xhcov-symbolize.sh [--uncovered] <kernel.elf> <cum_prefix>
#   <cum_prefix>  e.g. /dev/shm/xh0_cum  (globs <cum_prefix>.<vcpu>)
#
# Unions set bits across every vCPU file. Bit k at text_start+k*granularity
# (granularity is 4) is one covered block. Addresses go to addr2line -f,
# function names through c++filt (Rust v0 demangles with the default style).
#
# The coverage section does not use instrumentation. The denominator is every
# STT_FUNC symbol whose address lies in the ELF .text section. A function is
# covered when some covered block falls in [addr, addr+size). The uncovered
# list (largest functions first, at most 60) is always printed; --uncovered
# is accepted so callers can spell that intent.
set -eu

prog=$(basename "$0")
usage="usage: $prog [--uncovered] <kernel.elf> <cum_prefix>"

elf=
prefix=
while [ $# -gt 0 ]; do
    case "$1" in
        --uncovered)
            shift
            ;;
        --help|-h)
            echo "$usage" >&2
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "$usage" >&2
            exit 2
            ;;
        *)
            if [ -z "$elf" ]; then
                elf=$1
            elif [ -z "$prefix" ]; then
                prefix=$1
            else
                echo "$usage" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

if [ -z "$elf" ] || [ -z "$prefix" ]; then
    echo "$usage" >&2
    exit 2
fi

if [ ! -r "$elf" ]; then
    echo "$prog: cannot read '$elf'" >&2
    exit 1
fi
if ! command -v addr2line >/dev/null 2>&1; then
    echo "$prog: addr2line not found in PATH" >&2
    exit 1
fi
if ! command -v c++filt >/dev/null 2>&1; then
    echo "$prog: c++filt not found in PATH" >&2
    exit 1
fi
if ! command -v readelf >/dev/null 2>&1; then
    echo "$prog: readelf not found in PATH" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "$prog: python3 not found in PATH" >&2
    exit 1
fi

exec python3 - "$prog" "$elf" "$prefix" <<'PY'
import bisect
import glob
import struct
import subprocess
import sys

prog, elf, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
MAGIC = 0x323030564F434858
HDR = 4096
UNCOVERED_CAP = 60


def die(msg):
    sys.stderr.write("%s: %s\n" % (prog, msg))
    sys.exit(1)


def run_tool(argv, text_in):
    proc = subprocess.run(argv, input=text_in, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.exit(1)
    return proc.stdout


def demangle(raw_names):
    if not raw_names:
        return []
    out = run_tool(["c++filt"], "".join(n + "\n" for n in raw_names))
    names = out.splitlines()
    if len(names) != len(raw_names):
        return list(raw_names)
    return names


def text_bounds(path):
    out = run_tool(["readelf", "-S", "-W", path], "")
    for line in out.splitlines():
        s = line.strip()
        if not s.startswith("["):
            continue
        rb = s.find("]")
        if rb < 0:
            continue
        fields = s[rb + 1:].split()
        # name type addr offset size ...
        if len(fields) >= 5 and fields[0] == ".text":
            start = int(fields[2], 16)
            size = int(fields[4], 16)
            return start, start + size
    die("no .text section in %s" % path)


def load_funcs(path, text_lo, text_hi):
    """STT_FUNC symbols with a name, size > 0, and addr in [text_lo, text_hi).

    Duplicate addresses keep the larger size (equal sizes keep the first).
    """
    out = run_tool(["readelf", "-sW", path], "")
    by_addr = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        if fields[3] != "FUNC":
            continue
        try:
            addr = int(fields[1], 16)
            size = int(fields[2], 10)
        except ValueError:
            continue
        if size <= 0 or addr < text_lo or addr >= text_hi:
            continue
        name = " ".join(fields[7:])
        if not name:
            continue
        prev = by_addr.get(addr)
        if prev is None or size > prev[0]:
            by_addr[addr] = (size, name)
    funcs = [(addr, size, name) for addr, (size, name) in by_addr.items()]
    funcs.sort()
    return funcs


def load_covered_addrs(prefix_path):
    paths = []
    for path in sorted(glob.glob(prefix_path + ".*")):
        suffix = path[len(prefix_path) + 1:]
        if suffix.isdigit():
            paths.append(path)
    if not paths:
        die("no cumulative bitmap matching %s.*" % prefix_path)

    seen = set()
    addrs = []
    for path in paths:
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) < HDR:
            die("%s: too small (%d bytes)" % (path, len(data)))
        magic, ver, cpu, text_start, gran, nwords = struct.unpack_from("<6Q", data, 0)
        if magic != MAGIC:
            die("%s: bad magic %#x" % (path, magic))
        if ver != 2 or gran == 0:
            die("%s: bad header ver=%d gran=%d" % (path, ver, gran))
        if nwords == 0:
            continue
        need = HDR + nwords * 8
        if len(data) < need:
            die("%s: short file (%d < %d)" % (path, len(data), need))
        words = struct.unpack_from("<%dQ" % nwords, data, HDR)
        for wi, word in enumerate(words):
            if word == 0:
                continue
            bit = 0
            rest = word
            while rest:
                if rest & 1:
                    va = text_start + (wi * 64 + bit) * gran
                    if va not in seen:
                        seen.add(va)
                        addrs.append(va)
                rest >>= 1
                bit += 1
    addrs.sort()
    return addrs


def functions_hit(funcs, addrs):
    """Indices of funcs that contain at least one covered VA.

    A VA belongs to the function with the greatest addr <= VA and VA < addr+size.
    """
    if not funcs or not addrs:
        return set()
    starts = [f[0] for f in funcs]
    hit = set()
    for va in addrs:
        i = bisect.bisect_right(starts, va) - 1
        if i < 0:
            continue
        addr, size, _name = funcs[i]
        if va < addr + size:
            hit.add(i)
    return hit


def print_covered_report(addrs):
    if not addrs:
        print("total covered blocks: 0")
        print("distinct functions: 0")
        return

    blob = "".join("%#x\n" % a for a in addrs)
    out = run_tool(["addr2line", "-f", "-e", elf], blob)
    lines = out.splitlines()
    if len(lines) < 2 * len(addrs):
        die("addr2line returned %d lines for %d addresses" % (len(lines), len(addrs)))
    raw_names = lines[0::2][:len(addrs)]
    locs = lines[1::2][:len(addrs)]
    names = demangle(raw_names)

    agg = {}
    order = []
    for name, loc in zip(names, locs):
        rec = agg.get(name)
        if rec is None:
            agg[name] = [1, loc]
            order.append(name)
        else:
            rec[0] += 1

    rows = sorted(order, key=lambda n: (-agg[n][0], n))
    for name in rows:
        count, loc = agg[name]
        print("%d\t%s\t%s" % (count, name, loc))
    print("total covered blocks: %d" % len(addrs))
    print("distinct functions: %d" % len(agg))


def print_coverage(funcs, hit):
    universe = len(funcs)
    covered = len(hit)
    pct = (100.0 * covered / universe) if universe else 0.0
    print("COVERAGE: %d/%d functions (%.1f%%)" % (covered, universe, pct))
    uncovered = [funcs[i] for i in range(universe) if i not in hit]
    print("uncovered functions: %d" % len(uncovered))
    uncovered.sort(key=lambda item: (-item[1], item[2]))
    top = uncovered[:UNCOVERED_CAP]
    if not top:
        return
    pretty = demangle([item[2] for item in top])
    locs = run_tool(
        ["addr2line", "-e", elf],
        "".join("%#x\n" % item[0] for item in top),
    ).splitlines()
    if len(locs) < len(top):
        die("addr2line returned %d lines for %d function starts" % (len(locs), len(top)))
    for item, name, loc in zip(top, pretty, locs):
        _addr, size, _raw = item
        print("%d\t%s\t%s" % (size, name, loc))


text_lo, text_hi = text_bounds(elf)
addrs = load_covered_addrs(prefix)
funcs = load_funcs(elf, text_lo, text_hi)
print_covered_report(addrs)
print_coverage(funcs, functions_hit(funcs, addrs))
PY
