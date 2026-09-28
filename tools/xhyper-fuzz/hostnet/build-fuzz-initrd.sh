#!/bin/bash
# Repack a gzip newc initramfs with a static dropbear SSH server and /fuzz-init.
# Usage: build-fuzz-initrd.sh <base-initrd> <output-initrd>
# Adds files only. The base image is not modified.
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 <base-initrd> <output-initrd>" >&2
	exit 1
fi

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
BASE=$1
OUT_DIR=$(CDPATH= cd -- "$(dirname "$2")" && pwd)
OUT="$OUT_DIR/$(basename "$2")"
DROPBEAR="$HERE/dropbearmulti"
AUTHKEYS="/home/jose/xhyper-workspace/tools/xhyper-fuzz/hostnet/authorized_keys"

[[ -f "$BASE" ]] || { echo "base initrd not found: $BASE" >&2; exit 1; }
[[ -f "$DROPBEAR" ]] || { echo "static dropbearmulti not found: $DROPBEAR" >&2; exit 1; }
[[ -f "$AUTHKEYS" ]] || { echo "authorized_keys not found: $AUTHKEYS" >&2; exit 1; }

WORKDIR=$(mktemp -d "$HERE/.initrd-build.XXXXXX")
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT
ROOT="$WORKDIR/root"
mkdir -p "$ROOT"

# Unpack as the current user. The archive is repacked with uid 0 below;
# this image has no device nodes, so a non-root extract does not drop any.
gzip -dc "$BASE" | (cd "$ROOT" && cpio -idm --quiet)

install -m 755 "$DROPBEAR" "$ROOT/sbin/dropbearmulti"
ln -s ../../sbin/dropbearmulti "$ROOT/sbin/dropbear"
ln -s ../../sbin/dropbearmulti "$ROOT/sbin/dropbearkey"
ln -s ../../sbin/dropbearmulti "$ROOT/usr/bin/scp"

# glibc getpwnam/initgroups read these directly (files NSS is inside the
# static binary). An empty password field would allow a blank-password login.
if [[ ! -e "$ROOT/etc/passwd" ]]; then
	printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
printf '127.0.0.1 localhost\n::1 localhost\n' > "$ROOT/etc/hosts"
	chmod 644 "$ROOT/etc/passwd"
fi
if [[ ! -e "$ROOT/etc/group" ]]; then
	printf 'root:x:0:\n' > "$ROOT/etc/group"
	chmod 644 "$ROOT/etc/group"
fi
if [[ ! -e "$ROOT/etc/nsswitch.conf" ]]; then
	printf 'passwd: files\ngroup: files\nshadow: files\nhosts: files\n' > "$ROOT/etc/nsswitch.conf"
	chmod 644 "$ROOT/etc/nsswitch.conf"
fi

install -d -m 700 "$ROOT/root" "$ROOT/root/.ssh"
install -m 600 "$AUTHKEYS" "$ROOT/root/.ssh/authorized_keys"

install -d -m 755 "$ROOT/etc/dropbear"
# Host key is generated on the build host. The file format is portable.
"$DROPBEAR" dropbearkey -t ed25519 -f "$ROOT/etc/dropbear/dropbear_ed25519_host_key"
chmod 600 "$ROOT/etc/dropbear/dropbear_ed25519_host_key"

# The host kernel is started with rdinit=/sbin/init (busybox). It never
# executes /init, so the hook has to be an added inittab action. `once`
# runs after rcS and does not block the askfirst console shell.
if ! grep -q '/fuzz-init' "$ROOT/etc/inittab"; then
	printf '\n# Fuzzing SSH server. Added; nothing above is removed.\n::once:/fuzz-init\n' >> "$ROOT/etc/inittab"
fi

cat > "$ROOT/fuzz-init" << 'EOF'
#!/bin/sh
# Bring up the QEMU user-net address and a root-key dropbear, then stay up.
# Started from /etc/inittab (::once:) after the base /etc/init.d/rcS.

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /var/run
# SSH sessions need a pty. devtmpfs alone does not mount devpts.
mount -t devpts devpts /dev/pts 2>/dev/null || true
# syz-executor mmaps its input region at a fixed address (0x20000000) with
# MAP_FIXED_NOREPLACE; ASLR occasionally squats there -> EEXIST. Disable it.
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null || true
# syzkaller scp's syz-executor into /tmp (vm/qemu TargetDir default); the
# initramfs root is writable RAM but has no /tmp, so create it world-writable.
mkdir -p /tmp && chmod 1777 /tmp
mount -t tmpfs tmpfs /tmp 2>/dev/null || true

i=0
while [ ! -e /sys/class/net/eth0 ]; do
	i=$((i + 1))
	if [ "$i" -gt 10 ]; then
		echo "FUZZ_ETH0_MISSING" >&2
		break
	fi
	sleep 1
done
/sbin/ip link set lo up 2>/dev/null || true
/sbin/ip link set eth0 up 2>/dev/null || true
/sbin/ip addr add 10.0.2.15/24 dev eth0 2>/dev/null || true
/sbin/ip route add default via 10.0.2.2 2>/dev/null || true

echo FUZZ_SSHD_UP
/sbin/dropbear -R -E -p 22

while true; do
	sleep 3600
done
EOF
chmod 755 "$ROOT/fuzz-init"

# --owner keeps every entry uid 0, which dropbear requires for ~/.ssh.
(cd "$ROOT" && find . | cpio -o -H newc --owner 0:0 --quiet) | gzip > "$WORKDIR/out.img"
mv -f "$WORKDIR/out.img" "$OUT"
echo "wrote $OUT"
