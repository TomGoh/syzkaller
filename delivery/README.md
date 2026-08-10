# pKVM 模糊测试 —— 使用说明

针对 arm64 pKVM 管理程序（EL2）的系统调用模糊测试。基于 syzkaller，通过 `/dev/kvm` 驱动 host↔EL2 的 hypercall 接口，并采集 **EL2 侧的代码覆盖率**。

本目录是交付件：一个可直接运行的验收配置、一套缺陷复现套件、以及报告生成工具。

---

## 目录

1. [前提条件](#1-前提条件)
2. [第一步：确认内核带有哪些已知缺陷](#2-第一步确认内核带有哪些已知缺陷)
3. [第二步：运行模糊测试](#3-第二步运行模糊测试)
4. [第三步：看结果](#4-第三步看结果)
5. [出问题怎么查](#5-出问题怎么查)
6. [当前已知缺陷清单](#6-当前已知缺陷清单)
7. [已知限制](#7-已知限制)

---

## 1. 前提条件

### 内核

被测板子的内核需要满足三点：

| 要求 | 怎么确认 |
| --- | --- |
| `CONFIG_PKVM_EL2_COV=y`（EL2 覆盖率桥） | 板子上存在 `/sys/kernel/debug/kvm/pkvm_cov/` |
| `CONFIG_KCOV=y` | 存在 `/sys/kernel/debug/kcov` |
| 以 `kvm-arm.mode=protected` 启动 | `cat /proc/cmdline` |

**没有 EL2 覆盖率桥也能跑**，但那样只能看到 host 侧 KVM 的覆盖率，看不见管理程序内部——本项目的价值主要在后者。

### 板子

覆盖率 ring 需要在开机后武装。板子上已配置为开机自动执行：

```bash
systemctl status pkvm-cov-arm      # 应为 active
```

手工武装（每核一个 ring，无需绑核）：

```bash
echo 32 > /sys/kernel/debug/kvm/pkvm_cov/nr_pages
echo 1  > /sys/kernel/debug/kvm/pkvm_cov/enable
grep armed_cpus /sys/kernel/debug/kvm/pkvm_cov/stats     # 应等于在线 CPU 数
```

### 构建

```bash
make manager
make TARGETOS=linux TARGETARCH=arm64 executor execprog
```

> **manager 和 executor 必须来自同一个 git 版本。** 不一致时错误要等 RPC 握手之后才报出来，现象看起来像“程序一直不动”，很容易误判。`check` 子命令会替你核对这一点。

---

## 2. 第一步：确认内核带有哪些已知缺陷

**先做这一步，再跑模糊测试。** 模糊测试报告“零崩溃”只有在你知道这个内核确实带着缺陷时才有意义——否则你分不清是筛选生效了，还是内核本来就没问题。

复现套件在 `issues/tools/repro-pack/`。打包后拷到目标机运行：

```bash
cd issues/tools/repro-pack
./make-portable.sh                        # 生成 pkvm-repro-pack.tar.gz（自带源码）
scp pkvm-repro-pack.tar.gz root@<板子>:/root/

# 在板子上
tar xzf pkvm-repro-pack.tar.gz && cd repro-pack
./run-all.sh                              # 002–006，无害
```

输出是一张判定表：

```
ISSUE  RESULT           RC   WHY
002    REPRODUCED       10   4 new dmesg line(s) naming kvm_tlb_flush_vmid_range ...
003    REPRODUCED       10   5/5 default-THP runs: second KVM_RUN returned -1/errno=7 ...
004    REPRODUCED       10   1 donation-accounting message(s) at VM teardown ...
005    REPRODUCED       10   page A's round-3 write under dirty logging is absent ...
006    REPRODUCED       10   12 new WARN(s) naming kvm_unshare_hyp ...
001    SKIPPED          -    hazard: wedges-target; pass --include-hazardous to run it
007    SKIPPED          -    hazard: wedges-target; pass --include-hazardous to run it
```

三种结果的含义：

| 结果 | 含义 |
| --- | --- |
| `REPRODUCED` | 这个内核有该缺陷 |
| `NOT-REPRODUCED` | 一次合格的运行没有触发它 |
| `INCONCLUSIVE` | **不是通过**。构建失败、超时、前置条件不满足都会落在这里，判定行会说明是哪一种 |

### 危险项：001 与 007

这两个会**让整台机器失去响应**，默认跳过。要跑必须显式加参数：

```bash
./run-all.sh --include-hazardous     # 会打印警告并倒数 10 秒
```

- **001** —— 任务卡在 D 态并持有 `mmap_lock`；rwsem 的公平性会让后续每一个读者排队，`ps`、系统守护进程会逐个冻住。有时还能通过尚未断开的 ssh 用 sysrq 抢救。
- **007** —— **没有任何软件恢复手段**。CPU 在 EL2 的 panic 处理器里空转、中断屏蔽，不响应 IPI/RCU/NMI，任务杀不掉、CPU 下不了线、sysrq 够不着。板子 ping 得通但 ssh 连不上，**只能断电重启**。它也**不产生任何崩溃报告**，不要等报告，直接看 `RESULT` 行。

只在能物理断电的机器上跑这两项，跑完先重启再跑别的。

`006` 虽然标为无害，但它**永久污染页面**：每触发一次就毒化几页 vCPU slab，重启前不会回收，次数多了 `KVM_CREATE_VCPU` 本身就开始失败。所以它排在所有需要 vCPU 的项之后，重跑或验证其修复之前请先重启。

---

## 3. 第二步：运行模糊测试

一体化脚本：

```bash
./delivery/pkvm-fuzz.sh check      # 环境自检，不通过不要往下走
./delivery/pkvm-fuzz.sh start      # 推送二进制并启动
./delivery/pkvm-fuzz.sh status     # 查看进度
./delivery/pkvm-fuzz.sh report     # 生成报告
./delivery/pkvm-fuzz.sh stop       # 停止
```

默认目标是 `root@10.42.27.17`、配置 `delivery/n90-delivery.cfg`。换目标：

```bash
BOARD=root@10.0.0.5 CFG=/path/to/my.cfg ./delivery/pkvm-fuzz.sh start
```

### 关于这份配置

`n90-delivery.cfg` 是**验收配置**：调用面经过筛选，使其在带有已知缺陷的内核上也能长时间稳定运行。筛选的每一条理由都写在配置文件的注释里。

它的 `ignores` 是**空的**，这是刻意的：不屏蔽任何已知缺陷的签名。因此——

> **这份配置下的预期结果是零崩溃。出现崩溃就必须有人看：要么是新缺陷，要么是筛选有遗漏。两种都要查，都不该忽略。**

要做更宽面的缺陷挖掘（内核已修复的前提下），用 `issues/runs/2026-08-06-n90-full-surface/n90-full.cfg`。

### 跑多久

**至少几小时。** 短跑证明不了稳定性——验收过程中出现过跑几分钟正常、31 分钟后才暴露问题的情况。覆盖率通常在头十几分钟快速上升，之后趋缓。

---

## 4. 第三步：看结果

### 报告

```bash
./delivery/pkvm-fuzz.sh report
```

生成 `<workdir>/report/report.md`，包含：

| 章节 | 内容 |
| --- | --- |
| 结论 | 崩溃数、EL2 handler 覆盖 |
| EL2 可达面与覆盖 | 逐个 handler 的覆盖状态 |
| 数据链完整性 | ring 计数器与自洽性校验 |
| 运行环境 | 内核版本、运行时长 |
| 已知缺陷状态 | 与复现套件结果的对照 |

### 核心指标：EL2 handler 覆盖 `N / 30`

这是本项目唯一有意义的覆盖率指标。syzkaller 自己报的 “coverage = 12433” 数的是 host 内核的基本块，**和管理程序没有关系**。

分母 30 是这样来的：EL2 的派发表共有 47 个带 handler 的 id，但派发前会先拒绝所有 `id < hcall_min`（本内核为 18），所以其中 17 个从 `/dev/kvm` 根本够不到。**报告里的分子分母都是从内核源码现算的**，派发表一改就跟着变，不会因为写死数字而失真。

### 网页界面

manager 运行时提供 `http://localhost:56760`：

| 路径 | 用途 |
| --- | --- |
| `/` | 总览 |
| `/syscalls` | 每个系统调用的覆盖贡献 |
| `/cover`、`/funccover`、`/filecover` | 按文件/函数看覆盖 |
| `/rawcover` | 原始 PC 列表 |
| `/crash?id=…` | 崩溃详情 |
| `/metrics` | Prometheus 格式指标 |

### 覆盖率 HTML 报告

```bash
curl -s http://localhost:56760/rawcover > /tmp/rawcover.txt
./bin/syz-cover -config delivery/n90-delivery.cfg /tmp/rawcover.txt
```

### EL2 覆盖率数据链

```bash
ssh root@<板子> 'cat /sys/kernel/debug/kvm/pkvm_cov/stats'
```

几个关键计数器：

| 字段 | 含义 |
| --- | --- |
| `armed_cpus` | 已武装 ring 的 CPU 数，应等于在线 CPU 数 |
| `begin_calls` / `drains` | 打开 / 排空的采集窗口数 |
| `el2_hits` / `el2_written` | EL2 产生 / 成功写入 ring 的 PC 数 |
| `LOST_IN_RING` | ring 满导致丢弃的 PC 数，**应为 0** |
| `leaked_bytes` | 因拆除未确认而永久保留的内存，**应为 0** |

自洽性等式（报告会自动校验）：

```
drains == begin_calls - skip_not_owner - skip_no_kcov
```

差几个属正常（快照时正在飞行中的窗口），差很多说明计数被并发污染，该次数据不可信。

---

## 5. 出问题怎么查

### 测试报了崩溃

1. `<workdir>/crashes/<id>/description` —— 标题
2. `report0` —— 完整调用栈
3. `log0` —— 该 executor 会话的**全部**控制台输出，崩溃前执行过的程序都在里面
4. 对照第 6 节的清单，看是不是已知缺陷

从日志里找出触发程序：

```bash
grep -B 30 'WARNING\|BUG:' <workdir>/crashes/<id>/log0 | grep -E '^r[0-9]+ = |^ioctl|^syz_'
```

### 板子 ping 得通但 ssh 连不上

典型的 EL2 卡死（issue 007 的形态）。此时：

- **不要等崩溃报告**，这种情况不产生报告
- sshd 建连需要 `kick_all_cpus_sync()`，卡死的 CPU 不应答，所以新连接必然失败
- 已建立的连接可能还活着，`dmesg --follow` 还能出数据
- **只能断电重启**

事后从内核日志里找：连锁反应通常表现为 `cleanup_net`、`sshd`、`khugepaged` 的 soft lockup，但**那些都是下游受害者**，起点要往前找第一条 `rcu_sched detected stalls`，它会点名是哪个 CPU。

### 想知道 EL2 到底停在哪

卡死的 CPU 的 EL2 覆盖率 ring 里留着它执行过的最后一批 PC（因为排空发生在 hypercall 返回后，而它没返回）：

```bash
cat /sys/kernel/debug/kvm/pkvm_cov/ring_dump
```

`flags=0x1` 的那个 CPU 就是卡住的那个（窗口打开了但从未关闭）。把最后几个 PC 拿去符号化：

```bash
aarch64-linux-gnu-addr2line -e /path/to/vmlinux -f -i -C <PC>
```

> 这个文件**必须在卡死之前就有办法读到** —— 卡死后开不了新 ssh。做法是提前用 `setsid` 挂一个后台脚本，定时把它写进 `/dev/kmsg`，这样内容会进内核日志、被已建立的 console 采集收走。参考 `issues/007-*/repro/catch-ring.sh`。

### 测试跑不动 / 覆盖率一直是 0

按可能性排序：

1. **manager 与 executor 版本不一致** —— `check` 子命令会检查
2. **ring 没武装** —— `armed_cpus` 为 0
3. **任务没开 KCOV** —— `stats` 里 `skip_no_kcov` 很大。EL2 的 ring 只在调用任务开启了 KCOV trace-pc 时才武装
4. **板子已被前一轮测试拖垮** —— `uptime` 看 load，D 态任务看 `ps -eo stat,pid,comm | awk '$1 ~ /^D/'`

---

## 6. 当前已知缺陷清单

完整记录在 [`issues/`](../issues/)，索引见 [`issues/INDEX.md`](../issues/INDEX.md)。每个缺陷都有独立的复现件。

| ID | 摘要 | 危害 | 状态 |
| --- | --- | --- | --- |
| [001](../issues/001-pkvm-unmap-selfdeadlock/ISSUE.md) | `pkvm_unmap_guest()` 在 `stage2_unmap_vm()` 持读锁时取同一 `mmap_lock` 的写锁，自死锁 | **卡死整机** | 已有修复并验证 |
| [002](../issues/002-tlb-range-flush-missing-pkvm-branch/ISSUE.md) | `kvm_arch_flush_remote_tlbs_range()` 没有 pKVM 分支，EL2 拒绝该 hypercall，TLB 失效实际没执行 | 无 | 已有修复并验证 |
| [003](../issues/003-dirty-log-guest-no-huge-page-support/ISSUE.md) | `__pkvm_host_dirty_log_guest()` 假定 4 KiB 映射，大页背衬的客户机开启脏页跟踪后 `KVM_RUN` 返回 `-E2BIG` | 无 | 已有修复并验证 |
| [004](../issues/004-hyp-donation-accounting-imbalance/ISSUE.md) | 脏页跟踪的 memcache 补充从未记账，`protected_hyp_mem` 变负，泄漏检查报出虚假的 18 EB | 无 | 已有修复并验证 |
| [005](../issues/005-unmap-guest-fails-after-dirty-log/ISSUE.md) | 脏页跟踪 hypercall 成功后会抹掉页面的 EL2 共享状态，之后所有写保护失败、脏页写入被静默丢失 | 无 | 已有修复并验证 |
| [006](../issues/006-unshare-hyp-pfn-missing-on-vcpu-destroy/ISSUE.md) | 首次 `KVM_RUN` 之前设置 `KVM_MP_STATE_SUSPENDED` 会泄漏 EL2 pin，**永久毒化页面**，最终 `KVM_CREATE_VCPU` 失败 | 无（但不可恢复） | 已有修复并验证 |
| [007](../issues/007-protected-block-mapping-el2-panic/ISSUE.md) | `guest_get_page_state()` 用原始 PTE 代替提取出的 prot 做判断，受保护客户机的 2 MiB 块映射得到非法页面状态，EL2 触发 `bug_on!`，而 Rust 的 panic 处理器是 `loop {}` | **卡死整机，无任何报告** | **未修复** |

阅读顺序建议：先看 [`issues/README.md`](../issues/README.md) 了解记录规范（`diagnosis` / `disposition` / `hazard` 各字段的含义），再看 [`issues/REACHABILITY.md`](../issues/REACHABILITY.md) —— 它说明这些缺陷分别需要什么前提才能触发，直接影响优先级判断。

---

## 7. 已知限制

**交付配置覆盖不到脏页跟踪路径。** `__pkvm_host_dirty_log_guest` 和 `__pkvm_host_wrprotect_guest` 没有别的调用者，而走到它们必然触发 002/004/005（004 的门槛最低：任何普通 VM 跑过 vCPU 后在 memslot 上开启该标志即可）。这不是配置的短板，而是**这个内核上不存在不触发缺陷的走法**。003/004/005 修复合入后应重新放开。

**syzkaller 的描述不是访问控制。** 想禁止某个取值或某个资源类型，只靠"从 flags 列表里删掉"或"把参数类型收窄"是不够的——生成器对两者都做宽松处理（flags 有小概率返回完全随机值；资源子类型为提高覆盖率而宽松匹配）。真正能排除的只有：把调用整个移除，或改用 `const[]`。

**EL2 覆盖率是按 hypercall 窗口采集的。** 窗口之外的 EL2 执行——客户机退出、host stage-2 abort、中断——不在统计内，目前也没有计数器可以量化这部分。报告里的 `LOST_IN_KCOV_AREA` 只覆盖已进入 ring 之后的损失。

**报告的模块覆盖率可能不准。** 如果板子上的内核模块和 `kernel_obj` 指向的构建不是同一次产物，manager 会打印 `kernel build has changed; instance module ... differs from canonical`。pKVM 相关代码是内建的，不受影响；但模块代码的覆盖率归属会出错。正式测试前应确保模块与内核同批安装。
