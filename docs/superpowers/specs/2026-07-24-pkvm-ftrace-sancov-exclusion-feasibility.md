# Rust hyp ftrace/trace SanCov 排除 —— board-free 可行性实验

日期：2026-07-24
范围：完全 board-free，独立 worktree，不部署 N90，不改主 prototype，不启动 campaign。
原始数据：`evidence/ftrace-sancov-feasibility-2026-07-24/`

---

## 1. 结论

第一步测得 hyp 自己的 `ftrace.rs` / `trace.rs` 占**运行时**已投递 EL2 PC 的 58.5%。第二步之后 ring 已零截断，所以排除它们的收益不再是「减少截断」，而是**提高每个 ring 槽位和每条 KCOV 条目的信息密度**。

结论是好的，而且比预想的更干净：

> **不需要任何 SanCov ignorelist 或 `#[coverage(off)]` 的按模块排除机制。噪声源 `CONFIG_PROTECTED_NVHE_FTRACE` 本身就是一个 Kconfig，且上游默认 `n`；fuzz config 里被显式打开成了 `=y`。把它关掉（回到上游默认）就干净地移除了 Step-1 运行时噪声的主体，其余模块 SanCov 完好、闭环不退化。**

也就是说，colleague 决策树的第一支（「先检查是否已有可关闭 hyp trace/ftrace 的 Kconfig」）命中，第二支（模块级排除机制）**无需进入**。

**唯一 board-free 无法证明的**：运行时降幅的具体数值（Step-1 的 58.5%）。已证明的是：Step-1 运行时前 5 名里的 4 个函数在 `=n` 下被移除或改成空桩，机制成立；数值需要一次 N90 运行确认（已按 board-free 约束推迟）。

---

## 2. 方法

独立 git worktree `/home/jose/common-ftrace-expt`，base `da966ce9a047` + 规范 `stage2-el2-kcov.patch` + `stage2.config`，**只在两次构建之间切换 `CONFIG_PROTECTED_NVHE_FTRACE` 这一个 Kconfig**，其余（`PKVM_EL2_COV=y`、`X1_RMS=y`、`KCOV=y`、`RANDOMIZE_BASE=n`）不变。实验后 worktree 已删除，主 prototype 未动。

度量对象是 `nvhe_rust.o` 里所有 `bl __sanitizer_cov_trace_pc` 的静态调用点（`R_AARCH64_CALL26` 重定位），用该对象自带的 DWARF 逐点归因到函数（脚本 `evidence/.../sancov-attrib.sh`）。

**一个方法上的坑必须说清楚：按源文件归因不可靠。** ftrace 的 SanCov 调用点经内联后，DWARF 会把它们归到内联展开处或 extern "C" 名下，于是「ftrace.rs/trace.rs 文件桶」在 `=y` 和 `=n` 下都显示 29，看似没变。**正确的度量是按函数（symbol）归因**——它清楚显示 ftrace 函数整组丢失了 SanCov。所以下文用的是按函数的结果，不是按文件的。

---

## 3. 结果

### 3.1 静态调用点

| CONFIG_PROTECTED_NVHE_FTRACE | nvhe_rust.o 里 `__sanitizer_cov_trace_pc` 静态调用点 |
|---|---|
| `=y`（fuzz 现状） | 7311 |
| `=n`（上游默认） | 7167（**−144**） |

### 3.2 完全丢失 SanCov 的 10 个函数——全部是 ftrace/trace

```
nvhe_rust::ftrace::hyp_ftrace_patch                     32
nvhe_rust::hyp_main::…::handle___pkvm_disable_ftrace    19
nvhe_rust::ftrace::hyp_ftrace_setup                     19
nvhe_rust::ftrace::__get_enable_disable_ins_from_funcs_pg  9
__hyp_ftrace_trace                                       8
nvhe_rust::ftrace::hyp_ftrace_ret_flush                  7
__hyp_ftrace_ret_trace                                   6
nvhe_rust::ftrace::__get_enable_disable_ins_early        4
nvhe_rust::ftrace::__get_offset_idx_ins                  1
nvhe_rust::ftrace::__get_disable_ins                     1
```

**非 ftrace/trace 函数被完全移除 instrumentation 的：0 个**（criterion 2 满足）。

其余非 ftrace 函数只有由「移除 `-fpatchable-function-entry` 钩子」带来的**微小 codegen 抖动**（如 `__pkvm_init_finalise`、`__host_stage2_set_owner_locked` 的基本块计数小幅增减），没有任何一个被剥掉 instrumentation；净差 −144 全部可归因于 ftrace。

### 3.3 与 Step-1 运行时噪声的对应

`=n` 移除了这些**符号**：`__kvm_nvhe___hyp_ftrace_trace`、`__kvm_nvhe___hyp_ftrace_ret_trace`（各 1→0）；并把 `trace_func` / `trace_func_ret` 变成空桩（`#[cfg(not(CONFIG_PROTECTED_NVHE_FTRACE))]`，`trace.rs:2100-2106`）。

这四个正是 Step-1 运行时前 5 名里的 4 个（`__hyp_ftrace_trace` 3104、`hyp_ftrace_func_push` 3103、`trace_func` 3103、`__hyp_ftrace_ret_trace` 3082、`trace_func_ret` 3082）。它们靠 per-function-entry 钩子（patchable-function-entry）在每次 hyp 函数进入/退出时被调用，所以少量静态点却产生海量运行时 PC。`=n` 同时去掉了这些函数**和**调用它们的钩子。

### 3.4 闭环不退化（criterion 3）

`=n` 构建：
- callback `__kvm_nvhe___sanitizer_cov_trace_pc` 仍链接（1 个符号）；
- DWARF：`__pkvm_init_vm → pkvm.rs:4014`、`__pkvm_host_share_hyp → permissions.rs:251`、`handle___pkvm_host_map_guest` 符号存在——与 `=y` 逐字相同。

---

## 4. 为什么这是正确且安全的做法

- **上游默认就是 `n`。** Kconfig `default n`；是 fuzz config 显式开成 `=y`。关掉它 = 回到上游默认，本质安全。
- **对 fuzz 目标零损失。** ftrace 是 hyp 的**自追踪调试设施**，与 KCOV/SanCov 冗余；被 fuzz 的是 pKVM 安全逻辑，不是 tracing 基础设施。#23 donation/map 路径不依赖 ftrace（`trace_host_hcall` 等仅在 dispatch 尾部记录事件，桩化后不跳过任何真实工作）。
- **不引入脆弱机制。** 不用 clang `-fsanitize-coverage-ignorelist=`（走 `SpecialCaseList`，`-C llvm-args` 基本到不了），也不用 `#[coverage(off)]`（属 `-C instrument-coverage`/instrprof，与 SanCov 是两套机制）。这两条 fallback **无需进入**。
- **可复现。** 一个 Kconfig 开关，非默认项即上游默认；`git diff` 一行。

代价：失去对 ftrace 控制类 hypercall（`handle___pkvm_disable_ftrace` 等）的覆盖。它们是调试 hypercall、非核心安全逻辑、当前 fuzz 面不触及；若将来要专门 fuzz 它们，用另一个 config 即可。

---

## 5. 建议与后续

**建议：从 fuzz `.config` 中去掉 `CONFIG_PROTECTED_NVHE_FTRACE`（让其回落到上游默认 `n`）。** 这是一次 config 变更，不改任何 Stage-2 源码，因此不影响已冻结的 producer/host-glue/consumer 闭环。

**采纳前需要的唯一 board 步骤**（本实验按约定未做）：在 N90 上用同一固定 smoke 跑一次 `=n` 内核，确认：
1. `el2_hits` / 已投递 EL2 PC 里 ftrace.rs/trace.rs 的占比从 ~58.5% 降到接近 0；
2. `LOST_IN_RING` 仍为 0、闭环仍产出 Rust `.rs:line`、132 之外新增的 pKVM-逻辑 unique PC（如有）；
3. 无 WARN/BUG。

这次 board 运行可以和后续 3A/3B 的部署合并，不必单独占用一次重启。

按 colleague 的次序：ftrace 实验到此结束（board-free 部分已完成、结论明确）。接下来并行推进 **3A 串行 executor + CPU pin**、**3B 可生成 Host composite 输入面**、**EFI pstore ↔ isolated backend**；**3A/3B 完成前不启动 EL2 manager campaign**。
