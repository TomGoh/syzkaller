#!/usr/bin/env python3
"""Assemble a pKVM fuzzing delivery report.

The headline number a pKVM deliverable needs is NOT syzkaller's own "N PCs
covered" -- that counts host kernel basic blocks and says nothing about the
hypervisor. It is:

    how many of the EL2 hypercall handlers that a process holding /dev/kvm can
    actually reach were exercised

so this script derives BOTH halves of that fraction from the kernel source
rather than hardcoding either:

  denominator  the EL2 dispatch table (HOST_HCALL in the Rust hypervisor) says
               which ids have a handler at all; dispatch also rejects every id
               below hcall_min before looking the table up, so the ids below it
               are unreachable no matter what userspace does. Reachable = has a
               handler AND id >= hcall_min.

  numerator    EL2 program counters seen in the run, bucketed into handler
               symbol ranges taken from vmlinux.

Hardcoding "30" would have been shorter and would silently rot the first time
the table or hcall_min changed.

Usage:
  make-report.py --workdir DIR --vmlinux FILE --klinux DIR [--manager URL]
                 [--board USER@HOST] [--out DIR]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone


def run(argv, timeout=60):
    """Run argv with no shell. Paths come from the command line and can contain
    spaces; a shell would also make them injectable."""
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.stdout
    except Exception as e:
        return f"<<failed: {e}>>"


def ssh(board, remote_cmd, timeout=60):
    if not board:
        return ""
    return run(["ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes",
                board, remote_cmd], timeout=timeout)


def fetch(url, timeout=30):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return f"<<unavailable: {e}>>"


# ---------------------------------------------------------------- denominator


def dispatch_table(klinux):
    """id -> handler name, indexed by POSITION in the HOST_HCALL array.

    NOT by the "// N" comments beside the entries: those are off by two from the
    real hypercall id (handle___kvm_adjust_pc is commented // 28 while the
    generated enum puts __kvm_adjust_pc at 26), and dispatch indexes the array
    directly -- `HOST_HCALL[id]` -- so position is what decides reachability.
    Reading the comments produced 46 "reachable" ids out of a 50-slot array with
    a floor of 18, which is arithmetically impossible and is how the bug showed.
    """
    p = os.path.join(klinux, "arch/arm64/kvm/hyp/xhypervisor/src/hyp_main.rs")
    src = open(p, encoding="utf-8", errors="replace").read()
    # "\n\s*\];" -- the array closes with " ];", a leading space, and anchoring
    # on a bare "\n];" ran the match on to a later one and swept in 18 extra
    # tokens.
    m = re.search(r"static HOST_HCALL\s*:\s*\[Option<HcallFn>;\s*(\d+)\]\s*=\s*\[(.*?)\n\s*\];",
                  src, re.S)
    if not m:
        sys.exit(f"could not find HOST_HCALL in {p}")
    declared = int(m.group(1))
    table, idx = {}, 0
    # Strip line comments FIRST. The array carries 20 commented-out entries
    # (disabled iommu and tracing hypercalls) written as "//Some(...)", and a
    # tokenizer that does not know about comments counts them as slots: 50 real
    # elements + 20 commented = the 70 that tripped the size guard below.
    #
    # Then EVERY surviving element advances the index, including the ones that
    # are not kvm_cpu_context::handle_* -- the tail holds Some(xcore_stats_entry)
    # and friends, and skipping those would shift every id after them.
    body = "\n".join(line.split("//")[0] for line in m.group(2).splitlines())
    for tok in re.finditer(r"\bNone\b|\bSome\([^()]*\)", body):
        e = re.search(r"kvm_cpu_context::(handle_[A-Za-z0-9_]+)", tok.group(0))
        if e:
            table[idx] = e.group(1)
        idx += 1
    if idx != declared:
        sys.exit(f"HOST_HCALL declares {declared} slots but {idx} were parsed -- "
                 "the array layout changed, fix the parser rather than trusting this")
    return table


def hcall_min(klinux):
    p = os.path.join(klinux, "arch/arm64/kvm/hyp/xhypervisor/src/bindings/bindings_generated.rs")
    src = open(p, encoding="utf-8", errors="replace").read()
    m = re.search(r"___pkvm_prot_finalize:\s*__kvm_host_smccc_func\s*=\s*(\d+)", src, re.S)
    if not m:
        sys.exit("could not read hcall_min (__pkvm_prot_finalize) from bindings")
    return int(m.group(1))


# ------------------------------------------------------------------ numerator


def handler_ranges(vmlinux):
    """handler name -> (lo, hi) link-address range, from the nvhe symbols."""
    out = run(["nm", "-S", "--size-sort", vmlinux], timeout=300)
    ranges = {}
    for line in out.splitlines():
        if "__kvm_nvhe_" not in line or "handle_" not in line:
            continue
        f = line.split()
        if len(f) < 4:
            continue
        addr, size, name = int(f[0], 16), int(f[1], 16), f[3]
        # Rust mangles as ..._ZN...handle___pkvm_foo17h<hash>E; C is plain.
        m = re.search(r"(handle_[a-z0-9_]+?)(?:17h[0-9a-f]+E)?$", name)
        if m:
            ranges[m.group(1)] = (addr, addr + size)
    return ranges


def covered_handlers(pcs, ranges):
    hit = set()
    items = sorted(ranges.items(), key=lambda kv: kv[1][0])
    for pc in pcs:
        for name, (lo, hi) in items:
            if lo <= pc < hi:
                hit.add(name)
                break
    return hit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--vmlinux", required=True)
    ap.add_argument("--klinux", required=True)
    ap.add_argument("--manager", default="http://localhost:56760")
    ap.add_argument("--board", default="")
    ap.add_argument("--config", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    out = a.out or os.path.join(a.workdir, "report")
    os.makedirs(out, exist_ok=True)

    table = dispatch_table(a.klinux)
    hmin = hcall_min(a.klinux)
    reachable = {i: n for i, n in table.items() if i >= hmin}
    below = {i: n for i, n in table.items() if i < hmin}

    raw = fetch(a.manager + "/rawcover", timeout=120)
    pcs = []
    if not raw.startswith("<<"):
        open(os.path.join(out, "rawcover.txt"), "w").write(raw)
        for line in raw.split():
            try:
                pcs.append(int(line, 16))
            except ValueError:
                pass

    ranges = handler_ranges(a.vmlinux)
    hit = covered_handlers(set(pcs), ranges)
    hit_reachable = sorted(n for n in reachable.values() if n in hit)
    missing = sorted(n for n in reachable.values() if n not in hit)

    stats = fetch(a.manager + "/stats")
    open(os.path.join(out, "stats.html"), "w").write(stats)
    syscalls = fetch(a.manager + "/syscalls")
    open(os.path.join(out, "syscalls.html"), "w").write(syscalls)

    ring = ""
    kver = ""
    if a.board:
        ring = ssh(a.board, "cat /sys/kernel/debug/kvm/pkvm_cov/stats")
        kver = ssh(a.board, "uname -r; uptime")
        open(os.path.join(out, "pkvm_cov-stats.txt"), "w").write(ring)

    crashdir = os.path.join(a.workdir, "crashes")
    crashes = sorted(os.listdir(crashdir)) if os.path.isdir(crashdir) else []
    crash_titles = []
    for c in crashes:
        d = os.path.join(crashdir, c, "description")
        if os.path.isfile(d):
            crash_titles.append(open(d).read().strip())

    now = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M %z")
    L = []
    L.append("# pKVM 模糊测试交付报告\n")
    L.append(f"生成时间：{now}\n")
    L.append("## 1. 结论\n")
    verdict = "**通过**" if not crashes else "**未通过**"
    L.append(f"- 崩溃数：**{len(crashes)}** → {verdict}")
    L.append(f"- EL2 hypercall handler 覆盖：**{len(hit_reachable)} / {len(reachable)}** 个可达 handler")
    L.append("")
    L.append("崩溃数为 0 是本次运行的**预期结果**，且这个结果是有意义的：同一内核上"
             "复现套件已证明 7 个已知缺陷全部存在（见第 5 节），所以零崩溃说明调用面"
             "筛选生效，而不是内核本身没有问题。\n")

    L.append("## 2. EL2 可达面与覆盖\n")
    L.append(f"派发表 `HOST_HCALL` 共 {len(table)} 个有 handler 的 id；"
             f"`hcall_min = {hmin}`，EL2 在查表前就拒绝所有 id < {hmin} 的调用，"
             f"因此其中 **{len(below)}** 个从 `/dev/kvm` 不可达。"
             f"**可达面 = {len(reachable)}**。\n")
    L.append("| 状态 | handler |")
    L.append("| --- | --- |")
    for n in hit_reachable:
        L.append(f"| ✅ 已覆盖 | `{n}` |")
    for n in missing:
        L.append(f"| ⬜ 未覆盖 | `{n}` |")
    L.append("")

    L.append("## 3. EL2 覆盖率数据链完整性\n")
    if ring:
        L.append("```")
        L.append(ring.strip())
        L.append("```")
        g = dict(re.findall(r"^(\w+)\s+(-?\d+)$", ring, re.M))
        try:
            b, sn, sk, dr = (int(g[k]) for k in
                             ("begin_calls", "skip_not_owner", "skip_no_kcov", "drains"))
            ok = "成立" if abs((b - sn - sk) - dr) <= 16 else "**不成立**"
            L.append(f"\n自洽性 `drains == begin_calls - skip_not_owner - skip_no_kcov`："
                     f"{b} - {sn} - {sk} = {b-sn-sk} vs drains {dr} → {ok}"
                     "（差值为快照时在飞行中的窗口，个位数属正常）")
        except (KeyError, ValueError):
            L.append("\n自洽性检查：计数器字段不完整，跳过")
    else:
        L.append("未采集（未提供 --board）")
    L.append("")

    L.append("## 4. 运行环境\n")
    L.append("```")
    L.append(kver.strip() or "(未采集)")
    L.append("```")
    if a.config:
        L.append(f"\n配置：`{a.config}`")
    L.append("")

    L.append("## 5. 已知缺陷状态\n")
    L.append("本次运行的目标内核**不含任何 pKVM 修复**。同一内核上复现套件的结果："
             "`delivery/evidence/2026-08-10-repro-pack-summary.txt`。\n")
    L.append("交付 config 通过收窄调用面来回避这些缺陷，理由逐条写在 "
             "`delivery/n90-delivery.cfg` 的注释里。**一旦本次运行出现崩溃，"
             "要么是筛选有洞，要么是新缺陷 —— 两种都要查。**\n")

    if crash_titles:
        L.append("本次出现的崩溃：\n")
        for t in crash_titles:
            L.append(f"- {t}")
        L.append("")

    L.append("## 6. 附件\n")
    for f in ("rawcover.txt", "stats.html", "syscalls.html", "pkvm_cov-stats.txt"):
        if os.path.isfile(os.path.join(out, f)):
            L.append(f"- `{f}`")

    rp = os.path.join(out, "report.md")
    open(rp, "w", encoding="utf-8").write("\n".join(L) + "\n")
    print(f"wrote {rp}")
    print(f"EL2 handlers: {len(hit_reachable)}/{len(reachable)} reachable covered; "
          f"{len(crashes)} crash(es); {len(pcs)} raw PCs")


if __name__ == "__main__":
    main()
