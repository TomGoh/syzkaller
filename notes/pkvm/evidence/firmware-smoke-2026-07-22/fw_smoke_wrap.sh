#!/bin/bash
# Arm a kprobe on pkvm_mem_abort (fault IPA = *x1, return = retval), run the bounded smoke, read the trace.
T=/sys/kernel/tracing
[ -d "$T" ] || T=/sys/kernel/debug/tracing
echo > "$T/kprobe_events" 2>/dev/null
echo 'p:fw_abort_in pkvm_mem_abort ipa=+0(%x1):x64' >> "$T/kprobe_events" || { echo "kprobe add failed"; exit 1; }
echo 'r:fw_abort_ret pkvm_mem_abort ret=$retval:s64' >> "$T/kprobe_events"
echo 1 > "$T/events/kprobes/fw_abort_in/enable"
echo 1 > "$T/events/kprobes/fw_abort_ret/enable"
echo > "$T/trace"
dmesg -C
echo "=== running fw_smoke (hard timeout 8s) ==="
timeout -s KILL 8 ./fw_smoke; echo "smoke exit: $?"
echo "=== pkvm_mem_abort trace (fault IPA + return) ==="
grep -E "fw_abort_(in|ret)" "$T/trace" | head -20
echo "=== dmesg WARN/BUG after run ==="
dmesg | grep -icE "WARNING|BUG:|panic|call trace"
# cleanup
echo 0 > "$T/events/kprobes/fw_abort_in/enable" 2>/dev/null
echo 0 > "$T/events/kprobes/fw_abort_ret/enable" 2>/dev/null
echo > "$T/kprobe_events" 2>/dev/null
echo "=== board still alive ==="; uptime
