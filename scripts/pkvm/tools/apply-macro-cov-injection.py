#!/usr/bin/env python3
"""pKVM EL2 coverage: macro auto-injection.

1. Revert the 5 per-site #ifdef CONFIG_PKVM_EL2_COV wraps (mmu.c #23,
   pkvm.c init_vcpu/reclaim/start_teardown/init_vm) back to plain calls.
2. Inject KVM_PKVM_COV_HVC into kvm_call_hyp_nvhe (kvm_host.h) and
   kvm_call_refill_hyp_nvhe (kvm_pkvm.h) so every host->hyp HVC is auto-wrapped.
"""
import re, sys

K = "/home/jose/common-stage2mvp"

# ---- The injected macro block (identical in both headers; #ifndef makes it idempotent) ----
COV_BLOCK = (
"/*\n"
" * pKVM EL2 coverage auto-injection. Wrapping the one host->hyp HVC primitive\n"
" * captures EL2 (Rust hyp) coverage for EVERY boundary that funnels through\n"
" * kvm_call_hyp_nvhe() / kvm_call_hyp() / kvm_call_hyp_ret() (nVHE path) and\n"
" * kvm_call_refill_hyp_nvhe() -- no per-call-site edit. pkvm_cov_begin() self-\n"
" * guards (ring armed + owner CPU + KCOV active), so only the fuzzer task's HVCs\n"
" * on the owner CPU arm the ring; every other HVC is a cheap no-op. The wrap is\n"
" * atomic (one HVC, no sleep between begin/end) so it never holds the ring across\n"
" * a sleep. Host-only + config-gated; hyp builds and !CONFIG_PKVM_EL2_COV get the\n"
" * bare arm_smccc_1_1_hvc().\n"
" */\n"
"#ifndef KVM_PKVM_COV_HVC\n"
"#if defined(CONFIG_PKVM_EL2_COV) && !defined(__KVM_NVHE_HYPERVISOR__)\n"
"#include <asm/kvm_pkvm_cov.h>\n"
"#define KVM_PKVM_COV_HVC(_res, f, ...)\t\t\t\t\t\\\n"
"\tdo {\t\t\t\t\t\t\t\t\\\n"
"\t\tstruct pkvm_cov_ctx __cov;\t\t\t\t\\\n"
"\t\tbool __cov_on = pkvm_cov_begin(&__cov);\t\t\t\\\n"
"\t\tarm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f),\t\t\\\n"
"\t\t\t\t  ##__VA_ARGS__, &(_res));\t\t\\\n"
"\t\tif (__cov_on)\t\t\t\t\t\t\\\n"
"\t\t\tpkvm_cov_end(&__cov);\t\t\t\t\\\n"
"\t} while (0)\n"
"#else\n"
"#define KVM_PKVM_COV_HVC(_res, f, ...)\t\t\t\t\t\\\n"
"\tarm_smccc_1_1_hvc(KVM_HOST_SMCCC_FUNC(f), ##__VA_ARGS__, &(_res))\n"
"#endif\n"
"#endif /* KVM_PKVM_COV_HVC */\n"
"\n"
)

def revert_wraps(path):
    with open(path) as f:
        lines = f.readlines()
    out, i, n, changed = [], 0, len(lines), 0
    while i < n:
        if lines[i].strip() == "#ifdef CONFIG_PKVM_EL2_COV":
            j, depth, else_idx, endif_idx = i + 1, 0, None, None
            while j < n:
                s = lines[j].strip()
                if s.startswith("#if"):
                    depth += 1
                elif s == "#else" and depth == 0 and else_idx is None:
                    else_idx = j
                elif s == "#endif":
                    if depth == 0:
                        endif_idx = j; break
                    depth -= 1
                j += 1
            if else_idx is not None and endif_idx is not None:
                cov_body = "".join(lines[i + 1:else_idx])
                if "pkvm_cov_begin" in cov_body:
                    out.extend(lines[else_idx + 1:endif_idx])
                    changed += 1
                    i = endif_idx + 1
                    continue
        out.append(lines[i]); i += 1
    with open(path, "w") as f:
        f.writelines(out)
    return changed

def inject_macro(path, marker):
    text = open(path).read()
    if "KVM_PKVM_COV_HVC(res, f" in text:
        print(f"  {path}: already injected, skipping"); return 0
    idx = text.index(marker)
    before, after = text[:idx], text[idx:]
    pat = re.compile(
        r'arm_smccc_1_1_hvc\(KVM_HOST_SMCCC_FUNC\(f\),[ \t]*\\\n'
        r'[ \t]*##__VA_ARGS__, &res\);([ \t]*\\)')
    after, nsub = pat.subn(r'KVM_PKVM_COV_HVC(res, f, ##__VA_ARGS__);\1', after, count=1)
    if nsub != 1:
        sys.exit(f"ERROR: expected 1 arm_smccc replacement in {path}, got {nsub}")
    open(path, "w").write(before + COV_BLOCK + after)
    return nsub

r1 = revert_wraps(f"{K}/arch/arm64/kvm/mmu.c")
r2 = revert_wraps(f"{K}/arch/arm64/kvm/pkvm.c")
print(f"reverted wraps: mmu.c={r1} pkvm.c={r2} (expect 1 and 4)")
assert r1 == 1 and r2 == 4, "revert count mismatch"

inject_macro(f"{K}/arch/arm64/include/asm/kvm_host.h", "#define kvm_call_hyp_nvhe(f, ...)")
inject_macro(f"{K}/arch/arm64/include/asm/kvm_pkvm.h", "#define kvm_call_refill_hyp_nvhe(f, ...)")
print("injected KVM_PKVM_COV_HVC into kvm_host.h + kvm_pkvm.h")
