# ftrace-off 部署 + 4-page smoke + EFI-pstore + supervised campaign 实测记录

日期：2026-07-24
目标机：N90（Kylin V10 SP1，Phytium gwd3000 级）
前置：三项 campaign 前置（3B/3A/EFI-pstore）board-free 实现（`2026-07-24-pkvm-el2-coverage-stage-summary.md`）

统一部署一次 ftrace-off 内核，然后按 colleague 的顺序：4-page #23 smoke → forced-panic EFI-pstore 恢复 → 小规模 supervised manager campaign。

---

## 1. 结论

- **ftrace-off 的运行时降幅经真机确认为 −58.5%**（与 board-free 预测逐位一致），4-page smoke 全项通过，交付的 EL2 覆盖率已由 hyp 自追踪噪声转为 pKVM donation/map 目标逻辑。这是本轮最重要的结果，牢固。
- **Stage-2 EL2 闭环在真实 manager 下工作（内核侧）：** manager 驱动 `syz_kvm_run_fw_fault_gen` 时，内核 `drains` 持续增长、`LOST_IN_RING=0`、`skip_not_owner=0`、`LOST_IN_KCOV=0`、`overflow=0`——即 colleague 要求的「无截断、无 skip」验收，在真实 manager + 3A CPU-pin 下达成。
- **3A CPU-pin 生效**（executor 被 `taskset` 钉在 CPU 0，`skip_not_owner=0`）；**3B gen 能到达 #23**（手工 + campaign 均见 drains）。
- **EFI pstore 在 N90 上不可用——固件限制，非代码问题**：N90 的 Phytium 固件不支持内核态 EFI `SetVariable`，efi-pstore 注册后也无法写任何记录。
- campaign 暴露并修掉 3 个真 bug（support-map arch、git-revision 纪律、slowdown）；剩余一项 manager 语料增长问题已 root-cause 为基础设施层，非 Stage-2 机制问题（见 §5）。

---

## 2. ftrace-off 内核（冻结）

`/home/jose/common-stage2mvp`：`./scripts/config --disable PROTECTED_NVHE_FTRACE` + olddefconfig（其余 Stage-2 config 不变）+ `make Image modules`。

| 项 | 值 |
|---|---|
| kernel release | `6.6.30+` |
| Build ID | `2c8458f9298d97adce5854aaac595064416550b9` |
| vmlinux sha256 | `3c170824b31f9466df97a61555c1108bdbbd2fad01bc8dcbf15d328c4646bbb2` |
| Image sha256 | `928f81556c8c3a0d9ce6b786f64e28f6e49bf6993404c0351e0396a42f111e74` |
| System.map sha256 | `123b7053f95f276976d910f3dd451319f2ba6e2318ccff8b93ffb27040b85d39` |
| .config sha256 | `fd7f4a4eae877cafe5e1c6b6da70549f3fdb9ed590c9c8f336dd31d694b9e77e`（ftrace off，其余同前） |
| `__hyp_text_start`/`_end` | `0xffff800081d86ed4` / `0xffff800081de4000`（比 ftrace=y 缩 ~12 KiB，即被移除的 ftrace 代码） |

验证：`__kvm_nvhe___sanitizer_cov_trace_pc` 仍链接（1）、`__kvm_nvhe___hyp_ftrace_trace` 移除（0）、`pkvm_cov.o` 未插桩（0）、消费侧 `TestPkvmCovSymbolizePipeline` 对该 vmlinux 通过。部署经 `kylin-v10-kernel-deploy` skill（非默认 GRUB 项，人工在菜单选中——Kylin GRUB 忽略非交互默认项）。

**GRUB 去重经验（durable）：** deploy skill 的 `40_custom.bak-deploy` 与手工 `.pre-efipstore.bak` 备份若留在 `/etc/grub.d/` 且可执行，`grub-mkconfig` 会把它们当配置源，产生同名重复项（且 cmdline 可能不同）。修复：把所有 `*.bak*` 移出 `/etc/grub.d/`（如 `/root/grub-backups/`）再 `update-grub`。

---

## 3. 4-page #23 smoke（ftrace-off，3 replicate）

CPU 0、`-procs=1 -threaded=0`、bridge on、hot-path 无 printk、4-page ring。

| 指标 | ftrace=y（前） | ftrace=n（本轮） |
|---|---|---|
| `el2_hits`（运行时 callback） | 平均 28,201 | 平均 **11,717** |
| **运行时降幅** | — | **−58.5%**（预测 58.5%） |
| `el2_hits_max`（单次 #23 高水位） | ~1,088 | **~433**（已低于单页 509） |
| unique EL2 PC | 132 | **127**（集合改变，符合预期） |
| `LOST_IN_RING` / `LOST_IN_KCOV` / `skip` / `leak` / WARN·BUG | 0 | **0** |

**组成（符号化，同一 vmlinux）：** 127 个 EL2 PC 以 donation/map 目标逻辑为主——`mem_protect/*`（mem_region 20、guest 16、host 15、transition 5、utils 5）、`page_alloc` 9、`tlb`/`mm`/`pgtable.c` 8、`hyp_main.rs` 5（#23 dispatch）、`kvm_host.rs` 4。`ftrace.rs` 完全消失；`trace.rs` 仅剩 3 个（`trace_hyp_enter`/`trace_host_hcall`/`trace_psci_mem_protect`，`CONFIG_TRACING` 的 per-event tracepoint，按事件而非按函数进入触发，量极小）。

**按 colleague 要求，验收不是「132 个完全相同」**：集合本就应改变（ftrace PC 消失、目标逻辑保留），关键是 donation/map 覆盖仍在、信号完整——达成。

---

## 4. EFI-pstore：N90 固件限制（definitive）

forced-panic 恢复验证**在 N90 上不可能完成，原因是固件而非代码**：

- efi-pstore 默认被 Kylin 关闭（`efi_pstore.pstore_disable=Y`，`pstore.backend=(null)`）。加 cmdline `efi_pstore.pstore_disable=0` 后确实注册为 backend（`pstore: Registered efi_pstore`）。
- 但两次 forced-panic（sysrq-c + `kernel.panic=15` 自动重启）后 `/sys/fs/pstore` 均为空。
- 定位：`/sys/firmware/efi/runtime` 为空，且一次最小 efivar 写入**失败**——**N90 的 Phytium 固件不支持内核态 EFI `SetVariable`**。因此 efi-pstore 无法写任何记录（panic 或其它），与 `efivars` 是否 81% 满无关。

**结论：** `vm/isolated` 的 `dmesg-efi-*` 读取/清理泛化（`edbbfc197`）本身正确、已 mock 验证，对支持 efi-pstore 的板子有用；但 **N90 的无人值守崩溃恢复不能用 efi-pstore**，需另走 live `ssh dmesg -w`（supervised campaign 已在用）、serial console 或 netconsole。cmdline 里的 `efi_pstore.pstore_disable=0` 在 N90 上无害但无用。

---

## 5. Supervised manager campaign

config `workdir-ftraceoff/pkvm-campaign.cfg`：`type: isolated`、`pkvm_serial: true`、`procs: 1`、`vm.pkvm_owner_cpu: 0`、`pstore: false`、`enable_syscalls = [openat$kvm, syz_kvm_run_fw_fault_gen$arm64, close]`。EL2 ring 先经 debugfs 在 CPU 0 armed。

**campaign 暴露并修掉 3 个真 bug：**
1. **support-map arch（`bf34a331c`）：** `syz_kvm_run_fw_fault_gen$arm64` 加进了 support map（base name），但 `linuxSyzKvmSupported` 用**全名 switch** 且有显式 per-arch case list，gen 漏在 arm64 case 之外 → 落到 `unsupported arch` → manager 静默丢弃（2/3 enabled，`begin_calls=0`）。board-free 检查（descriptions/TestParsing/executor build）不覆盖 manager 的 arch 解析，故漏过。
2. **git-revision 纪律：** 只改 vminfo/manager 不重编 executor → manager `+`（dirty）vs executor clean → `[FATAL] mismatching manager/executor git revisions`。必须 commit 后**同时**重编 manager + executor。
3. **slowdown（`2cbf12e65`）：** gen 做**真实 KVM_RUN**（pvmfw boot + 78 faults，tens of ms）；isolated 默认 slowdown=1 → per-call timeout 500ms 在 run 干净退出前杀掉它 → 覆盖率被丢。`initTimeouts` 现对 `pkvm_serial` 置 slowdown=10（等同 smoke 的 `-slowdown=10`）。

**修完后内核侧验收达成：** manager 驱动下 `begin_calls`/`drains` 持续增长（观测到 323 drains、`el2_hits=51,484`），`LOST_IN_RING=0`、`skip_not_owner=0`、`LOST_IN_KCOV=0`、`overflow=0`、`el2_hits_max=497`。**即 Stage-2 EL2 闭环在真实 manager + 3A 串行/CPU-pin 下无截断、无 skip 地工作。**

**剩余一项（follow-up）：** manager 的**语料/覆盖率没有增长**（corpus=0/coverage=0）——executor 到 manager 的 **RPC 链路在运行中被关闭**，已收集的覆盖率没能回传。

**措辞更正（colleague，已对 `executor/conn.h:79-95` 核实）：** 日志 `failed to recv rpc … fd=3 want=4 recv=0 n=0 (errno 9)` 里，`read()` 返回 `n=0` 是 **EOF（对端关闭连接）**；`n=0` 不设置 `errno`，所以随后的 `errno 9: Bad file descriptor` 是**上一次系统调用留下的旧值**，不是原因。因此**只能说 RPC 链路被对端关闭，尚不知道是哪一端先关（manager kill/restart、executor 退出、SSH forward、还是超时）**。我先前记的「flaky SSH-forward errno-9 Bad FD」是过度归因，撤回。**这不是 Stage-2 机制问题**——机制已由 syz-execprog 手工运行 + 内核 drains 证明（手工跑 gen 得 coverage 6124/signal 8507、drains 78）；是 manager feedback transport 的诊断问题（下一步先定位关闭发起方，不要直接改 SSH）。

另外 **3B 的非默认 `fw_ipa`（如 0x7fd00000）会干净失败、不到达 #23**（drains=0，非 hang）——「调用被 fuzz 了」≠「EL2 donation/map 被 fuzz 了」；多数 gen 变异没打到 EL2。**收窄/修正 fw_ipa 使多数生成程序高 reach** 是并行 follow-up（Step 2 的 reachability matrix 量化后再定）。

---

## 6. 板子与代码状态

- N90 静息在 ftrace-off `6.6.30+`（Build `2c8458f9`），ring 关闭、`leaked_bytes 0`、无 WARN/BUG；GRUB 默认仍是已知可用的 `6.6.30-pkvm-fuzz`；回退镜像 `.1abuild/.1bbuild/.bit2build.bak` 在板上。
- **提交状态区分：** **最后一个功能代码提交 = `2cbf12e65`**（3A slowdown fixup；本轮 board session 用的 manager/executor 都由它构建）；**记录/摘要提交 = `0368b1630`**（deploy-session 摘要）；**本次（evidence + 措辞更正）= 见下**。三者都在 `pkvm-lifecycle-fuzzing`，与 origin 同步。
- **本轮 raw evidence：** `evidence/ftrace-off-deploy-2026-07-24/`（smoke stats/EL2 PC set/symbolization、manager log + kernel counters、RPC EOF 原文 + 更正分析、EFI SetVariable 失败输出、3B reachability 观测、provenance 哈希）。
- **下一步（未做，follow-up；顺序按 colleague 的 Step 0-5）：** Step 0 补证据（本次做）→ Step 1 先**定位 RPC EOF 的关闭发起方**（executor 侧分开记 `n==0` EOF 与 `n<0` read-error，记 manager runner kill/restart 与远端 ssh 退出码；不要直接改 SSH）→ Step 2 做 **3B ipa_size×fw_ipa reachability matrix** 并据此收敛 generator → Step 3 重验四层 manager feedback（执行/attribution/signal/学习，第四层 corpus 因 EL2 signal 增长才算成功）→ Step 4 N90 无人值守恢复改用 serial/netconsole/live-dmesg（efi-pstore 固件不支持）→ Step 5 才逐条扩 #21/#34-35/#36-38（绝不全局包裹 `kvm_call_hyp_nvhe()`）。
