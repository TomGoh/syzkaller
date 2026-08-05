# pKVM 修复系列记录 — 2026-07-30

配套阅读：`evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-CONFIRMED.md`（根因分析与置信度状态表）。本文记录的是**修复侧**：查了什么、为什么这么修、迁移与自写的取舍、预期效果、以及哪些没修。

工作区：`/home/jose/ksrc-pkvmfix`，分支 `review/8707`（独立 clone，非主插桩树）。主插桩树 `/home/jose/common-stage2mvp` 全程未被触碰（始终 17 个改动文件）。

> **修订说明（第二轮同事审查后）**：第一版有一处**阻塞级错误** —— 迁移进来的 THP 记账提交在本 lineage 上语义是错的，会漏记 `(1<<order) - pins` 页。该提交已撤掉、改为本地提交并同时修好第二处（见 §2.3、§4.2、§11.6）。另修正 6 处数值/方法错误，并补做了完整内核构建。第一版 §12 的自查写"全部通过"是错的 —— 自查漏掉了这个 bug，原因见 §12。

---

## 1. 摘要

一条 **7 提交**的 Gerrit 链，修掉 4 个缺陷：3 个上游迁移 + 4 个本地 `KYLIN:` 提交。净改动 **+96 / -20**（`kvm_host.h` 8/0、`mmu.c` 73/13、`pkvm.c` 15/7）。**尚未推送。**

| 缺陷 | 严重性 | 修复来源 | 置信度 |
|---|---|---|---|
| `union {mmu_page_cache, stage2_mc}` 类型混用 → `__kvm_mmu_topup_memory_cache` Oops | 用户态 5 个 syscall 可触发的 host 崩溃 | 本地（上游 6.6 系无对应修复） | **CONFIRMED**，寄存器级 |
| `kvm_arch_flush_remote_tlbs_range()` 缺 pKVM 分支 → TLBI 未执行 + 27,674 条 WARN | 正确性 + 可观测性 | 本地（hunk 取自 mainline） | **CONFIRMED**，`res.a0 == -1` |
| THP 记账在两个销毁路径少退 `(1<<order) - 1` 页 | 可用性（最终永久 `-ENOMEM` from `KVM_RUN`） | **本地**（上游 hunk 语义不适用，见 §2.3） | 代码确证 |
| 销毁/回收路径记到 `current->mm` 而非 `kvm->mm` | 记错地址空间的 `locked_vm` | 上游迁移（带 `Cc: stable`） | 代码确证 + 上游独立确认 |

---

## 2. 修了什么

### 2.1 union 类型混用（那个可复现的 Oops）

`vcpu->arch.mmu_page_cache` 和 `vcpu->arch.stage2_mc` 是**同一块 32 字节**（`kvm_host.h` 里的匿名 union），两种视图对每个字段的解释都不同：

| 偏移 | 作为 `mmu_page_cache` | 作为 `stage2_mc` |
|---|---|---|
| 0–3 | `gfp_zero` | `head[31:0]` |
| 4–7 | `gfp_custom` | `head[63:32]` |
| **8–15** | **`kmem_cache`** | **`nr_pages`** |
| 16–23 | `capacity`, `nobjs` | `flags` |

在 `kvm-arm.mode=protected` 主机上，同一个 vCPU 会被两种视图同时驱动：翻译异常走 `mmu.c:2322` 的 pKVM 分支进 `pkvm_mem_abort()`，把这块内存填成 hyp memcache；随后的**权限异常**不满足那个判据，落到 `user_mem_abort()`，其序言把同一块字节当 mmu memory cache 去 top-up，于是 `stage2_mc.nr_pages` 被当作 `mc->kmem_cache` 指针交给 `kmem_cache_alloc()`。

两次崩溃的寄存器**逐位吻合**：`x19`（kmem_cache 参数）= 3 和 2，正是 `topup_hyp_memcache()` 留在 `kvm_mmu_cache_min_pages()` 水位的 `nr_pages`；`x20`（gfp = `gfp_custom | gfp_zero`）= `0x2cc28022` 和 `0xf350d021`，把 `stage2_mc.head` 的两个半字 OR 起来正好得到这两个值，且 `head[31:0]` 页对齐、`head[63:32]` 等于低 12 位 —— 还原出 ~136 GiB 处的合法页对齐物理地址。**这两次 Oops 无需假设任何先前的内存损坏即可完整解释**：两侧都是对一个活着的 `struct kvm_vcpu` 的合法、类型正确的访问，这就是 KASAN 沉默的原因。

**修法**：把序言那次 top-up 用 `!is_protected_kvm_enabled()` 门禁掉。这样足够且最小 —— pKVM 下 `user_mem_abort()` 唯一做的事是分派到 `pkvm_relax_perms()`，而后者在 logging 路径上**自己会补 `stage2_mc`**，所以序言那次纯属多余。

**刻意没有做的事**：没有把 `memcache` 的声明搬进分支内部。上游正是这么做才引入了 CVE-2025-37996（守卫为假时该指针未初始化就进了 `kvm_pgtable_stage2_map()`）。

**一条被撤回的过强结论**：早期版本写过 "pKVM 下绝不能进入 `user_mem_abort()`"。错。进入是**刻意设计**，代码自己断言了这一点（`WARN_ON(fault_status != ESR_ELx_FSC_PERM)`）；要禁的只有那次 legacy top-up。

**顺带的效果**：这道门禁也**堵掉了泄漏候选 B** —— legacy top-up 一旦不再于 pKVM 下运行，那 40 页 `objects[]` 分配就不会发生。但这**不能反向证明**历史上的 OOM 就是候选 B 造成的。

### 2.2 TLB range flush 缺 pKVM 分支

`kvm_arch_flush_remote_tlbs_range()` 不认识 pKVM，于是在 protected 主机上用 finalize 前的 hypercall、并传一个指向 `kvm->arch.mmu` 的**host 裸指针**。EL2 在 protected 模式初始化后把 `hcall_min` 抬到 id 20（`__pkvm_prot_finalize`），低于它的一律拒收且**根本不调 handler**，返回 `SMCCC_RET_NOT_SUPPORTED`。

**两个分支都受影响**：D3000 没有 FEAT_TLBIRANGE，走 `kvm_call_hyp(__kvm_tlb_flush_vmid, mmu)` = **id 11**；有 TLBIRANGE 的机器走 `kvm_call_hyp(__kvm_tlb_flush_vmid_range, …)` = **id 12**，同样位于 finalize gate 之前。所以这不是 D3000 特有问题，本修复的 `is_protected_kvm_enabled()` 判断在两个分支之前，两者都覆盖。

这一点是**寄存器级确证**而非推断：反汇编归档 vmlinux 得知该帧里 `x21` 就是 `res.a0`，而 `#51` 构建的 27,674 条 WARN 里 `x21` 全是 `0xffffffffffffffff` = -1。这个值**唯一地**指向 dispatcher 的 `id < hcall_min` 拒绝路径 —— `handle_host_hcall()` 是在调 handler **之前**就写好 `SMCCC_RET_SUCCESS` 的，而 `handle___kvm_tlb_flush_vmid()` 本身没有错误返回，所以任何"进了 handler"的情况都不可能留下 -1。**TLBI 不是失败了，是压根没执行。**

**证据边界**：已确认的是"旧 HVC 被拒绝、TLBI 没执行"。至于脏页漏记 —— 调用者是脏页日志的 write-protect 路径，write-protect 由 EL2 的 `__pkvm_wrprotect` 完成、它按设计不做 TLBI 而依赖这次 flush，所以 guest 可以继续通过残留的可写 stage-2 TLB 表项写入且不被记录。**这是源码可推导的风险，实际的 migration/snapshot 数据丢失尚未运行验证**（验证方法见 §9.1）。另外没人会发现，因为该函数无条件 `return 0`，把通用层"失败就退化为全量 flush"的兜底也关掉了 —— 而那个全量路径 `kvm_arch_flush_remote_tlbs()` **有** pKVM 分支、本来能成功。

**修法**：给 range 版本补上兄弟函数同样的分支，退化到 handle-based 的 `__pkvm_tlb_flush_vmid`（id 28），它收的是不透明 `pkvm_handle_t`、由 EL2 对自己的 VM 表校验。

**一条被撤回的错误建议**：早期版本提过"或者把 TLB 系列 id 移到 `__pkvm_prot_finalize` 之后"。**绝不可采用** —— id 11 收 host 裸指针并在 EL2 `kern_hyp_va()` 后解引用，finalize 之后 host 是不可信的，重新暴露它等于主动削弱 pKVM 威胁模型。那道门禁在正确工作，bug 是 host 还在调 finalize 前的 ABI。

**成因可精确定年**，这是一次 merge 漏改而非设计选择：`commit ad6e033c0464`（Quentin Perret，2022-07-07，已在我们树里）给 `kvm_arch_flush_remote_tlbs()` 加分支时，range 版本还不存在；mainline 的 `commit c42b6f0b1cde`（Raghavendra Rao Ananta，2023-08-11）**晚 13 个月**才加了 range 版本，且没有 pKVM 分支 —— 因为 mainline 直到 6.14 才有 EL2 托管的 guest stage-2。Android rebase 到 6.6 时把新函数原样吸收，兄弟函数的 pKVM 感知没跟过来。它的 6.12 双胞胎 `commit b4e76eacd6fa` 同样只改了非 range 版本。

### 2.3 THP 记账：**必须按整个 folio 退，不能按 `pins` 退**

这是第一版的**阻塞级错误**，由同事审查发现。

先把账本讲清。charge 侧，`pkvm_mem_abort()` 一次性按整个 folio 收费：

```c
	ret = account_locked_vm(mm, page_size >> PAGE_SHIFT, true);   /* 2 MiB THP = 512 */
	...
	ppage->pins = 1 << ppage->order;
```

退款侧，`pkvm_host_reclaim_page()` 在**部分 relinquish 时一分钱都不退**，只有 `pins` 归零那一次才一次性退掉整个 `1 << order`：

```c
	pins = ppage->pins;
	if (!pins)
		mtree_erase(...);
	...
	if (WARN_ON(!ppage) || pins)
		return;                                    /* 部分 relinquish：不退 */
	account_locked_vm(mm, 1 << ppage->order, false);   /* 归零时：整额退 */
```

**因此：一个还在树里的 entry，其全额 `1 << order` charge 都还挂着，与 guest 已经 relinquish 了多少个子页无关。** 按 `pins` 退就会漏掉 `(1 << order) - pins`：

```
map 时 charge            512
guest relinquish 100     pins = 412，已退 0
teardown 按 pins 退      412
永久残留                 100
```

两个会整体销毁 entry 的路径原本都只退 1，两处均改为 `1UL << ppage->order`：

- `pkvm_unmap_range()` 的 `cnt++` → `cnt += 1UL << ppage->order`（喂给 `pkvm_flush_unaccount()`）；必须在 `pkvm_unmap_guest()` **之前**快照大小，后者会 `kfree(ppage)`
- `__pkvm_destroy_hyp_vm()` 的 `account_locked_vm(mm, 1, false)` → `account_locked_vm(mm, 1UL << ppage->order, false)`

**为什么不能照搬上游的 `pages += pins`**：6.12 的 `commit 9ad2c87f0789` 用的是 `pages += pins`，那个形式假设 teardown 时 `pins == 1 << order`，而**本 lineage 不满足这个不变式** —— `pkvm_call_hyp_nvhe_ppage()` 明确处理拆分状态（`if (ppage->pins < (1 << order)) order = 0;`），部分状态是预期会出现的。活跃 `android15-6.6` 分支头后来给 `pkvm_host_reclaim_page()` 加了 `WARN_ON_ONCE(ppage->pins != 1)`，即**上游是用断言来保证那个不变式，而不是在记账上兼容它的缺失**；那条断言我们这里也没有。所以这必须是本地提交：正确的代码已经不是原 hunk 的语义，继续挂原作者和原 Change-Id 会造成误导。

**后果**：漂移方向向上且单调，长寿命 VM 最终永久坐在 `RLIMIT_MEMLOCK` 上限，之后每次缺页在 `pkvm_mem_abort()` 的 charge 处失败、`-ENOMEM` 从 `KVM_RUN` 返回。它还会把 `d7c317637aa2` 自己声明的那个代价（短窗口内的 spurious `-ENOMEM`）放大成永久失败。

**与今天调查的三个发现无关**，写清以免误读为 OOM 那条线的进展：`locked_vm` 是拿去和 rlimit 比较的**计数器**，多记它不消耗任何物理页，数学上不可能贡献那 25.9 GiB 残差。而且已验证 charge 失败时 `pkvm_mem_abort()` 走 `goto unpin` → `unpin_user_pages()` + `kfree(ppage)`，**连失败路径也不漏页**。

### 2.4 记错 mm

`__pkvm_destroy_hyp_vm()` 和 `pkvm_host_reclaim_page()` 都用 `current->mm` 退账，但收费发生在 `pkvm_mem_abort()`，那里 `current->mm == kvm->mm` 由 `kvm_main.c:4124` 的 `if (vcpu->kvm->mm != current->mm) return -EIO;` 保证。销毁路径从 `kvm_vm_release` 走，**可以是任意持有 fd 的进程**（继承的 fd、或经 `SCM_RIGHTS` 传递），于是会去减错一个地址空间的 `locked_vm`。同一个文件里 `pkvm_unmap_range` 用 `kvm->mm`、`__pkvm_destroy_hyp_vm` 用 `current->mm` 这个不一致本身就是线索。

---

## 3. 上游调研

### 3.1 搜索方法：四条已确认的教训

1. **`-S` 只统计字符串出现次数** —— 在调用外面加一个 `if` 不改变次数，会漏掉修复。
2. **`-G <函数名>` 也不够** —— 它只匹配**增删行**。一个只改函数体、不动签名行的提交，其签名行是**上下文行**，`-G` 匹配不到。已实测：`git log --all -G'kvm_arch_flush_remote_tlbs_range' -- arch/arm64/kvm/mmu.c` 只返回引入提交 `c42b6f0b1cde`，**漏掉了确实改过该函数体的 `fce886a60207`**。正确做法是 `git log -L '/函数签名/,+N:文件'`，或直接搜"修复会新增的那行内容"。第一版据 `-G` 写下的"从来没有任何 commit 修过它"是无根据的。
3. **本仓库有 4017 个 tag、25 个分支，上游修复经常只能从 tag 到达。** 必须 `git log --all --source`。
4. **"这个 commit 我们已经有了"≠"这个修复我们已经有了"，而且"同名 hunk"≠"同语义 hunk"。** 多 lineage 的同一逻辑改动会有两个改编版：`e602818486b6`（6.6）与 `9ad2c87f0789`（6.12）同作者同日期同标题，但 6.6 版丢了 teardown 记账那一块。更危险的是反方向 —— hunk 的语义可能依赖 lineage 特有的不变式，§2.3 就是这么翻车的。`merge-base --is-ancestor` 只证明 commit 在，不证明 hunk 在，更不证明 hunk 的前提成立。

### 3.2 用到的数据源

| 来源 | 状态 | 用途 |
|---|---|---|
| 本地对象库（`common-stage2mvp`，4017 tag） | 可用 | 绝大部分考古 |
| `android.googlesource.com/kernel/common` gitiles `+/<ref>/<path>?format=TEXT` | **可用** | 直接取活跃分支的文件内容 |
| 同上的 `+log/` history 页 | **403，需登录** | 拿不到 commit 历史 |
| 同上的 git 传输（冷启动） | **两次 TLS 中断失败** | — |
| `github.com/aosp-mirror/kernel_common` | 可用 | 拉到 `android15-6.6` 完整历史（滞后：head `e148935a1d48`，2025-10-08，6.6.111） |
| 上一条补齐对象后再拉 googlesource | **成功**（增量） | 拿到真正的分支头 `9f24219bc984` = **6.6.142** |

注意坑：第一次后台拉取报 exit 0，实际是管道里 `tail` 的返回码；fetch 本身 TLS 失败了。**判断 fetch 成功要看 ref 是否存在，不看 exit code。**

### 3.3 结论：哪些有现成 patch

| 需要的修复 | 上游有吗 | 处理 |
|---|---|---|
| `pkvm_host_reclaim_page` 解锁后读 `pins` 的 UAF + `1<<order` | ✅ `63a99c1fb8e5` | 已迁（`3ae13572d106`，本轮之前完成） |
| `pkvm_mem_abort` 大页回退路径记账 | ✅ `ed14b491ec76` | 已迁（`9a13ca20af8d`，本轮之前完成） |
| `current->mm` → `host_kvm->mm` | ✅ `04512258010d`（活跃 6.6，`Cc: stable`） | **本轮迁移** |
| 两个销毁路径的 THP 记账 | ⚠️ 6.12 有 `9ad2c87f0789`，但**语义不适用本 lineage** | **本地写**（见 §2.3） |
| union / 那个 Oops | ❌ 只存在于 mainline 6.14 的大改内 | 本地写 |
| TLB range flush | ❌ 同上 | 本地写（hunk 取自上游） |

**"没有"的证据（已按 §3.1 的教训重做）：**

- `git log -L '/int kvm_arch_flush_remote_tlbs_range/,+10:arch/arm64/kvm/mmu.c' aosp/android15-6.6` → 只有引入提交 `c42b6f0b1cde`。这是在**真正的 6.6 分支**上按函数行范围搜的，比 `-G` 可靠。
- gitiles 直取 `android15-6.6` **分支头（2026-07-30，6.6.142）**的 `mmu.c`：`user_mem_abort()` 的 top-up 无 pKVM 门禁、`kvm_arch_flush_remote_tlbs_range()` 无 pKVM 分支，两处都未修。这是最直接的证据。
- 实测 `git cherry-pick 73a4f4ffe584` → **`mmu.c` 里 19 个冲突 hunk**。携带 union 修复的那个（#14）依赖 `fault_is_perm` 重构、`kvm_mmu_cache_min_pages(vcpu->arch.hw_mmu)` 和 `void *memcache`（CVE-2025-37996 的成因）；其余 17 个是 `KVM_PGT_FN()`、`kvm_s2_mmu` 参数化、`kvm_init_ipa_range()`、`kvm_nested_s2_flush()`、`topup_hyp_memcache_account()`、`pkvm_mem_abort_device()` / `__pkvm_host_map_guest_mmio` —— 全是本 lineage 不存在的设计，且都要同步移植进 Rust EL2。这不是"冲突多少"的问题，是**两套 pKVM guest 内存模型**。

### 3.4 CVE-2025-37996 与本系列的关系

`CVE-2025-37996 = "KVM: arm64: Fix uninitialized memcache pointer in user_mem_abort()"`，`Fixes: fce886a60207`，由 `a26d50f8a4a5`（Sebastian Ott）+ `157dbc4a321f` 修复。它**不是我们的 bug，而是"修我们这个 bug 的补丁"引入的新 bug**，两者触发在同一个 `if` 的**相反分支**上：我们的需要守卫为真，CVE 需要守卫为假。我们不受影响（`fce886a60207` 未应用），但**一旦 backport 那个大改就会引入它**，顺序是强制的。这也是 §2.1 里"不搬 `memcache` 声明"那条决定的直接依据。

上游受影响范围（NVD）：**6.14 – 6.14.6，以及 6.15-rc1 – rc5**。Android 6.12 只能表述为"包含引入提交的 backport 分支"，不能写成 upstream `v6.12–v6.15-rc4`。

---

## 4. 迁移与自写的取舍

原则：**能迁就迁，不限 lineage、不限 ACK；实在找不到、或上游 hunk 的语义在本 lineage 不成立，才自己写。同时只迁修复必需的，不把上游欠账全搬过来。**

### 4.1 `9b47e206590b` — 迁移 `04512258010d`

上游原版改的是 ACK 里不存在的 `__pkvm_pgtable_stage2_reclaim()`，Fuad Tabba 在 ACK 侧的适配注记写着"applied the same current->mm to host_kvm->mm fix in the equivalent functions: `__pkvm_destroy_hyp_vm()` and `pkvm_host_reclaim_page()`" —— 和我们需要的两处完全一致。1 个冲突（`tmp` 指针来自我们没有的 interval-tree 转换），解为保留本地声明集。带 `Cc: stable`、Marc Zyngier 签署。

### 4.2 THP 记账：**从"迁移"改成"本地写"**

第一版把 `9ad2c87f0789` 迁了进来，并为省掉批量化前置 `f9e18319445d` 而窄化成逐次退 `pins`。**那是错的** —— 见 §2.3：`pins` 不是本 lineage 的正确退款额。现已撤掉该迁移提交（`cdf7ff6526e1`），改为一条本地 `KYLIN:` 提交（`56cd6857fe73`），同时修 `pkvm_unmap_range()` 和 `__pkvm_destroy_hyp_vm()` 两处。

这也一并解决了第一版遗留的 `cnt++`：第一版说"等附近提交定案后再做"，那个理由**没有 correctness 依据**，同事的批评是对的 —— 既然这条链本来就在修 THP 记账，就该一次修完。

### 4.3 `7dc007fdbdd4` / `37807076230a` — 本地 `KYLIN:` 提交

见 §3.3 的证据。两个提交的信息里都写明了"hunk 取自哪个上游 commit"以及"整 commit 为何不可 backport"，便于将来 rebase 时对照。

---

## 5. 刻意没有迁的

活跃 `android15-6.6`（**6.6.142**，我们基线是 6.6.30 时代）在 `mmu.c`/`pkvm.c` 上我们缺 **24 个提交**。按"不把欠账全搬过来"的要求，只取对应**已确证问题**的，明确未取：

| commit | 主题 | 未取的理由 |
|---|---|---|
| `9cfc94644ba4` | Pre-alloctate mtree nodes in `pkvm_mem_abort()` | 它在 EL2 map 之前预留 dummy range，因此同时消除 `-ENOMEM` **和** `-EEXIST` 两个前提（`-EEXIST` 也会在 pin/account 清理后退出）。但它服务的是泄漏候选 A 与 OOM 的**因果关系**，而那条因果未确认。依赖顺序已摸清：排在 interval-tree 转换**之前**，仍作用于 maple tree，前置是 `aeaa3ba902c2` + `d53a54962630` |
| `f9e18319445d` | count the pages … account once | 批量优化，不修 bug |
| `960d37426bef` | Handle races gracefully in `pkvm_relax_perms()` | 未对应到已确证现象（但函数正在我们的 Oops 路径上，值得后续关注） |
| `ccc8abc3d9b0` | Enable RCU for pinned pages mtree | 同上 |
| `e56d181356a4` | Convert `kvm_pinned_pages` to an interval-tree | **结构性分水岭**，之后的提交都假设 interval tree。真正的迁移断点 |
| `9fb1a06cda29` | Fix accounting when VM creation fails | 未对应已确证现象 |
| `21e2595401ca` | Fix error path of allocator topup | 同上 |
| `9849a1376ae3` | Fix protected mode handling of pages larger than 4kB | 同上（比 GitHub 镜像新） |
| `1569a0028664` | Handle missing userspace mapping in pKVM mem abort | 同上（比 GitHub 镜像新） |
| 其余 ~11 个 | nVHE 栈、kmemleak、iommu CMA、hyp SPLIT、debugfs 等 | 与本次调查无关 |

---

## 6. 最终链

```
56cd6857fe73  Haoze Wu           KYLIN: pkvm: uncharge the whole folio for THP pinned pages
9b47e206590b  Bradley Morgan     BACKPORT: FROMGIT: KVM: arm64: account pKVM reclaim against the VM mm
37807076230a  Haoze Wu           KYLIN: pkvm: flush guest TLBs by handle in the range helper
7dc007fdbdd4  Haoze Wu           KYLIN: pkvm: don't top up the legacy memcache under pKVM
d7c317637aa2  Haoze Wu           KYLIN: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock
3ae13572d106  Vincent Donnefort  ANDROID: KVM: arm64: Fix THPs reclaim with ballooning
9a13ca20af8d  Quentin Perret     ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM
```

3 个上游迁移全部保留原作者与原 `Change-Id`；`9b47e206590b` 带 `(cherry picked from commit …)` 出处行，适配差异写在 trailers 下方的 `[ Adapted … ]` 注记里。

逐提交精确改动量（`git show --numstat`）：`d7c317637aa2` = `kvm_host.h` **+8/-0**、`mmu.c` **+49/-7**。

---

## 7. 验证记录

### 7.1 完整内核构建（已完成）

`d7c317637aa2` 改了 `struct kvm_protected_vm` 的布局，会影响 `struct kvm_arch` / `struct kvm` 的后续字段偏移，进而影响 **Rust nVHE bindings 与 `kvm_nvhe.o`**。因此对象级 compile-test **不足以**证明系列可用。已执行全量构建：

```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gwd3000_lenovox1d3000m_defconfig
./scripts/config --file .config --disable CONFIG_DEBUG_INFO_BTF
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image
```

结果（exit 0，日志中 0 个 error）：

| 产物 | 时间 | 大小 / hash |
|---|---|---|
| `bindings_generated.rs`（Rust nVHE bindings） | 13:58:38 | **已重新生成** |
| `arch/arm64/kvm/mmu.o` | 13:58:36 | 828,816 B |
| `arch/arm64/kvm/pkvm.o` | 13:59:43 | 462,624 B |
| `arch/arm64/kvm/hyp/nvhe/kvm_nvhe.o` | 13:59:32 | 4,376,504 B |
| `vmlinux` | 14:03:17 | `sha256 121fc5e049ee662fc456c213e42211a941594255ceec5c9981f29d6302e0f9b5` |
| `arch/arm64/boot/Image` | 14:03:18 | `sha256 37d22ae3edb35eae39971fb596400c7a1f6862df1f01db80a47d2f30af4a7a4f` |

kernel release `6.6.30+`。构建日志：`fullbuild.log`（会话 scratchpad）。关掉 `CONFIG_DEBUG_INFO_BTF` 是因为本机 `tools/bpf/resolve_btfids/libbpf` 编不过，与本系列无关。

### 7.2 其他

- **对象级 compile-test**：`scripts/build-changed-objects.sh --range d7c317637aa2..HEAD` 通过。**注意**：首次运行会报 STALE —— 对象已最新时 make 不重编，脚本按 mtime 判失败。必须 `rm` 掉 `.o` 强制重编才算真验证。
- **checkpatch**：本地提交各 1 error + ≤1 warning。error 全是 `Remove Gerrit Change-Id's before submitting upstream` —— checkpatch 是按上游投递写的，Gerrit 反过来强制要求，**忽略、不要删**。warning 是 `commit <hash> ("<长标题>")` 无法折行导致的 >75 字符。**`9b47e206590b` 的 warning 不是长行，而是 `UNKNOWN_COMMIT_ID`** —— 它上游 `Fixes:` 指向的 mainline 提交不在本仓库，正常。
- **Change-Id / Signed-off-by**：全部提交各 1 个 Change-Id。
- **工作区**：`ksrc-pkvmfix` 干净；主插桩树 `common-stage2mvp` 全程 17 个改动文件未变。
- **未做**：**没有推送**。命令 `cd /home/jose/ksrc-pkvmfix && git review`。链底三个提交的哈希与已推送到 Gerrit 的三个相同，**据此推测** `git review` 只会为上面 4 个创建新 change；但这依赖 Gerrit 服务端当前状态，本地仓库无法独立证明，实际以 `git review` 输出为准。
- **未做**：§9 的全部运行时验证。**构建通过不等于行为正确**，尤其见 §8.3。

---

## 8. 预期结果

### 8.1 WARN 与 Oops

按 `#51` 构建的实测计数（脚本按 WARN 站点 + 紧随其后的 `Tainted:` 行的构建标签配对统计）：

| WARN 站点 | `#51` | `#47` | 修后 |
|---|---|---|---|
| `pgtable.c:639 kvm_tlb_flush_vmid_range` | **27,674** | 2,460 | **消失** |
| `context_tracking.c:128 ct_kernel_exit` | 1 | 0 | 消失（Oops 后 "Fixing recursive fault" 的连带产物） |
| `kvm_emulate.h:563 kvm_handle_mmio_return` | **10** | 9 | **仍在**（独立问题） |
| `arch_timer.c:461 kvm_timer_update_irq` | **2** | 10 | **仍在**（独立问题） |
| `vmid.c:69 kvm_arm_vmid_update` | 1 | 0 | **仍在** |
| `__kvm_mmu_topup_memory_cache` Oops + `fpsimd.c:54` BUG | 2 次事件 | 0 | **消失** |

`#51` 这五个站点合计 **27,688** 条，本系列消除 **27,675** 条 = **99.953%**。平均从 0.524 WARN/s 降到基本安静。实际收益：串口不再被刷屏（突发时的 printk 节流会消失），syzkaller 的 report 解析与去重不再被这条 WARN 占位，真正的新 crash 不再被截断或交错。

（第一版把 `kvm_emulate.h:563` 的 19 条全算在 `#51` 上、总数写成 27,697、比例写成 99.92%，均已按构建拆分修正。）

### 8.2 记账

charge 与 discharge 现在**全部对称**：

| 位置 | 数量 | 状态 |
|---|---|---|
| `pkvm_mem_abort` 回退路径 | `page_size >> PAGE_SHIFT` | ✅ |
| `pkvm_mem_abort` 错误路径 | `page_size >> PAGE_SHIFT` | ✅ |
| `pkvm_host_reclaim_page` | `1 << ppage->order`（仅 `pins` 归零时退） | ✅ |
| `__pkvm_destroy_hyp_vm` | `1UL << ppage->order` | ✅ 本轮修复 |
| `pkvm_unmap_range` → `pkvm_flush_unaccount` | `cnt += 1UL << ppage->order` | ✅ 本轮修复 |
| 上述路径的 mm | `host_kvm->mm` / `kvm->mm` | ✅ 本轮修复 |

### 8.3 一个硬性前置条件，不是脚注

新的 id-28 handler 是 `pkvm_get_hyp_vm(handle); if (!vm) return;` —— **查不到就静默返回，不 WARN**。而 `kvm_arch_commit_memory_region` 可以在任何 vCPU 运行**之前**触发这次 flush（那时 hyp VM 还没建、`pkvm.handle == 0`）。那种情况下 guest stage-2 本来是空的、无需无效化，所以语义安全；但这意味着 **"WARN 消失"与"TLB 现在被正确无效化"是两件事，前者不蕴含后者**。否则这条链有可能只是把一个会喊的 bug 换成一个不喊的 bug。§9.1 因此是**发布前必做**。

---

## 9. 推送前的最低验证集（全部未做）

### 9.1 脏页跟踪功能验证（必做）

在**已经存在 stage-2 映射之后**再开启脏页日志，确认后续 guest 写进入 dirty bitmap。只看 WARN 消失不够（见 §8.3）。

### 9.2 THP 记账矩阵（必做）

每项都确认 `VmLck` 回到基线、且 `kvm->arch.pkvm.pending_unaccount == 0`：

- THP 直接销毁（不 relinquish）
- relinquish **1 / 100 / 511** 页后销毁（覆盖 §2.3 那个反例）
- **512 页全部 relinquish**（`pins` 自然归零，走 `pkvm_host_reclaim_page` 的整额退款）
- THP memslot / range unmap（走 `pkvm_unmap_range`）
- 第二次 `KVM_ARM_VCPU_INIT`（`d7c317637aa2` 的原始死锁触发路径）
- 通过 `SCM_RIGHTS` 由**另一个进程**关掉最后一个 VM fd（验证 §2.4 的 `host_kvm->mm`）

### 9.3 回归复现

用 `crashes/a854c8a8…/repro.prog` 配 `syz-execprog` 打（该 crash 没有 C reproducer，`Extracting C` 两次各 60 s 均失败），确认 Oops 不再出现。**这个复现是概率性的**（最小化过程 15 轮中 5 次命中，且必须 `Repeat:true`），阴性结果单独不足以证明修好；更可靠的是给 legacy top-up 加计数器，确认修复前非 0、修复后恒 0。

### 9.4 OOM 那条线

在拿到 `insert_ppage` 失败次数、legacy top-up 命中次数、GUP pin acquire/release、THP accounting 这四个计数器数据之前，**不要宣布 OOM 根因**。

---

## 10. 仍然未修 / 仍然开放

- **OOM 根因（25.9 GiB 残差）**：仍 OPEN。
- **`mmu.c` 的 `WARN_ON(insert_ppage(...))` 之后仍 `return 0`**：**这本身是已确认的代码缺陷** —— 吞掉错误、丢失 tracking 记录与 GUP pin，且向调用者报告成功。未确认的只是它与那 25.9 GiB 的**因果关系**。第一版把它整体称为"未建立的 bug"，措辞不准。本系列未动它。
- **泄漏候选 B**：已被 §2.1 的 union 门禁堵掉。但这不能反向证明历史 OOM 由候选 B 导致。
- **`fpsimd.c:54` 的 `BUG_ON(!current->mm)`**：任何带着已 load 的 vCPU 死掉的任务都会命中，是严重性放大器。Oops 修掉后不再有触发者，但这个 `BUG_ON` 本身仍应降级。
- **`pkvm_cov` 的 begin/end 锁契约**：代码里声称的 `kvm->mmu_lock`/preempt-off 契约**已经失效**（那个调用点被通用 HVC wrapper 取代了）。当前构建（`PREEMPT_VOLUNTARY` + `TREE_RCU`、begin→HVC→end 内无调度点）下 UAF **未被证明**，但在 `CONFIG_PREEMPT=y` 构建上是真实的 lifetime / CPU-owner 竞态。修法要覆盖整个 begin→HVC→end **并**阻止 CPU 迁移；单独 `rcu_read_lock()` 不保证 owner CPU 不变。
- **`MEM_RELINQUISH` 的并发/计账**：`pkvm_host_reclaim_page()` 的 `mt_find` / `pins--` / `mtree_erase` 与 EL2 每 `PAGE_SIZE` 的 zap 在多 vCPU 下的交错未审计。**注意：不要加 `kvm_vm_is_protected()` 门禁** —— EL2 刻意以 `PKVM_PAGE_SHARED_BORROWED` 支持 non-protected guest，加门禁会破坏合法协议（`kvm_hyp_handle_hvc64()` 的注释原文就是 "Handler for non-protected VM HVC calls"）。
- **三条独立 WARN**：`kvm_emulate.h:563`、`arch_timer.c:461`、`vmid.c:69`。其中 `arch_timer.c:461` 是 `kvm_timer_update_irq()` 里 `kvm_vgic_inject_irq()` 返回错误、走 vCPU reset 路径，与 TLB hypercall 无关，已单独记录待分析。

---

## 11. 过程中的失误

1. **`-S` 当 `-G` 用** —— 漏掉"在调用外加 `if`"这类修复。
2. **`-G <函数名>` 当充分证据用** —— 匹配不到只改函数体的提交（`fce886a60207` 就是反例）。要用 `git log -L` 或搜"修复会新增的内容"。
3. **只查 tag 不查活跃分支** —— 据此断言 `current->mm → host_kvm->mm` 无上游对应物，错了；活跃 6.6 分支上有，且带 `Cc: stable`。镜像最新 tag（2025-04）≠ 分支头（6.6.142）。
4. **把 fetch 的 exit code 当成功判据** —— 管道里 `tail` 的返回码掩盖了 TLS 失败。
5. **过度迁移** —— 先迁进了 `f9e18319445d`（纯批量优化、不修 bug）只为满足依赖，后按"只迁必需"窄化掉。
6. **迁移了一个语义不适用本 lineage 的 hunk（阻塞级）** —— `9ad2c87f0789` 的 `pages += pins` 依赖"teardown 时 `pins == 1 << order`"这个本 lineage 不成立的不变式。**"优先迁移"不能凌驾于"验证 hunk 的前置不变式在本 lineage 成立"之上。** 讽刺的是：最初手写的 `1UL << ppage->order` 是对的，改成"迁移"反而引入了 bug。
7. **`git commit --amend` 打在错误的提交上** —— rebase 其实已自动完成，amend 落到了链顶，把 mm backport 的信息覆盖成了 THP 的。从 reflog 的 `rebase (finish)` 点 `git reset --hard` 恢复。**改非 HEAD 提交的信息用 `git commit-tree` 重建（不动工作区），不要赌 rebase 状态。**
8. **构建脚本的 STALE 误报** —— 对象已最新时 make 不重编，脚本按 mtime 判失败。要 `rm .o` 强制重编。
9. **把"两个对象 compile-test 通过"写成"编译验证通过"** —— `kvm_host.h` 布局变更会影响 Rust nVHE bindings 与其他依赖对象，必须全量构建 + 链接才算验证（§7.1 已补做）。
10. **数值失误** —— 净 diff、`d7c317637aa2` 的增删数、WARN 按构建的拆分、活跃分支的内核版本、`9b47e206590b` 的 checkpatch warning 类型，第一版均写错，已按实测修正。

---

## 12. 为什么自查不够

第一版有一节自查，声称"全部通过，未发现需要改动的结论"，但它**漏掉了 §2.3 那个阻塞级记账 bug**。原因值得记下来。

那次自查逐项核对了"文档说的和代码/仓库是否一致"：哈希、作者、字节数、某个函数是否存在某一行。而 `pages += pins` 与代码完全一致、与上游 hunk 完全一致、与文档描述完全一致 —— 三者自洽，所以一致性检查全绿。要发现它错，必须做一件自查没做的事：**把 charge 与 discharge 的账本在纸上走一遍，特别是"部分 relinquish"这个中间状态。**

**一致性检查（consistency）不能替代正确性检查（correctness）。** 对记账、引用计数、生命周期这类代码，审查必须构造具体的状态序列并逐步算账，而不是比对代码与文档是否吻合。同事的审查正是这么做的，因此抓到了。
