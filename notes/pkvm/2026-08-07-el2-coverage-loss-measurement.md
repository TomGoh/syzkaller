# EL2 覆盖率丢在哪：ring 不丢，KCOV area 丢 99.1%，而 per-CPU ring 的价值是吞吐不是覆盖率

日期：2026-08-07  
目标机：N90（10.42.27.17），内核 `6.6.103+ #18 @684f834c7ca5`，BuildID `0886c85b017d48d3…`  
原始数据：`evidence/2026-08-07-el2-coverage-loss/`  
前置：`2026-07-24-pkvm-stage2-step1-feedback-measurement.md`（方法来源与对照）

> **本文第一版有一处严重归因错误，已在 §5 完整记录。** 结论从"campaign 一直被 CPU 闸门砍掉 97% 的覆盖率"改成"campaign 一直是绑核的，闸门从未生效"。下面所有 campaign 数字均取自**归档快照** `evidence/../2026-08-06-n90-full-surface/pkvm_cov-stats-final.txt`，不是实时读数。

---

## 1. 结论

1. **campaign 的丢包只有一个大头：task KCOV area，99.1%。** EL2 ring 一个 PC 都没丢，HVC 窗口层几乎不丢。

2. **campaign 一直是绑核的**（`vm.pkvm_owner_cpu = 0`），`skip_not_owner` 在 11.18 亿次窗口里只有 **397**。CPU 闸门对 campaign 从未生效。

3. **per-CPU ring 不会带来任何新覆盖。** 在 CPU 0 / 1 / 4 分别武装并绑核跑同一负载，三次各得 443 个 unique EL2 PC，**三个集合逐字节相同**；换真实 corpus 后 CPU 1 的结果是 CPU 0 的**严格子集**。

4. **但 per-CPU ring 仍然值得做——理由是吞吐，不是覆盖率。** 它是解开 `procs=1` 的唯一途径。板子 8 核，campaign 期间 load average **1.07**，即 **87% 空闲**。

5. **不绑核会摧毁 EL2 测量。** 同一批程序两次跑出 407 和 73 个 unique EL2 PC（相差 5.6 倍），`drains` 只剩 79 和 10。所以"直接 `procs=8` 不绑核"不是可行的省事方案。

---

## 2. campaign 的真实情况（一手，来自归档）

### 并行度

```
procs             = 1          单进程
pkvm_serial       = true       强制 procs=1、slowdown=10、关 threaded、关 collide
vm.targets        = 1 块板子
vm.pkvm_owner_cpu = 0          整个 executor 命令被 taskset -c 0 包住
sandbox           = none
```

**8 核用 1 核，完全串行。** load average 1.07，exec 速率从 289/min 一路降到 103/min（corpus 变大所致，不是资源不足）。

### 丢包阶梯

窗口层：

| | 数量 | 占比 |
|---|---|---|
| `begin_calls` | 11,184,511,389 | |
| `skip_not_owner` | **397** | 0.0000035% |
| `skip_no_kcov` | 18,127,550 | 0.1621% |
| `drains` | 11,166,383,441 | **99.8379%** |

自洽检查 `begin − skip_not_owner − skip_no_kcov = 11,166,383,442` vs `drains = 11,166,383,441`，**差 1**——快照读取时有一个窗口在飞行中，正常。

PC 层：

| | 数量 | 占比 |
|---|---|---|
| `el2_hits` | 1,024,565,809,778 | |
| `el2_written` | 1,024,565,809,778 | ring 丢失 **0** |
| `link_dropped` | 0 | |
| `kcov_requested` | 1,024,565,809,778 | |
| `kcov_accepted` | 9,233,295,413 | **0.9012%** |
| `LOST_IN_KCOV_AREA` | 1,015,332,514,365 | **99.0988%** |

**端到端通过率 0.90%，唯一丢失点是 task KCOV area。**

`el2_hits_max = 2213` 而 ring 容量 16381——单窗口最多用到 **13.5%**，`nr_pages=32` 充裕，ring 不需要加大。

### 丢的是重复

1.02 万亿个 PC 只对应约 525 个 unique，平均每个 unique PC 被打 **19.5 亿次**。`hits_64_127` 和 `hits_128_255` 各占 38 亿个窗口，与"少数热点代码反复循环"吻合。

间接证据（本次实验）：绑核时 `LOST_IN_KCOV_AREA` = 3.03 亿而 unique 稳定 443；不绑核时该项 = 0 而 unique 只有 73~407。**丢得多的那组 unique 反而更多更稳定。** 这是间接推断，不是直接证明——直接证明需要加大 `kCoverSize`（现 4M）重测 unique 数。

---

## 3. 实验数据

### Phase 1 — 绑核 vs 不绑核（W1 固定探针集，2×2 轮换）

| arm | unique EL2 PC | `begin_calls` | `skip_not_owner` | `drains` | `LOST_IN_KCOV_AREA` |
|---|---|---|---|---|---|
| pin0 | **443** | 2,569,970 | 0 | 2,566,242 | 303,605,467 |
| nopin | 407 | 7,334,592 | 7,334,513 | **79** | 0 |
| nopin | **73** | 7,308,026 | 7,308,016 | **10** | 0 |
| pin0 | **443** | 2,564,426 | 0 | 2,560,638 | 302,932,878 |

绑核两轮逐字节一致；不绑核两轮相差 5.6 倍。**不绑核的结果取决于调度运气。**

### Phase 2 — 逐核并集（W1）

| arm | owner | unique EL2 PC |
|---|---|---|
| pin0 / pin1 / pin4 | 0 / 1 / 4 | 443 / 443 / 443 |
| **并集** | | **443（三集合逐字节相同）** |

### Phase 3 — 真实 corpus（归档 corpus 前 50 条）

| arm | unique EL2 PC | `skip_not_owner` | `drains` |
|---|---|---|---|
| pin0 | **442** | 0 | 4,785 |
| pin1 | 431 | 0 | 4,821 |
| nopin | 313 | 4,667 | 82 |
| **并集** | **442** | | |

`pin1 独有 = 0`，`nopin 独有 = 0`——均为 pin0 的子集。

### handler 口径

W1 触及 **19** 个 handler，W2 触及 **18**，并集 **21**（可达面 30）。对比 19 小时 campaign 的 **16** 个。

---

## 4. 方法与适用范围

harness：`evidence/2026-08-07-el2-coverage-loss/scripts/run-pin.sh`，以 `evidence/step1-measurement-2026-07-24/scripts/run-1a.sh` 为基础改。

- **控制台全程 quiet**，07-24 那套 `/proc/sys/kernel/printk` bind-mount 屏蔽（syz-executor 会无条件写 `"7 4 1 3"`；115200 串口上一条 printk ≈ 10 万条 KCOV 条目）。每轮回读 shadow 确认，全部为 `1413`。
- **arm 与 execprog 必须同核**：setup hypercall 把 `owner_cpu` 记成写 `enable` 那个任务当时所在的 CPU（`pkvm_cov.c:283-290`），无法指定目标核。每轮 `cat owner_cpu` 验证。
- **每轮 `stats_reset` 前置**，`stats` 后置快照。10 轮自洽检查全部成立，`leaked_bytes` 全部为 0。
- **符号化只对板子上正在跑的 vmlinux**，分桶边界为 `#18` 重新取（`__hyp_text_start = 0xffff800081439f00`）。

**适用范围**

- W1 是 5 程序探针集，W2 是归档 corpus 前 50 条。换输入面必须重测——07-24 那份记录写明了这一点，而它的"KCOV area 从不是瓶颈"在今天的 W1 下确实不再成立。
- **"逐核并集"是近似上界。** 把负载绑到 CPU N 与"自由调度 + per-CPU ring"不等价：绑核改变执行本身（跨核 TLB 广播、vCPU 迁移、并发窗口）。结论"per-CPU 不带来新覆盖"对**顺序可达性**成立，对**并发缺陷的可观测性**未作论证。

**本次测不到的**（丢包阶梯上三层无计数器，要量化必须改内核）：HVC 窗口之外的 EL2 执行（客户机退出、host stage-2 abort、IRQ——包装宏只在 `arch/arm64/include/asm/kvm_host.h:1245-1254`）、ring 关闭期间的 HVC、非 owner 核上的 EL2 回调。**本文所有比例的分母都不含这三层。**

---

## 5. 本文第一版的错误

第一版写着"campaign 的 97.2% HVC 窗口被 owner-CPU 闸门丢弃"，并据此得出"之前所有 EL2 覆盖率数字都要打折扣"。**这是错的。**

那个 97.2% 来自我在 **2026-08-07 14:23 读的实时 `stats`**。板子 11:02 才重启，而 11:02 到 14:23 之间我在同一块板子上跑了几十次**不绑核**的 execprog 测试。`pkvm_cov` 的计数器是**开机累计**的，所以我读到的是**我自己留下的痕迹**，不是 campaign 的。

campaign 的真实数据一直在归档里（`pkvm_cov-stats-final.txt`，08:51 采集），`skip_not_owner = 397`。

**教训**：这次实验的设计里专门写了"每轮 `stats_reset` 前置"来防止这个问题，但我在**设计实验之前**做的那次归因没有这一步。纪律只用在了正式实验上，而假设恰恰决定了要做什么实验——归因阶段和测量阶段需要同样的卫生标准。

---

## 6. 已实施：per-CPU ring 与并行

结论 4 落地了。改动与实测见 `2026-08-07-per-cpu-ring-and-parallel.md`。要点：

- klinux `b93d9fb02e7c`（`pkvm-el2-kcov` 分支，一个压缩提交）：每核一个 ring、per-CPU 统计、`preempt_disable` 包住 begin/HVC/end、逐核 IPI 生命周期
- syzkaller：把"内核有 EL2 覆盖率桥"（`pkvm_el2_cov`，只决定 slowdown）与"必须串行"（`pkvm_serial`）拆开，`procs` 提到 6
- 实测 `procs=6`：**29–35 exec/秒**（原 103/分 ≈ 1.7/秒，约 18×），load 6.97/8 核，`skip_not_owner` 在 16 亿次窗口里为 **0**，`LOST_IN_RING` 0，`leaked_bytes` 0，无 WARN/BUG
- **`LOST_IN_KCOV_AREA` 比例没变**（仍 99%+）。并行提高的是吞吐，不是这一层

## 7. 仍未解决

- `LOST_IN_KCOV_AREA` 那 99% 里到底有多少是 unique，**仍是间接推断**。直接证明要加大 `kCoverSize`（现 4M）重测 unique 数
- 丢包阶梯上三层无计数器（HVC 窗口外的 EL2 执行、ring 关闭期间的 HVC、非 owner 核回调），要量化必须改内核
- **不要**用"不绑核 + 单 ring"这条捷径（现已无意义，但记录在此）：那会让 EL2 信号变成噪声（407 vs 73），syzkaller 拿覆盖率做反馈，会把运气好的程序当成高覆盖存进 corpus
