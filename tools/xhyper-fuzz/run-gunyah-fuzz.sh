#!/bin/bash
# End-to-end syzkaller fuzzing of XHyper's /dev/gunyah under QEMU TCG.
# Usage: run-gunyah-fuzz.sh [smoke|fuzz]   (default: smoke)
set -e
SZ=/home/jose/Dev/syzkaller
XF=/home/jose/xhyper-workspace/tools/xhyper-fuzz
CFG="$XF/gunyah-qemu.cfg"
MODE="${1:-smoke}"
# The guest initramfs is pure-static (no libc/loader): the executor MUST be
# plain static and NON-PIE. syzkaller's arm64 default is -static-pie, which
# segfaults at startup in this guest, so override it here.
( cd "$SZ" && make manager && \
  make TARGETOS=linux TARGETARCH=arm64 executor execprog CXXFLAGS="-no-pie" LDFLAGS="-static -no-pie" )
LOCK=/home/jose/xhyper-workspace/.optee-gate.lock
if [ "$MODE" = smoke ]; then
  flock "$LOCK" "$SZ/bin/syz-manager" -config "$CFG" -mode smoke-test -debug
else
  flock "$LOCK" "$SZ/bin/syz-manager" -config "$CFG"
fi
