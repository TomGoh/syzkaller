#!/bin/sh
# Arm the pKVM EL2-coverage ring on the owner CPU, at boot.
#
# WHY THIS EXISTS (the #1 silent failure): the ring is disarmed on every boot and
# nr_pages resets to the default (4). A weeks-long campaign reboots the target on
# crashes; after the first reboot the ring is disarmed and EL2 coverage silently
# goes to ZERO -- no error, /cover just stops growing on the .rs side. This unit
# re-arms it automatically so the successor never has to remember the manual
# `taskset -c 0 sh -c 'echo 1 > .../enable'`.
#
# Idempotent + CAMPAIGN-SAFE: if the ring is already armed on the target CPU with
# the target page count, it does NOTHING (it never disarms a live ring -- disarm
# does synchronize_rcu and the operator caveat is "disarm only when idle"). So it
# is safe to run at any time, including while a manager is fuzzing.
set -u
COV=/sys/kernel/debug/kvm/pkvm_cov
CPU="${PKVM_COV_OWNER_CPU:-0}"
PAGES="${PKVM_COV_PAGES:-32}"
log() { logger -t pkvm-cov-arm "$*" 2>/dev/null; echo "pkvm-cov-arm: $*"; }

# pkvm_cov (built into KVM) can appear a little after userspace starts; wait for it.
i=0
while [ ! -e "$COV/stats" ] && [ "$i" -lt 60 ]; do sleep 1; i=$((i+1)); done
if [ ! -e "$COV/stats" ]; then
	log "pkvm_cov debugfs absent after ${i}s -- kernel may lack CONFIG_PKVM_EL2_COV; NOT arming"
	exit 1
fi

owner() { awk '/^owner_cpu/{print $2}' "$COV/stats" 2>/dev/null; }
pages() { awk '/^ring_pages/{print $2}' "$COV/stats" 2>/dev/null; }

if [ "$(owner)" = "$CPU" ] && [ "$(pages)" = "$PAGES" ]; then
	log "already armed (owner_cpu=$(owner) ring_pages=$(pages)) -- leaving as-is (campaign-safe)"
	exit 0
fi

# Fresh arm (boot path). Set page count, then enable pinned to the owner CPU so
# owner_cpu == the CPU the executor is taskset-pinned to (see manager pkvm_owner_cpu).
echo 0 > "$COV/enable" 2>/dev/null || true
if ! echo "$PAGES" > "$COV/nr_pages"; then log "failed to set nr_pages=$PAGES"; exit 1; fi
if ! taskset -c "$CPU" sh -c "echo 1 > $COV/enable"; then log "failed to arm on CPU $CPU"; exit 1; fi

if [ "$(owner)" = "$CPU" ]; then
	log "armed: owner_cpu=$(owner) ring_pages=$(pages)"
	exit 0
fi
log "arm attempted but owner_cpu=$(owner) (expected $CPU) -- CHECK executor CPU pinning"
exit 1
