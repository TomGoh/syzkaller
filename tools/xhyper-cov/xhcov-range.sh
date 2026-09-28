#!/usr/bin/env bash
# Print the .text range of an ELF for libxhcov.so's text_start/text_end args.
# Output (exactly one line): text_start=0x<..> text_end=0x<..>
#   text_start = .text sh_addr
#   text_end   = sh_addr + sh_size, rounded up to 8 bytes
set -u

prog=$(basename "$0")

if [ $# -ne 1 ]; then
    echo "usage: $prog <elf-file>" >&2
    exit 1
fi
elf=$1

if ! command -v readelf >/dev/null 2>&1; then
    echo "$prog: readelf not found in PATH" >&2
    exit 1
fi
if [ ! -r "$elf" ]; then
    echo "$prog: cannot read '$elf'" >&2
    exit 1
fi

# -W keeps one line per section; strip the "[Nr]" prefix, then pick the row
# whose name is exactly ".text": fields become name type addr off size ...
row=$(readelf -S -W "$elf" 2>/dev/null \
      | sed -e 's/^[[:space:]]*\[[[:space:]]*[0-9]*\][[:space:]]*//' \
      | awk '$1 == ".text" { print $3, $5; exit }')

if [ -z "$row" ]; then
    echo "$prog: no .text section found in '$elf'" >&2
    exit 1
fi

addr=${row% *}
size=${row#* }
start=$((16#$addr))
end=$((start + 16#$size))
end=$(((end + 7) / 8 * 8))
printf 'text_start=0x%x text_end=0x%x\n' "$start" "$end"
