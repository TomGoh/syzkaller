# XHyper coverage tooling (phase0-cov)

Host-side tooling around `libxhcov.c`, a QEMU TCG plugin that records an
EL2 coverage bitmap over `[text_start, text_end)`. The default mode records
translation-block start addresses. Passing `trace_pc=<hex>` switches to
SanCov call-site mode (below) so the same bitmap lines up with syzkaller's
coverage report. Both modes ignore EL1: EL1 and EL2 can share high VAs, and
the plugin reads EL from the `cpsr` gdb-core register (PSTATE bits [3:2]).

Coverage is a bitmap, not an execution trace. A free-running hypervisor
re-executes the same few thousand EL2 blocks on the order of a billion times;
a ring of those executions churns and is not a usable per-program set. Bit k
set means the address `text_start + k * granularity` has run at EL2. In the
default mode that address is a TB start; in SanCov mode it is the return
address of a `bl __sanitizer_cov_trace_pc` (call site + 4). The writer only
ORs bits. Each vCPU has two files with the same layout:

- **Window** `<prefix>.<vcpu_index>`: an external reader atomically swaps
  each word to 0 (read-and-clear) to take one program window's covered set.
- **Cumulative** `<prefix>_cum.<vcpu_index>`: the same bits, ORed on every
  EL2 hit and never cleared. syz-manager does not open this file. It is the
  input to an offline "which functions are covered" report.

Files:

| file                | role                                                        |
|---------------------|-------------------------------------------------------------|
| `libxhcov.c`        | QEMU plugin (writer): per-vCPU window + cumulative bitmaps  |
| `libxhcov.so`       | built plugin (`make`)                                       |
| `xhcov-symbolize.sh`| unions `<prefix>_cum.*` and prints a ranked function table  |
| `xhcov-reader.c`    | previous ring drain (magic `XHCOV001`); does not read this bitmap |
| `xhcov-range.sh`    | prints `text_start=`/`text_end=` from an ELF's `.text`      |
| `selftest.sh`       | EL2/EL1 A-B selftest of the plugin                          |

## Build

```sh
make                                            # -> libxhcov.so (needs glib-2.0 dev headers)
```

`xhcov-reader.c` still drains the old ring format and is not part of the
bitmap path.

Optional: `make selftest` runs `./selftest.sh`, which boots a tiny bare-metal
payload for 3 s at EL2 (`-M virt,virtualization=on`) and 3 s at EL1 and checks
the plugin sets coverage bits only for the EL2 run.

## Getting text_start / text_end from a built kernel.elf

```sh
./xhcov-range.sh /path/to/kernel.elf
# text_start=0x40080000 text_end=0x40090000
```

`text_start` is the `.text` section's `sh_addr` (VirtAddr), `text_end` is
`sh_addr + sh_size` rounded up to 8 bytes (parsed from `readelf -S -W`).
The script errors to stderr and exits 1 if the ELF has no `.text`.

## Running QEMU with the plugin

Plugin arguments: `text_start=<hex>`, `text_end=<hex>`, `shm=<prefix>`,
and optionally `trace_pc=<hex>`. Each vCPU gets two files: the window
`<prefix>.<vcpu_index>` and the cumulative `<prefix>_cum.<vcpu_index>`
(for example `shm=/dev/shm/xh0` creates `/dev/shm/xh0.0` and
`/dev/shm/xh0_cum.0`):

```sh
qemu-system-aarch64 -M virt,virtualization=on -cpu max -smp 1 -m 128M \
    -display none -serial null -no-reboot -kernel payload.bin \
    -plugin ./libxhcov.so,text_start=0x40080000,text_end=0x40090000,shm=./rd_test
```

Without `trace_pc`, only TBs whose start vaddr is in `[text_start, text_end)`
are instrumented, and only their EL2 executions set bits. The plugin's hot
path is non-blocking (one relaxed load of the bitmap word, and an OR only
when the bit was clear; no syscalls/locks). Bitmaps are `msync`ed and diag
counters printed to stderr at QEMU exit.

### SanCov call-site mode (`trace_pc=<hex>`)

Pass `trace_pc` as the virtual address of `__sanitizer_cov_trace_pc` in a
SanCov-instrumented XHyper (the same value `nm` prints for that symbol).
The two modes are exclusive: SanCov mode does not set TB-start bits.

At translation time the plugin walks every instruction whose vaddr is in
`[text_start, text_end)`, reads its 4 bytes (`qemu_plugin_insn_data`), and
decodes an AArch64 `BL` (`(enc & 0xFC000000) == 0x94000000`). The branch
target is `iva + SignExtend(imm26 << 2)`. When that target equals `trace_pc`,
the instruction is a SanCov call site and gets a per-instruction execution
callback.

On EL2 execution of that `bl`, the plugin ORs the bit for **`iva + 4`** (the
return address) into both the window and the cumulative bitmap. syzkaller
recovers a call site by subtracting 4 from the PC it is given, so the bitmap
must store the return address, not the `bl` itself and not the TB start.
The file header (magic `XHCOV002`, bitmap at offset 4096, separate cumulative
file) is unchanged, so the syz-manager drainer and `xhcov-symbolize.sh` do
not care which mode produced the bits.

```sh
qemu-system-aarch64 ... \
    -plugin ./libxhcov.so,text_start=0xffff800000005000,text_end=0xffff80000019f008,trace_pc=0xffff800000006000,shm=/tmp/sc
```

## shm file layout (must match `libxhcov.c`)

Both `"<prefix>.<idx>"` (window) and `"<prefix>_cum.<idx>"` (cumulative)
use this layout, little-endian. Total size is `4096 + nwords * 8`, where

```
nwords = ceil(((text_end - text_start) / granularity) / 64)
granularity = 4
```

`ftruncate` sizes the file to exactly that. For the example range
`0x40080000 .. 0x40090000` the span is 65536 bytes, so `nwords = 256` and
the file is 6144 bytes.

### Header (offset 0, 4096 bytes)

All fields are `uint64_t`:

| offset | field          | meaning                                                            |
|-------:|----------------|--------------------------------------------------------------------|
| 0x00   | `magic`        | `0x323030564f434858` ("XHCOV002" as LE bytes)                      |
| 0x08   | `version`      | format version, currently `2`                                      |
| 0x10   | `cpu_index`    | vCPU index this file belongs to (equals the `.<idx>` suffix)       |
| 0x18   | `text_start`   | parsed `text_start`; base for the bit-index formula                |
| 0x20   | `granularity`  | `4` (AArch64 instructions are 4 bytes; TB starts are 4-aligned)    |
| 0x28   | `nwords`       | number of `uint64_t` words in the bitmap                           |
| 0x30.. | `reserved[]`   | zero up to offset 4096                                             |

### Bitmap (offset 4096)

`nwords` little-endian `uint64_t` words. Bit k lives in word `k/64`, bit
`k%64` (mask `1ULL << (k % 64)`). When that bit is set, the address `text_start + k * granularity` has been
covered. In the default mode that is a TB start. In SanCov mode it is the
return address of an executed `bl __sanitizer_cov_trace_pc`. On the window
file that is "since the last clear". On the cumulative file the bit stays
set for the rest of the QEMU process.

The writer, only for an EL2 hit, does this load-check-then-OR on the window
bitmap and then the same operation on the cumulative bitmap (`idx` is
identical, `(vaddr - text_start) / 4`). `vaddr` is the TB start, or in
SanCov mode the call's return address:

```c
uint64_t idx = (vaddr - text_start) / granularity;
uint64_t w = idx >> 6, b = idx & 63, mask = 1ULL << b;
if (idx < nwords * 64 &&
    (__atomic_load_n(&bitmap[w], __ATOMIC_RELAXED) & mask) == 0)
    __atomic_fetch_or(&bitmap[w], mask, __ATOMIC_RELAXED);
```

`vaddr` is the TB start and is already known to be in range. The load check
means a hot loop sets a bit once and then does nothing: no ring churn and no
overflow counter. The writer never clears a bit in either file.

### Read-and-clear

The external reader takes one window (one fuzz-program execution) by
atomically swapping every **window** bitmap word with 0 and decoding the
bits it received. It does not open `<prefix>_cum.<idx>`.

```c
uint64_t got = __atomic_exchange_n(&bitmap[w], 0, __ATOMIC_RELAXED);
```

Each set bit in `got` is one covered address in that window (a TB start, or
in SanCov mode a `bl __sanitizer_cov_trace_pc` return address). The writer and
the reader share no other synchronization; both sides use atomics on the
same `uint64_t` words, and the writer only ORs. A bit the reader has already
swapped out stays clear until the TB executes again, which starts the next
window. The matching cumulative bit is left set.

## Offline function report

`xhcov-symbolize.sh` unions every vCPU cumulative bitmap and ranks functions
by how many covered blocks they contain:

```sh
./xhcov-symbolize.sh /path/to/kernel.elf /dev/shm/xh0_cum
```

The script globs `<cum_prefix>.*`, turns each set bit into
`text_start + k * 4`, batches those addresses through
`addr2line -f -e <elf>`, demangles with `c++filt` (including Rust v0 names),
and prints one row per function, sorted by covered-block count descending:

```
<blocks>	<function>	<file:line of the first covered block>
total covered blocks: N
distinct functions: M
```
