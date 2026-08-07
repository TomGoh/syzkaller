#!/bin/sh
# Catch the EL2 coverage ring of whichever CPU is wedged inside a hypercall.
#
# Must be started BEFORE the trigger and detached: once the board wedges, a new
# ssh login cannot complete (sshd's seccomp setup needs kick_all_cpus_sync, and
# the stuck CPU never answers), so anything that has not already been launched
# is unreachable. Output goes to /dev/kmsg for the same reason -- the already
# established `dmesg --follow` keeps streaming, stdout does not.
#
# A ring whose flags still carry ENABLED (bit 0) is one where pkvm_cov_begin()
# armed the window and pkvm_cov_end() never ran, i.e. an HVC that did not come
# back. Its pcs[] tail is the last EL2 code that executed.
RD=/sys/kernel/debug/kvm/pkvm_cov/ring_dump

emit() { while IFS= read -r l; do echo "$1 $l" > /dev/kmsg; done; }

round() {
	tag="$1"
	cat "$RD" > "/root/ring-$tag.txt" 2>&1
	sync
	# Headers for every CPU: cheap, and shows at a glance which one is stuck.
	grep 'flags=' "/root/ring-$tag.txt" | emit "RINGHDR[$tag]"
	# Only the still-armed CPUs get their PC tail dumped -- dumping all eight
	# would be thousands of printk lines and could itself stall the console.
	for c in $(grep -E 'flags=0x[13] ' "/root/ring-$tag.txt" | sed 's/^cpu\([0-9]*\) .*/\1/'); do
		grep "^cpu$c " "/root/ring-$tag.txt" | tail -70 | emit "RINGPC[$tag]"
	done
	echo "RINGDUMP[$tag] complete" > /dev/kmsg
}

sleep 8
round early
sleep 20
round late
