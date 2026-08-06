# 003 —— 大页支撑的客户机一旦开启脏页日志就无法继续运行

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

> **本文所有行号以 `klinux @348c94763cc6`(`#4`)为准** —— 即所有观测所在的那个构建,可用 `git show 348c94763cc6:<path>` 复核。早前的版本引用的是工作树,而工作树带着 002 的修复、`mmu.c` 整体下移 5 行,导致每一处 `mmu.c` 行号都偏了正好这么多。

在 `kvm-arm.mode=protected` 的宿主上,对一个内存由**透明大页**支撑的客户机开启 `KVM_MEM_LOG_DIRTY_PAGES`,客户机就**再也无法恢复运行**:下一次 `KVM_RUN` 返回 `-E2BIG`,vCPU 不再前进。透明大页是默认行为,所以这就是常规配置;而脏页日志是热迁移与快照的基础机制。

板子本身不受影响 —— 不挂死、不 Oops,死的只是客户机。

## 症状

```
first  KVM_RUN ret=0 errno=0 exit=6            (KVM_EXIT_MMIO —— 客户机运行正常)
after dirty logging on
second KVM_RUN ret=-1 errno=7 (Argument list too long) exit=0
```

`errno 7` 即 `E2BIG`。**在 `#4` 上高频但并非确定性:独立复核测得 25 次中 22 次(88%)**;本文早前那次 5 连测恰好是 5/5,这也是"确定性"这一说法的由来。

差的那几次不是复现器不稳,而是本问题自己的机制段就预测到的:要走到出问题的路径,客户机那次写必须**真的缺页**,而在 `#4` 上让写保护对客户机可见的 TLB 失效被问题 002 拒掉了,于是是否缺页取决于 CPU 有没有换出陈旧的可写表项。**002 修好之后这里应当变成 100%** —— 这比"002 的补丁更小"是更有力的先修理由。

## 机制

**置信度:已根因定位。** 只改变页粒度的硬件 A/B 把原因隔离出来,代码链与之逐项吻合。

**实测**(三种模式,每种 5 次,`evidence/2026-08-05-mode-matrix.txt`):

| 模式 | 客户机内存 | 脏页日志 | `KVM_RUN` 失败 |
| --- | --- | --- | --- |
| default | 2 MiB THP 块 | 开 | **5/5**,更大样本下 22/25 |
| `nohuge` | `MADV_NOHUGEPAGE` 强制 4 KiB | 开 | **0/5** |
| `nodirty` | 2 MiB THP 块 | 关 | **0/5** |

两个条件缺一不可,单独任一个都不复现。矩阵真正依赖的是那两个 `0/5` 的格子,它们是硬零 —— 不确定性只出现在失败那一格,原因见上文症状段。

**因果链**,每一环都完整读过:

```
客户机内存被 2 MiB 块映射
  → 开启脏页日志 → 写保护
  → 客户机写 → stage-2 权限异常
  → mmu.c:1755   kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)   ← 只有 gfn,没有 order,
                                                                        也没有重试包装
  → EL2 permissions.rs:147  guest_get_valid_pte(&mut host_addr, guest_addr, 0, &mut pte)
                                                                        ↑ order 硬编码为 0
  → EL2 guest.rs:1598,1618   size = PAGE_SIZE << 0 = 4 KiB
                             if kvm_granule_size(level) != size { return -E2BIG }
                                          ↑ 实际粒度是 2 MiB → 不匹配
  → 原样返回宿主(permissions.rs:148-152),经 pkvm_relax_perms() 的
    logging 分支(mmu.c:1755-1757)一路抛出 KVM_RUN
```

**`-E2BIG` 是设计好的信号,而这是唯一没人接住它的调用点。** `pkvm_call_hyp_nvhe_ppage()`(`pkvm.c:240-268`)存在的意义就是吸收这个错误:

```c
	int err = call_hyp_nvhe(pfn, gfn, order, args);
	switch (err) {
	/* The stage-2 huge page has been broken down */
	case -E2BIG:                        /* pkvm.c:258 */
		if (order)
			order = 0;          /* 降级到 4 KiB 重试 */
		else
			return -EINVAL;     /* 绝不让 -E2BIG 外泄 */
		break;
```

也就是说 `-E2BIG` 的语义是"order 不对,换小的再来",而这个包装保证它永远到不了调用方。三处调用都走它 —— `mmu.c:325`(unmap)、`mmu.c:1366`(写保护)、`mmu.c:1762`(放宽权限)。**唯独脏页日志这次是裸调:**

```c
	ret = kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn);   /* mmu.c:1755 —— 裸调 */
	if (ret)
		return ret;
```

**缺陷的两半在同一个函数里碰头。** `pkvm_relax_perms()` 有两个分支:`logging_active` 在 `:1755` 裸调,`else` 分支在 `:1762` 走包装。脏页日志这条同时是**唯一 EL2 侧永远匹配不上块**(order 写死 0)、也是**唯一没有重试包装接住后果**的调用。

**EL2 侧的结构性佐证。** `guest_get_valid_pte()` 有**三个活的调用点** —— 两个传真实 `order`,一个传 `0`:

```
host.rs:1714        guest_get_valid_pte(..., order, ...)
host.rs:1897        guest_get_valid_pte(..., order, ...)
permissions.rs:147  guest_get_valid_pte(..., 0,     ...)   ← 脏页日志
```

早前的版本数成了四个,多列了 `host.rs:1669`。那一行位于一段**被注释掉的同名函数副本**内 —— `/*` 在 `host.rs:1635`,`*/` 在 `host.rs:1683`。**`mem_protect/*.rs` 里有好几段这样的注释块**(`guest.rs:1521` 是 `guest_get_valid_pte` 自己的另一份副本),所以在这些文件里 grep 到的行,引用前必须先验注释边界。

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

模式矩阵把它和本问题分开了:它在**两种**开启脏页日志的模式下都出现(包括 `KVM_RUN` 成功的那种),在 `nodirty` 下从不出现。所以它只与"开启脏页日志"相关,与大页无关,也不是 `-E2BIG` 失败的后果。它已单独立为 [issue 004](../004-hyp-donation-accounting-imbalance/ISSUE_zh.md),机制在那里定了下来:未记账的 topup 在 `mmu.c:1749`,与本问题所失败的那个超级调用**同处一个分支、只差六行**;而销毁时 `arm.c:511` 按 `stage2_mc.nr_pages` 全量减。

这条线正是 2026-07-30 那次 OOM 调查列为最有希望、却记为 *"Unverified — no evidence gathered yet"* 的候选(`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`)。现在它有确定性复现了。另注:那份文档称 `handle_hyp_req_mem()` "accounts them into `kvm->stat.protected_hyp_mem`" —— 按本树代码**并非如此**,这很可能就是线索当时断在那里的原因。

## 为什么现在才浮出来 —— 以及与问题 002 的关系

它**不是**修复问题 002 之后才暴露的。上面所有观测都在 `#4` 上,而该构建**不含** 002 的修复;那个修复构建为 `#5`,尚未部署。也不是 001 的修复暴露的:001 的复现程序**重度执行**了该修复改过的代码,却不产生这里的任何一种签名。

真正把它暴露出来的,是**验证 002 的需求**。评审确认 002 的复现程序永远无法验证 002 的修复,因为它不创建 vCPU:`pkvm.handle` 仍为 0 时,EL2 会把这个 handle 映射成越界索引并直接返回、不做刷新,于是修复之后告警消失只是因为调用变成了 no-op。要验证 002,就必须写一个**先把 vCPU 跑起来**、再开启脏页日志并恢复运行的程序。

这个序列在本项目里从未被产生过。001 的复现器跑 vCPU 但从不碰脏页日志;002 的碰脏页日志但从不跑 vCPU;模糊测试同时启用了 `ioctl$KVM_SET_USER_MEMORY_REGION` 和 `ioctl$KVM_RUN`,但 2026-07-31 与 2026-08-05 两轮的控制台日志里这个签名是**零命中** —— 那些运行从未恰好把两者按所需顺序组合到同一个 VM 上。这个组合第一次被执行就失败了,之后每次都失败。

另外,确实存在一层意义上"**修好 002 会让 003 在原本可能藏住的硬件上暴露出来**":要走到出问题的路径,客户机那次写必须**真的缺页**。在 002 修复之前,写保护施加了,但让它对客户机可见的 TLB 失效被拒绝了,所以是否缺页取决于 CPU 有没有把陈旧的可写表项换出去。在 N90 上它照样缺页了 —— 这正是 `-E2BIG` 能在 `#4` 上被观测到的原因 —— 但那是这台硬件与时序的性质,不是保证。002 修好之后,失效真的发生,缺页随之必然,这个失败也就从"偶然可见"变成"稳定可见"。

顺序对后来读 tracker 的人很重要:**002 不导致 003,003 也不是我们两个补丁引入的回归。** 003 是 klinux 既有行为,被一种新的输入形状第一次触达;而 002 的修复会让触达它变成确定的,而不是碰运气。

## 修复状态

未修。**上游没有可 backport 的补丁**,但理由不是最初记在这里的那个 —— 见紧接着的更正。证据:`evidence/2026-08-05-upstream-ack66.txt` 与 `../005-unmap-guest-fails-after-dirty-log/evidence/2026-08-06-upstream-provenance.txt`,均取自 `kernel-refs/ack`。

> **更正(2026-08-06):这个缺陷是从 ACK 继承的,不是 klinux 引入的。**
>
> 本节原先写的是"klinux 把上游的处理函数改了名、丢掉了 `pfn`"。这是错的,错因是当初只拿 `android15-6.6` 做比较,而那并不是 klinux 所继承的那条线。ACK 提交 **`ee88afa47b55`**(2024-04-03,*"Huge page support for pKVM guest relax perm"*)引入的正是 `__pkvm_host_dirty_log_guest(u64 gfn, struct pkvm_hyp_vcpu *vcpu)` —— 与 klinux 完全相同的签名 —— 它通过 `__check_host_unshare_guest(vm, &phys, ipa, 0)` 在 EL2 侧反查地址,**order 硬编码为 0**。而 `-E2BIG` 粒度检查在那笔提交时就已经在 `guest_get_valid_pte()` 里了。那笔提交的宿主侧改动,就是 klinux 的 `mmu.c:1749/1755/1762`,逐行相同。
>
> 所以 klinux 既没有改名也没有丢掉 `pfn`:它带的就是 ACK 自己的设计,连缺陷一起。上游对它做的处置是**删掉** —— `6e3ff69cb190`(2025-01-28),在 np-guest 脏页日志被挪进通用 `user_mem_abort()` 之后。
>
> 核查时顺带发现的拆块系列(`c8303029c094`、`024d995fb`、`8d4b47fe9`、`b052adb82`,2025-05 至 2025-12)**不是**这条路径的修复:它们是在该超级调用被删除之后才落的,而且 `8d4b47fe9` 自己写明,它处理的是 relinquish 路径上客户机**共享**类调用方的 `-E2BIG`。
>
> 原分析中站得住的是下面这部分:`android15-6.6` 带的是**另一个**处理函数,它从宿主拿 `pfn`,因而根本不会在 order 0 上走表。那仍然是一个真实的、很小的、可以照抄的设计 —— 只是它属于上游的另一条线,而不能用来证明 klinux 弄坏了什么。

**`android15-6.6` 的 EL2 处理函数不需要 order,因为宿主把 `pfn` 直接给了它:**

```c
/* ACK android15-6.6, arch/arm64/kvm/hyp/nvhe/mem_protect.c:2771 */
int __pkvm_dirty_log(struct pkvm_hyp_vcpu *hyp_vcpu, u64 pfn, u64 gfn)
{
	u64 host_addr = hyp_pfn_to_phys(pfn);      /* 宿主直接给的 */
	...                                        /* 没有 guest_get_valid_pte,没有粒度检查 */
	ret = kvm_pgtable_stage2_map(&vm->pgt, guest_addr, PAGE_SIZE, host_addr, ...);
```

klinux 的处理函数是 `__pkvm_host_dirty_log_guest(gfn)`,它没有 `pfn`,于是不得不在 EL2 侧走一遍客户机页表去反查 `host_addr` —— 那次只认 4 KiB 的 `guest_get_valid_pte(..., 0, ...)` 就是这么来的。按上面的更正,这个形状是 `ee88afa47b55` 的,不是 klinux 的;`android15-6.6` 只是走了另一条路。

**而且那条线会先把块拆开,靠的是 klinux 完全没有的一套机制:**

| 组件 | ACK `android15-6.6` | klinux |
| --- | --- | --- |
| `__pkvm_host_split_guest` 超级调用 | 有 | **无** |
| 宿主侧 `__pkvm_pgtable_stage2_split()` | `mmu.c:1916` | **无** |
| `handle_hyp_req_split()` | `handle_exit.c:366` | **无** |
| `grep -rn split_guest arch/arm64/` | 有命中 | **无任何命中** |

上游在那个函数上方的注释写得很直白:*"`pkvm_pgtable_stage2_split()` **can be called with dirty logging**"*(`mmu.c:1912-1914`)。流程是:EL2 撞上块 → 通过 hyp 请求让宿主拆 → 宿主钉住 511 个子页、调 `__pkvm_host_split_guest`、把固定页跟踪记录改写成 `order = 0` → 之后下游一律面对 4 KiB。这就是上游的脏页日志处理函数可以无条件映射 `PAGE_SIZE` 的原因。

注意上游**并不是**"一棵没有大页的树":两边的 `struct kvm_pinned_page` 都带 `order` 和 `pins`。

所以修复是本地工作,有三种形状:

0. **照搬 `android15-6.6` 的处理函数签名** —— 由宿主把 `pfn` 传进来,像 `__pkvm_dirty_log(hyp_vcpu, pfn, gfn)` 那样,EL2 就再也不会在 order 0 上走客户机页表,粒度检查根本不会被问到。三者中最小,而且是照抄在跑的上游代码、不是发明 —— 只不过来自兄弟分支,而非 klinux 自己的祖先。注意它**解决不了问题 005**,那个处理函数同样有 005。
1. **完整照 `android15-6.6` 做** —— 把拆块链补上(`handle_hyp_req_split` + `__pkvm_pgtable_stage2_split` + `__pkvm_host_split_guest` 超级调用),让块根本到不了脏页日志路径。改动最大;但与上游完全一致,若将来要与 ACK 对账,这一点很值钱。
2. **照本树的兄弟做** —— 给脏页日志的超级调用补一个 `order` 参数(和 `__pkvm_host_wrprotect_guest` 一样),让 EL2 侧按 `PAGE_SIZE << order` 映射,并通过 `pkvm_call_hyp_nvhe_ppage()` 配一个 `__pkvm_dirty_log_call(pfn, gfn, order, args)` 回调走。改动更小、更局部,而且"块已经被拆过"的情形由重试分支免费兜住。

形状 0 最小,而且是直接照抄一个在跑的上游设计;形状 2 最贴合本树的写法;形状 1 能阻止本树继续偏离 ACK。三者都不是 cherry-pick —— klinux 自己的祖先 `ee88afa47b55` 就带着这个缺陷,而继承了它的那条分支选择了删功能而不是修它。

## 残留风险

本问题**阻塞了 002 的既定验证路线**。原计划是通过"对运行中的客户机开启脏页日志,确认写保护之后的写入进入 dirty bitmap"来证明范围 TLB 刷新生效 —— 而客户机无法恢复运行,这条路就走不完。要么先修 003,要么换一条路验证 002:用 EL2 覆盖率证明 `handle___pkvm_tlb_flush_vmid` 确实被执行,或者在 `MADV_NOHUGEPAGE` 下做同一个 dirty bitmap 检查 —— 矩阵显示那种模式下客户机是能恢复的。
