#!/bin/bash
# Build a self-contained bundle of this pack that runs on a machine which does
# not have the syzkaller-pkvm repo.
#
# WHY THIS EXISTS: the run-NNN.sh scripts read their reproducers from the
# tracker, at <repo>/issues/<issue-dir>/repro/*.c, located relative to this
# directory. That works in place and fails the moment someone copies only
# repro-pack/ across -- every script then reports "reproducer source missing"
# pointing two levels above wherever it landed. Handing the pack to a colleague
# is the whole point of it, so the bundle carries its own copy of the sources
# under src/, and common.sh prefers that copy when it is present.
#
# The sources are copied per issue rather than deduplicated. 003, 004 and 005
# currently share a byte-identical probe-dirtylog-thp.c; keeping three copies
# means the bundle still does the right thing if they ever diverge.
#
# Usage:
#   ./make-portable.sh [output.tar.gz]
#
# Then, on the target:
#   tar xzf pkvm-repro-pack.tar.gz && cd repro-pack && sudo ./run-all.sh

set -euo pipefail

PACK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ISSUES_DIR=$(cd "$PACK_DIR/../.." && pwd)
OUT=${1:-$PWD/pkvm-repro-pack.tar.gz}

if [ -d "$PACK_DIR/src" ]; then
	echo "refusing to run inside an already-portable bundle ($PACK_DIR/src exists)" >&2
	echo "run this from the copy inside the repo instead" >&2
	exit 2
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DEST=$STAGE/repro-pack
mkdir -p "$DEST/src"

# Scripts and docs, but never build products -- .gitignore lists them and the
# tracker's repro/ dirs hold .c only.
for f in README.md common.sh make-portable.sh run-all.sh run-00*.sh .gitignore; do
	[ -e "$PACK_DIR/$f" ] && cp -a "$PACK_DIR/$f" "$DEST/"
done

# Which repro/ directories the scripts read, derived from the scripts themselves
# so this cannot drift out of step with them. Whole directories rather than
# individual files: run-002.sh builds two reproducers out of one DIR= variable,
# so matching on .c paths alone would silently miss one of them.
n=0
for d in $(grep -hoE '\$ISSUES_DIR/[0-9]{3}-[a-z0-9-]+/repro' "$PACK_DIR"/run-00*.sh |
	sed 's#^\$ISSUES_DIR/##' | sort -u); do
	if [ ! -d "$ISSUES_DIR/$d" ]; then
		echo "MISSING: $ISSUES_DIR/$d -- refusing to build an incomplete bundle" >&2
		exit 1
	fi
	mkdir -p "$DEST/src/$d"
	for c in "$ISSUES_DIR/$d"/*.c; do
		[ -f "$c" ] || continue
		cp -a "$c" "$DEST/src/$d/"
		echo "  + $d/$(basename "$c")"
		n=$((n + 1))
	done
done

[ "$n" -gt 0 ] || { echo "no reproducer sources found -- aborting" >&2; exit 1; }

tar czf "$OUT" -C "$STAGE" repro-pack
echo
echo "wrote $OUT  ($(du -h "$OUT" | cut -f1), $n source files)"
echo "on the target:  tar xzf $(basename "$OUT") && cd repro-pack && sudo ./run-all.sh"
