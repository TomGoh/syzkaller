#!/bin/bash
# a2 EL2-coverage set algebra + symbolization — RUNS ON THE HOST (needs vmlinux).
#
# Consumes the per-run EL2 PC sets (<label>.<i>.el2) that a2-capture.sh produced
# on N90 and pulled here, then computes the a2 metrics from the plan:
#   B_union  = ∪ Sᵢ   (the diff reference — the baseline's full reach)
#   B_common = ∩ Sᵢ   (stable core; instability = |union| − |common|)
# For a candidate, pass the baseline union as <baseline_union>:
#   confirmed new coverage = C_common − B_union   (stable in the candidate AND
#   never in any baseline run); probabilistic-only = (C_union − B_union) − C_common.
#
# Usage:
#   a2-analyze.sh pull   <n90_out_dir> <local_dir>
#   a2-analyze.sh crunch <label> <N> <local_dir> [baseline_union_file]
set -u
VM="${VMLINUX:-/home/jose/common-stage2mvp/vmlinux}"
A2L="${A2L:-aarch64-linux-gnu-addr2line}"
N90="${N90:-root@10.42.27.17}"

sym_lines() {  # stdin: PCs -> stdout: distinct hyp/nvhe/rust/src/*.rs:line
  "$A2L" -e "$VM" -f 2>/dev/null | paste - - | grep -oE 'hyp/nvhe/rust/src/[^ ]+\.rs:[0-9]+' | sort -u
}

cmd="${1:?pull|crunch}"; shift
case "$cmd" in
  pull)
    n90dir="${1:?n90_out_dir}"; local="${2:?local_dir}"; mkdir -p "$local"
    rsync -a --no-owner --no-group "$N90:$n90dir/" "$local/"
    echo "pulled $(ls "$local"/*.el2 2>/dev/null | wc -l) el2 sets + stats -> $local"
    ;;
  crunch)
    label="${1:?label}"; N="${2:?N}"; DIR="${3:?local_dir}"; BASE="${4:-}"
    ls "$DIR/$label".*.el2 >/dev/null 2>&1 || { echo "no $DIR/$label.*.el2 files"; exit 1; }
    # Only NON-EMPTY runs count: a dropped run (e.g. KCOV-remote EEXIST) must not
    # zero the intersection. neff = successful runs; intersection is over those.
    neff=0; for f in "$DIR/$label".*.el2; do [ -s "$f" ] && neff=$((neff+1)); done
    cat "$DIR/$label".*.el2 | sort -u > "$DIR/$label.union"
    cat "$DIR/$label".*.el2 | sort | uniq -c | awk -v n="$neff" '$1==n{print $2}' | sort > "$DIR/$label.common"
    u=$(wc -l < "$DIR/$label.union"); c=$(wc -l < "$DIR/$label.common")
    echo "== $label (N=$N, successful=$neff) =="
    [ "$neff" -lt "$N" ] && echo "  WARNING: $((N-neff)) run(s) produced no EL2 coverage (dropped from intersection)"
    echo "  B_union=$u  B_common=$c  instability=$((u-c))"
    sort "$DIR/$label.union" | sym_lines > "$DIR/$label.lines"
    echo "  distinct EL2 .rs:line (union): $(wc -l < "$DIR/$label.lines")"
    if [ -n "$BASE" ] && [ -f "$BASE" ]; then
      comm -23 "$DIR/$label.common" <(sort "$BASE") > "$DIR/$label.new_pcs"
      np=$(wc -l < "$DIR/$label.new_pcs")
      echo "  CONFIRMED NEW PCs (C_common − B_union): $np"
      if [ "$np" -gt 0 ]; then
        sort "$DIR/$label.new_pcs" | sym_lines > "$DIR/$label.new_lines"
        echo "  CONFIRMED NEW .rs:line ($(wc -l < "$DIR/$label.new_lines")):"
        sed 's/^/    + /' "$DIR/$label.new_lines"
      else
        echo "  -> no confirmed new coverage vs baseline (negative result for this candidate)"
      fi
    fi
    ;;
  *) echo "usage: $0 pull|crunch ..."; exit 2 ;;
esac
