#!/usr/bin/env bash
# Launch long/overnight pKVM fuzzing campaigns, one syz-manager per board.
#
#   overnight-run.sh status          what is running / are the boards reachable
#   overnight-run.sh start fleet     ONE manager driving BOTH boards (preferred)
#   overnight-run.sh start d3000     start a single board on its own manager
#   overnight-run.sh start n90
#   overnight-run.sh start all       start every board that is reachable AND armed
#   overnight-run.sh stop [target]   SIGINT the manager(s) so the corpus is flushed
#
# `fleet` is the preferred mode: one manager, both boards as `vm.targets`, one
# shared corpus and one coverage number, so the two machines cooperate instead
# of duplicating each other's work.
#
# It is only CORRECT while both boards boot the *same* vmlinux. syz-manager
# carries a single `kernel_obj` and symbolizes every PC it receives against that
# one binary; point it at boards running different builds and half the coverage
# is attributed to the wrong symbols, silently, with no error anywhere. Both
# boards were unified onto 6.6.30-covfix (built from common-stage2mvp) on
# 2026-07-29 for exactly this reason. If they ever diverge again, fall back to
# the per-board managers below and check with:
#   strings -a <kernel_obj>/vmlinux | grep -m1 '^Linux version'   # vs uname -a
# Compare the embedded build string, never mtimes — a restore from a build
# backup rewrites mtimes without changing content.
set -euo pipefail

SYZ=/home/jose/syzkaller-pkvm
MGR="$SYZ/bin/syz-manager"
KEY=/home/jose/.ssh/id_ed25519

# board  ip             config                                                        log
declare -A IP=(   [n90]=10.42.27.17                       [d3000]=10.42.27.18 )
declare -A CFG=(  [n90]=/home/jose/syzkaller/workdir-macrocov-thru/maxcov.cfg
                  [d3000]=/home/jose/syzkaller/workdir-d3000-overnight/d3000.cfg
                  [fleet]=/home/jose/syzkaller/workdir-fleet-overnight/fleet.cfg )
declare -A LOGD=( [n90]=/home/jose/syzkaller/workdir-macrocov-thru
                  [d3000]=/home/jose/syzkaller/workdir-d3000-overnight
                  [fleet]=/home/jose/syzkaller/workdir-fleet-overnight )
# boards a given target drives — fleet drives both
declare -A BOARDS=( [n90]="n90" [d3000]="d3000" [fleet]="n90 d3000" )

ssh_t() { timeout 20 ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no \
                        -o LogLevel=ERROR -i "$KEY" "root@$1" "$2" 2>/dev/null; }

# A board is only worth fuzzing if it answers SSH *and* its EL2 coverage ring is
# armed. An unarmed ring yields a campaign that runs happily and records nothing
# from the hypervisor — the exact silent failure pkvm-cov-arm.service exists to
# prevent, so check rather than assume it fired.
check() {
    # Split these: bash expands every word of a `local` command line BEFORE
    # making any assignment, so `local b=$1 ip=${IP[$b]}` would resolve $b to
    # the CALLER's b (bash is dynamically scoped), not to $1.
    local b=$1
    local ip=${IP[$b]} rel ring
    rel=$(ssh_t "$ip" 'uname -r') || { echo "$b ($ip): UNREACHABLE"; return 1; }
    ring=$(ssh_t "$ip" 'cat /sys/kernel/debug/kvm/pkvm_cov/stats 2>/dev/null | head -1')
    if [[ -z "$ring" ]]; then
        echo "$b ($ip): kernel $rel — pkvm_cov MISSING (no EL2 coverage; run pkvm-cov-arm.sh)"
        return 1
    fi
    echo "$b ($ip): kernel $rel — $ring — ready"
}

# Match the manager BINARY, not any process whose command line happens to
# contain the config path — a shell running `pgrep -f "syz-manager.*<cfg>"`
# matches itself, so the naive form reports a dead manager as alive (and, worse,
# makes `start` refuse to launch). -x against the exact binary path avoids that.
running() { pgrep -a -f "^$MGR -config $1\$" | head -1; }

start() {
    local b=$1
    [[ -n "${CFG[$b]:-}" ]] || { echo "unknown target: $b" >&2; return 1; }
    if running "${CFG[$b]}" >/dev/null; then
        echo "$b: already running — $(running "${CFG[$b]}")"; return 0
    fi
    # A manager owns its boards exclusively; a per-board manager still holding a
    # board would fight the fleet manager over the same executor dir and ring.
    local other bd1 bd2
    for other in "${!CFG[@]}"; do
        [[ $other == "$b" ]] && continue
        running "${CFG[$other]}" >/dev/null || continue
        for bd1 in ${BOARDS[$b]}; do
            for bd2 in ${BOARDS[$other]}; do
                if [[ $bd1 == "$bd2" ]]; then
                    echo "$b: refusing — '$other' is already driving $bd1. Stop it first:" >&2
                    echo "    $0 stop $other" >&2
                    return 1
                fi
            done
        done
    done
    local bd
    for bd in ${BOARDS[$b]}; do check "$bd" || { echo "$b: not started"; return 1; }; done

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
        for b in n90 d3000; do check "$b" || true; done
        for t in fleet n90 d3000; do
            r=$(running "${CFG[$t]}") && echo "  manager[$t]: $r" || echo "  manager[$t]: not running"
        done ;;
    start) if [[ $board == all ]]; then for b in n90 d3000; do start "$b" || true; done
           else start "$board"; fi ;;
    stop)  if [[ $board == all ]]; then for b in fleet n90 d3000; do stop "$b"; done
           else stop "$board"; fi ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
