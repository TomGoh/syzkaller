# pKVM 修复系列记录 — 2026-07-30

配套阅读：`evidence/finding-oom-leak-and-mmu-topup-oops-2026-07-30/ROOT-CAUSE-CONFIRMED.md`（根因分析与置信度状态表）。本文记录的是**修复侧**：查了什么、为什么这么修、迁移与自写的取舍、预期效果、以及哪些没修。

工作区：`/home/jose/ksrc-pkvmfix`，分支 `review/8707`（独立 clone，非主插桩树）。主插桩树 `/home/jose/common-stage2mvp` 全程未被触碰（始终 17 个改动文件）。

---

## 1. 摘要

一条 7 提交的 Gerrit 链，修掉 4 个缺陷，其中 4 个提交是上游迁移、3 个是本地 `KYLIN:` 提交。净改动 `+86 / -19`，涉及 `kvm_host.h`、`mmu.c`、`pkvm.c` 三个文件。编译验证通过，checkpatch 只剩 Gerrit 必需的 `Change-Id` 一项预期报错。**尚未推送**。

| 缺陷 | 严重性 | 修复来源 | 置信度 |
|---|---|---|---|
| `union {mmu_page_cache, stage2_mc}` 类型混用 → `__kvm_mmu_topup_memory_cache` Oops | 用户态 5 个 syscall 可触发的 host 崩溃 | 本地（上游无 6.6 版本） | **CONFIRMED**，寄存器级 |
| `kvm_arch_flush_remote_tlbs_range()` 缺 pKVM 分支 → 脏页跟踪漏写 + 27,674 条 WARN | 正确性（迁移/快照数据丢失）+ 可观测性 | 本地（hunk 取自 mainline） | **CONFIRMED**，`res.a0 == -1` |
| 销毁路径 THP 记账少记 511 页/大页 → `RLIMIT_MEMLOCK` 单向漂移 | 可用性（最终永久 `-ENOMEM` from `KVM_RUN`） | 上游迁移（6.12） | 代码确证 |
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

两次崩溃的寄存器**逐位吻合**：`x19`（kmem_cache 参数）= 3 和 2，正是 `topup_hyp_memcache()` 留在 `kvm_mmu_cache_min_pages()` 水位的 `nr_pages`；`x20`（gfp = `gfp_custom | gfp_zero`）= `0x2cc28022` 和 `0xf350d021`，把 `stage2_mc.head` 的两个半字 OR 起来正好得到这两个值，且 `head[31:0]` 页对齐、`head[63:32]` 等于低 12 位 —— 还原出 ~136 GiB 处的合法页对齐物理地址。**没有任何内存被写坏**：两侧都是对一个活着的 `struct kvm_vcpu` 的合法、类型正确的访问，这就是 KASAN 沉默的原因。

**修法**：把序言那次 top-up 用 `!is_protected_kvm_enabled()` 门禁掉。这样足够且最小 —— pKVM 下 `user_mem_abort()` 唯一做的事是分派到 `pkvm_relax_perms()`，而后者在 logging 路径上**自己会补 `stage2_mc`**，所以序言那次纯属多余。

**刻意没有做的事**：没有把 `memcache` 的声明搬进分支内部。上游正是这么做才引入了 CVE-2025-37996（守卫为假时该指针未初始化就进了 `kvm_pgtable_stage2_map()`）。

**一条被撤回的过强结论**：早期版本写过 "pKVM 下绝不能进入 `user_mem_abort()`"。错。进入是**刻意设计**，代码自己断言了这一点（`WARN_ON(fault_status != ESR_ELx_FSC_PERM)`）；要禁的只有那次 legacy top-up。

### 2.2 TLB range flush 缺 pKVM 分支

`kvm_arch_flush_remote_tlbs_range()` 不认识 pKVM，于是在 protected 主机上用 finalize 前的 hypercall id 11（`__kvm_tlb_flush_vmid`）、并传一个指向 `kvm->arch.mmu` 的**host 裸指针**。EL2 在 protected 模式初始化后把 `hcall_min` 抬到 id 20（`__pkvm_prot_finalize`），低于它的一律拒收且**根本不调 handler**，返回 `SMCCC_RET_NOT_SUPPORTED`。

这一点是**寄存器级确证**而非推断：反汇编归档 vmlinux 得知该帧里 `x21` 就是 `res.a0`，而 `#51` 构建的 27,674 条 WARN 里 `x21` 全是 `0xffffffffffffffff` = -1。这个值**唯一地**指向 dispatcher 的 `id < hcall_min` 拒绝路径 —— `handle_host_hcall()` 是在调 handler **之前**就写好 `SMCCC_RET_SUCCESS` 的，而 `handle___kvm_tlb_flush_vmid()` 本身没有错误返回，所以任何"进了 handler"的情况都不可能留下 -1。**TLBI 不是失败了，是压根没执行。**

后果超出日志噪音：调用者是脏页日志的 write-protect 路径，而 write-protect 由 EL2 的 `__pkvm_wrprotect` 完成，它**按设计不做 TLBI**、依赖这次 flush。于是 guest vCPU 可以继续通过残留的可写 stage-2 TLB 表项写入，**这些写不会被脏页跟踪记录**。而且没人会发现，因为 `kvm_arch_flush_remote_tlbs_range()` 无条件 `return 0`，把通用层"失败就退化为全量 flush"的兜底也关掉了 —— 讽刺的是那个全量路径 `kvm_arch_flush_remote_tlbs()` **有** pKVM 分支、本来能成功。

**修法**：给 range 版本补上兄弟函数同样的分支，退化到 handle-based 的 `__pkvm_tlb_flush_vmid`（id 28），它收的是不透明 `pkvm_handle_t`、由 EL2 对自己的 VM 表校验。

**一条被撤回的错误建议**：早期版本提过"或者把 TLB 系列 id 移到 `__pkvm_prot_finalize` 之后"。**绝不可采用** —— id 11 收 host 裸指针并在 EL2 `kern_hyp_va()` 后解引用，finalize 之后 host 是不可信的，重新暴露它等于主动削弱 pKVM 威胁模型。那道门禁在正确工作，bug 是 host 还在调 finalize 前的 ABI。

**成因可精确定年**，这是一次 merge 漏改而非设计选择：`ad6e033c0464`（Quentin Perret，2022-07-07，已在我们树里）给 `kvm_arch_flush_remote_tlbs()` 加分支时，range 版本还不存在；mainline 的 `c42b6f0b1cde`（Raghavendra Rao Ananta，2023-08-11）**晚 13 个月**才加了 range 版本，且没有 pKVM 分支 —— 因为 mainline 直到 6.14 才有 EL2 托管的 guest stage-2。Android rebase 到 6.6 时把新函数原样吸收，兄弟函数的 pKVM 感知没跟过来。它的 6.12 双胞胎 `b4e76eacd6fa` 同样只改了非 range 版本。

### 2.3 销毁路径 THP 记账

`pkvm_mem_abort()` 按页收费（`account_locked_vm(mm, page_size >> PAGE_SHIFT, true)`，2 MiB THP = 512），而 `__pkvm_destroy_hyp_vm()` 每个 `kvm_pinned_page` 条目只退 1。每拆一个 THP 后备的 guest 页，`mm->locked_vm` 就多留 511，单向累积。后果是 `RLIMIT_MEMLOCK` 被逐渐耗尽 → 上面那个 charge 返回 `-ENOMEM` → guest 缺页失败并从 `KVM_RUN` 返回错误。它还会把 `d7c317637aa2` 自己声明的那个代价（"a fault racing at the RLIMIT_MEMLOCK ceiling can see a spurious -ENOMEM"）从"短暂窗口偶发"放大成"最终永久失败"。

**与今天调查的三个发现无关**，这一点必须写清楚以免误读为 OOM 那条线的进展：`locked_vm` 是一个拿去和 rlimit 比较的**计数器**，多记它**不消耗任何物理页**，数学上不可能贡献那 25 GB 残差。而且已验证 charge 失败时 `pkvm_mem_abort()` 走 `goto unpin` → `unpin_user_pages()` + `kfree(ppage)`，**连失败路径也不漏页**。它出现在这条链里的唯一理由是 8707 的两个父提交本身就是 THP 记账主题。

### 2.4 记错 mm

`__pkvm_destroy_hyp_vm()` 和 `pkvm_host_reclaim_page()` 都用 `current->mm` 退账，但收费发生在 `pkvm_mem_abort()`，那里 `current->mm == kvm->mm` 由 `kvm_main.c:4124` 的 `if (vcpu->kvm->mm != current->mm) return -EIO;` 保证。销毁路径从 `kvm_vm_release` 走，**可以是任意持有 fd 的进程**（继承的 fd、或经 `SCM_RIGHTS` 传递），于是会去减错一个地址空间的 `locked_vm`。同一个文件里 `pkvm_unmap_range` 用 `kvm->mm`、`__pkvm_destroy_hyp_vm` 用 `current->mm` 这个不一致本身就是线索。

---

## 3. 上游调研

### 3.1 搜索方法与一个必须记住的教训

**`-S` 只统计字符串出现次数，在调用外面加一个 `if` 不会改变次数，因此会漏掉修复。** 必须用 `-G`（对 diff 行做正则）。这个错误一开始让我误判"上游从未碰过那个 top-up"。

另一条：本仓库有 **4017 个 tag、25 个分支**，而上游修复经常**只能从 tag 到达**。必须 `git log --all --source`；只搜分支会得出"上游无修复"的错误结论。

第三条：**"这个 commit 我们已经有了"不等于"这个修复我们已经有了"。** 多 lineage 仓库里同一个逻辑改动会有两个改编版，内容可能不同。`merge-base --is-ancestor` 只能证明 commit 在，不能证明 hunk 在。典型例子见 3.3。

### 3.2 用到的数据源

| 来源 | 状态 | 用途 |
|---|---|---|
| 本地对象库（`common-stage2mvp`，4017 tag） | 可用 | 绝大部分考古 |
| `android.googlesource.com/kernel/common` gitiles `+/<ref>/<path>?format=TEXT` | **可用** | 直接取活跃分支的文件内容 |
| 同上的 `+log/` history 页 | **403，需登录** | 拿不到 commit 历史 |
| 同上的 git 传输（冷启动） | **两次 TLS 中断失败** | — |
| `github.com/aosp-mirror/kernel_common` | 可用 | 拉到 `android15-6.6` 完整历史（滞后到 2025-10-08） |
| 上一条补齐对象后再拉 googlesource | **成功**（增量） | 拿到真正的分支头 `9f24219bc984` |

注意坑：第一次后台拉取报 exit 0，实际是管道里 `tail` 的返回码；fetch 本身 TLS 失败了。**判断 fetch 成功要看 ref 是否存在，不看 exit code。**

### 3.3 结论：哪些有现成 patch

| 需要的修复 | 上游有吗 | 处理 |
|---|---|---|
| `pkvm_host_reclaim_page` 解锁后读 `pins` 的 UAF + `1<<order` | ✅ `63a99c1fb8e5` | 已迁（`3ae13572d106`，本轮之前完成） |
| `pkvm_mem_abort` 大页回退路径记账 | ✅ `ed14b491ec76` | 已迁（`9a13ca20af8d`，本轮之前完成） |
| 销毁路径 THP 记账 | ✅ `9ad2c87f0789`（6.12） | **本轮迁移**，窄化后见 4.2 |
| `current->mm` → `host_kvm->mm` | ✅ `04512258010d`（活跃 6.6，`Cc: stable`） | **本轮迁移** |
| union / 那个 Oops | ❌ 只存在于 mainline 6.14 的大改内 | 本地写 |
| TLB range flush | ❌ 同上 | 本地写（hunk 取自上游） |
| `pkvm_unmap_range` 的 `cnt++` | ❌ 活跃 6.6.111 也还是 `cnt++` | **未修**，见 8.2 |

**"没有"是实测证据，不是判断：**

- `-G'kvm_mmu_topup_memory_cache\(memcache'` 扫全部 ref：只有 `73a4f4ffe584`（6.12 backport）和 `fce886a60207`（mainline 6.14），外加三个无关老 commit。
- `-G'kvm_arch_flush_remote_tlbs_range'` 扫全部 ref：**只有引入它的 `c42b6f0b1cde`**，从来没有任何 commit 修过它。
- 实测 `git cherry-pick 73a4f4ffe584` → **`mmu.c` 里 19 个冲突 hunk**。携带 union 修复的那个（#14）依赖 `fault_is_perm` 重构、`kvm_mmu_cache_min_pages(vcpu->arch.hw_mmu)` 和 `void *memcache`（CVE-2025-37996 的成因）；其余 17 个是 `KVM_PGT_FN()`、`kvm_s2_mmu` 参数化、`kvm_init_ipa_range()`、`kvm_nested_s2_flush()`、`topup_hyp_memcache_account()`、`pkvm_mem_abort_device()` / `__pkvm_host_map_guest_mmio` —— 全是本 lineage 不存在的设计，而且都要同步移植进 Rust EL2。这不是"冲突多少"的问题，是**两套 pKVM guest 内存模型**。
- gitiles 直取 `android15-6.6` **分支头（2026-07-30）**的 `mmu.c`：那两处仍未修。

### 3.4 CVE-2025-37996 与本系列的关系

`CVE-2025-37996 = "KVM: arm64: Fix uninitialized memcache pointer in user_mem_abort()"`，`Fixes: fce886a60207`，由 `a26d50f8a4a5`（Sebastian Ott）+ `157dbc4a321f` 修复。它**不是我们的 bug，而是"修我们这个 bug 的补丁"引入的新 bug**，两者触发在同一个 `if` 的**相反分支**上：我们的需要守卫为真，CVE 需要守卫为假。我们不受影响（`fce886a60207` 未应用），但**一旦 backport 那个大改就会引入它**，顺序是强制的。这也是 2.1 里"不搬 `memcache` 声明"那条决定的直接依据。

（NVD 记录的上游受影响范围是 6.14 – 6.14.6 与 6.15-rc1 – rc5；Android 6.12 只能表述为"包含引入提交的 backport 分支"。）

---

## 4. 迁移与自写的取舍

原则（来自 `kylin-futlab-patch-migration` 技能第一条规则，以及本次明确的口头要求）：**能迁就迁，不限 lineage、不限 ACK，后面内核的修复 backport 过来能用就行；实在找不到才自己写。同时只迁修复必需的，不把上游欠账全搬过来。**

### 4.1 `f32978fb495c` — 迁移 `04512258010d`

上游原版改的是 ACK 里不存在的 `__pkvm_pgtable_stage2_reclaim()`，Fuad Tabba 在 ACK 侧的适配注记写着"applied the same current->mm to host_kvm->mm fix in the equivalent functions: `__pkvm_destroy_hyp_vm()` and `pkvm_host_reclaim_page()`" —— 和我们需要的两处完全一致。1 个冲突（`tmp` 指针来自我们没有的 interval-tree 转换），解为保留本地声明集。带 `Cc: stable`、Marc Zyngier 签署。

### 4.2 `cdf7ff6526e1` — 迁移 `9ad2c87f0789`，并**刻意窄化**

上游把修复表达为 `pages += pins` 喂给循环后的一次 charge，这需要 `f9e18319445d`（"count the pages to account for and account once"）引入 `pages` 累加器作为前置。**那个前置不修任何 bug —— 它是批量记账优化**，按"只迁必需的"原则不迁，于是 charge 留在循环内、把数量从 1 改成 `pins`：

```c
 	mt_for_each(&host_kvm->arch.pkvm.pinned_pages, ppage, ipa, ULONG_MAX) {
+		u16 pins = ppage->pins;
+
 		WARN_ON(pkvm_call_hyp_nvhe_ppage(ppage, ...));
 		cond_resched();
-		account_locked_vm(mm, 1, false);
+		account_locked_vm(mm, pins, false);
```

净记账效果相同，一个提交而不是两个。

**一个关键适配细节**：`pins` 必须在循环**顶部**读 —— `pkvm_call_hyp_nvhe_ppage(..., unmap=true)` 会递减 `ppage->pins`，放到后面读就是 0。上游那个位置不是随手放的。

另外这个 commit 的 6.6 双胞胎 `e602818486b6`（同作者、同日期、同标题）**已在我们树里**，它引入了 `ppage->order/->pins` 和 maple tree，但改编时**丢掉了这个 hunk** —— 这正是 3.1 第三条教训的实例。

### 4.3 `0c1de5bc2127` / `10240dcc3d8d` — 本地 `KYLIN:` 提交

见 3.3 的实测依据。两个提交的信息里都写明了"hunk 取自哪个上游 commit"以及"整 commit 为何不可 backport"，便于将来 rebase 时对照。

---

## 5. 刻意没有迁的

活跃 `android15-6.6`（**6.6.111**，我们基线是 6.6.30 时代）在 `mmu.c`/`pkvm.c` 上我们缺 **24 个提交**。按"不把欠账全搬过来"的要求，只取对应**已确证问题**的，明确未取：

| commit | 主题 | 未取的理由 |
|---|---|---|
| `9cfc94644ba4` | Pre-alloctate mtree nodes in `pkvm_mem_abort()` | 修的是**泄漏候选 A 的前提条件**，而候选 A 本身仍是未确证假设（OOM 根因 OPEN）。为未建立的 bug 迁 3 个补丁不合原则。依赖顺序已摸清：它排在 interval-tree 转换**之前**，仍作用于我们的 maple tree，前置是 `aeaa3ba902c2` + `d53a54962630` |
| `f9e18319445d` | count the pages … account once | 批量优化，不修 bug（见 4.2） |
| `960d37426bef` | Handle races gracefully in `pkvm_relax_perms()` | 未对应到已确证现象（但函数正在我们的 Oops 路径上，值得后续关注） |
| `ccc8abc3d9b0` | Enable RCU for pinned pages mtree | 同上 |
| `e56d181356a4` | Convert `kvm_pinned_pages` to an interval-tree | **结构性分水岭**，之后的提交都假设 interval tree。这是真正的迁移断点 |
| `9fb1a06cda29` | Fix accounting when VM creation fails | 未对应已确证现象 |
| `21e2595401ca` | Fix error path of allocator topup | 同上 |
| `9849a1376ae3` | Fix protected mode handling of pages larger than 4kB | 比 GitHub 镜像新，同上 |
| `1569a0028664` | Handle missing userspace mapping in pKVM mem abort | 比 GitHub 镜像新，同上 |
| 其余 ~11 个 | nVHE 栈、kmemleak、iommu CMA、hyp SPLIT、debugfs 等 | 与本次调查无关 |

清单和依赖顺序都已确定，需要时可单独取。

---

## 6. 最终链

```
f32978fb495c  Bradley Morgan     BACKPORT: FROMGIT: KVM: arm64: account pKVM reclaim against the VM mm
10240dcc3d8d  Haoze Wu           KYLIN: KVM: arm64: pkvm: flush guest TLBs by handle in the range helper
0c1de5bc2127  Haoze Wu           KYLIN: KVM: arm64: pkvm: don't top up the legacy memcache under pKVM
cdf7ff6526e1  Vincent Donnefort  ANDROID: KVM: arm64: Huge page support for pkvm_pinned_page
d7c317637aa2  Haoze Wu           KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock
3ae13572d106  Vincent Donnefort  ANDROID: KVM: arm64: Fix THPs reclaim with ballooning
9a13ca20af8d  Quentin Perret     ANDROID: KVM: arm64: Fix accounting of pinned THPs with pKVM
```

4 个上游迁移全部保留原作者、原 `Change-Id`；本轮新增的两个带 `(cherry picked from commit …)` 出处行，适配差异写在 trailers 下方的 `[ Adapted … ]` 注记里。

逐提交改动量：`f32978fb495c` pkvm.c 2+/2-；`10240dcc3d8d` mmu.c 7+/2-；`0c1de5bc2127` mmu.c 7+/2-；`cdf7ff6526e1` pkvm.c 3+/1-；`d7c317637aa2` kvm_host.h 8+ / mmu.c 51+/5-；`3ae13572d106` pkvm.c 5+/4-；`9a13ca20af8d` mmu.c 1+/1-。

---

## 7. 验证记录

- **编译**：删掉 `.o` 强制重编（首次跑脚本报 STALE 是因为对象已最新、make 没重编，不是真失败）—— `arch/arm64/kvm/mmu.o` **828,552 B**、`arch/arm64/kvm/pkvm.o` **462,856 B**，`all objects built clean`。用 `scripts/build-changed-objects.sh --range d7c317637aa2..HEAD`，`gwd3000_lenovox1d3000m_defconfig`，关掉 `CONFIG_DEBUG_INFO_BTF`（本机 `resolve_btfids/libbpf` 编不过，与补丁无关）。
- **checkpatch**：4 个新提交各 1 error + ≤1 warning。error 全是 `Remove Gerrit Change-Id's before submitting upstream` —— checkpatch 是按上游投递写的，Gerrit 反过来强制要求 Change-Id，**忽略、不要删**。warning 是 `commit <hash> ("<长标题>")` 无法折行导致的 >75 字符。
- **Change-Id / Signed-off-by**：7 个提交全部各 1 个 Change-Id。
- **工作区**：`ksrc-pkvmfix` 干净；主插桩树 `common-stage2mvp` 全程 17 个改动文件未变。
- **未做**：**没有推送**。推送命令 `cd /home/jose/ksrc-pkvmfix && git review`，留给操作者。

---

## 8. 预期结果

### 8.1 WARN 与 Oops

按 `#51` 构建的实测计数（27,674 条 pgtable.c:639，跨 20,391 个 PID，平均 0.524 WARN/s，峰值 162 条/秒的 kernel-timestamp 聚合突发，每条 3,297 B / 36 行）：

| WARN 站点 | 实测条数 | 修后 |
|---|---|---|
| `pgtable.c:639 kvm_tlb_flush_vmid_range` | 27,674 | **消失** |
| `context_tracking.c:128 ct_kernel_exit` | 1 | 消失（是 Oops 后 "Fixing recursive fault" 的连带产物） |
| `__kvm_mmu_topup_memory_cache` Oops + `fpsimd.c:54` BUG | 2 次事件 | **消失** |
| `kvm_emulate.h:563 kvm_handle_mmio_return` | 19 | **仍在**（独立问题） |
| `arch_timer.c:461 kvm_timer_update_irq` | 2（另 10 条在 `#47`） | **仍在**（独立问题） |
| `vmid.c:69 kvm_arm_vmid_update` | 1 | **仍在** |

即约 **99.9% 的量（27,675 / 27,697）消失**，从 0.524 WARN/s 降到基本安静。实际收益：串口不再被刷屏（突发时的 printk 节流会消失），syzkaller 的 report 解析与去重不再被这条 WARN 占位，真正的新 crash 不再被截断或交错。

### 8.2 记账

charge 与 discharge 在**四个**路径上对称，还剩**一个**不对称：

| 位置 | 数量 | 状态 |
|---|---|---|
| `pkvm_mem_abort` 回退/错误路径 | `page_size >> PAGE_SHIFT` | ✅ |
| `pkvm_host_reclaim_page` | `1 << ppage->order` | ✅ |
| `__pkvm_destroy_hyp_vm` | `pins` | ✅ 本轮修复 |
| 上述三处的 mm | `host_kvm->mm` | ✅ 本轮修复 |
| **`pkvm_unmap_range`（`cnt++` → `pkvm_flush_unaccount`）** | **1 / 条目** | ❌ **仍少记** |

最后这一处：基线 `origin/2030/bug930` 上就是 `cnt++`（在 `d7c317637aa2` 的 diff 里是前导空格的**上下文行**、不是 `+`），活跃 6.6.111 也还是 `cnt++`，**是上游一直存在的缺陷，不是我们改出来的**。无上游可迁。当时经 Codex code review 决定不放进 `d7c317637aa2`——若理由是"一个提交只做一件事"，那是对的。补它需要本地写；建议等第 5 节里 `960d37426bef`/`ccc8abc3d9b0` 之类会改到附近代码的提交定案后再做，免得白解一遍冲突。

### 8.3 一个必须留意的副作用

新的 id-28 handler 是 `pkvm_get_hyp_vm(handle); if (!vm) return;` —— **查不到就静默返回，不 WARN**。而 `kvm_arch_commit_memory_region` 可以在任何 vCPU 运行**之前**触发这次 flush（那时 hyp VM 还没建、`pkvm.handle == 0`）。那种情况下 guest stage-2 本来是空的、无需无效化，所以语义安全；但这意味着 **"WARN 消失"本身不能当作"TLB 现在被正确无效化了"的证据**。见第 9 节。

---

## 9. 还需要做的验证

1. **脏页跟踪的功能验证（必须做）**：在**已经存在 stage-2 映射之后**再开启脏页日志，确认后续 guest 写能被 dirty bitmap 捕获。只看 WARN 消失是不够的（见 8.3）。
2. **回归复现**：用 `crashes/a854c8a8…/repro.prog` 配 `syz-execprog` 打（该 crash 没有 C reproducer，`Extracting C` 失败过），确认 Oops 不再出现。
3. **RLIMIT_MEMLOCK 漂移**：长跑一个 THP 后备的 guest，采样 `/proc/<pid>/status` 的 `VmLck`，确认不再单向增长。
4. **OOM 那条线**：在拿到 `insert_ppage` 失败次数、legacy top-up 命中次数、GUP pin acquire/release、THP accounting 这四个计数器数据之前，**不要宣布 OOM 根因**。

---

## 10. 仍然未修 / 仍然开放

- **OOM 根因（25.9 GiB 残差）**：仍 OPEN。泄漏候选 A（`mmu.c:1864` 的 `WARN_ON(insert_ppage(...))` 失败后仍 `return 0`，孤立 GUP pin）是最好的代码级解释但**未确证**——那个 WARN 在任何捕获的日志里都没出现过。候选 B（40 页/vCPU）也**不能排除**（比值 1.32，同一数量级）。详见 `ROOT-CAUSE-CONFIRMED.md`。
- **`mmu.c:1864` 的错误路径**：失败后应该回滚 EL2 映射、`unpin_user_pages()`、`kfree(ppage)` 并返回错误，绝不能 `return 0`。本系列未动（属于候选 A，未确证）。
- **`fpsimd.c:54` 的 `BUG_ON(!current->mm)`**：任何带着已 load 的 vCPU 死掉的任务都会命中，是严重性放大器。本系列未动（Oops 修掉之后不再有触发者，但这个 BUG_ON 本身仍应降级）。
- **`pkvm_cov` 的 begin/end 锁契约**：代码里声称的 `kvm->mmu_lock`/preempt-off 契约**已经失效**（那个调用点被通用 HVC wrapper 取代了）。当前构建（`PREEMPT_VOLUNTARY` + `TREE_RCU`、begin→HVC→end 内无调度点）下 UAF **未被证明**，但在 `CONFIG_PREEMPT=y` 构建上是真实的 lifetime / CPU-owner 竞态。修法要覆盖整个 begin→HVC→end **并**阻止 CPU 迁移；单独 `rcu_read_lock()` 不保证 owner CPU 不变。
- **`MEM_RELINQUISH` 的并发/计账**：`pkvm_host_reclaim_page()` 的 `mt_find` / `pins--` / `mtree_erase` 与 EL2 每 `PAGE_SIZE` 的 zap 在多 vCPU 下的交错未审计。**注意：不要加 `kvm_vm_is_protected()` 门禁** —— EL2 刻意以 `PKVM_PAGE_SHARED_BORROWED` 支持 non-protected guest，加门禁会破坏合法协议。
- **三条独立 WARN**：`kvm_emulate.h:563`、`arch_timer.c:461`、`vmid.c:69`。其中 `arch_timer.c:461` 是 `kvm_timer_update_irq()` 里 `kvm_vgic_inject_irq()` 返回错误、走 vCPU reset 路径，与 TLB hypercall 无关，已单独记录待分析。

---

## 11. 过程中的失误（供后来者避坑）

1. **`-S` 当 `-G` 用**：漏掉了"在调用外面加 `if`"这类修复。见 3.1。
2. **只查 tag 不查活跃分支**：据此断言 `current->mm → host_kvm->mm` "无上游对应物"，是错的 —— 活跃 6.6 分支上有，而且带 `Cc: stable`。教训：镜像的最新 tag（2025-04）不等于分支头（2026-07）。
3. **把 fetch 的 exit code 当成功判据**：管道里 `tail` 的返回码掩盖了 TLS 失败。要看 ref 是否存在。
4. **过度迁移**：先迁进了 `f9e18319445d`（纯批量优化、不修 bug）只为满足依赖，后按"只迁必需"要求窄化掉。
5. **`git commit --amend` 打在错误的提交上**：rebase 其实已自动完成，amend 落到了链顶的 mm backport 上，把它的信息覆盖成了 THP 的。从 reflog 的 `rebase (finish)` 点 `git reset --hard` 恢复。**教训：改非 HEAD 提交的信息用 `git commit-tree` 重建（不动工作区），不要赌 rebase 状态。**
6. **构建脚本的 STALE 误报**：对象已最新时 make 不重编，脚本按 mtime 判失败。要 `rm` 掉 `.o` 强制重编才算真验证。

---

## 12. 独立复核（2026-07-30）

逐条对源码与仓库实测复核，不采信文档自述。**全部通过，未发现需要改动的结论。**

| 复核项 | 方法 | 结果 |
|---|---|---|
| 7 提交链 | `git log` on `ksrc-pkvmfix` `review/8707` | ✔ 哈希、作者、顺序与 §6 完全一致 |
| 工作区干净 | `git status --short` | ✔ 空 |
| 主插桩树未动 | `common-stage2mvp` `git status` | ✔ 仍 17 个改动文件 |
| **union 修复是否安全** | 读 `mmu.c:1697-1700` | ✔ `pkvm_relax_perms()` 在 `if (logging_active)` 里确实自己 `topup_hyp_memcache(&vcpu->arch.stage2_mc, …)`，序言那次确属多余 |
| 编译产物 | `ls -la` | ✔ `mmu.o` **828,552 B**、`pkvm.o` **462,856 B**，与 §7 逐字节相符 |
| `cnt++` 是上游既有缺陷 | `git show origin/2030/bug930:arch/arm64/kvm/mmu.c` | ✔ 基线 `:334` 就是 `cnt++`，非本轮引入 |
| `MEM_RELINQUISH` 不该加门禁（§10） | 读 `hyp/nvhe/pkvm.c:1767-1780`、`mem_protect.c:429-432` | ✔ `kvm_hyp_handle_hvc64()` 注释原文即 "Handler for **non-protected** VM HVC calls" 且转发 `MEM_RELINQUISH`；`expected_state` 按 `pkvm_hyp_vcpu_is_protected()` 三元选择 `PKVM_PAGE_OWNED` / `PKVM_PAGE_SHARED_BORROWED` |
| 迁移后 `pkvm_host_reclaim_page` | 读 `pkvm.c:535-563` | ✔ 已用 `host_kvm->mm`、已按 `1 << ppage->order` 退账、`pins` 有 `WARN_ON(1)` 下溢保护 |

**§10 与 `ROOT-CAUSE-CONFIRMED.md` 早期版本关于 `MEM_RELINQUISH` 门禁的冲突已不存在** —— 后者 round 3 已自行撤回该建议，方向与 §10 一致。两份文档现已收敛，无需再裁决。

### 复核中认为最有价值的一条

**§8.3 必须被当作硬性前置条件，而不是脚注。** 新的 id-28 handler 查不到 handle 就静默返回，因此"WARN 消失"与"TLB 现在被正确无效化"是两件事，前者不蕴含后者。这意味着 §9.1 的脏页跟踪功能验证是**发布前必做**，不是可选回归——否则这条链可能只是把一个会喊的 bug 换成一个不喊的 bug。

### 关于推送顺序的一个提示（非缺陷）

链底三个提交 `d7c317637aa2` / `3ae13572d106` / `9a13ca20af8d` 的哈希与已推送到 Gerrit 的那三个**完全相同**，说明基线与内容都未变，`git review` 只会把上面 4 个作为新 change 推出去，不会给已有 review 生成多余 patchset。链的构造是干净的。
