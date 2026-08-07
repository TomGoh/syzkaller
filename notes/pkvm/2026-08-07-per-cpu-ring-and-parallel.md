# per-CPU 覆盖率 ring 与并行 fuzzing

日期：2026-08-07  
目标机：N90（10.42.27.17），8 核  
前置：`2026-08-07-el2-coverage-loss-measurement.md`（为什么要做这件事）  
哈希对照：`2026-08-07-klinux-hash-rewrite.md`（klinux 在此期间 rebase 过）

---

## 1. 做了什么，为什么

EL2 覆盖率只有**一个 ring，绑在一个 CPU 上**，而 EL2 侧本来就是 per-CPU 的（`hyp/nvhe/cov.c:23` `DEFINE_PER_CPU`）。单 ring 无法被并发 procs 共享，于是 `pkg/mgrconfig/load.go` 拒绝 `pkvm_serial` 配 `procs != 1`，campaign 只能单进程绑在 CPU 0。

**代价是吞吐，不是覆盖率。** 前一份测量已经证明 per-CPU 不会带来任何新覆盖（CPU 0/1/4 各得 443 个 EL2 PC，三个集合逐字节相同）。真正的账是：17 小时 campaign 期间 8 核板子的 load average **1.07**，即 **87% 空闲**，103 exec/分。

---

## 2. 结果

| | 之前 | 之后 |
|---|---|---|
| exec 速率 | 103/分 ≈ **1.7/秒** | **29–35/秒**（约 **18×**） |
| load average | 1.07 / 8 核 | **6.97 / 8 核** |
| executor 进程 | 1 | 13 |
| 核亲和性 | 钉死 CPU 0 | **0–7 自由调度** |
| `armed_cpus` | 1 | **8** |
| `skip_not_owner` | 不绑核时 99.999% | **0**（16 亿次窗口） |
| `LOST_IN_RING` | 0 | **0** |
| `leaked_bytes` | 0 | **0** |
| 内核 WARN/BUG | 0 | **0** |

自洽性在并发下仍精确成立：`1,599,575,553 − 0 − 9,100,992 = 1,590,474,561` vs `drains 1,590,474,556`，差 5（快照时在飞行中的窗口）。

**`LOST_IN_KCOV_AREA` 比例未变**（仍 99%+）。并行提高的是吞吐，这一层没动。

---

## 3. 改动

### 内核 —— klinux `b93d9fb02e7c`（`pkvm-el2-kcov` 分支，单个提交）

拆掉"只有一个 owner CPU"这条不变量，**必须显式补回它顺带保证的三件事**。这是本次改动的全部难点：

| 它原本保证的 | 补回的方式 | 漏掉会怎样 |
|---|---|---|
| ring 无并发写 | 每核一个 ring，`this_cpu_ptr` | 编译错误（立刻发现） |
| 统计无并发写 | 统计改 per-CPU，读时求和 | 计数慢慢对不上（靠自洽性检查发现） |
| 窗口不跨核 | **`preempt_disable()` 包住 begin/HVC/end** | **覆盖率记到别的程序头上（无症状）** |

第三条是最危险的。原设计假定 begin/end 跑在 `mmu_lock` 下（抢占关闭），但包装宏后来变成**全局**包住每个 `kvm_call_hyp*`，大多数调用点可抢占：

```
begin() 在 CPU A 武装 ring A
  ↓ 迁移到 CPU B
HVC 在 CPU B 执行 → EL2 写进 ring B（可能是别人正开着的窗口）
end() 通过 ctx->ring 排空 ring A → 空的
```

**单 ring 时这只是丢数据**（迁移后的核没有 ring，EL2 直接丢弃）；**per-CPU 后变成串数据** —— fuzzer 会把 A 程序的覆盖率记到 B 头上并存进 corpus。

生命周期也跟着改：setup hypercall 必须 `smp_call_function_single()` 到目标核（EL2 用 `__this_cpu_write`，原来的 `get_cpu()` 形式只能武装调用者所在的核）；enable 全程持 `cpus_read_lock()` 且全有或全无；disable 先全部下电、**一次** `synchronize_rcu()`、再逐核拆除，够不到的核只泄漏自己那一块。

### syzkaller

把两件被混在一起的事拆开：

```json
"pkvm_el2_cov": true,   // 新增：内核有 EL2 覆盖率桥 → slowdown 10（KVM_RUN 需要）
"pkvm_serial": false,   // 现在只为单 ring 老内核保留
"procs": 6
```

`pkvm_serial` 原本同时压制 `procs`、`ExecFlagThreaded`、collide，三条理由全部是"单 ring 单 CPU"。但 slowdown=10 是**内核属性**而非并发度属性，必须能在关掉串行后存活，所以挪到新键上。

还加了 `syz_kvm_dirty_log_cycle$arm64` 复合调用，以及把 `issues/` 和 `notes/pkvm/evidence/` 排除出 `make format`（归档件被 clang-format 重排会让归档与它所记录的东西不一致）。

---

## 4. 路上踩的坑

按"发现成本"排序，越靠后越贵：

1. **新伪系统调用忘了在 `pkg/vminfo` 注册** → manager 启动 panic。有明确报错，最便宜。
2. **manager / executor 版本不同步** → 两次 `FATAL: mismatching git revisions`。同样有明确报错。
3. **per-CPU 符号当普通指针读** → `pkvm_cov_nr_pages_set()` 漏改，开机第一次写 `nr_pages` 就 Oops，ring 静默没武装。**编译器一声不吭** —— per-CPU 符号当普通变量用是合法 C，语义全错。这类只能靠 grep 审查：每处 `pkvm_cov_host_ring` / `pkvm_cov_attempted_pages` 都必须经过 `per_cpu_ptr()` / `this_cpu_ptr()` / `__this_cpu_*`。
4. **把 `owner_cpu` 从 `stats` 里改名成 `armed_cpus`** → 板子上的 `pkvm-cov-arm.service` 回读它做校验，读不到就 `exit 1`，systemd 每 5 秒重启一次，**ring 被反复拆了又装**，dmesg 一直刷。

   第 4 条最值得记：**功能全对**——ring 每次都武装成功、`leaked_bytes` 始终为 0——**只有报告是错的**。任何检查"ring 是否武装"的测试都会通过。是有人看 dmesg 才发现的。

   我在提交信息里写过"`owner_cpu` 保留是因为测量 harness 会回读它"，却只想到**我自己的** harness。改一个"看起来只是显示用"的字段，忘了它是个被程序消费的契约——**改名一个有人 grep 的字段就是 ABI 变更**。最终 `stats` 同时输出两行。

5. **rebase 后批量替换文档里的哈希** → 把引用的内核 WARN 原文里的 `Source Version` 也改了，等于篡改引文。已全部回退，改为不动文档、用对照表换算。详见 `2026-08-07-klinux-hash-rewrite.md`。

---

## 5. 验证

- 开机：**一条** `armed on 8 CPUs` 日志，武装服务 `active` 且 `NRestarts=0`，`stats` 里 `owner_cpu -1` 与 `armed_cpus 8` 并存，`leaked_bytes 0`，无 WARN/BUG
- 反复 arm/disarm 四轮（开机脚本重试）后 `leaked_bytes` 仍为 0 —— 逐核 IPI 拆除路径正确
- `procs=4 -threaded=1`：`begin_calls 950008 / skip_not_owner 0 / skip_no_kcov 1022 / drains 948986`，自洽
- `procs=6` 实跑 20 分钟：见 §2

---

## 6. 未做

- `LOST_IN_KCOV_AREA` 那 99% 里 unique 占比，仍是间接推断
- 丢包阶梯上三层无计数器的丢失，要量化必须改内核
- `pkvm-cov-arm.sh` 的兼容性补丁是**直接在板子上改的**（备份在 `/root/pkvm-cov-arm.sh.bak`），没有进版本库。内核已改回同时输出两个字段，所以那个补丁其实不再必要，但也不冲突
