# scripts/pkvm/tools — pKVM EL2-coverage investigation tools

Reusable tooling from the EL2-coverage fuzzing work. See `notes/pkvm/` for the
runbook, the macro-injection design (`a1-macro-cov-injection.md`), and evidence.

## `hvc-census.py` — HVC-hit census (which host→hyp handlers the campaign drives)
Once the macro auto-injection instruments every host→hyp HVC, the fuzzer's own
coverage reveals which handlers fire. This cross-references the campaign's corpus
coverage (`/rawcover`) against the 66-entry `HOST_HCALL` table to print a measured
**FIRED / NEVER-FIRED** list — boundary selection by data, not code-reading.

```
./hvc-census.py                                   # live: curls http://127.0.0.1:56750/rawcover
./hvc-census.py /path/to/vmlinux ./rawcover.txt   # from a saved rawcover file
./hvc-census.py /path/to/vmlinux http://10.42.27.16:56750/rawcover
```
Needs a `vmlinux` matching the running kernel and `aarch64-linux-gnu-nm` (override `$NM`).
The `HOST_HCALL` TABLE is kernel-specific (common-stage2mvp `hyp_main.rs`); update it if
the dispatch table changes. **Caveats:** it reads the CORPUS union, so rare events that
never produce stable new signal (e.g. the VMID-rollover `__kvm_flush_vm_context`) can be
undercounted; attribution is at `handle_*` granularity, so trust the NEVER-FIRED list.

Baseline result (2026-07-28, campaign corpus): 14 handlers fired
(create/map/share/teardown/vcpu run/load/put/vgic-aprs), 49 never fired — the gap is
dominated by tracing/IOMMU/module-load/hyp-alloc-mgt (dedicated-driver surfaces, not
KVM-composite reachable). See `notes/pkvm/evidence/a1-macro-autoinject-2026-07-28/hvc-census-2026-07-28.txt`.

## `apply-macro-cov-injection.py` — record of the macro auto-injection transform
The one-time editor that produced the macro-injection hunks in the kernel patch
snapshot (`notes/pkvm/evidence/kernel-instrumentation-2026-07-27/stage2-el2-kcov.patch`):
reverts the 5 per-site `#ifdef CONFIG_PKVM_EL2_COV` wraps and injects `KVM_PKVM_COV_HVC`
into `kvm_call_hyp_nvhe` (kvm_host.h) + `kvm_call_refill_hyp_nvhe` (kvm_pkvm.h). Paths are
hardcoded to `/home/jose/common-stage2mvp` — kept as a reproducibility record; the applied
result is the committed patch. See `notes/pkvm/a1-macro-cov-injection.md` for the design.
