#!/bin/bash
# Issue 007 -- guest_get_page_state() masks the raw PTE instead of the extracted
#              prot, so every 2 MiB block mapping in a protected guest yields a
#              page state no arm of __pkvm_host_reclaim_page() accepts, EL2 hits
#              bug_on!(), and the Rust panic handler is loop {}.
#              signature: board wedges, no crash report at all
#
# hazard: wedges-target, and WORSE THAN 001. There is no host-side recovery: the
# CPU is spinning at EL2 with interrupts masked, so it answers neither IPI nor
# RCU nor NMI. The task cannot be killed, the CPU cannot be offlined, and sysrq
# cannot reach it -- 001 can sometimes still be rescued over a live ssh, this
# cannot. Recovery is a POWER CYCLE, every time.
#
# Verdict source: the probe's OWN phase markers, not dmesg.
#
# That is deliberate. This failure produces no WARN, no Oops and no crash
# report; dmesg only ever shows the downstream cascade (soft lockups in
# unrelated tasks) minutes later, and on a wedged board it may never be
# collectable at all. What IS decisive is whether close(vm) returns, and the
# probe reports that itself:
#
#     D before close(vm)   ... reached the teardown
#     E after close(vm)    ... teardown COMPLETED  -> fixed kernel
#
# D printed and E absent is the defect, and it is unambiguous.
#
# The markers also go to /dev/kmsg, so on a board with a console capture they
# survive the ssh session dying. This script reads them from the probe's own
# output, which is enough when the probe is run locally.

# shellcheck source=common.sh
. "$(dirname "$0")/common.sh"

pack_init 007 "a 2 MiB guest mapping makes EL2 panic, and the panic handler is loop {}"

# Long enough to be sure it is not merely slow: a healthy teardown finishes in
# milliseconds, and the control arm below proves the machine can do it.
TIMEOUT=${TIMEOUT_007:-60}
SRC=$ISSUES_DIR/007-protected-block-mapping-el2-panic/repro/probe-thp-kcov.c

build_repro "$SRC" probe-thp
BIN=$REPRO_BIN
build_only_stop

need_native
need_kvm
need_pkvm
need_thp_always
warn_if_campaign

note "there is NO software recovery from this one. If it reproduces, the board must be power cycled before anything else in this pack is run."

# ---- control arm first --------------------------------------------------
#
# thp=0 asks for MADV_NOHUGEPAGE, so the same protected VM is backed by 4 KiB
# pages and takes the page-descriptor path. It MUST complete. Running it first
# is what separates "this kernel has the defect" from "protected VMs are broken
# here for some unrelated reason" -- without it, a failure in the test arm has
# two explanations and this script could not choose between them.
run_bounded "$TIMEOUT" "$OUTDIR/007-control.log" "$BIN" 0
CTRL_RC=$RC
if ! grep -q 'RPGPROBE: F done' "$OUTDIR/007-control.log" 2>/dev/null; then
	verdict INCONCLUSIVE "the 4 KiB control arm did not complete (rc=$CTRL_RC), so protected VMs do not work on this board for reasons unrelated to 007 -- see $OUTDIR/007-control.log"
fi
note "control arm (4 KiB) completed: protected-VM create/run/teardown works here"

# ---- test arm -----------------------------------------------------------
#
# thp=1 leaves the 2 MiB mapping THP-eligible. Everything else is identical.
run_bounded "$TIMEOUT" "$OUTDIR/007-test.log" "$BIN" 1
TEST_RC=$RC

D=$(grep -c 'RPGPROBE: D before close(vm)' "$OUTDIR/007-test.log" 2>/dev/null || true)
E=$(grep -c 'RPGPROBE: E after close(vm)'  "$OUTDIR/007-test.log" 2>/dev/null || true)
ALIGNED=$(sed -n 's/.*aligned2M=\([01]\).*/\1/p' "$OUTDIR/007-test.log" 2>/dev/null | head -1)
printf 'markers: D(before close)=%s  E(after close)=%s  mmap 2MiB-aligned=%s  rc=%s\n' \
	"$D" "$E" "${ALIGNED:-?}" "$TEST_RC"

if [ "$ALIGNED" = 0 ]; then
	verdict INCONCLUSIVE "the guest mapping did not land 2 MiB aligned, so it could not be block-mapped and the trigger was never issued -- re-run"
fi
if [ "${D:-0}" = 0 ]; then
	verdict INCONCLUSIVE "the probe never reached close(vm) -- it failed earlier, see $OUTDIR/007-test.log"
fi
if [ "${E:-0}" != 0 ]; then
	verdict NOT-REPRODUCED "close(vm) returned and the probe ran to completion with a 2 MiB block mapping, so the teardown reclaim accepted the block-descriptor page state"
fi
verdict REPRODUCED "close(vm) never returned with a 2 MiB block mapping while the 4 KiB control arm completed normally -- EL2 is spinning in its panic handler and this board now needs a POWER CYCLE"
