# 004 —— hyp 捐赠泄漏检查坏了,而且是两个方向都坏

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

> **本文所有行号以 `klinux @348c94763cc6`(`#4`)为准** —— 即所有观测所在的构建,可用 `git show 348c94763cc6:<path>` 复核。早前版本引用的是工作树,而工作树带着 002 的修复、`mmu.c` 整体下移 5 行。

每一个开启过脏页日志的 VM,在销毁时都会打印:

```
kvm [35488]: 18446744073709543424B of donations to the nVHE hyp are missing
```

读起来像是"有 18 EB 的客户机内存捐给了管理程序、再也没回来"。这个读法的**每一部分都是错的**:量是 8 KiB,符号是反的,没有内存丢失,而产生它的这个检查,恰恰是全树里唯一用来抓真正的 hyp 捐赠泄漏的东西。

---

## 背景知识

### pKVM 的"捐赠"机制

在 pKVM 架构下,EL2（管理程序）运行在比 Host 内核更高的特权级,需要自己的数据结构——stage-2 页表、VM 管理结构、vCPU 状态等。EL2 有自己的分配器和内存池,但**池子里的页全部来自 Host**:它没有独立的内存来源,只能靠 Host"捐赠"（donate）页面来补充。每次 Host 把页面交给 EL2,就在一个叫 `protected_hyp_mem` 的计数器里记上一笔;VM 销毁时,EL2 把页面还给 Host,计数器相应扣减。如果一切正常,VM 销毁后这个计数器应当归零。

如果计数器没归零——比如有页面捐出去没还回来——那就是真正的内存泄漏,`kvm_arch_destroy_vm()` 末尾的检查就会报警。这个检查是全树里**唯一**用来抓 hyp 捐赠泄漏的东西,所以它坏了不只是一个 cosmetic 问题。

### memcache 与 topup

EL2 在运行过程中会消耗 Host 捐赠的页面（比如建立新的 stage-2 页表项）。为了不每次都走超级调用向 Host 要内存,Host 会预先准备一个"内存缓存"（memcache）给 EL2 用。当缓存里的页不够了,Host 就往里面补充——这个操作叫 topup。

topup 的本意是"往缓存里加页直到达到最低水位"。关键在于:Host 往缓存里加的页会被记入 `protected_hyp_mem` 计数器,而销毁时按缓存的**全部剩余量**扣减计数器。如果某次 topup 忘了记账（加了页但没在计数器上加）,销毁时扣减照常发生——计数器就变成了负数。

### 有符号 vs 无符号打印

C 语言里 `atomic64_t` 内部存的是有符号的 `s64`,但代码用 `%llu`（无符号长长整型）来打印它。一个负数用无符号格式打印时,会被解释成一个巨大的正数:-8192 变成 18446744073709543424（约 18 EB）。而文案里的 "missing"（捐出去没还回来）描述的是正残值的含义——真正的泄漏留下正数。负残值意味着记账出了问题,不是泄漏。

---

## 症状

`arm.c:263-265`,位于 `kvm_arch_destroy_vm()` 末尾:

```c
	if (atomic64_read(&kvm->stat.protected_hyp_mem))
		kvm_err("%lluB of donations to the nVHE hyp are missing\n",
			atomic64_read(&kvm->stat.protected_hyp_mem));
```

`atomic64_read()` 返回有符号的 `s64`,却用 `%llu` 打印:

```
18446744073709543424  −  2^64  =  −8192 bytes  =  −2 页
```

所以计数器是**负两页**,而文案里的 "missing"(捐出去没还回来)描述的是**相反**的方向。真正的泄漏留下的是**正**残值。

## 机制

**置信度:已根因定位。** 失衡是实测的,触发条件由对照矩阵隔离,代码路径与之吻合。

**这个计数器记的是什么。** `kvm->stat.protected_hyp_mem` 记录的是宿主交给 EL2 的内存量（字节数）。每交给 EL2 一批页,就在这个计数器上加上相应的量;EL2 把页还回来时,再减掉。VM 销毁时计数器应当归零——如果不归零,说明有页面捐出去没还回来,即真正的泄漏。整个树里,往这个计数器加的有三处,减的有四处:

```
加   mmu.c:1796     pkvm_mem_abort()          它那次 topup 实际增加的量
加   pkvm.c:401     __pkvm_create_hyp_vm()    pgd_sz
加   alloc.rs:1648  hyp_alloc_account()       EL2 侧分配        (C 对应:nvhe/alloc.c:626)

减   arm.c:511      kvm_arch_vcpu_destroy()   stage2_mc.nr_pages 全量
减   pkvm.c:347     pkvm_destroy_hyp_vm()     stage2_teardown_mc.nr_pages 全量
减   pkvm.c:431     __pkvm_create_hyp_vm()    pgd_sz,建 VM 失败时的回滚
减   alloc.rs:1729  hyp_free_account()        EL2 侧释放        (C 对应:nvhe/alloc.c:661)
```

其中两对是**自平衡**的 —— pgd 的加/回滚,以及 EL2 分配器自己的加/释放 —— 都不参与失衡。早前版本只列了两加两减;结论不变,但枚举不全,原因是漏掉的那两对都跨两行,单行 grep 看不见。

**不对称:减法按全量,加法按增量。** 问题的关键在于记账粒度的不对称。两处销毁时的减法都按**整个 memcache 的剩余量**来,不管这些页当初是从哪条路进来的:

```c
// arm.c:511 —— 销毁 vCPU 时
atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT,
             &vcpu->kvm->stat.protected_hyp_mem);
free_hyp_memcache(&vcpu->arch.stage2_mc);   // 真正把页还回去
```

也就是说,减法数的是 memcache 里**现在有多少页**,而不是"当初记账加了多少页"。如果某些页进来时没有被记账,销毁时减法照常按全量扣——计数器就少了这么多。造成伤害的正是这一半。

**`__topup_hyp_memcache()` 自己从不记账**(`kvm_host.h:119-135`,它只负责分配和入栈),所以记账完全是调用方的责任。而往 `vcpu->arch.stage2_mc` 里 topup 的一共三处,只有一处做了:

| 位置 | 上下文 | 记账 |
| --- | --- | --- |
| `mmu.c:1791` | `pkvm_mem_abort()`,普通缺页路径 | **有**,`mmu.c:1796` |
| `mmu.c:1749` | `pkvm_relax_perms()` 的 `logging_active` 分支 | **无** |
| `handle_exit.c:369` | `handle_hyp_req_mem()`,EL2 向宿主要内存 | **无** |

从未记账的路进来的页,会被减掉却从未被加过,计数器就负了这么多。具体到脏页日志路径:`mmu.c:1749` 那次 topup 往 memcache 里加了两页（见下文）,没有记账;销毁时那两页被减掉了——计数器就少了两页。

注意前提条件比早前版本写的“销毁时还留在 memcache 里”更弱:被 EL2 **消费掉**的页同样会让它变负 —— 那些页在销毁时经 `stage2_teardown_mc` 回流,并在 `pkvm.c:347` 被减掉。两种情形下减法都真实发生,而加法从未发生。

**量级是可预测的,不只是被观测到的。** `kvm_mmu_cache_min_pages(mmu)` 就是 `kvm_stage2_levels(mmu) - 1`(`stage2_pgtable.h:31`),而 logging 分支那次 topup 要的正是这个数。本板 40 位 IPA 对应三级 stage-2,于是它等于 **2** —— 与实测的 −8192 字节精确吻合。这把第一版留下的一个开放问题关掉了。

**触发条件是隔离出来的,不是论证出来的。** 三种模式,每种 5 次(`../003-dirty-log-guest-no-huge-page-support/evidence/2026-08-05-mode-matrix.txt`):

| 模式 | 脏页日志 | 大页 | `KVM_RUN` | 本消息 |
| --- | --- | --- | --- | --- |
| default | 开 | 2 MiB 块 | 失败(问题 003) | **有** |
| `nohuge` | 开 | 强制 4 KiB | 成功 | **有** |
| `nodirty` | 关 | 2 MiB 块 | 成功 | **无**,0/5 |

页大小不影响,`KVM_RUN` 成败也不影响。

**但"开启脏页日志"这个触发条件写得太松了。** 15 次逐次相关给出 13/15 `-E2BIG` 与 13/15 teardown 消息,且完全重合;那两次客户机没有缺页的运行,两者都没出现。消息真正需要的是**脏页日志的缺页路径被执行** —— 这反而强化了因果判断,因为 `mmu.c:1749` 正在那条路径上。

最后一列正是指向 `mmu.c:1749` 的依据:它是 `logging_active` 分支**内部**的 topup,而且执行在问题 003 会失败的那个超级调用**之前** ——

```c
	if (logging_active) {
		struct kvm_hyp_memcache *hyp_memcache = &vcpu->arch.stage2_mc;
		int ret = topup_hyp_memcache(hyp_memcache,              /* mmu.c:1749 —— 未记账 */
					     kvm_mmu_cache_min_pages(&kvm->arch.mmu), 0);
		if (ret)
			return ret;

		ret = kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn);  /* 003 在这里失败 */
```

—— 这正是失衡在两种结局下都存在的原因。

**有一步是推断而非实测:** 那两页来自 `mmu.c:1749` 而不是 `handle_exit.c:369`,是按排除法得到的 —— `:1749` 是唯一"仅在开启脏页日志时才会到达"的未记账 topup。给三个 topup 点各加一个计数器,或把 `stage2_mc.nr_pages` 暴露到 debugfs,就能坐实。

## 触发条件

`kvm-arm.mode=protected` 宿主上,任何普通 VM,跑过 vCPU 之后对 memslot 开启 `KVM_MEM_LOG_DIRTY_PAGES`。**不需要大页。**

## 复现

`repro/probe-dirtylog-thp.c` —— 与问题 003 是同一个程序,它的 `nohuge` 模式能干净地把本问题隔离出来:

```
./probe-dirtylog-thp nohuge     # KVM_RUN 成功,本消息照样出现
./probe-dirtylog-thp nodirty    # 不开脏页日志,不出现
```

该消息由 `kvm_arch_destroy_vm()` 打印,也就是**进程退出之后**。程序返回后立刻读 `dmesg` 会间歇性漏掉 —— 见 run 记录,先前两次就是这样读的,并因此得出过错误结论。

## 影响面

**没有内存丢失,而且代码给出的依据比测量更硬。** 做减法的地方,正是把页还回去的地方:

```c
	atomic64_sub(vcpu->arch.stage2_mc.nr_pages << PAGE_SHIFT,   /* arm.c:511  */
		     &vcpu->kvm->stat.protected_hyp_mem);
	free_hyp_memcache(&vcpu->arch.stage2_mc);                   /* arm.c:513  */
```

而 `__free_hyp_memcache()` 会把 cache 里每一页弹出并释放(`kvm_host.h:137-149`)。**销毁时被减掉的每一页,都是被还回去的那一页。** 减法是对的,是与之配对的**加法**从未发生——这正是残值为负而非为正的原因。换句话说:页面本身的生命周期是完整的（给了 EL2、又还了回来）,只是记账少加了一笔。所以没有真正的内存泄漏,只是计数器不准。

直接测内存的尝试做了,但**分辨不了**,已记录在 `evidence/2026-08-05-memory-ab.txt` 里,免得后人重做:每组 300 次 VM 循环、每个边界都 `drop_caches`,`MemFree` 的增量在两组里都会向两个方向摆动 ±55 MB,而 2 页/VM 的损失只有 2400 kB——**低一个数量级**。更早那次不带 `drop_caches` 的尝试,两次重复给出了互相矛盾的结果。这个测量最多只能支持"不存在**大规模**泄漏"。

真正坏掉的是**泄漏探测器本身**,这也是它值得立案而不是当作 cosmetic 的原因:

- **真从未记账的通道漏掉的内存,是看不见的。** `handle_hyp_req_mem()` 和脏页日志那次 topup 把页交给 EL2 而统计毫不知情。如果那些页真的没还回来,`protected_hyp_mem` 不会升高,这个检查会保持沉默——探测器对真正的泄漏是瞎的。
- **探测器现在天天喊狼来了。** 每个开过脏页日志的 VM 销毁都报一次骇人的、错误的 18 EB,真出事时没人会信——这是"狼来了"效应,不是无害的噪音。

## 与其他问题的关系

002、003、004 都在同一种输入上触发 —— 普通 VM、vCPU 跑过、然后开启脏页日志 —— 这也是它们被一起发现的原因。但它们**不是同一个缺陷**,而且是用矩阵**实验分开**的,不是靠论证:

| | 问题 002 | 问题 003 | **问题 004** |
| --- | --- | --- | --- |
| 是什么 | 范围 TLB 刷新请求了 EL2 已停用的超级调用 | 脏页日志路径假定 4 KiB 映射 | 脏页日志的 topup 未记账 |
| 需要脏页日志 | 是 | 是 | 是 |
| 需要大页 | **否** | **是** | **否** |
| `KVM_RUN` 成功时仍出现 | 是 | 不适用(它就是那个失败) | **是** |
| 位置 | 宿主 `mmu.c:190` | EL2 `permissions.rs:147` | 宿主 `mmu.c:1749` |
| 后果 | TLB 失效静默地从未执行 | 客户机无法恢复运行 | 一个计数器是错的 |

`nohuge` 模式是判别器:`KVM_RUN` 成功,003 消失,而 002 与 004 照旧出现。`nodirty` 则三者全消。

**003 与 004 是同一个函数里的邻居。** `pkvm_relax_perms()` 的 `logging_active` 分支同时包含两者:`:1749` 那次未记账的 topup(本问题),以及六行之后被 003 弄失败的那个超级调用。两者看起来是同一类疏忽 —— 一个分支没有得到它的兄弟们得到的处理 —— 但它们互相独立:修好任一个,另一个仍在。

**两者都不是我们自己补丁的回归**,004 也不是 003 引起的。所有观测都在 `#4` 上,该构建含 001 的修复、不含 002 的修复。完整来龙去脉见 003 的"为什么现在才浮出来":真正暴露这一切的,是**验证 002 的需求** —— 它迫使我们写出本项目第一个"先跑 vCPU、再开脏页日志"的程序。

**本树里的前情。** 2026-07-30 那次 OOM 调查把这条未被跟踪的 host→EL2 捐赠通道列为最有希望的剩余候选,并记为 *"Unverified — no evidence gathered yet"*(`notes/pkvm/evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-ANALYSIS.md:221`、`ROOT-CAUSE-CONFIRMED.md:274`)。本问题就是那条通道,而且有了确定性复现器 —— 但要注意它解决了什么、没解决什么:它证明该通道**存在且不记账**,但**并不解释**那次调查里 25.9 GiB 的残差(方向和量级都对不上)。另外那份文档称 `handle_hyp_req_mem()` 会 "accounts them into `kvm->stat.protected_hyp_mem`",按本树代码**并非如此**,这很可能就是线索当时断在那里的原因。

## 修复状态

> **修复已提交到 klinux:`61173329e416`**((a)(c) 两处,补回缺失的 `atomic64_add()`)与 **`f8ac14623978`**((b),按有符号打印并区分方向)。**仅通过编译**,`disposition: fix-proposed`,未验证。判据是销毁时那条消息彻底不再出现。

已按上述方式本地修复。上游检索**已经做过**,对象是 `kernel-refs/ack` 的三个分支(`aosp/android15-6.6`、`android16-6.12`、`android17-6.18`),证据在 `evidence/2026-08-05-upstream-ack.txt`。答案按部分而不同 —— 本问题其实是三个来源各异的缺陷:

### (a) `handle_hyp_req_mem()` —— klinux **删掉了**上游的记账

上游带着这里缺失的那段代码:

```c
/* ACK android15-6.6, arch/arm64/kvm/handle_exit.c:344-352 */
	case REQ_MEM_DEST_VCPU_MEMCACHE:
		nr_pages = vcpu->arch.stage2_mc.nr_pages;
		ret = topup_hyp_memcache(&vcpu->arch.stage2_mc, req->mem.nr_pages, 0);
		nr_pages = vcpu->arch.stage2_mc.nr_pages - nr_pages;
		atomic64_add(nr_pages << PAGE_SHIFT, &kvm->stat.protected_hyp_mem);
		atomic64_add(nr_pages << PAGE_SHIFT, &kvm->stat.protected_pgtable_mem);
		return ret;

/* klinux @348c94763cc6, arch/arm64/kvm/handle_exit.c:365-371 */
	case REQ_MEM_DEST_VCPU_MEMCACHE:
		return topup_hyp_memcache(&vcpu->arch.stage2_mc, req->mem.nr_pages, 0);
```

增量计算和两个 add 一起被删。`protected_pgtable_mem` 在 `klinux/arch/arm64/` 全树都不存在 —— 第二个统计跟着一起没了。**这是有精确上游参照的 klinux 回归,修法就是把上游那段恢复回来。**

### (b) `%llu` 打印有符号 —— **上游**缺陷,三个分支全未修

三个 ACK 分支里逐字相同,最新的也一样:

```
aosp/android15-6.6   arm.c:235
aosp/android16-6.12  arm.c:267
aosp/android17-6.18  arm.c:284
	kvm_err("%lluB of donations to the nVHE hyp are missing\n",
		atomic64_read(&kvm->stat.protected_hyp_mem));
```

没有可 backport 的东西,修法是本地的;而且这一条**值得报给上游**:按有符号打印,并区分两个方向 —— 正残值是"捐出去没还回来",负残值是记账缺陷。把两者混为一谈,正是这条消息无法解读的原因。

### (c) logging 分支的 topup —— **上游同样未记账**

ACK 6.6 在自己的 `mmu.c:1670` 有一模一样的缺口:`logging_active` 的 topup 之后直接就是超级调用,没有 `atomic64_add`。上游在这一点上**自相矛盾**,因为两百行之后的拆块路径(`mmu.c:1931-1938`)是记账的。所以这一半是继承来的、上游未修,本地修掉就意味着与上游产生偏差 —— 照 `pkvm_mem_abort()` 的 `nr_pages` 增量写法来。

### 动手顺序

(a) 最便宜,而且是唯一有上游答案可抄的;(b) 两行;(c) 才是真正消掉那 −2 页的,最好和 (a) 一起做,免得记账只恢复一半。三者都不碰 EL2 ABI,这也是它整体比 003 简单得多的原因。

## 残留风险

只修报告不修记账,等于把消息静音、让探测器彻底失明。只修记账而不审计其余捐赠通道,则未被跟踪的仍然未被跟踪:`__pkvm_topup_hyp_alloc()`(`pkvm.c:1243`)topup 的是一个局部 memcache,同样不记账,而本复现器并未触及它。
