#!/bin/bash
# Rebuild every board-side binary AT THE SAME REVISION and push them together.
#
# syzkaller refuses to run when the manager/execprog and the executor were built
# from different revisions -- and it only says so after the RPC handshake, i.e.
# after a program has already appeared to hang:
#
#   aborting RPC server: mismatching manager/executor git revisions
#
# Rebuilding one and forgetting the other cost three debugging rounds in a single
# day, each time looking like "the workload produced no coverage" rather than
# like a version error. Always go through this script.
set -euo pipefail
BOARD="${1:-10.42.27.17}"
cd "$(git rev-parse --show-toplevel)"
make TARGETOS=linux TARGETARCH=arm64 executor execprog >/dev/null
make manager >/dev/null
rev=$(git rev-parse HEAD)
# A dirty tree makes syzkaller embed "<sha>+", and the binaries ALSO carry a
# 40-hex descriptions hash that changes whenever sys/*.txt does -- so anchor on
# the sha itself and allow the trailing "+", rather than taking the first 40-hex
# string in the binary (which silently became the descriptions hash the first
# time a syscall was added).
for b in bin/linux_arm64/syz-executor bin/linux_arm64/syz-execprog; do
	got=$(strings "$b" | grep -oE "^${rev}\+?$" | head -1)
	[ -n "$got" ] || { echo "$b was not built at HEAD ($rev) -- aborting" >&2; exit 1; }
done
case "$(strings bin/linux_arm64/syz-executor | grep -oE "^${rev}\+?$" | head -1)" in
*+) echo "NOTE: tree is dirty; binaries are HEAD+uncommitted changes" >&2 ;;
esac
scp -q bin/linux_arm64/syz-executor bin/linux_arm64/syz-execprog "root@$BOARD:/tmp/"
ssh "root@$BOARD" 'chmod +x /tmp/syz-executor /tmp/syz-execprog'
echo "pushed executor+execprog @ ${rev:0:12} to $BOARD"
