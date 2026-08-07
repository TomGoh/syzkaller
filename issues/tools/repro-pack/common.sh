#!/bin/bash
# Shared helpers for the pKVM issue reproducer pack.  Sourced by run-NNN.sh.
#
# DELIBERATELY NOT `set -e`.
#
# The point of this pack is that silence must never read as a pass (see
# ../../README.md, "A hung reproducer prints nothing"). `set -e` makes a script
# die part-way through without printing its RESULT line, which is exactly the
# failure mode we are guarding against. So every failure path here is explicit,
# and an EXIT trap prints INCONCLUSIVE if a verdict was never reached.

set -uo pipefail

# ISSUES_DIR and DMESG_WRAPPED are consumed by the run-NNN.sh scripts that
# source this file, which shellcheck cannot see from here.
# shellcheck disable=SC2034
PACK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC2034
ISSUES_DIR=$(cd "$PACK_DIR/../.." && pwd)

# Exit codes. Chosen not to collide with the shell's own (126/127), timeout(1)'s
# (124/125), or the reproducers' (1 = setup failure, 2 = FATAL, 3 = alarm).
RC_REPRODUCED=10
RC_NOT_REPRODUCED=11
RC_INCONCLUSIVE=12

# Where binaries and captured logs go. A temp dir by default, so nothing
# compiled ever lands in the work tree. run-all.sh exports one for all children.
if [ -z "${OUTDIR:-}" ]; then
	OUTDIR=$(mktemp -d "${TMPDIR:-/tmp}/pkvm-repro-pack.XXXXXX") || {
		echo "RESULT ???: INCONCLUSIVE  (cannot create a temp dir)" >&2
		exit 12
	}
fi
mkdir -p "$OUTDIR"

HOST_ARCH=$(uname -m)
case "$HOST_ARCH" in
aarch64 | arm64) NATIVE=1; CC=${CC:-cc} ;;
*)               NATIVE=0; CC=${CC:-aarch64-linux-gnu-gcc} ;;
esac

# The ISSUE.md build lines use -O2 for 001-005 and -O1 for 006; this pack builds
# everything at -O1 as specified for it. Set CFLAGS_OPT=-O2 to match the exact
# build every recorded observation was made with.
CFLAGS_OPT=${CFLAGS_OPT:--O1}

ISSUE_ID="???"
VERDICT_PRINTED=0
RC=0
DMESG_WRAPPED=0

on_exit() {
	local rc=$?
	if [ "$VERDICT_PRINTED" = 0 ]; then
		printf 'RESULT %s: INCONCLUSIVE  (script exited rc=%d without reaching a verdict -- see the log above)\n' \
			"$ISSUE_ID" "$rc"
		exit "$RC_INCONCLUSIVE"
	fi
}

pack_init() {	# pack_init <issue-id> <one-line title>
	ISSUE_ID=$1
	trap on_exit EXIT
	printf '===== issue %s : %s =====\n' "$1" "$2"
	printf 'host=%s  native=%s  cc=%s %s -static  outdir=%s\n' \
		"$HOST_ARCH" "$NATIVE" "$CC" "$CFLAGS_OPT" "$OUTDIR"
}

verdict() {	# verdict REPRODUCED|NOT-REPRODUCED|INCONCLUSIVE "<one-line why>"
	VERDICT_PRINTED=1
	printf 'RESULT %s: %s  (%s)\n' "$ISSUE_ID" "$1" "$2"
	case $1 in
	REPRODUCED)     exit "$RC_REPRODUCED" ;;
	NOT-REPRODUCED) exit "$RC_NOT_REPRODUCED" ;;
	*)              exit "$RC_INCONCLUSIVE" ;;
	esac
}

note() { printf 'NOTE  %s: %s\n' "$ISSUE_ID" "$*"; }

# ---------------------------------------------------------------- build ------

# build_repro <src.c> <binary-name>  -> echoes the binary path.
# Set BINDIR=<dir> to use pre-built binaries instead (a board with no compiler:
# build on a workstation with run-all.sh --build-only, then copy OUTDIR across).
build_repro() {
	local src=$1 name=$2
	local bin="$OUTDIR/$name"

	if [ -n "${BINDIR:-}" ] && [ -x "$BINDIR/$name" ]; then
		printf '%s\n' "$BINDIR/$name"
		return 0
	fi
	if [ ! -f "$src" ]; then
		verdict INCONCLUSIVE "reproducer source missing: $src"
	fi
	if ! command -v "$CC" >/dev/null 2>&1; then
		verdict INCONCLUSIVE "no compiler: '$CC' not found -- set CC=, or build elsewhere and set BINDIR="
	fi
	# shellcheck disable=SC2086  # CFLAGS_OPT is intentionally word-split
	if ! "$CC" $CFLAGS_OPT -static -o "$bin" "$src" > "$bin.build.log" 2>&1; then
		sed 's/^/    /' "$bin.build.log" >&2
		verdict INCONCLUSIVE "build failed: $CC $CFLAGS_OPT -static $(basename "$src") -- log in $bin.build.log"
	fi
	printf '%s\n' "$bin"
}

# Call after every build_repro in a script. Building is all --build-only does,
# and "nothing ran" is INCONCLUSIVE, never a pass.
build_only_stop() {
	if [ "${BUILD_ONLY:-0}" = 1 ]; then
		verdict INCONCLUSIVE "build-only: binaries are in $OUTDIR, nothing was executed"
	fi
}

# ------------------------------------------------------- preconditions ------

need_native() {
	[ "$NATIVE" = 1 ] || verdict INCONCLUSIVE \
		"host is $HOST_ARCH, not aarch64 -- cross-built only; run this script on the target board"
}

need_kvm() {
	[ -e /dev/kvm ] || verdict INCONCLUSIVE \
		"/dev/kvm does not exist -- KVM is not enabled on this host"
	{ [ -r /dev/kvm ] && [ -w /dev/kvm ]; } || verdict INCONCLUSIVE \
		"/dev/kvm is not readable+writable by uid $(id -u) -- run as root"
}

# Every issue in this tracker needs a host booted kvm-arm.mode=protected.
# The cmdline is the request; the dmesg line is the confirmation that it took.
need_pkvm() {
	grep -q 'kvm-arm\.mode=protected' /proc/cmdline 2>/dev/null || verdict INCONCLUSIVE \
		"/proc/cmdline lacks kvm-arm.mode=protected -- none of these defects is reachable without it"

	if read_dmesg > "$OUTDIR/dmesg-boot.txt" 2>/dev/null; then
		if grep -q 'Protected nVHE mode initialized successfully' "$OUTDIR/dmesg-boot.txt"; then
			return 0
		fi
		if grep -qE '(VHE|Hyp) mode initialized successfully' "$OUTDIR/dmesg-boot.txt"; then
			verdict INCONCLUSIVE \
				"kernel reports a NON-protected hyp mode despite the cmdline -- pKVM did not initialise"
		fi
		note "could not find 'Protected nVHE mode initialized successfully' in dmesg (buffer has probably wrapped); proceeding on /proc/cmdline alone"
	fi
}

need_dmesg() {
	read_dmesg >/dev/null 2>&1 || verdict INCONCLUSIVE \
		"cannot read dmesg (kernel.dmesg_restrict?) -- this issue's signature is a kernel log line; run as root"
}

# probe-dirtylog-thp.c only ever calls MADV_NOHUGEPAGE (in its 'nohuge' mode);
# it never asks for a huge page. So its default mode is only THP-backed when the
# system default is 'always'. Under [madvise] or [never] the huge-page
# precondition is absent and a clean run means nothing.
need_thp_always() {
	local f=/sys/kernel/mm/transparent_hugepage/enabled cur
	[ -r "$f" ] || verdict INCONCLUSIVE \
		"$f is unreadable -- cannot confirm the huge-page precondition"
	cur=$(cat "$f")
	case "$cur" in
	*'[always]'*) return 0 ;;
	esac
	verdict INCONCLUSIVE \
		"transparent_hugepage/enabled is '$cur', not [always] -- the reproducer never calls MADV_HUGEPAGE, so its default mode would not be huge-page-backed"
}

warn_if_campaign() {
	command -v pgrep >/dev/null 2>&1 || return 0
	if pgrep -f 'syz-executor|syz-fuzzer|syz-manager' >/dev/null 2>&1; then
		note "a syzkaller process is running on this host. It emits the same WARNs, and it clears the dmesg buffer per instance session (issue 006's method notes). Stop the campaign before trusting any verdict here."
	fi
}

# ---------------------------------------------------------------- dmesg ------

read_dmesg() {
	dmesg 2>/dev/null && return 0
	sudo -n dmesg 2>/dev/null && return 0
	return 1
}

dmesg_snap() {	# dmesg_snap <tag>
	read_dmesg > "$OUTDIR/dmesg-$1.txt" 2>/dev/null
}

# dmesg_delta <before-tag> <after-tag>
# Writes only the genuinely new lines to $OUTDIR/dmesg-new.txt. If the ring
# buffer wrapped (or was cleared) between the two snapshots the prefix no longer
# matches, and DMESG_WRAPPED=1 -- in which case counts are not trustworthy and
# the caller must return INCONCLUSIVE rather than NOT-REPRODUCED.
dmesg_delta() {
	local b="$OUTDIR/dmesg-$1.txt" a="$OUTDIR/dmesg-$2.txt" n="$OUTDIR/dmesg-new.txt" bl al
	DMESG_WRAPPED=0
	: > "$n"
	[ -f "$b" ] && [ -f "$a" ] || { DMESG_WRAPPED=1; return 0; }
	bl=$(wc -l < "$b")
	al=$(wc -l < "$a")
	if [ "$al" -ge "$bl" ] && head -n "$bl" "$a" | cmp -s - "$b"; then
		tail -n +$((bl + 1)) "$a" > "$n"
	else
		# shellcheck disable=SC2034
		DMESG_WRAPPED=1
		cp "$a" "$n"
	fi
}

dmesg_count() {	# dmesg_count <ERE>  -> count of matching NEW lines
	grep -cE "$1" "$OUTDIR/dmesg-new.txt" 2>/dev/null || true
}

# ------------------------------------------------------------------ run ------

# run_bounded <secs> <logfile> <cmd...>; sets RC. A hard outer bound on top of
# the reproducers' own alarm(): rc=124 is timeout(1), rc=3 is their alarm.
run_bounded() {
	local secs=$1 log=$2
	shift 2
	RC=0
	timeout -k 5 "$secs" "$@" > "$log" 2>&1 || RC=$?
	printf -- '--- %s %s (rc=%d) ---\n' "$(basename "$1")" "${2:-}" "$RC"
	sed 's/^/    /' "$log"
	if [ "$RC" = 3 ]; then
		note "rc=3 is the reproducer's own alarm() + _exit(3): it HUNG. _exit() does not flush stdio, so an empty log above is expected and is NOT a clean run."
	fi
}
