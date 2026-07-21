#!/bin/bash
# pKVM campaign runbook — reproducible, archivable, no-false-crash campaigns on N90.
# Encodes the discipline from the implementation record §6.3/§6.4 (colleague-reviewed).
#
#   ./campaign-runbook.sh preflight              # gate checks + clean live state (run before launch)
#   ./campaign-runbook.sh archive  <run-id>      # snapshot rawcover/corpus/logs/dmesg (run before stop)
#
# Invariants until efi-pstore recovery is adapted (record §6): the manager config MUST keep
#   pstore:false, target_reboot:false, procs:1, and be launched WITHOUT -debug
#   (the vm/vmimpl/util.go LogLevel=ERROR SSH fix is gated on non-debug).
# Each campaign gets a NEW run-id / fresh workdir; never reuse a dir with stale dmesg or crashes/.
set -u
IP="${PKVM_N90:-10.42.27.17}"; KEY="${PKVM_KEY:-$HOME/.ssh/id_ed25519}"
SSH="ssh -o BatchMode=yes -o LogLevel=ERROR -i $KEY root@$IP"
SYZ="$(cd "$(dirname "$0")/../../.." && pwd)"   # syzkaller repo root

preflight() {
  echo "== N90 gate checks =="
  $SSH 'set -e
    id | grep -q "uid=0" && echo "  [ok] root SSH"
    ls /dev/kvm >/dev/null && echo "  [ok] /dev/kvm"
    grep -q "kvm-arm.mode=protected" /proc/cmdline && echo "  [ok] kvm-arm.mode=protected"
    dmesg 2>/dev/null | grep -q "Protected KVM" && echo "  [ok] Protected KVM detected" || echo "  [warn] no Protected-KVM dmesg (need root/earlier boot log)"
    ls /sys/kernel/debug/kcov >/dev/null 2>&1 && echo "  [ok] KCOV debugfs"
  ' || { echo "  [FAIL] gate check failed — do not launch"; return 1; }
  echo "== clean live state (archive old evidence first!) =="
  $SSH 'dmesg -C && echo "  [ok] dmesg cleared"'
  echo "  reminder: move any existing workdir/crashes/ aside before launch (mv workdir/crashes workdir/crashes-<runid>)"
  echo "== provenance to record with the run =="
  echo "  syzkaller: $(cd "$SYZ" && git rev-parse --short HEAD) ($(cd "$SYZ" && git status --porcelain | wc -l) dirty files)"
  echo "  launch: ./bin/syz-manager -config=workdir/pkvm-arm64.cfg   (NO -debug)"
}

archive() {
  local run="${1:?usage: archive <run-id>}"; local d="$SYZ/docs/superpowers/specs/evidence/campaign-$run"
  mkdir -p "$d"
  curl -s http://127.0.0.1:56741/rawcover > "$d/rawcover.txt" && echo "  [ok] rawcover ($(wc -l < "$d/rawcover.txt") PCs)"
  cp "$SYZ/workdir/corpus.db" "$d/" 2>/dev/null && echo "  [ok] corpus.db"
  cp "$SYZ/workdir/manager-supervised.log" "$d/manager.log" 2>/dev/null && echo "  [ok] manager.log"
  $SSH 'dmesg' > "$d/n90-dmesg.txt" 2>/dev/null && echo "  [ok] N90 dmesg"
  cp "$SYZ/docs/superpowers/specs/evidence/campaign-2026-07-21/symbolize.sh" "$d/" 2>/dev/null
  echo "  next: cp rawcover, then ./symbolize.sh <vmlinux> to recompute coverage attribution."
}

case "${1:-}" in
  preflight) preflight ;;
  archive)   archive "${2:-}" ;;
  *) echo "usage: $0 {preflight|archive <run-id>}"; exit 2 ;;
esac
