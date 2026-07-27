# Stage-2 第一步：两层反馈完整性度量（1A / 1A+ / 1B）实测记录

日期：2026-07-24
目标机：N90（Kylin V10 SP1，Phytium gwd3000 级，8 CPU，24 GB）
对应计划：`2026-07-23-pkvm-syzkaller-status-and-roadmap.md` §7「第一步」
原始数据：`evidence/step1-measurement-2026-07-24/`

---

## 1. 结论

第一步要回答三个问题：**哪里先截断、谁在填满 task KCOV、下一步应该改哪一层**。三个都有了确定答案，而且其中两个的答案与第一步开始前的假设不同。

1. **task KCOV area 不是瓶颈，而且从来都不是。** 在关掉两个测量仪器的干扰之后，bridge 打开时整次调用只用掉 256,078 项，是默认 524,287 上限的 49%。把 executor 的 `kCoverSize` 从 512K 翻倍到 1M（1A+）没有多出任何一个新的 unique EL2 PC——两边都是 132 个。

2. **2026-07-23 smoke 里那次「524287 项打满」不是 bridge 的覆盖率，是串口控制台。** 该 coverfile 里 84% 的条目来自 pl011 串口驱动自旋等待 UART FIFO（`pl011_read` / `pl011_console_write` / `pl011_console_putchar`），而触发它的只是 bridge 自己那 4 行 `pr_warn_ratelimited("ring overflow")`。**在 115200 波特率的串口控制台上，被测调用内部每打印一行 printk ≈ 10 万条 KCOV 条目。** 当时报告的「87 个 EL2 PC」也是这个洪水挤出来的假象：真值是 132。

3. **bridge 自身的 EL1 drain 循环曾经是第二大消耗方，占 42%。** `pkvm_cov.o` 之前是被 KCOV 插桩的，于是 `pkvm_cov_end()` / `pkvm_cov_runtime_to_link()` 在搬运 EL2 PC 的同时给自己记账：每转发 1 个 EL2 PC 产生约 7 条自噪声条目。加一行 `KCOV_INSTRUMENT_pkvm_cov.o := n` 后这一项精确归零。

4. **真正还在丢覆盖率的只有 EL2 ring，而且丢得不多、位置很明确。** 内核侧计数（1B）给出：每次 `syz_kvm_run_fw_fault` 有 78 次 #23，EL2 callback 命中 28,178 次，成功写入 ring 26,762 次，**ring 内丢失 1,416 次 = 5.02%**；78 次里只有 4 次溢出；**单次 #23 的 raw high-water = 1,088**（ring 容量 509）。下游 `kcov_add_pcs()` 的 requested == accepted，**task KCOV 丢失恒为 0**。

修完 1A 发现的两个问题之后，bridge 的 KCOV 开销从「每个 EL2 PC 附带约 8 条其它条目」变成**近似 1:1**（bridge ON 与 OFF 的差值 27,085，其中 26,670 就是 EL2 PC 本身）。

一句话：**先前判断的「两层容量截断」里，下游那一层是测量仪器造成的假象；上游 ring 的截断是真的，但只有 5%，而且已经量化到可以直接定容量。**

**适用范围（贯穿全文）：以上所有数字都来自同一个固定输入 `syz_kvm_run_fw_fault`（`no_generate`）、同一条 #23 边界、单 CPU 手工串行。** 78 次 #23 的形状由 pvmfw 启动决定。它们是这条路径的可信实测值，不是 pKVM 的普遍结论；换输入面（3B）或换 boundary（#21 / #34-35 / #36-38）之后必须重测。

---

## 2. 方法与三个混杂因素

三次实验用同一个固定 smoke 输入（`sys/linux/test/arm64-syz_kvm_run_fw_fault`，`no_generate`）、同一块板、同一个 CPU（0）、同一组 flag：

```
taskset -c 0 syz-execprog -executor=... -procs=1 -threaded=0 \
        -cover=1 -slowdown=10 -vv=1 -coverfile=<prefix> arm64-syz_kvm_run_fw_fault
```

写 `enable` 的 shell 也绑定到 CPU 0，因为 setup 时的 `get_cpu()` 决定 `owner_cpu`。每个实验 3 个 replicate，**每个 replicate 内部轮换 arm 顺序**，使预热/漂移不会伪装成 arm 效应。

计划原本设想的是简单 ON/OFF 对照。实测发现两个「背景」其实是一阶效应，必须升格为显式因子，因此改成 **2×2×2 全因子**：

### 2.1 console（串口控制台）

板子的引导参数带 `console=ttyAMA0,115200 loglevel=8`——这是部署调试时加的。任何在被测调用内部发生的 printk 都要在 UART 上同步自旋等待。直接测量：4 行 kmsg 在控制台开启时 55 ms，抑制时 3 ms。

**但单纯降低 `console_loglevel` 没用**：`syz-executor` 在 sandbox 建立阶段无条件写 `"7 4 1 3"` 到 `/proc/sys/kernel/printk`（`executor/common_linux.h:5579`），把它改了回去。

quiet arm 因此改成：先把真实 `console_loglevel` 设为 1，再把一个普通文件 bind-mount 到 `/proc/sys/kernel/printk` 上。executor 的写落进影子文件，内核值不变。已验证：跑完之后影子文件是 `7 4 1 3`，真实值仍是 `1 4 1 7`，kmsg 计时仍是被抑制的 3 ms。

### 2.2 kprobe（观测 #23 用的探针）

`pkvm_mem_abort` 的 kprobe + kretprobe 本身很贵（kretprobe trampoline + 栈回溯）：装上它使 OFF 基线从 229,490 涨到 263,500，约 +34,000 条。它是测量仪器不是 campaign 的一部分，所以必须作为因子变化，而不是一直开着。

### 2.3 dedup（这个是解释历史数字用的）

不带 `-coverfile` 时 executor 会在读出后就地去重，日志里的 `coverage` 打印的是 unique 数（约 5,900）；带 `-coverfile` 才是 raw（约 229,000）。两者相差 39 倍，比较历史数字时不要混用。

---

## 3. 1A —— 现有内核上的 debugfs A/B

无需重编内核。24 次运行（8 arm × 3 replicate），每次 call 1 的结果：

| bridge | console | kprobe | total(avg) | uniqPC | EL2 raw | EL2 uniq | bridge-EL1 raw | pl011 raw |
|---|---|---|---|---|---|---|---|---|
| off | loud  | off | 229,415 | 5,847 | 0 | 0 | 156 | 0 |
| off | quiet | off | 230,442 | 5,882 | 0 | 0 | 156 | 0 |
| off | loud  | on  | 263,237 | 6,195 | 0 | 0 | 156 | 0 |
| off | quiet | on  | 263,638 | 6,177 | 0 | 0 | 156 | 0 |
| on  | loud  | off | **524,287 (满)** | 4,643 | 1,022 | 87 | 7,173 | 439,026 |
| on  | loud  | on  | **524,287 (满)** | 4,930 | 1,022 | 87 | 7,173 | 438,190 |
| on  | quiet | off | 446,058 | 6,220 | **26,530** | **132** | **186,488** | 0 |
| on  | quiet | on  | 480,323 | 6,573 | 26,523 | 132 | 186,441 | 0 |

**OFF 组直接回答了「普通 EL1 路径是否已单独打满 KCOV」：没有**，229K / 524,287 = 44%（不带 kprobe）。

**ON/loud 与 ON/quiet 的差异就是整个故事**：同样的内核工作量，只是那 4 行 overflow 警告有没有真的送上 UART。送了，就多出 43.9 万条 pl011 条目、把 area 撑满、并把 EL2 unique 从 132 挤到 87。

### 3.1 谁填满了 KCOV（符号化归因，同一 vmlinux）

`bridge=on, console=loud`（打满的那一组，524,287 条）：

```
   219,730  41.9%  drivers/tty/serial/amba-pl011.c
   146,332  27.9%  include/asm-generic/io.h        (readl_relaxed，pl011 轮询)
    72,907  13.9%  asm/vdso/processor.h            (cpu_relax，同一自旋)
    35,343   6.7%  asm/stacktrace/common.h
     7,193   1.4%  arch/arm64/kvm/pkvm_cov.c
```
串口三项合计 438,969 = **83.7%**。

`bridge=on, console=quiet`（未打满，450,865 条）：

```
   190,685  42.3%  arch/arm64/kvm/pkvm_cov.c       <-- bridge 自己的 drain 循环
    96,670  21.4%  asm/stacktrace/common.h         <-- OFF 基线里也有
    31,388   7.0%  lib/maple_tree.c                <-- OFF 基线里也有
     9,289   2.1%  hyp/nvhe/rust/src/ftrace.rs     <-- 真 EL2
     6,411   1.4%  hyp/nvhe/rust/src/trace.rs      <-- 真 EL2
```
按函数拆：`pkvm_cov_runtime_to_link` 108,220 + `pkvm_cov_end` 82,075。搬运 26,530 个 EL2 PC 用掉 190,685 条自噪声，**比 7:1**——正好是 drain 循环体每次迭代的基本块数。

### 3.2 被控制台洪水挤掉的 45 个 EL2 PC

`comm` 两组的 EL2 PC 集合，quiet 独有的 45 个恰恰是最该看的 pKVM 逻辑：

```
check_donation / __do_donate / guest_complete_donation / guest_ack_donation
pkvm_ipa_range_has_pvmfw / find_memory_region / hyp_alloc_pages
__topup_hyp_memcache / __clean_dcache_guest_page / __invalidate_icache_guest_page
HypSpinlock::lock / assert_lock_held / psci_mem_protect / ...
```

也就是说 2026-07-23 smoke 报告的 EL2 覆盖面，被一个与被测对象无关的调试设置系统性地削掉了三分之一，而且削掉的是 donation 状态机本身。

---

## 4. 1A+ —— 只重编 executor 的下游容量 probe

`kCoverSize` 512K→1M。**同时必须改 `kMaxOutputCoverage`（6 MiB → 13 MiB）**：输出共享内存才是先到的天花板，`ConstMaxOutputSize = 14 MiB`（`pkg/flatrpc/flatrpc.fbs`）是硬上限，`write_cover()` 没有边界检查。补丁：`evidence/.../1aplus-executor-kcovsize.patch`。

**实验 executor 的身份（可复现）：**

```
syz-executor (kCoverSize = 1<<20)  sha256 0946370918ed3bdd7dd7cdb30ed73dfae075b3b97b766542056621d88b31af8b
syz-execprog (未改动)              sha256 5fa8ccc625d4944790ab40d54d4ff0bd765a1871957bc204441c84053f9c2324
```

两个都验证过 **bit-for-bit 可复现**：在同一棵树上打上 `1aplus-executor-kcovsize.patch` 重编，`syz-executor` 得到同一个 sha256；不打补丁重编，`syz-execprog` 与冻结件 `5fa8ccc6…` 完全一致（因此 `kCoverSize` 确实是唯一变量）。工具链 gcc 15.2.0 / GNU ld 2.46、Go 1.26.5。二进制本身没有入库（48 MB），但补丁在库里、且已证明能重建出同一个哈希；板上副本为 `/root/stage2-1a/syz-executor-1m`。运行时旁证：该 executor 的 KCOV 上限确实变成 `1,048,575`。

| arm | total(avg) | EL2 raw | EL2 uniq | pl011 raw |
|---|---|---|---|---|
| off/loud/kpoff | 229,248 | 0 | 0 | 0 |
| off/quiet/kpoff | 229,576 | 0 | 0 | 0 |
| on/loud/kpoff | **1,048,575 (又满)** | 2,239 | **132** | 949,143 |
| on/quiet/kpoff | 442,582 | 26,176 | **132** | 0 |

三个结论：

- OFF 组和 ON/quiet 组的数字**一个字都没变** → 它们在 512K 下就从未被截断过，之前看到的就是真值。
- ON/loud 组在 1M 下**再次打满**，串口条目从 43.9 万涨到 94.9 万 → 串口洪水的需求量没有上界，**扩大 area 永远解决不了它**，只能不在被测调用里 printk。
- **EL2 unique 在所有 ON arm 都是 132，翻倍 area 没有换来任何新的 unique EL2 PC。** 这直接回答了「仅增大 area 是否足够」——问题根本不在这一层。

---

## 5. 1B —— 内核侧 loss accounting

### 5.1 实现（与计划的一处偏差）

计划建议用**单独一张 host-shared stats page**。实际改为把计数器放进已有 ring page 的头部，代价是 PC 槽位从 511 降到 509（0.4%）。

理由：那条 share / EL2 pin / unregister / unshare / 失败则保守泄漏 的生命周期，是这个特性里所有 UAF 风险的所在地，经过了 7 轮 review 才收敛；为了 6 个计数器把它复制一份，在一个「产出必须可信」的度量步骤里是不划算的交易。Step 2 做多页 ring 时本来就要设计正式的 header。

布局与发布：

```c
struct pkvm_cov_ring {
    __u32 count, flags;
    __u32 hits, dropped, nested, __reserved;   /* 每次 #23 的 producer 计数 */
    __u64 pcs[509];
};
```

- `hits` 在**判断能不能放下之前**自增——这正是关键：`count` 在 ring 满之后就不动了，`hits` 不会，所以 `hits - count` 就是这次 #23 精确丢掉的 EL2 PC 数。
- 发布：`hits`/`dropped` 写在 hyp 对 `count`/`flags` 的 release-store 之前，host 的 acquire load 即可看到。`nested` 没有配对的 release，靠 **EL2→EL1 的 ERET**：owner-CPU guard 保证 producer 与 consumer 是同一个 PE，异常返回是 context synchronization event。代码里写明了：将来做 per-CPU / 跨 CPU ring 必须换成显式 release。
- `kcov_add_pcs()` 改为返回实际写入条数，或 `-ENODEV`（任务已不在 trace-pc 模式），使 drain 能区分「area 满」和「消费者中途消失」。
- 每次 #23 的 `pr_warn_ratelimited` **删除**，改为计数 + debugfs 导出；只在 disarm 时（被测调用之外）打一行汇总。
- 新增 `KCOV_INSTRUMENT_pkvm_cov.o := n`。

导出：`/sys/kernel/debug/kvm/pkvm_cov/stats`（只读）与 `stats_reset`（只写）。

### 5.2 数据表（bridge ON，12 次运行）

| 指标 | 值 |
|---|---|
| 每次 smoke 的 #23 drain 数 | 78（12/12 次一致） |
| EL2 callback 命中 `el2_hits` | 平均 28,178 |
| 写入 ring `el2_written` | 平均 26,762 |
| **`LOST_IN_RING`** | 平均 **1,416**（1,231–1,538）= **hits 的 5.02%** |
| 溢出的 #23 次数 | **4 / 78**（12/12 次一致） |
| **单次 #23 raw high-water `hits_max`** | 平均 1,019，**最大 1,088** |
| `nested`（递归 guard 丢弃） | **0** |
| `link_ok` / `link_dropped` | 26,762 / **0** |
| `kcov_requested` / `kcov_accepted` | 相等，**`LOST_IN_KCOV_AREA` = 0** |
| `skip_not_owner` / `skip_no_kcov` | **0 / 0** |

**单次 #23 的命中分布（log2 桶）。** 12 次运行分成两种形态，**必须两种都列**——只引用其中一种会漏掉最上面那个桶：

```
6 / 12 次运行：   hits_256_511 74    hits_512_1023 4
6 / 12 次运行：   hits_256_511 74    hits_512_1023 3    hits_1024_2047 1
```

也就是说：**74 次稳定落在 [256,511]；溢出的永远是另外 4 次，但其中最大的那一次会在 1023 上下摆动**（`hits_max` 实测区间 966–1088，12 次里有 6 次 > 1023）。这个「高水位正好骑在 1024 边界上」的事实，是下面反对 2 页 ring 的直接依据。

### 5.3 1A 两个修复的验证

同一块板、同一 smoke，1B 内核上的 coverfile：

| arm | total | EL2 raw | EL2 uniq | bridge-EL1 raw | pl011 raw |
|---|---|---|---|---|---|
| off/quiet/kpoff | 228,993 | 0 | 0 | **0** | 0 |
| on/quiet/kpoff | 256,078 | 26,670 | 132 | **0** | 0 |
| on/loud/kpoff | 256,072 | 26,702 | 132 | **0** | **0** |

- `bridge-EL1 raw` 由 186,488 → **0**（`KCOV_INSTRUMENT_pkvm_cov.o := n` 生效；连 OFF 组那 156 条 `pkvm_cov_begin` 也消失了）。
- **console 因子彻底退化**：loud 256,072 vs quiet 256,078。为排除「这次启动 console_loglevel 恰好是 0」的可能，另做了一次显式确认：把 `console_loglevel` 设回 7、确认 4 行 kmsg 仍要 34 ms（UART 确实在被驱动），再跑 bridge ON —— `uartRAW = 0`，total 255,617。**bridge 已经不在被测调用内部产生任何 printk。**
- bridge 的净开销 = 256,078 − 228,993 = **27,085**，其中 26,670 是 EL2 PC 本身。**比值从 8.13:1 降到 1.02:1。**

---

## 6. 对第二步的直接输入

1. **EL2 ring 容量：4 页（2,045 个 PC）。** 观测到的单次 #23 raw high-water 是 1,088。2 页 = 1,021 个槽位，**仍然低于已观测到的最大值**，会继续偶发截断；4 页给出约 1.9× 余量。按计划的要求，这是按 **raw callback 高水位** 定的，不是按事后的 unique 数。

2. **不要做 EL1 去重来「保护 KCOV」——KCOV 侧没有压力。** `kcov_accepted == kcov_requested`，`LOST_IN_KCOV_AREA` 恒为 0，bridge ON 只用掉 area 的 49%。计划里那套预分配、per-`KVM_RUN`、无睡眠的 link-PC 去重表（连同它对 `KVM_RUN` session 边界和 `KCOV_ENABLE` 锁外分配点的要求）现在**没有数据支持**，应当推迟到真正观察到 KCOV 压力时再做。这省掉了第二步里最复杂的一块。

3. **下一个该砍的是 EL2 侧的自插桩，收益很大。** ON 组多出来的 26,670 条 EL2 条目里：

   ```
   9,098  ftrace.rs   \  hyp 自己的 ftrace/trace 机制
   6,285  trace.rs    /  合计 15,383 = 58.5%
   ```

   `__hyp_ftrace_trace` / `hyp_ftrace_func_push` / `trace_func` / `trace_func_ret` 这些是 hyp 的追踪基础设施，不是被 fuzz 的 pKVM 逻辑。把它们排除出 SanCov 插桩应当能让每个 ring 槽位承载的有效 pKVM 覆盖率翻倍以上。

   **但排除机制本身尚未验证，不能假定它像 EL1 侧那样是一行 Makefile。** EL1 侧能用 `KCOV_INSTRUMENT_pkvm_cov.o := n` 是因为 kbuild 按 .o 逐个决定编译选项；Rust hyp 是**一个 crate 一次编译**，SanCov 是通过 `-C passes=sancov-module -C llvm-args=-sanitizer-coverage-*` 全 crate 打开的，没有对应的 per-module 开关。两点具体风险：
   - clang 的 `-fsanitize-coverage-ignorelist=` 走的是 CodeGen 构造 pass 时传入的 `SpecialCaseList`，**不是 `-mllvm` 选项**，因此很可能根本无法通过 `-C llvm-args` 到达；
   - `#[coverage(off)]` 属于 `-C instrument-coverage`（LLVM instrprof）体系，与 SanCov 是**两套不同机制**，几乎可以肯定不适用。

   所以这一项要按**可行性实验**做，并且要先试最便宜的路径：查 hyp ftrace/trace 是否本来就有 Kconfig 可以在 fuzz build 里关掉；若没有，再考虑把这两个模块拆成独立编译单元、不带 SanCov 标志。在拿到 `nvhe_rust.o` 的 call-site 增减实测之前，不把任何一种写成既定方案。

   **外推的注意事项：** 58.5% 这个比例是在单次运行（r1-on-quiet-kpoff）**已投递**的 26,311 个 PC 上测的，而 high-water 说的是 `hits`（28,178，含被丢掉的 1,416）。若假设被丢掉的那部分成分相同，单次 #23 的 high-water 会从 1,088 降到约 460——那样现有 509 的 ring 就不再溢出。但这个假设本身未经验证：溢出发生在一次 #23 的尾部，尾部的代码成分未必与整体相同。所以建议的顺序是**先排除 EL2 自插桩、重测 `hits_max`，再据实测值决定 ring 要不要扩到 4 页**，而不是拿这个外推值直接定容量。

4. **campaign 配置：被测调用内部不得有 printk，板子上不要挂串口控制台。** 这不是调优，是数量级问题：一行 printk ≈ 10 万条 KCOV 条目，而且 executor 会强制把 `console_loglevel` 拉回 7，配置层面挡不住。

5. **OFF 基线本身的构成也值得知道**：229K 条里 96,670（42%）是 `stackinfo_on_stack` / `unwind_next_frame_record` 栈回溯，31,388 是 maple_tree。这是 executor sandbox 与 KASAN stack-depot 的开销，与 pKVM 无关。真要压缩每次调用的 KCOV 占用，这是最大的一块——但它同时也是 KASAN 的代价，属于取舍而非缺陷。

---

## 7. 本步骤没有回答的问题

- ~~**那 5% 的 ring 丢失里有没有 unique PC。**~~ **已于 2026-07-24 回答（见 `2026-07-24-pkvm-stage2-step2-multipage-ring.md`）：没有。** 把 ring 扫到 1/2/4 页（509/1021/2045 槽位）后，2 页和 4 页均 `LOST_IN_RING = 0`，而 unique EL2 PC 在三种容量下都是 **同一个 132 元素集合**（两两对称差为 0，`new − old = ∅`）。被丢掉的全是重复 PC。仍限于当前固定 #23 smoke。
- **只覆盖 #23 一条边界、只有一个固定输入。** 上面所有数字都是 `syz_kvm_run_fw_fault` 这一个 no_generate smoke 的，78 次 #23 的形状由 pvmfw 启动决定。可变异输入面（3B）与其它 boundary（#21 / #34-35 / #36-38）的分布还完全未知。
- **仍然是单 CPU 手工串行。** `skip_not_owner = 0` 只是说明 taskset 生效了，不代表 manager 下也会这样。3A 仍是 campaign 的前置条件。

---

## 8. 复现

板子：N90，非默认 GRUB 项 `6.6.30+ pKVM EL2-cov smoke`；默认项仍是已知可用的 `6.6.30-pkvm-fuzz`。

| 阶段 | 内核 Build ID | vmlinux sha256 |
|---|---|---|
| 1A / 1A+ | `33774811160f920789f518c8dd3fc884b9b73d6c` | `c5801a00…` |
| 1B | `3b2d02254e05253fb8d5e0441a249611d4c32964` | `7f858b99…` |

1B 内核 = 基线 `da966ce9a047` + `evidence/step1-measurement-2026-07-24/stage2-el2-kcov.patch`（13 文件，`git apply --whitespace=error` 干净）+ `evidence/stage2-mvp-2026-07-22/stage2.config`（`.config` sha256 未变，仍是 `b1f010f5…`）。Image sha256 `77b79907…`，System.map `1c84969b…`。

**符号化必须用与运行同一个 build 的 vmlinux**：两次链接之间地址会整体平移（`__hyp_text_start` 从 `…81d88ed4` 变成 `…81d86ed4`），脚本里的地址桶也要跟着换。

脚本在 `evidence/step1-measurement-2026-07-24/scripts/`：`run-1a.sh`（1A 与 1A+ 共用，`EXECUTOR=` 覆盖）、`run-1b.sh`（加 stats 采集）、`attribute.sh` / `attribute-1b.sh`（批量 addr2line 归因）。
