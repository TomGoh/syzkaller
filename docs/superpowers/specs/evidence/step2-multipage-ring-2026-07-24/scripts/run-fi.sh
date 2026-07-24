#!/bin/bash
# ============================================================================
# Fault-injection acceptance test for the pKVM EL2-coverage buffer lifecycle.
#
# Every check is an explicit assertion on: the enable/disable return value,
# owner_cpu, leaked_bytes, and whether the buffer is left in a usable state.
# A path that "did not crash" is not evidence; these are the invariants.
#
# The three injected failures are NOT symmetric, by design:
#   bit0 / bit1  fake a failure AFTER the real operation succeeded. That is safe
#                because the branch they select only ever LEAKS, so the worst
#                case is 16 KiB wasted.
#   bit2         selects a branch that can FREE, so it fakes nothing: it hands
#                EL2 an nr_pages the hyp must reject, which fails before the
#                hypervisor has pinned or registered anything. The rollback is
#                then the genuine "shared but never registered" path, and the
#                pages MUST be freed, not leaked -> leaked_bytes must NOT move.
# ============================================================================
set -u
D=/sys/kernel/debug/kvm/pkvm_cov
CPU="${CPU:-0}"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); echo "    PASS  $*"; }
bad()  { FAIL=$((FAIL+1)); echo "    FAIL  $*"; }
chk()  { # chk <desc> <actual> <expected>
	if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2', want '$3'"; fi
}
owner()  { cat $D/owner_cpu; }
leaked() { awk '$1=="leaked_bytes"{print $2}' $D/stats; }
enable() { taskset -c "$CPU" sh -c "echo 1 > $D/enable" 2>/dev/null; }   # rc matters
disable(){ echo 0 > $D/enable 2>/dev/null; }

echo "=== FI acceptance — $(date -Is) ==="
disable; echo 0 > $D/fault_inject; echo 4 > $D/nr_pages; echo 1 > $D/stats_reset
dmesg -C > /dev/null
BYTES=$(( $(cat $D/nr_pages) * 4096 ))
echo "buffer = $(cat $D/nr_pages) pages = $BYTES bytes; capacity = $(awk '$1=="ring_capacity_pcs"{print $2}' $D/stats) PCs"

echo
echo "--- 0. baseline: a normal lifecycle works and leaks nothing ---"
enable; chk "enable rc" "$?" "0"
chk "owner_cpu while armed" "$(owner)" "$CPU"
disable
chk "owner_cpu after disable" "$(owner)" "-1"
chk "leaked_bytes" "$(leaked)" "0"

echo
echo "--- 1. bit2: EL2 rejects the setup (must FAIL and FREE, never leak) ---"
base=$(leaked)
echo 4 > $D/fault_inject
enable; rc=$?
[ "$rc" != 0 ] && ok "enable failed as designed (rc=$rc)" || bad "enable should have failed, rc=$rc"
chk "owner_cpu (no buffer registered)" "$(owner)" "-1"
chk "leaked_bytes UNCHANGED (rollback must free)" "$(leaked)" "$base"
chk "injection bit consumed" "$(cat $D/fault_inject)" "0"
grep -q "asking EL2 to set up nr_pages" <(dmesg) && ok "injection logged" || bad "injection not logged"
enable; chk "buffer still usable after a rejected setup" "$?" "0"
chk "owner_cpu re-armed" "$(owner)" "$CPU"
disable

echo
echo "--- 2. bit0: EL2 teardown unconfirmed (must LEAK the whole block) ---"
base=$(leaked)
enable; chk "enable rc" "$?" "0"
echo 1 > $D/fault_inject
disable
chk "owner_cpu after disable" "$(owner)" "-1"
chk "leaked_bytes += whole block" "$(leaked)" "$((base + BYTES))"
grep -q "teardown unconfirmed" <(dmesg) && ok "teardown-unconfirmed logged" || bad "not logged"

echo
echo "--- 3. bit1: unshare unconfirmed (must LEAK the whole block) ---"
base=$(leaked)
enable; chk "enable rc" "$?" "0"
echo 2 > $D/fault_inject
disable
chk "owner_cpu after disable" "$(owner)" "-1"
chk "leaked_bytes += whole block" "$(leaked)" "$((base + BYTES))"
grep -q "unshare unconfirmed" <(dmesg) && ok "unshare-unconfirmed logged" || bad "not logged"
grep -q "over 4/4 pages" <(dmesg) && ok "unshare attempted the FULL range (4/4)" || bad "range-complete unshare not evidenced"

echo
echo "--- 4. recovery: normal lifecycle still works, nothing further leaks ---"
base=$(leaked)
enable; chk "enable rc" "$?" "0"
chk "owner_cpu" "$(owner)" "$CPU"
disable
chk "owner_cpu" "$(owner)" "-1"
chk "leaked_bytes stable" "$(leaked)" "$base"

echo
echo "--- 5. no kernel splats throughout ---"
chk "WARN/BUG splats" "$(dmesg | grep -cE 'WARNING: CPU|Call trace:|BUG:')" "0"
echo 0 > $D/fault_inject

echo
echo "=== dmesg ==="; dmesg | grep pkvm_cov
echo
echo "=== RESULT: $PASS passed, $FAIL failed ==="
[ "$FAIL" = 0 ] || exit 1
