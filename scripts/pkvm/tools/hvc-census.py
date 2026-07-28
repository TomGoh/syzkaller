#!/usr/bin/env python3
"""HVC-hit census — which pKVM host->hyp handlers the campaign actually drives.

Once the macro auto-injection instruments every host->hyp HVC, the fuzzer's own
coverage tells you which handlers fire. This cross-references the campaign's
corpus coverage against the 66-entry HOST_HCALL dispatch table to produce a
measured FIRED / NEVER-FIRED list — turning boundary selection from code-reading
guesswork into data (see notes/pkvm/a1-macro-cov-injection.md).

Method: for each `handle___*` symbol in the vmlinux [addr,addr+size), count how
many covered PCs (from /rawcover, filtered to the __hyp_text range) fall inside.
count>0 => the handler ran. Attribution is at handle_* granularity; the
NEVER-FIRED list (zero covered PCs) is the reliable signal.

Caveats:
  - /rawcover is the CORPUS union (deflake-stable saved programs), so rare events
    that never produce stable new signal (e.g. the VMID-rollover __kvm_flush_vm_context)
    can be undercounted. It measures "what the fuzzer's saved inputs drive".
  - Only owner-CPU + in-KCOV-window HVCs are captured (the right denominator).

Usage:
  # with the manager running (default http://127.0.0.1:56750):
  ./hvc-census.py [VMLINUX] [RAWCOVER_SRC]
    VMLINUX       default /home/jose/common-stage2mvp/vmlinux  (must match the running kernel)
    RAWCOVER_SRC  a file path, or an http URL to /rawcover (default the local manager)
  # examples:
  ./hvc-census.py
  ./hvc-census.py /path/to/vmlinux http://10.42.27.16:56750/rawcover
  ./hvc-census.py /path/to/vmlinux ./rawcover.txt

NM defaults to aarch64-linux-gnu-nm (override via $NM). The HOST_HCALL TABLE below
is kernel-specific (common-stage2mvp hyp_main.rs); update it if the table changes.
"""
import os, re, sys, subprocess, urllib.request

VM = sys.argv[1] if len(sys.argv) > 1 else "/home/jose/common-stage2mvp/vmlinux"
SRC = sys.argv[2] if len(sys.argv) > 2 else "http://127.0.0.1:56750/rawcover"
NM = os.environ.get("NM", "aarch64-linux-gnu-nm")

# HOST_HCALL slot -> handler name (hyp_main.rs). Slot 0 is None (__kvm_hyp_init).
TABLE = {
 1:"__kvm_get_mdcr_el2",2:"__pkvm_init",3:"__pkvm_create_private_mapping",4:"__pkvm_cpu_set_vector",
 5:"__kvm_enable_ssbs",6:"__vgic_v3_init_lrs",7:"__vgic_v3_get_gic_config",8:"__kvm_flush_vm_context",
 9:"__kvm_tlb_flush_vmid_ipa",10:"__kvm_tlb_flush_vmid_ipa_nsh",11:"__kvm_tlb_flush_vmid",
 12:"__kvm_tlb_flush_vmid_range",13:"__kvm_flush_cpu_context",14:"__pkvm_alloc_module_va",
 15:"__pkvm_map_module_page",16:"__pkvm_unmap_module_page",17:"__pkvm_init_module",18:"__pkvm_register_hcall",
 19:"__pkvm_iommu_init",20:"__pkvm_prot_finalize",21:"__pkvm_host_share_hyp",22:"__pkvm_host_unshare_hyp",
 23:"__pkvm_host_map_guest",24:"__pkvm_host_unmap_guest",25:"__pkvm_relax_perms",26:"__pkvm_wrprotect",
 27:"__pkvm_dirty_log",28:"__pkvm_tlb_flush_vmid",29:"__kvm_adjust_pc",30:"__kvm_vcpu_run",
 31:"__kvm_timer_set_cntvoff",32:"__vgic_v3_save_vmcr_aprs",33:"__vgic_v3_restore_vmcr_aprs",
 34:"__pkvm_init_vm",35:"__pkvm_init_vcpu",36:"__pkvm_start_teardown_vm",37:"__pkvm_finalize_teardown_vm",
 38:"__pkvm_reclaim_dying_guest_page",39:"__pkvm_vcpu_load",40:"__pkvm_vcpu_put",41:"__pkvm_vcpu_sync_state",
 42:"__pkvm_update_clock_tracing",43:"__pkvm_load_tracing",44:"__pkvm_teardown_tracing",45:"__pkvm_enable_tracing",
 46:"__pkvm_reset_tracing",47:"__pkvm_swap_reader_tracing",48:"__pkvm_enable_event",49:"__pkvm_selftest_event",
 50:"__pkvm_sync_ftrace",51:"__pkvm_disable_ftrace",52:"__pkvm_hyp_alloc_mgt_refill",
 53:"__pkvm_hyp_alloc_mgt_reclaimable",54:"__pkvm_hyp_alloc_mgt_reclaim",55:"__pkvm_host_iommu_alloc_domain",
 56:"__pkvm_host_iommu_free_domain",57:"__pkvm_host_iommu_attach_dev",58:"__pkvm_host_iommu_detach_dev",
 59:"__pkvm_host_iommu_map_pages",60:"__pkvm_host_iommu_unmap_pages",61:"__pkvm_host_iommu_iova_to_phys",
 62:"__pkvm_host_hvc_pd",63:"__pkvm_stage2_snapshot",64:"xcore_stats_entry",65:"xcore_unit_test_entry",
}
AREA = {
 1:"init/finalize",2:"init/finalize",3:"init/finalize",4:"init/finalize",5:"init/finalize",20:"init/finalize",
 6:"vgic",7:"vgic",32:"vgic",33:"vgic",
 8:"tlb/vmid/cache",9:"tlb/vmid/cache",10:"tlb/vmid/cache",11:"tlb/vmid/cache",12:"tlb/vmid/cache",
 13:"tlb/vmid/cache",28:"tlb/vmid/cache",
 14:"module-load",15:"module-load",16:"module-load",17:"module-load",18:"module-load",
 19:"iommu",55:"iommu",56:"iommu",57:"iommu",58:"iommu",59:"iommu",60:"iommu",61:"iommu",
 21:"host<->hyp share",22:"host<->hyp share",
 23:"guest map/donate/reclaim",24:"guest map/donate/reclaim",38:"guest map/donate/reclaim",
 25:"permissions/dirty",26:"permissions/dirty",27:"permissions/dirty",
 29:"vcpu run/state",30:"vcpu run/state",31:"vcpu run/state",39:"vcpu run/state",40:"vcpu run/state",41:"vcpu run/state",
 34:"vm/vcpu lifecycle",35:"vm/vcpu lifecycle",36:"vm/vcpu lifecycle",37:"vm/vcpu lifecycle",
 **{k:"tracing/ftrace/events" for k in range(42,52)},
 52:"hyp-alloc-mgt",53:"hyp-alloc-mgt",54:"hyp-alloc-mgt",62:"misc",63:"snapshot",64:"xcore",65:"xcore",
}

def read_rawcover(src):
    if src.startswith("http://") or src.startswith("https://"):
        return urllib.request.urlopen(src, timeout=60).read().decode()
    return open(src).read()

# nm: EL2 range + handler [addr,addr+size)
nm = subprocess.run([NM, "-S", VM], capture_output=True, text=True).stdout
lo = hi = None
ranges = {}
hre = re.compile(r'(handle___[a-z0-9_]+?)17h[0-9a-f]+E')
for line in nm.splitlines():
    p = line.split()
    if len(p) >= 3 and p[-1] == "__hyp_text_start": lo = int(p[0], 16)
    if len(p) >= 3 and p[-1] == "__hyp_text_end": hi = int(p[0], 16)
    if len(p) == 4 and p[2] in ("t", "T"):
        m = hre.search(p[3])
        if m:
            a = int(p[0], 16); ranges[m.group(1)[len("handle_"):]] = (a, a + int(p[1], 16))
        elif "xcore_stats_entry" in p[3]:
            a = int(p[0], 16); ranges["xcore_stats_entry"] = (a, a + int(p[1], 16))
        elif "xcore_unit_test_entry" in p[3]:
            a = int(p[0], 16); ranges["xcore_unit_test_entry"] = (a, a + int(p[1], 16))
if lo is None or hi is None:
    sys.exit("FATAL: __hyp_text_start/end not found — wrong vmlinux?")

pcs = sorted({int(l, 16) for l in read_rawcover(SRC).split() if l.strip().startswith("0x")})
el2 = [p for p in pcs if lo <= p < hi]

fired, never, missing = [], [], []
for slot in sorted(TABLE):
    name = TABLE[slot]; r = ranges.get(name)
    area = AREA.get(slot, "?")
    if r is None:
        missing.append((slot, name, area)); continue
    n = sum(1 for p in el2 if r[0] <= p < r[1])
    (fired if n > 0 else never).append((slot, name, area, n))

print(f"vmlinux={VM}")
print(f"EL2 range [0x{lo:x},0x{hi:x})  total PCs={len(pcs)}  EL2 PCs={len(el2)}  handler-syms={len(ranges)}")
print(f"\n=== FIRED ({len(fired)}) — handlers the corpus already drives ===")
for slot, name, area, n in fired: print(f"  #{slot:<3}{name:<34}{area:<26}pcs={n}")
print(f"\n=== NEVER FIRED ({len(never)}) — the coverage gap (reliable signal) ===")
for slot, name, area, n in never: print(f"  #{slot:<3}{name:<34}{area}")
if missing:
    print(f"\n=== SYMBOL NOT FOUND ({len(missing)}) — inlined/renamed; inconclusive ===")
    for slot, name, area in missing: print(f"  #{slot:<3}{name:<34}{area}")
from collections import Counter
print("\n=== never-fired count by area (input-expansion targets) ===")
for a, c in Counter(area for _, _, area, _ in never).most_common(): print(f"  {c:>2}  {a}")
