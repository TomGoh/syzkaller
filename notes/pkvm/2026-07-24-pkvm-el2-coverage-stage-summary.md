# pKVM EL2 coverage-guided fuzzing：阶段性记录

日期：2026-07-24  
工程分支：`pkvm-lifecycle-fuzzing@a7a74097f`  
适用范围：本文记录 pKVM fuzzing 工程线，不替代 `notes/` 下的 syzkaller 源码学习笔记。

---

## 1. 一句话结论

syzkaller 已能通过真实 `/dev/kvm` ioctl 对 protected VM 的 **Host→hyp 生命周期**做 coverage-guided fuzzing；Rust pKVM hyp 的 PC 也已经能在 N90 上经由共享 ring、EL1 KCOV 回流到 syzkaller，并符号化到 Rust 源码行。

不过，这还不是可无人值守长期运行的 EL2 campaign：当前 EL2 结果只来自一个固定输入、一条 EL1→EL2 边界和单 CPU 串行执行。下一阶段要解决的是输入可变性、manager 下的正确归属和崩溃恢复，而不是再次证明闭环是否存在。

## 2. 当前状态

| 工作项 | 状态 | 已经证明的内容 | 尚未证明的内容 |
|---|---|---|---|
| Stage 1 / 1.5 的 Host→hyp 生命周期子目标 | 已完成 | protected VM 的创建、vCPU 初始化、受控运行和 teardown 可由 syzkaller 驱动，并取得 EL1 Host KCOV feedback | guest HVC、SyzOS-in-protected-VM 等 Stage 1.5 其余方向仍未完成 |
| Stage 2 MVP：EL2 coverage 闭环 | 真机 smoke 已通过 | Rust SanCov PC 能写入 ring，经 EL1 drain 写入当前 task 的 KCOV，并映射到 Rust `.rs:line` | 还没有 manager 级、可变输入的 EL2 coverage-guided campaign |
| Step 1：反馈完整性度量 | 已完成 | 找到了真实的丢失层和测量仪器造成的假象 | 结论仅适用于固定 #23 smoke |
| Step 2：多页 ring 与生命周期 | 已完成并关闭 | 4 页 ring 的容量、清理路径和 fault injection 已在 N90 验证 | 新输入面或新 boundary 出现后仍要重新量测 |
| Rust `ftrace.rs` / `trace.rs` SanCov 排除 | 待做 | 这些自插桩 PC 是可观测的噪声来源 | Rust crate 级 SanCov 是否能按模块排除尚未验证 |
| EL2 manager campaign | 待做 | — | 串行执行模式、可生成输入面、长期恢复能力尚未齐备 |

## 3. 已完成的两条路径

### 3.1 Host 侧：受控的 pKVM 生命周期 fuzzing

syzkaller 的 syzlang 和 executor helper 已能构造 protected VM 生命周期。关键操作不是把所有原始 ioctl 无限制暴露给生成器，而是用 composite pseudo-call 在 executor 的 C helper 内完成有前后依赖的状态序列。这样可以绕开 syzkaller resource subtype 只能表达资源兼容、不能强制表达“已经运行过”等时序前提的限制。

N90 上的受控 campaign 达到：

- corpus：`0 → 93`；
- manager 的累计 Host KCOV signal：`0 → 6407`；
- `300k+` 次执行，约 `39 exec/s`；
- 约两小时内未出现新的 crash。

这里的 `6407` 不是 EL2 Rust 行覆盖，也不是纯 pKVM 代码覆盖：其中包含 executor 和普通 Host 内核路径。它证明的是 manager 的 Host-side coverage-guided feedback 确实能驱动 corpus 增长。已覆盖的目标侧路径包括 `pkvm_init_host_vm`、`pkvm_create_hyp_vm` 和 `pkvm_destroy_hyp_vm` 等。

已知的通用 arm64 KVM warning 没有被“修复”：BUG1 通过安全 `VCPU_INIT` 和 allowlist 避开；BUG2 属于 normal-VM dirty-log 路径，不是当前 protected-VM EL2 目标的一部分。

### 3.2 EL2 侧：最小 coverage 闭环

当前 MVP 选择 #23，即 `__pkvm_host_map_guest` 这条 host→hyp hypercall 边界。固定的 `syz_kvm_run_fw_fault` smoke 会让 pvmfw 首次执行并在 firmware IPA 处触发 guest fault；Host 的 `pkvm_mem_abort()` 因而调用这条 hypercall。

时间线如下：

```text
executor 的 KVM_RUN
  → EL1: pkvm_mem_abort()
  → EL1: pkvm_cov_begin() 清空并 arm owner CPU 的共享 ring
  → EL2: __pkvm_host_map_guest（#23）
  → EL2: Rust SanCov callback 记录返回 PC 到 ring
  → 返回 EL1: pkvm_cov_end() disarm、runtime-PC 转 link-PC
  → EL1: kcov_add_pcs() 写入发起 KVM_RUN 的 current task 的 KCOV area
  → executor 读 KCOV，syz-cover/addr2line 符号化到 Rust `.rs:line`
```

此设计不要求 EL2 存在 Linux `current`。EL2 只在 owner CPU 的共享 ring 中追加 PC；归属在同步回到 EL1 后恢复，因为 #23 仍在发起 `KVM_RUN` 的 executor 线程中执行，`kcov_add_pcs()` 看到的 `current` 就是应接收这批 PC 的 task。

## 4. Step 1：两层“截断”假设被测量推翻

### 4.1 下游 KCOV area 并不是当前瓶颈

最初的 smoke 出现过 `524287` 项 KCOV 打满。后续的 1A / 1A+ / 1B 测量表明，这不是 EL2 coverage 本身造成的容量问题：

- 约 84% 的满载条目来自 pl011 串口 busy-poll；触发源是 bridge 热路径内的 overflow printk。一行 printk 在 115200 baud 串口下约可带来十万条 KCOV。
- `pkvm_cov.o` 曾被 KCOV 插桩；其 drain 循环搬运 EL2 PC 时又给自己产生大量 EL1 自噪声。将该对象排除后，这部分归零。
- 将 executor 的 `kCoverSize` 从 512K 增至 1M 没有增加任何 unique EL2 PC。
- 清除上述测量干扰后，`kcov_requested == kcov_accepted`，`LOST_IN_KCOV_AREA == 0`。

因此，原先为 per-`KVM_RUN` EL1 dedup 设计的预分配 hash table 没有当前数据支持。它不是被永久删除，而是推迟到真实 campaign 重新观测到 KCOV 压力时再讨论。

准确的约束是：**被测路径内部不能打印大量日志**。这不等于板子必须没有 serial console；桥接代码去掉热路径 printk 后，loud/quiet console 运行结果已经收敛。

### 4.2 真正的丢失来自上游 ring

对固定 #23 smoke 的 12 次 1B 测量中：

- 每次 `KVM_RUN` 有 78 个 #23 drain window；
- 12 次测量中记录的 `el2_hits` 量级约为 `28,178`，写入 ring 约为 `26,762`；
- 平均 `LOST_IN_RING = 1,416`，即 5.02%；
- 78 个 window 中有 4 个溢出；
- 单 window raw high-water 最大为 1,088，而旧的一页 ring 只有 509 个槽位；
- `nested = 0`、`link_dropped = 0`、下游 KCOV 丢失为 0。

这些数据说明：对这个 workload，ring 是唯一**测得的**丢失点；不能把它外推为所有 boundary、所有输入下的永久结论。

## 5. Step 2：容量判决与多页生命周期

ring 从一页参数化为物理连续的多页块。当前 header 为 24 字节，PC 区使用柔性数组，因此 1/2/4 页对应 509/1021/2045 个 PC 槽位。

三种容量、各三次运行的 union EL2 PC 集合均为同一个 132 元素集合，两两对称差为 0：

| 页数 | 容量 | ring 丢失 | unique EL2 PC |
|---|---:|---:|---:|
| 1 | 509 | 994–1470 | 132 |
| 2 | 1021 | 0 | 132 |
| 4 | 2045 | 0 | 132 |

这说明一页 ring 截掉的 raw PC，在**这一整个固定 `KVM_RUN` 调用的 coverage union**中都已由其他 #23 window 出现；因此它不减少本 smoke 的 unique feedback。它不说明每个单独 fault window 都完整，也不说明换输入后仍会如此。

默认仍选 4 页，而不是 2 页：2 页虽然在三次 sweep 中未溢出，但曾达到 1020/1021；同批 4 页运行到 1032，Step 1 的最大值是 1088。4 页对已观测最大值保留约 1.9 倍余量。

### 5.1 生命周期 P0 与修复

多页化暴露了两个 happy-path smoke 看不见的 P0：

| P0 | 风险 | 修复与验证 |
|---|---|---|
| `kvm_unshare_hyp_checked()` 首个失败就返回 | 后续页仍处于 shared/EL2-mapped 状态 | 改为尝试完整范围、保存第一个错误；清理按 attempted 页而非 succeeded 页；任一页无法确认 unshare 就泄漏整个 order-N block |
| fault-injection bit2 在 setup 成功后才伪造失败 | 补偿 teardown 未检查时，可能 free 仍被 EL2 映射的页 | bit2 改为传 `PKVM_COV_MAX_PAGES + 1`；EL2 在 pin 或发布 per-CPU 指针之前拒绝，rollback 成为真正的“shared but never registered”路径 |

`run-fi.sh` 在 N90 上对三种注入做了 25 项可失败断言：bit2 必须失败、`owner_cpu=-1`、`leaked_bytes` 不变且 buffer 可立即复用；bit0/bit1 必须泄漏一整个 4 页块，并记录完整范围 unshare；任何断言失败都会以非零退出。随后 4 页正常 smoke 三次复测仍是零 ring/KCOV 丢失，132 个 EL2 PC 集合不变。

## 6. 现在已经保证什么，尚未保证什么

### 已保证

- 固定 #23 smoke 的 EL2 PC 可以进入 syzkaller feedback，并符号化到 Rust 行号。
- 当前单 CPU、串行手工执行下，EL2 PC 会归属到正确的 executor task。
- 当前固定 smoke 中，4 页 ring 没有测得截断或下游 KCOV 丢失。
- 共享 ring 的 setup、teardown、失败 rollback 和保守泄漏策略已做硬件验证。

### 尚未保证

- manager 的默认并发执行不会错归属或静默 skip；当前 `procs=1` 本身不够。
- EL2 signal 已经能因可变输入而形成 corpus 反馈梯度。固定 smoke 的 132 个 unique PC 集合稳定，但每次 raw hit 数仍有变化；这只能说明当前 set-based feedback 没有新增梯度，不等于 EL2 路径在语义上完全确定。
- 新 boundary 或新输入仍适合 4 页，也不会让 KCOV 再次成为瓶颈。
- 板子崩溃后 syz-manager 能自动取得正确的 pstore 日志并恢复。

## 7. 下一步与准入条件

### 7.1 先做：Rust `ftrace.rs` / `trace.rs` SanCov 排除可行性实验

这项工作在独立 worktree 中进行，不触碰 N90。它的目标是确认 Rust hyp 的 crate-wide SanCov 是否存在安全的模块级排除机制。

当前固定 quiet smoke 的已投递 EL2 PC 中，`ftrace.rs` 与 `trace.rs` 合计约 58.5%。这个比例只是一份样本的构成，不是全局常数；即使可以排除，收益也主要是提高 ring 和 KCOV 中 pKVM 逻辑 PC 的信息密度，而不是修复当前的截断问题。

验收应是：目标模块的 trace-pc call site 减少，而 Rust crate 其余部分仍保留 SanCov。`-C llvm-args` 未必能触达 clang 的 ignorelist，`#[coverage(off)]` 也属于不同的 instrprof 机制；若没有可靠实现路径，应记录“不可达”并保留现状，而不是为降噪破坏闭环。

### 7.2 进入 supervised manager campaign 前：3A 与 3B

3A 和 3B 可以并行，但都必须完成：

- **3A：Stage-2 专用串行执行模式。** 不只是配置 `procs=1`；需要让 manager 不设置 `ExecFlagThreaded`、不生成 collide 执行，并把承载 KVM 调用的 executor 线程固定到 ring owner CPU。错误 CPU 必须显式记录为 skip，不能静默当作没有 EL2 coverage。
- **3B：可生成的 Host 侧 composite 输入面。** 固定 `no_generate` smoke 继续保留为回归测试；fuzzer 真正可变异的对象应是受约束的 host KVM 配置和状态序列，例如合法 memslot 布局、IPA 范围、`SET_FW_IPA` 取值、fault 次数和顺序。不能直接把 protected guest 的任意代码或未验证 ioctl 组合放开。

完成 3A 和 3B 后，可以做小规模 **supervised** manager campaign，验收是 corpus 因 Rust EL2 signal 增长、信号可符号化、没有 ring/KCOV 丢失或 owner-CPU skip。

### 7.3 无人值守 campaign 前：恢复与维护条件

N90 使用 ACPI/EFI pstore，而 syzkaller isolated backend 当前依赖 `console-ramoops-0`。因此 EFI pstore 格式与 backend crash-log 约定的适配，是**无人值守** campaign 的硬前置；它不阻止有人值守、可实时看 dmesg 的小规模运行。

此外，当前 kernel prototype 仍以 `/home/jose/common-stage2mvp` 的未提交工作树存在。虽然 canonical patch、`stage2.config`、构建哈希和证据足以复现 smoke build，但在长期运行或上游化之前，应整理为明确的 kernel 分支和提交。

### 7.4 扩大 boundary

后续可审计 #21、#34/#35、#36–38 等 EL1→EL2 边界。每条边界都应单独定义：何时 arm、何时 drain、成功和失败如何归属、是否需要 CPU pin，以及该边界是否有不同的容量需求。

不要直接无过滤地全局包裹 `kvm_call_hyp_nvhe()`；它会把 boot、无关 worker 和没有正确 task 归属的 hypercall 混进 coverage。若 boundary 数量增长，可逐步采用集中宏、带 function-id allowlist 的 wrapper，或更长期的 EL2 边界标签机制；前提仍是先保住 EL1 的 task 归属和生命周期边界。

## 8. 证据与相关记录

- Host 生命周期实现与 campaign：`2026-07-20-pkvm-syzkaller-v1-implementation-record.md`
- 工程状态与较早 roadmap：`2026-07-23-pkvm-syzkaller-status-and-roadmap.md`
- Step 1 反馈度量：`2026-07-24-pkvm-stage2-step1-feedback-measurement.md`
- Step 2 多页 ring、fault injection 与容量判决：`2026-07-24-pkvm-stage2-step2-multipage-ring.md`
- 原始 N90 数据：`evidence/step1-measurement-2026-07-24/` 与 `evidence/step2-multipage-ring-2026-07-24/`

本文应随新增 boundary、输入面或 manager campaign 结果更新；不要用当前固定 smoke 的数字替代后续 workload 的重新度量。
