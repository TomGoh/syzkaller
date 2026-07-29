#!/usr/bin/env bash
# Launch long/overnight pKVM fuzzing campaigns, one syz-manager per board.
#
#   overnight-run.sh status          what is running / are the boards reachable
#   overnight-run.sh start d3000     start one board
#   overnight-run.sh start n90
#   overnight-run.sh start all       start every board that is reachable AND armed
#   overnight-run.sh stop [board]    SIGINT the manager(s) so the corpus is flushed
#
# Why one manager per board and not one manager with two `vm.targets`:
# syz-manager carries a single `kernel_obj`, and it symbolizes every PC it
# receives against that one vmlinux. The boards currently run *different*
# builds — N90 the instrumented common-stage2mvp kernel, D3000 6.6.30-pkvmfix
# out of ksrc-pkvmfix (instrumented AND carrying the unmap deadlock fix). Point
# one manager at both and half the coverage is symbolized against the wrong
# binary, silently. Merge them into one campaign only once both boards boot the
# same vmlinux.
set -euo pipefail

SYZ=/home/jose/syzkaller-pkvm
MGR="$SYZ/bin/syz-manager"
KEY=/home/jose/.ssh/id_ed25519

# board  ip             config                                                        log
declare -A IP=(   [n90]=10.42.27.17                       [d3000]=10.42.27.18 )
declare -A CFG=(  [n90]=/home/jose/syzkaller/workdir-macrocov-thru/maxcov.cfg
                  [d3000]=/home/jose/syzkaller/workdir-d3000-overnight/d3000.cfg )
declare -A LOGD=( [n90]=/home/jose/syzkaller/workdir-macrocov-thru
                  [d3000]=/home/jose/syzkaller/workdir-d3000-overnight )

ssh_t() { timeout 20 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
                        -o LogLevel=ERROR -i "$KEY" "root@$1" "$2" 2>/dev/null; }

# A board is only worth fuzzing if it answers SSH *and* its EL2 coverage ring is
# armed. An unarmed ring yields a campaign that runs happily and records nothing
# from the hypervisor — the exact silent failure pkvm-cov-arm.service exists to
# prevent, so check rather than assume it fired.
check() {
    local b=$1 ip=${IP[$b]} rel ring
    rel=$(ssh_t "$ip" 'uname -r') || { echo "$b ($ip): UNREACHABLE"; return 1; }
    ring=$(ssh_t "$ip" 'cat /sys/kernel/debug/kvm/pkvm_cov/stats 2>/dev/null | head -1')
    if [[ -z "$ring" ]]; then
        echo "$b ($ip): kernel $rel — pkvm_cov MISSING (no EL2 coverage; run pkvm-cov-arm.sh)"
        return 1
    fi
    echo "$b ($ip): kernel $rel — $ring — ready"
}

running() { pgrep -af "syz-manager.*$1" | head -1; }

start() {
    local b=$1
    [[ -n "${IP[$b]:-}" ]] || { echo "unknown board: $b" >&2; return 1; }
    if running "${CFG[$b]}" >/dev/null; then
        echo "$b: already running — $(running "${CFG[$b]}")"; return 0
    fi
    check "$b" || { echo "$b: not started"; return 1; }

    local n=1
    while [[ -e "${LOGD[$b]}/overnight$n.log" ]]; do n=$((n+1)); done
    local log="${LOGD[$b]}/overnight$n.log"

    # Absolute paths throughout: the manager resolves `syzkaller`/`workdir` from
    # the config, but a relative invocation has bitten this project before.
    nohup "$MGR" -config "${CFG[$b]}" >"$log" 2>&1 &
    echo "$b: started pid $! -> $log"
}

stop() {
    local b=$1 pid
    pid=$(pgrep -f "syz-manager.*${CFG[$b]}" | head -1) || true
    if [[ -z "$pid" ]]; then echo "$b: not running"; return 0; fi
    # SIGINT, not SIGKILL — the manager flushes corpus.db on a clean shutdown.
    kill -INT "$pid"; echo "$b: SIGINT sent to $pid (corpus flushing)"
}

cmd=${1:-status}; board=${2:-all}
case "$cmd" in
    status)
        for b in n90 d3000; do
            check "$b" || true
            r=$(running "${CFG[$b]}") && echo "    manager: $r" || echo "    manager: not running"
        done ;;
    start) if [[ $board == all ]]; then for b in n90 d3000; do start "$b" || true; done
           else start "$board"; fi ;;
    stop)  if [[ $board == all ]]; then for b in n90 d3000; do stop "$b"; done
           else stop "$board"; fi ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
