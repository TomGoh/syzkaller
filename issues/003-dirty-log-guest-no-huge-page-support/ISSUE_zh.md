# 003 —— 大页支撑的客户机一旦开启脏页日志就无法继续运行

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

在 `kvm-arm.mode=protected` 的宿主上,对一个内存由**透明大页**支撑的客户机开启 `KVM_MEM_LOG_DIRTY_PAGES`,客户机就**再也无法恢复运行**:下一次 `KVM_RUN` 返回 `-E2BIG`,vCPU 不再前进。透明大页是默认行为,所以这就是常规配置;而脏页日志是热迁移与快照的基础机制。

板子本身不受影响 —— 不挂死、不 Oops,死的只是客户机。

## 症状

```
first  KVM_RUN ret=0 errno=0 exit=6            (KVM_EXIT_MMIO —— 客户机运行正常)
after dirty logging on
second KVM_RUN ret=-1 errno=7 (Argument list too long) exit=0
```

`errno 7` 即 `E2BIG`。确定性:5 次运行 5 次失败。

## 机制

**置信度:已根因定位。** 只改变页粒度的硬件 A/B 把原因隔离出来,代码链与之逐项吻合。

**实测**(三种模式,每种 5 次,`evidence/2026-08-05-mode-matrix.txt`):

| 模式 | 客户机内存 | 脏页日志 | `KVM_RUN` 失败 |
| --- | --- | --- | --- |
| default | 2 MiB THP 块 | 开 | **5/5** |
| `nohuge` | `MADV_NOHUGEPAGE` 强制 4 KiB | 开 | **0/5** |
| `nodirty` | 2 MiB THP 块 | 关 | **0/5** |

两个条件缺一不可,单独任一个都不复现。

**因果链**,每一环都完整读过:

```
客户机内存被 2 MiB 块映射
  → 开启脏页日志 → 写保护
  → 客户机写 → stage-2 权限异常
  → mmu.c:1760   kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)   ← 只有 gfn,没有 order
  → EL2 permissions.rs:147  guest_get_valid_pte(&mut host_addr, guest_addr, 0, &mut pte)
                                                                        ↑ order 硬编码为 0
  → EL2 guest.rs:1598,1618   size = PAGE_SIZE << 0 = 4 KiB
                             if kvm_granule_size(level) != size { return -E2BIG }
                                          ↑ 实际粒度是 2 MiB → 不匹配
  → 原样返回宿主(permissions.rs:148-152),经 pkvm_relax_perms() 的
    logging 分支(mmu.c:1760-1762)一路抛出 KVM_RUN
```

**结构性佐证 —— 这是一处漏改,不是设计取舍。** `guest_get_valid_pte()` 有四个调用点,大页支持系列给其中三个补上了真实的 `order`,漏了一个:

```
host.rs:1669        guest_get_valid_pte(..., order, ...)
host.rs:1714        guest_get_valid_pte(..., order, ...)
host.rs:1897        guest_get_valid_pte(..., order, ...)
permissions.rs:147  guest_get_valid_pte(..., 0,     ...)   ← 脏页日志
```

宿主侧的 HVC ABI 呈现同样的缺口:

```c
kvm_call_hyp_nvhe(__pkvm_host_wrprotect_guest,   handle, gfn, order);      /* mmu.c:1361 */
kvm_call_hyp_nvhe(__pkvm_host_relax_guest_perms,         gfn, order, prot); /* mmu.c:1730 */
kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest,           gfn);             /* mmu.c:1760 */
```

而这棵树里的大页支持系列覆盖了五条路径,唯独没有这一条:

```
275629ed658f  THP support for pKVM guests
bb35d8934803  Huge page support for pKVM guest relax perm
5b77817a1876  Huge page support for pKVM guest wrprotect
972a2211ac11  Huge page support for pKVM guest unshare
ac23d6fd5aba  Huge page support for pKVM guest memory reclaim
b852c9e9fcea  Huge page support for pkvm_pinned_page
              (没有 "Huge page support for pKVM guest dirty log")
```

也就是说:对块做写保护是好的,对块放宽权限也是好的 —— 但被写保护的**块**一旦挨了一次写异常、控制流进入脏页日志分支,就撞上唯一没被改造的那条路。

**尚未直接观测:** 没有在 EL2 侧盯着 `guest_get_valid_pte()` 把这个 `-E2BIG` 返回出来。A/B 只改变了粒度这一个变量,而粒度检查是该路径上唯一的 `-E2BIG` 来源;但用 EL2 覆盖率对两个变体取差集、显示只在失败侧执行的那一行,才算把最后一步钉死。

## 触发条件

1. 宿主以 `kvm-arm.mode=protected` 启动
2. **普通** VM(`KVM_CREATE_VM` type 0)—— 保护型 VM 走另一条路
3. 客户机内存由大页支撑(匿名内存的默认行为)
4. vCPU **已经运行过**,因而 hyp VM 存在、页面已经过 EL2 映射
5. 对已存在的 memslot 追加 `KVM_MEM_LOG_DIRTY_PAGES`
6. 客户机写入一个被写保护的页

## 复现

`repro/probe-dirtylog-thp.c`,一个二进制三种模式:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-thp repro/probe-dirtylog-thp.c
./probe-dirtylog-thp            # 大页 + 脏页日志  -> KVM_RUN -1/E2BIG
./probe-dirtylog-thp nohuge     # 4 KiB + 脏页日志 -> 正常
./probe-dirtylog-thp nodirty    # 大页,不开脏页日志 -> 正常
```

它同时在每一步从 `/sys/kernel/debug/kvm/<pid>-<vmfd>/` 采样 `kvm->stat.protected_hyp_mem`。**无害**:由 `alarm()` 兜底,不挂死,可在无法断电的机器上运行。

## 影响面

**在默认配置下,这个平台的热迁移与快照是不工作的。** 任何 VMM 只要对 THP 支撑的客户机内存开启脏页日志 —— 而不特意声明的话拿到的就是 THP —— 客户机就会当场停住。所幸失败是响亮的:`KVM_RUN` 返回错误,而不是默默损坏数据。

目前可用且已实测的绕过办法:在客户机把内存缺页进来之前,对其 `madvise(MADV_NOHUGEPAGE)`,代价是丢掉大页的 TLB 收益。

## 同时观测到、但**不是**同一个缺陷

每一次开启脏页日志的运行,都会在 VM 销毁时把 `kvm->stat.protected_hyp_mem` 留成负数,由 `arm.c:263` 报出:

```
kvm [35488]: 18446744073709543424B of donations to the nVHE hyp are missing
```

`18446744073709543424` = `0xFFFFFFFFFFFFE000` = **−8192**,即 −2 页:该检查读的是有符号的 `atomic64_t` 却用 `%llu` 打印,而且文案假定是泄漏,实际方向相反。

模式矩阵把它和本问题分开了:它在**两种**开启脏页日志的模式下都出现(包括 `KVM_RUN` 成功的那种),在 `nodirty` 下从不出现。所以它只与"开启脏页日志"相关,与大页无关,也不是 `-E2BIG` 失败的后果。它需要自己的编号;当前首选假设是:经 `handle_hyp_req_mem()`(`handle_exit.c:369`)topup 的页从不计入 `protected_hyp_mem`,而 `kvm_arch_vcpu_destroy()`(`arm.c:512`)却按 `stage2_mc.nr_pages` 全量减 —— **未经证实**。

这条线正是 2026-07-30 那次 OOM 调查列为最有希望、却记为 *"Unverified — no evidence gathered yet"* 的候选(`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`)。现在它有确定性复现了。另注:那份文档称 `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`" —— 按本树代码**并非如此**,这很可能就是线索当时断在那里的原因。

## 为什么现在才浮出来 —— 以及与问题 002 的关系

它**不是**修复问题 002 之后才暴露的。上面所有观测都在 `#4` 上,而该构建**不含** 002 的修复;那个修复构建为 `#5`,尚未部署。也不是 001 的修复暴露的:001 的复现程序**重度执行**了该修复改过的代码,却不产生这里的任何一种签名。

真正把它暴露出来的,是**验证 002 的需求**。评审确认 002 的复现程序永远无法验证 002 的修复,因为它不创建 vCPU:`pkvm.handle` 仍为 0 时,EL2 会把这个 handle 映射成越界索引并直接返回、不做刷新,于是修复之后告警消失只是因为调用变成了 no-op。要验证 002,就必须写一个**先把 vCPU 跑起来**、再开启脏页日志并恢复运行的程序。

这个序列在本项目里从未被产生过。001 的复现器跑 vCPU 但从不碰脏页日志;002 的碰脏页日志但从不跑 vCPU;模糊测试同时启用了 `ioctl$KVM_SET_USER_MEMORY_REGION` 和 `ioctl$KVM_RUN`,但 2026-07-31 与 2026-08-05 两轮的控制台日志里这个签名是**零命中** —— 那些运行从未恰好把两者按所需顺序组合到同一个 VM 上。这个组合第一次被执行就失败了,之后每次都失败。

另外,确实存在一层意义上"**修好 002 会让 003 在原本可能藏住的硬件上暴露出来**":要走到出问题的路径,客户机那次写必须**真的缺页**。在 002 修复之前,写保护施加了,但让它对客户机可见的 TLB 失效被拒绝了,所以是否缺页取决于 CPU 有没有把陈旧的可写表项换出去。在 N90 上它照样缺页了 —— 这正是 `-E2BIG` 能在 `#4` 上被观测到的原因 —— 但那是这台硬件与时序的性质,不是保证。002 修好之后,失效真的发生,缺页随之必然,这个失败也就从"偶然可见"变成"稳定可见"。

顺序对后来读 tracker 的人很重要:**002 不导致 003,003 也不是我们两个补丁引入的回归。** 003 是 klinux 既有行为,被一种新的输入形状第一次触达;而 002 的修复会让触达它变成确定的,而不是碰运气。

## 残留风险

本问题**阻塞了 002 的既定验证路线**。原计划是通过"对运行中的客户机开启脏页日志,确认写保护之后的写入进入 dirty bitmap"来证明范围 TLB 刷新生效 —— 而客户机无法恢复运行,这条路就走不完。要么先修 003,要么换一条路验证 002:用 EL2 覆盖率证明 `handle___pkvm_tlb_flush_vmid` 确实被执行,或者在 `MADV_NOHUGEPAGE` 下做同一个 dirty bitmap 检查 —— 矩阵显示那种模式下客户机是能恢复的。
