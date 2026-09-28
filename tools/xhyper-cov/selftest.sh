#!/usr/bin/env bash
# Selftest for libxhcov.so: bare-metal AArch64 payload runs 3s at EL2
# (virt,virtualization=on) and 3s at EL1 (plain virt). The plugin must
# set a handful of coverage bits for the EL2 run and exactly 0 for EL1.
set -u
cd "$(dirname "$0")"

make -s libxhcov.so || { echo "SELFTEST FAIL (build)"; exit 1; }

cat > st_payload.S <<'EOF'
    .section .text
    .globl _start
_start:
    mov x0, #0
1:
    add x0, x0, #1
    cmp x0, #4096
    b.lt 1b
    mov x1, #0
2:
    add x1, x1, #1
    cmp x1, #4096
    b.lt 2b
    b .
EOF

as -o st_payload.o st_payload.S || { echo "SELFTEST FAIL (as)"; exit 1; }
ld -e _start -Ttext=0x40080000 -o st_payload.elf st_payload.o \
    || { echo "SELFTEST FAIL (ld)"; exit 1; }
objcopy -O binary st_payload.elf payload.bin \
    || { echo "SELFTEST FAIL (objcopy)"; exit 1; }

rm -f st_el2.* st_el1.*

QEMU=qemu-system-aarch64
COVARGS=text_start=0x40080000,text_end=0x40090000

# EL2 run (exit 124 from timeout is expected)
timeout 3 $QEMU -M virt,virtualization=on -cpu max -smp 1 -m 128M \
    -display none -serial null -no-reboot -kernel payload.bin \
    -plugin ./libxhcov.so,${COVARGS},shm=./st_el2 >/dev/null 2>st_el2.err
rc_el2=$?

# EL1 run
timeout 3 $QEMU -M virt -cpu max -smp 1 -m 128M \
    -display none -serial null -no-reboot -kernel payload.bin \
    -plugin ./libxhcov.so,${COVARGS},shm=./st_el1 >/dev/null 2>st_el1.err
rc_el1=$?

if [ $rc_el2 -ne 0 ] && [ $rc_el2 -ne 124 ]; then
    echo "qemu (EL2 run) exited rc=$rc_el2:"; cat st_el2.err
fi
if [ $rc_el1 -ne 0 ] && [ $rc_el1 -ne 124 ]; then
    echo "qemu (EL1 run) exited rc=$rc_el1:"; cat st_el1.err
fi

python3 - <<'PYEOF'
import glob, struct, sys

MAGIC, HDR = 0x323030564F434858, 4096

def popcount(prefix):
    total, files = 0, sorted(glob.glob(prefix + ".*[0-9]"))
    if not files:
        print(f"{prefix}: NO OUTPUT FILES")
    for f in files:
        with open(f, "rb") as fh:
            data = fh.read()
        if len(data) < HDR + 8:
            print(f"{f}: too small ({len(data)} bytes)")
            continue
        magic, ver, cpu, text_start, gran, nwords = struct.unpack_from("<6Q", data, 0)
        if magic != MAGIC:
            print(f"{f}: bad magic {magic:#x}")
            continue
        if nwords == 0 or len(data) < HDR + nwords * 8:
            print(f"{f}: bad nwords={nwords} size={len(data)}")
            continue
        words = struct.unpack_from(f"<{nwords}Q", data, HDR)
        c = sum(v.bit_count() for v in words)
        print(f"{f}: version={ver} cpu={cpu} text_start={text_start:#x} "
              f"granularity={gran} nwords={nwords} set_bits={c}")
        total += c
    return total

el2 = popcount("st_el2")
el1 = popcount("st_el1")
print(f"EL2_COUNT={el2}")
print(f"EL1_COUNT={el1}")
if 0 < el2 < 64 and el1 == 0:
    print("SELFTEST PASS")
    sys.exit(0)
print("SELFTEST FAIL")
sys.exit(1)
PYEOF
exit $?
