# pKVM EL2 覆盖率反馈：阶段总结（2026-07-24）

本文把「让 Rust EL2 pKVM 产生 syzkaller 能符号化到源码行的覆盖率」这个目标的当前状态放在一张图里：什么已经证明、什么被推翻、什么还没做、下一步是什么。它是状态快照，不替代各步骤的详细记录。

**目标（不变）：** 在物理板 N90（Kylin V10 SP1，Phytium gwd3000 级）上 fuzz `/home/jose/common` 的 **Rust 改写版 arm64 pKVM（EL2 nVHE hypervisor，`CONFIG_X1_RMS=y`）**，让 EL2 覆盖率经 KCOV 到达 syzkaller 并解析为 `rust/src/*.rs:line`。

**分支：** `pkvm-lifecycle-fuzzing`（syzkaller，`origin` 同步）。内核 Stage-2 原型在 `/home/jose/common-stage2mvp`，本地未提交（按约定）。

---

## 1. 一句话现状

> Host 侧 pKVM fuzzing 早已是可用工具；EL2 覆盖率闭环已在真机跑通并硬化到「零 ring 截断、零 KCOV 截断、失败分支真机注入验证」；下一步不是开 campaign，而是先把最后的噪声源和串行执行/输入面前置条件补齐。

---

## 2. 已经证明的（真机）

| 里程碑 | 状态 | 关键证据 |
|---|---|---|
| Host→hyp 生命周期 coverage-guided campaign | ✅ 完成 | corpus 0→93、coverage 0→6407、~440k execs、0 crash（`campaign-2026-07-21/`） |
| Slice 1/2/3（dual-vCPU、memslot rejects、ENABLE_CAP 配置） | ✅ 完成 | 覆盖 pkvm.c 生命周期 + reject + INFO/SET_FW_IPA |
| pvmfw firmware donation 路径可达 | ✅ 完成 | `pkvm_mem_abort` 首个 fault @ `0x7fc00000` ret 0（`firmware-smoke-2026-07-22/`） |
| **EL2 覆盖率闭环（Stage-2 MVP）** | ✅ **真机通过** | executor KVM_RUN → #23 → Rust EL2 SanCov → ring → EL1 drain → `kcov_add_pcs` → coverfile → `hyp_main.rs:1069` 等（`stage2-smoke-2026-07-23/`） |
| **两层反馈完整性度量（Step 1：1A/1A+/1B）** | ✅ 完成 | 见 §3 |
| **多页 ring 生命周期 + 4 页判决 smoke（Step 2）** | ✅ 完成 | 见 §4 |
| **ftrace/trace SanCov 排除可行性（board-free）** | ✅ 完成 | 见 §5 |

---

## 3. Step 1（度量）——两个假设被推翻

记录：`2026-07-24-pkvm-stage2-step1-feedback-measurement.md`。

- **task KCOV area 从来不是瓶颈。** 去掉两个测量仪器干扰后，bridge on 整次调用只用掉 area 的 49%；把 `kCoverSize` 翻倍（1A+）**没有换来任何新的 unique EL2 PC**。
- **2026-07-23 smoke 的「524287 打满」是串口控制台，不是覆盖率。** 84% 条目来自 pl011 自旋，由 bridge 自己 4 行 overflow printk 触发。**一行 printk ≈ 10 万条 KCOV 条目**（115200 串口）。当时报的「87 个 EL2 PC」是洪水挤出的假象，**真值 132**。
- **bridge 自己的 drain 循环曾是第二大消耗（42%）**——`pkvm_cov.o` 之前被 KCOV 插桩，`KCOV_INSTRUMENT_pkvm_cov.o := n` 后归零；bridge 开销比 8.13:1 → 1.02:1。
- **1B 内核侧 loss accounting** 给出确定数：每次 smoke 78 次 #23，EL2 命中 28,178、写入 ring 26,762，**ring 内丢失 5.02%**，task KCOV 丢失恒为 0。

---

## 4. Step 2（容量 + 生命周期）——判决与硬化

记录：`2026-07-24-pkvm-stage2-step2-multipage-ring.md`。

- **P0 真 bug（review 抓出）：** `kvm_unshare_hyp_checked()` 首个失败即返回，多页下会留下后续页 shared+EL2-mapped、host 记录未清。改为**范围完整**契约（每页都试、返回第一个错误、按 `attempted` 而非 `succeeded` 清理、任一页未确认则泄漏整个 order-N 块）。
- **多页 ring：** `nr_pages` 参数化（默认 4，debugfs 可扫），HVC `__pkvm_cov_setup(pfn, nr_pages)`（id 不变），**EL2 校验不可信的 host nr_pages**，producer 上界取自 EL2 自算 capacity。
- **判决（Step-1 未决问题的答案）：** ring 扫 1/2/4 页，**unique EL2 PC 恒为 132 且是同一集合**（两两对称差 0，`new − old = ∅`）——**5.02% 的 raw 截断没有吞掉任何 unique 覆盖率**（限当前固定 #23 smoke）。
- **失败分支真机注入：** test-only `fault_inject`（bit0/1/2），**FI 断言测试 25/25 通过**；关键的 bit2（EL2 拒绝 setup）经修正后 `leaked_bytes` 保持 0（正确 FREE 而非泄漏）。
- 仍选 4 页而非 2 页：2 页零丢失是运气（`hits_max` 骑在 1021 上，Step-1 见过 1088）；4 页对已观测最大值有 ~1.9× 余量，代价 16 KiB/CPU。

---

## 5. ftrace/trace SanCov 排除（board-free 可行性）

记录：`2026-07-24-pkvm-ftrace-sancov-exclusion-feasibility.md`。

- Step-1 测得 `ftrace.rs`/`trace.rs` 占**运行时**已投递 EL2 PC 的 58.5%。ring 已零截断，所以排除它们的收益是**信息密度**，不是减少截断。
- **噪声源就是一个 Kconfig：`CONFIG_PROTECTED_NVHE_FTRACE`（上游默认 `n`，fuzz config 里被开成 `=y`）。** 独立 worktree 里只切这一个开关：静态 SanCov 调用点 7311→7167（−144），**完全丢失 instrumentation 的 10 个函数都与 ftrace 功能相关（但不都在 ftrace/trace 文件——`handle___pkvm_disable_ftrace` 在 `hyp_main.rs`），无一与 ftrace 无关的函数受损**；`__hyp_ftrace_trace`/`__hyp_ftrace_ret_trace` 移除、`trace_func`/`trace_func_ret` 桩化（正是 Step-1 运行时前 5 名里的 4 个）；闭环不退化（callback 链接、DWARF 逐字相同）。
- **无需** SanCov ignorelist 或 `#[coverage(off)]` 的按模块排除机制（那两条 fallback 用不到）。
- **建议：从 fuzz `.config` 去掉 `CONFIG_PROTECTED_NVHE_FTRACE`**（回落上游默认 `n`）。这是 config 变更、不改 Stage-2 源码。**代价：ftrace 控制类 HVC 退化为不支持**——对当前目标面可接受；ftrace 本身仍是独立的 EL2 调试能力，将来要 fuzz/调试它应另开配置。

---

## 6. 当前主要缺口

1. **ftrace `=n` 的运行时降幅未在真机确认。** board-free 只证明了机制成立（相关函数被移除/桩化）；58.5% → ? 需要一次 N90 运行。可并入后续 3A/3B 部署，不必单独占重启。
2. **仍是单 CPU 手工串行，不等于 manager campaign。** owner-CPU guard 在错 CPU 上 skip；`procs=1` 不能阻止 executor 线程迁移，也不能自动关 Threaded/Collide。**需要 Stage-2 专用串行 executor 模式 + CPU pin（3A）。**
3. **只采集 #23 donation/map 一条边界，且只有一个固定 no_generate 输入。** 78 次 #23 的形状由 pvmfw 启动决定。**需要可生成的 Host 侧受控 composite 输入面（3B）。** 其它 boundary（#21、#34/35、#36-38）尚无各自的 arm/drain 归属。
4. **无人值守恢复未解决。** N90 是 ACPI/EFI，原 DT ramoops 恢复设想不适用；需要 efi-pstore ↔ isolated backend crash-log 约定对接（阻塞无人值守 campaign，不阻塞有人看守 smoke）。
5. **N90 部署操作约束（持久）：** Kylin GRUB 忽略一切非交互式默认项选择（`next_entry`/`GRUB_DEFAULT`，标题/下标、两处 grubenv 都试过，~7 次重启全落默认）；**非默认内核只能人工在菜单选中**。已记入 deploy skill §10。

---

## 7. 下一步（colleague 约定的次序）

```
[完成] ftrace SanCov 排除 board-free 可行性 —— 结论：用 CONFIG_PROTECTED_NVHE_FTRACE=n，无需按模块排除
[完成] 3B 可生成 Host composite 输入面 syz_kvm_run_fw_fault_gen（固定 no_generate smoke 保留为回归）
[完成] 3A 串行 executor：pkvm_serial（无 Threaded/无 Collide/procs=1）+ isolated pkvm_owner_cpu taskset pin
[完成] EFI pstore：isolated backend 改为 glob 整个 /sys/fs/pstore（发现/读取/回收 dmesg-efi-*）
      ↓（三者均已 code + board-free 验证；board 证明批到下面这次部署）
[下一步] 统一 ftrace-off 新内核部署：关 CONFIG_PROTECTED_NVHE_FTRACE，重新冻结 .config/vmlinux/Build ID/hashes
         → 先跑 4-page #23 smoke（符号化仍在、LOST_IN_RING=0、LOST_IN_KCOV=0、无 skip、量化 ftrace-off 降幅）
         → forced-panic 的 EFI-pstore 恢复验证
      ↓
小规模 supervised EL2 manager campaign
      ↓
逐个为 #21 / #34-35 / #36-38 boundary 建立 arm/drain 归属（绝不全局包裹 kvm_call_hyp_nvhe）
```

**三项 campaign 前置的实现要点（board-free 已验证；真机证明批到统一部署）：**
- **3B（`80d6cf6ba`）：** `syz_kvm_run_fw_fault_gen$arm64`，generateable composite，C helper 保持 protected-VM 生命周期/firmware window/KVM_RUN 边界/fd 清理，只开放 `ipa_size` + `fw_ipa` 两个受控维度（都在 C 内 re-validate）。固定 `no_generate` smoke 不动。验证：make descriptions/TestParsing/arm64 executor 交叉编译/`NoGenerate` 属性均 OK。
- **3A（`713683cc7`）：** 顶层 `pkvm_serial`（procs==1 校验、`DefaultExecOpts` 不设 `ExecFlagThreaded`、manager `Collide=!pkvm_serial`）+ isolated 的 `pkvm_owner_cpu`（`taskset -c N` 包住 executor，SSH 下比 env 稳，等同 smoke 用的 taskset）。CPU mismatch 由内核 `skip_not_owner`（1B 已加）可观测。未用新 flatrpc flag（本机无 flatc）。
- **EFI pstore（`edbbfc197`）：** isolated backend 两处硬编码的 `console-ramoops-0` 改为 glob 整个 `/sys/fs/pstore`，对 ramoops 与 efi-pstore 都发现/读取/回收；空目录是干净 no-op。

**明确不做：** 在 3A/3B 完成前启动 EL2 manager campaign；为降噪去改坏已冻结的闭环；把固定 `no_generate` smoke 直接当 fuzz 输入面。

---

## 8. 构建与板子状态（当前）

- 部署到 N90 非默认项的内核：`6.6.30+`，Build ID `e81d2c18452a871075163940eeaba2446354ec0d`（含 bit2 修正），vmlinux `45c1e553…`，Image `7b31222e…`，`.config` `b1f010f5…`（未变）。
- 板子当前静息在该内核，ring 关闭、`leaked_bytes 0`、`fault_inject 0`、无 WARN/BUG；GRUB 默认项仍是已知可用的 `6.6.30-pkvm-fuzz`；`.1abuild.bak`/`.1bbuild.bak` 两个回退镜像在板上。
- 规范内核 patch：`evidence/step2-multipage-ring-2026-07-24/stage2-el2-kcov.patch`（13 文件），`git apply --whitespace=error` 干净。**符号化只能用同一次链接的 vmlinux**（每次重链地址整体平移）。
- syzkaller 侧全部在 `pkvm-lifecycle-fuzzing`，与 `origin` 同步。
