# 005 —— 脏页日志成功一次,就抹掉该页的 EL2 共享状态:此后每次写保护都失败、脏页写入被静默丢失、销毁时 unmap 告警

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

> **本文行号以 `klinux @348c94763cc6`(`#4`)为准** —— 即观测所在的构建,可用 `git show 348c94763cc6:<path>` 复核。

一个脏页日志超级调用**成功过**的 VM,销毁时无法干净地拆掉:stage-2 unmap 返回错误,`__unmap_stage2_range()` 在 `exit_mmap()` 内部就此告警。它目前是隐形的,因为问题 003 让那个超级调用在**每一个大页支撑的客户机**上都失败 —— 而默认情况下,每个客户机都是大页支撑的。

> **销毁时的告警只是小的那一半。** 2026-08-06 已在硬件上确认(`evidence/2026-08-06-reprotect-eperm-and-lost-dirty-page.txt`):EL2 处理函数抹掉了该页的共享状态标注,于是**之后对该页的每一次 `__check_host_unshare_guest()` 都会返回 `-EPERM`** —— 包括 `KVM_GET_DIRTY_LOG` 所做的那次重新写保护。那个错误被直接丢弃(`kvm_arch_mmu_enable_log_dirty_pt_masked()` 是 `void`),该页保持可写,客户机的下一次写入不再缺页,`mark_page_dirty_in_slot()` 永不执行。**脏页日志在每一页第一次被写之后就静默地停止跟踪该页,5/5。** 用这种状态做热迁移或快照,拷走的是陈旧数据,而且报告成功。

现在就立案而不是以后,正是因为这层依赖:**修好 003 之后,它会在每一次带脏页日志的销毁中出现**,看上去像是 003 补丁引入的回归。它不是 —— 它比那个补丁更老,只是被它遮住了。

---

## 背景知识

### pKVM 的页面归属状态

在 pKVM 架构下,每一页内存都有一个"归属状态",记录这页内存当前归谁管。这个状态存在 stage-2 页表项（PTE）的两个软件位里,和页面的访问权限装在同一个字（`prot`）中传递。状态有三种:

- `PKVM_PAGE_OWNED`（0）:Host 自己拥有这页
- `PKVM_PAGE_SHARED_OWNED`（1）:Host 把页共享给了 EL2,但所有权还在 Host 手里
- `PKVM_PAGE_SHARED_BORROWED`（2）:客户机借用了这页,Host 不能随意处置它

这些状态不是摆设——EL2 在执行 unshare（解除共享）等操作前会检查它。如果状态不对,EL2 会拒绝操作并返回 `-EPERM`。

### `stage2_map()` vs `stage2_relax_perms()`:两种修改 PTE 的方式

这两种函数都能改 stage-2 页表项,但行为完全不同:

- **`kvm_pgtable_stage2_map()`**:从头**重建**一条 PTE。它会按传入的 `prot` 参数写入所有位——包括软件状态位。所以如果 `prot` 里不带状态信息,它会把状态写成 `PKVM_PAGE_OWNED`（0）,**覆盖掉**原有的状态标注。
- **`kvm_pgtable_stage2_relax_perms()`**:对现有的 PTE 做**读改写**——只修改权限位（读/写/执行）,不碰软件状态位。原有状态原封不动。

这个差别是本缺陷的核心:脏页日志处理函数用的是 `stage2_map()` 而不是 `relax_perms()`,而且传的是不带状态的裸 `prot`——于是它先把页的状态校验通过,然后立刻把这个状态标注覆盖掉了。

### 脏页日志的写保护循环

脏页日志的工作方式是一个循环:客户机写入被写保护的页 → 触发权限异常 → 内核标记该页为脏 → 恢复该页可写 → 客户机继续。但在 pKVM 下,"恢复可写"这一步要通过超级调用让 EL2 改 stage-2 PTE。本缺陷说的就是这个超级调用在恢复权限的同时,把页面的归属状态也写没了。状态一丢,后续所有依赖状态检查的操作（包括重新写保护、unmap 等）都会失败。

---

## 症状

```
WARNING: CPU: 7 PID: 94783 at arch/arm64/kvm/mmu.c:456 __unmap_stage2_range+0x26c/0x2b8
CPU: 7 PID: 94783 Comm: probe3 Tainted: G        W          6.6.103+ #4
Call trace:
 __unmap_stage2_range+0x26c/0x2b8
 kvm_uninit_stage2_mmu+0x6c/0xe0
 kvm_arch_flush_shadow_all+0x20/0x30
 kvm_mmu_notifier_release+0x38/0x98
 __mmu_notifier_release+0x90/0x2b0
 exit_mmap+0x46c/0x4c8
 __mmput+0x48/0x1e8
 do_exit+0x388/0xcb0
 __arm64_sys_exit_group+0x28/0x30
```

`mmu.c:456` 就是对 unmap 走查本身的返回值检查:

```c
	lockdep_assert_held_write(&kvm->mmu_lock);
	WARN_ON(size & ~PAGE_MASK);                                    /* :455 */
	WARN_ON(stage2_apply_range(mmu, start, end, ___unmap_stage2_range,
				   may_block));                        /* :456 */
```

板子不受影响:不挂死、不 Oops,进程正常退出。丢掉的是这条告警存在的意义所保护的那个保证 —— **一个 VM 死掉时,它在 EL2 的映射全部被释放**。

## 触发条件(已隔离)

三种模式,每种 5 次,在 `#4` 上(`evidence/2026-08-05-mode-matrix-and-instance.txt`):

| 模式 | 客户机内存 | 脏页日志 | `-E2BIG`(003) | **本告警** |
| --- | --- | --- | --- | --- |
| default | 2 MiB THP 块 | 开 | 5/5 | **0/5** |
| `nohuge` | 强制 4 KiB | 开 | 0/5 | **5/5** |
| `nodirty` | 2 MiB THP 块 | 关 | 0/5 | **0/5** |

**与问题 003 完全反相关。** 这条告警恰好在脏页日志超级调用**成功**时出现,在它失败或根本没被调用时从不出现。脏页日志是必要条件(`nodirty` 是硬零),但不充分 —— 那次调用还必须**真的走通**。

## 机制

**置信度:假设。** 失败的是哪个调用、错误码是什么,两者都已实测。**EL2 为什么拒绝**尚未确定 —— 而且用目前这套手段够不着。

**失败的是 EL2 的客户机 unmap 超级调用。** `pkvm_unmap_range()`、`___unmap_stage2_range()` 和 `stage2_apply_range()` 全部被内联进 `__unmap_stage2_range()`,所以告警的 `brk` 是由那次失败的调用直接到达的(`evidence/2026-08-05-warn-site-disasm.txt`):

```
0x8c4  bl   pkvm_call_hyp_nvhe_ppage      /* mmu.c:325,__pkvm_unmap_guest_call */
0x8c8  cbz  w0, +0x1f4                    /* w0 就是错误码 */
0x8cc  bl   __sanitizer_cov_trace_pc
0x8d0  b    +0x268  ->  +0x26c 处的 brk    /* 告警 */
```

**不是内层的范围检查。** `pkvm_unmap_range()` 自己在 `mmu.c:364` 有一条 `WARN_ON(end < ppage->ipa + (PAGE_SIZE << ppage->order))`,若是它触发会单独打印。所有捕获里都没有它 —— `nohuge` 恰好只产生两条告警,`pgtable.c:654` 和 `mmu.c:456` —— 所以错误确实是**从 EL2 回来的**,不是宿主自己走查出来的。

**错误码是 `-1`,已实测。** 在 `pkvm_call_hyp_nvhe_ppage()`(EL1 宿主代码,够得着)上挂 kretprobe,每种模式跑 3 次(`evidence/2026-08-05-kretprobe-el2-return.txt`):

| 模式 | 返回次数 | 取值 |
| --- | --- | --- |
| default | 9 | 全部 `0` |
| `nohuge` | 21 | 18 × `0`,**3 × `4294967295`** —— 每次运行一个 |
| `nodirty` | 6 | 全部 `0` |

`4294967295` 即 `0xFFFFFFFF`,也就是 32 位 `int` 的 `-1`。`EPERM` 是 1,而其他 errno 都不是(`EINVAL` 22、`ENOENT` 2、`E2BIG` 7、`ENOMEM` 12),所以这个值就是 **`-EPERM`**。于是反相关不只在告警层面成立,在**返回值层面**同样成立:default 模式产生**零个**非零返回。

两条值得保留的方法说明。寄存器转储不可能给出这个值 —— `w0` 在 `cbz` 处确实是错误码,但 `brk` 之前那次 SanCov 调用会破坏 `x0`,`#4` 的转储里 `x0 : 0` 正是如此。另外第一次挂探针在三种模式下都记录到 0 条,原因是 `tracing_on` 为 `0`;探针安装和使能都不会报错,只是静默地什么都不记。

**这推翻了本问题最初的假设。** `pkvm_call_hyp_nvhe_ppage()`(`pkvm.c:240-296`)会吸收掉大部分失败,能到达调用方的只有四个返回 —— `pkvm.c:263`、`:272`、`:281` 三条 `-EINVAL` 臂,以及 `:293` 的 `return err`。早前的版本认为前两条(EL2 回答 `-E2BIG` 或 `-ENOENT` 而 `order` 已为 0 时触发的那两条)解释了告警为何随页大小变化。**不成立**:`-EPERM` 不是这两个值中的任何一个,所以失败走的是 `pkvm.c:293`,把 EL2 的错误原样上抛。**页大小相关性因此仍然缺一个解释**,而且这个解释已经不可能从宿主侧得到。

**当前主导假设:unshare 检查里的页状态不匹配。** 在 EL2 侧,unmap 就是一次 unshare ——

```
handle___pkvm_host_unmap_guest        hyp_main.rs:1094
  → __pkvm_host_unshare_guest         permissions.rs:739
    → __check_host_unshare_guest      permissions.rs:747  →  host.rs:1706
```

—— 而那个检查里恰好有两处 `-EPERM` 返回,都是在比较页状态:

```rust
	let state = guest_get_page_state(pte, ipa) & !PKVM_PAGE_RESTRICTED_PROT;
	if state != PKVM_PAGE_SHARED_BORROWED {
		return -(EPERM as i32);                                  /* host.rs:1722 */
	}
	...
	__host_check_page_state_range(phys_value, PAGE_SIZE << order,
				      PKVM_PAGE_SHARED_OWNED)            /* host.rs:1728 */
```

后者在**宿主侧**状态不符时由 `___host_check_page_state_range()` 返回 `-EPERM`。两处检查的对象,恰恰就是一次成功的 `__pkvm_host_dirty_log_guest` 所改动的状态:它执行 `check_unshare()`,然后把该页以 `KVM_PGTABLE_PROT_RWX` 重新映射(`permissions.rs:135-211`)。当问题 003 用 `-E2BIG` 把这次调用打断时,这些改动一件都不会发生 —— 这正是遮蔽关系的形状。

另外注意 `host.rs:1720` 在比较前**已经把 `PKVM_PAGE_RESTRICTED_PROT` 掩掉了**,所以那一位是被容忍的,不匹配必然发生在基础状态上。

### 成因:那次重新映射把状态写没了

**置信度:2026-08-06 已在硬件上确认** —— 用的正是这个机制自己预言的一次前后对照实验(`evidence/2026-08-06-reprotect-eperm-and-lost-dirty-page.txt`,运行记录 [2026-08-06-dirtylog-reprotect-confirm](../runs/2026-08-06-dirtylog-reprotect-confirm.md));代码引文与上游来源见 `evidence/2026-08-06-upstream-provenance.txt`。仍属推断的部分在本节末尾点明。

pKVM 把一页的归属状态存**在 stage-2 PTE 里**,占用它的两个软件位,并且和权限装在同一个字里传递:

```c
enum pkvm_page_state {
	PKVM_PAGE_OWNED           = 0ULL,
	PKVM_PAGE_SHARED_OWNED    = BIT(0),
	PKVM_PAGE_SHARED_BORROWED = BIT(1),
};
#define PKVM_PAGE_STATE_PROT_MASK  (KVM_PGTABLE_PROT_SW0 | KVM_PGTABLE_PROT_SW1)
```

这三个状态表示一页内存在 Host 和客户机之间的关系:`OWNED` 表示 Host 自己拥有（客户机看不到）,`SHARED_OWNED` 表示 Host 把页共享给了 EL2 但所有权还在 Host,`SHARED_BORROWED` 表示客户机借用了这页（Host 不能随意处置它）。EL2 在执行 unshare 等操作前会检查这个状态——状态不对就拒绝。

关键在于:状态和权限装在**同一个字**（`prot`）里。所以一个不带状态的 prot 字**并不表示"维持原样"**,它会被解读成 `PKVM_PAGE_OWNED`（因为 `OWNED` 的值是 0）。这就让写入客户机 PTE 的两种方式产生了决定性的差别:

| | 对软件状态位的影响 |
| --- | --- |
| `kvm_pgtable_stage2_map(..., prot, ...)` | 按 `prot` **重建**一条全新 PTE —— 状态变成 `prot` 里携带的值 |
| `kvm_pgtable_stage2_relax_perms(..., prot, ...)` | 对现有叶子做读改写 —— 状态**原封不动** |

`relax_perms` 只会置 `S2AP_R`/`S2AP_W` 和清 `XN`（`pgtable.c:1617-1640`）,碰不到软件位。而脏页日志处理函数用的是另一个,并且传的是**裸** prot:

```rust
	kvm_pgtable_stage2_map(&mut vm_ref.pgt, guest_addr, PAGE_SIZE as u64,
			       host_addr,
			       KVM_PGTABLE_PROT_RWX,        /* permissions.rs:202 —— 不带状态 */
			       ... stage2_mc ..., 0)
```

`KVM_PGTABLE_PROT_RWX` 只包含读、写、执行三个权限位,不包含任何状态位——所以它解码出来的状态是 `PKVM_PAGE_OWNED`（0）。于是一次成功的脏页日志,留下的是一条有效、全 RWX、而状态标注为 `PKVM_PAGE_OWNED` 的客户机 PTE —— 而就在两条语句之前,`check_unshare()` 刚刚验证过它是 `PKVM_PAGE_SHARED_BORROWED`。**这个处理函数先校验了那个标注,然后把它覆盖掉了。**

打个比方:这就像保安检查了你的访客证件（`SHARED_BORROWED`）,然后发给你一张新门禁卡,但新卡上的身份字段是空白的（`OWNED`）——下次你再刷这张卡进同一个门,保安看到身份不对,就把你拦下了。

这是遗漏而非某种有意的约定,由同一个 crate 自己坐实:`guest_complete_share()` 写的是**完全相同**的 host→guest 共享映射,而它写对了——`let prot = pkvm_mkstate(perms, PKVM_PAGE_SHARED_BORROWED);`（`guest.rs:1116`）。`pkvm_mkstate()` 就在 `utils.rs:401`,它先把 prot 里原有的状态字段清掉、再写入新状态（`prot & !MASK` 然后 `|= field_prep!(MASK, state)`)——对一个不带状态位的 prot 来说等价于"把状态填进去"。脏页日志处理函数漏调了这个。

**这个机制明确指向站点 A**,并且预言了具体取值:`state == PKVM_PAGE_OWNED == 0`,而期望值是 `BIT(1)`,于是返回 `-EPERM` —— 正是实测到的那个码。它同时解释了为什么触发的不是**宿主侧**那个检查(站点 B):脏页日志处理函数只动客户机页表,宿主那份 `PKVM_PAGE_SHARED_OWNED` 记录仍然完好。

它也顺带排除了审阅提出的 `guest_ack_unshare()` 线索:那条回退是改用 `SHARED_BORROWED | RESTRICTED_PROT` 重试(`guest.rs:1072-1089`),仍然不等于 `0`,救不了这种情况。

### 已确认:同一页、同一个检查,先通过后失败

这个机制预言了一次**从 EL1 就够得着、根本不用碰管理程序**的失败。`__pkvm_host_wrprotect_guest()` 调的是**同一个** `__check_host_unshare_guest()`,而它可以由 `KVM_GET_DIRTY_LOG` 触达 —— 后者会把自己报告为脏的页重新写保护。所以一个刚被脏页日志处理过的页,必然无法被重新写保护。`repro/probe-dirtylog-twice.c` 就是为了**证伪**这一点而写的。它没能证伪。

```
PHASE enable-dirty-logging
  ppage_ret (__stage2_wp_range <- pkvm_call_hyp_nvhe_ppage) ret=0   x3   <- 代码页、A、B
PHASE round2-run                                                        <- 无探针命中
PHASE getdirty-1-reprotect
  ppage_ret (__stage2_wp_range <- pkvm_call_hyp_nvhe_ppage) ret=4294967295   <- 仅页 A,-EPERM
  wpr_ret   (kvm_arch_mmu_enable_log_dirty_pt_masked <- __stage2_wp_range) ret=4294967295
PHASE round3-run                                                        <- 无探针命中
after round3:  A(page 16)=0   B(page 32)=1
```

**`__pkvm_host_wrprotect_guest` 先在页 A 上成功,约 500 微秒后又在页 A 上失败。** 两者之间只发生了一件事:对 A 的一次成功的 `__pkvm_host_dirty_log_guest`。这排除了"这个检查本来就会在这里失败",也排除了该页的任何静态属性。5/5:初次写保护 `0`,重新写保护 `-EPERM`,从无反例。

两个**空相位**同样有分量。round 2 没有 `ppage_ret`,是因为脏页日志分支是一次**裸** `kvm_call_hyp_nvhe(__pkvm_host_dirty_log_guest, gfn)`(`mmu.c:1755`),不经过 `pkvm_call_hyp_nvhe_ppage()`;也没有 `pkvm_mem_abort`,因为那是权限缺页。round 3 是空的,则是因为 **A 根本没有缺页** —— 这正是缺陷本身。

**这同时收窄了站点,而且没有进入 EL2。** 脏页日志处理函数只写 `vm->pgt`,那条路径上没有任何东西碰宿主状态或 vmemmap。所以在"通过"与"失败"这两次调用之间,宿主侧的那个比较不可能发生变化,剩下的只有 `host.rs:1722` 处的客户机状态比较。

**仍属推断的部分:** PTE 的软件位从未被直接读出。以上一切都与 `state == PKVM_PAGE_OWNED` 相符,也没有别的解释能同时满足这些观测,但直接读出仍需 EL2 覆盖率捕获。两个 `-EPERM` 站点都在 `hvc` 之后,而 **kprobe 无法跨越异常级** —— kretprobe 之所以奏效,只因为 `pkvm_call_hyp_nvhe_ppage()` 是宿主代码。这与问题 002 上读寄存器那一手撞的是同一堵墙。

### 更重的后果:脏页日志会静默丢写

上面说的是销毁时的告警,但真正严重的后果在销毁之前就已经发生了——只是它不报错、不告警,完全静默。

事情是这样的。脏页日志的工作循环是:写保护 → 客户机写 → 缺页 → 标记脏 → 恢复可写 → 客户机继续。而 `KVM_GET_DIRTY_LOG` 在读取脏页位图后,会把自己报告为脏的页**重新写保护**,开始下一轮跟踪。这次重新写保护调用的正是 `__pkvm_host_wrprotect_guest()`,而它内部的 `__pkvm_wrprotect()`（`permissions.rs:400`）在改页表之前先调 `__check_host_unshare_guest_order()`（`permissions.rs:411`）——和站点 A 是同一族的状态检查。

但那个页的状态已经在上一轮被脏页日志处理函数覆盖成 `PKVM_PAGE_OWNED` 了,所以这次重新写保护必然失败,返回 `-EPERM`。而这个错误被直接丢弃——因为 `kvm_stage2_wp_range()` 是 `void` 函数,`kvm_arch_mmu_enable_log_dirty_pt_masked()` 根本没地方把它返回出去。

于是该页保持可写。客户机的下一次写入不再缺页（因为页表项已经是可写的了）,`user_mem_abort()` 不会运行,`mark_page_dirty_in_slot()`（`mmu.c:2194`）也就永远不会被调用。**这一页从此在脏页位图里消失——它不会再被标记为脏,即使客户机继续往它写。**

实测 5/5:页 A 在脏页日志下的第二次写入**不出现在位图里**,而同一个程序对同一页的 round 2 写入是被正确记录的。这条失败从头到尾都是静默的——用户态没有错误、`dmesg` 没有消息、`KVM_GET_DIRTY_LOG` 返回成功。

对热迁移或快照而言,这意味着拷走陈旧数据并返回成功。它是脏页日志本身的正确性缺陷,而不只是本问题立案时那点销毁期的不整洁。

## 复现

`repro/probe-dirtylog-thp.c` 的 `nohuge` 模式:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-thp repro/probe-dirtylog-thp.c
./probe-dirtylog-thp nohuge     # KVM_RUN 成功,退出时随即出现本告警
./probe-dirtylog-thp            # 大页:改为 003 触发,本告警不出现
./probe-dirtylog-thp nodirty    # 两者都不出现
```

该告警在进程退出时经 `exit_mmap` 打印。读 `dmesg` 要带沉降 —— 程序返回后立刻读会间歇性漏掉。

要复现**脏页丢失**和那次失败的重新写保护,用 `repro/probe-dirtylog-twice.c` —— 它多跑一轮、带一个对照页,而且完全不需要看 `dmesg`:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-twice repro/probe-dirtylog-twice.c
./probe-dirtylog-twice nohuge
#   after round2:  A(page 16)=1  B(page 32)=0     <- 正确
#   after round3:  A(page 16)=0  B(page 32)=1     <- A 的写入丢了
```

想直接看到 `-EPERM`,就挂上 `r:ppage_ret pkvm_call_hyp_nvhe_ppage ret=$retval:s64` 与 `r:wpr_ret __stage2_wp_range ret=$retval:s64` 再跑。**先把 `tracing_on` 置 `1`** —— 探针安装和使能都不会报错,而它为 `0` 时什么都不记。程序会把相位名写进 `trace_marker`,正是这一点让每个返回值能归位到某一轮。

## 影响面

unmap 走查在失败处中止。`stage2_apply_range()` 返回第一个错误,所以该范围内**位于失败点之后的固定页不会被 unmap、不会被解除固定、也不会被记账** —— 而这是一个正在被销毁的 VM。这是否构成真正的泄漏,取决于随后的 `__pkvm_finalize_teardown_vm` 与 `drain_hyp_pool` 能回收多少,尚未核查。

还有第二个更窄的后果,是代码能直接定下来的。`__pkvm_host_unshare_guest()` 在检查失败时,是在它的两项副作用**之前**就返回的(`permissions.rs:739-777`):

```rust
	let ret = __check_host_unshare_guest(vm, &mut phys, ipa, order);
	if ret != 0 { ...解锁...; return ret; }                 /* <-- 走的是这条 */

	kvm_pgtable_stage2_unmap(&mut vm_ref.pgt, ipa, PAGE_SIZE)          /* 被跳过 */
	__host_set_page_state_range(phys, PAGE_SIZE << order, PKVM_PAGE_OWNED)  /* 被跳过 */
```

所以那个在脏页日志期间被客户机写过的页,既保留着一条活的客户机 stage-2 映射,**宿主**对它的记录也仍然停在 `PKVM_PAGE_SHARED_OWNED` 而没有回到 `PKVM_PAGE_OWNED` —— 而这个 VM 已经不存在了。之后是否有别的东西把它收回,正是上面 `__pkvm_finalize_teardown_vm` 那个问题。

本问题没有做内存损失测量。问题 004 的教训适用:`MemFree` 在这个量级上被噪声主导,与其称量机器,不如去追销毁路径的代码。

## 与其他问题的关系

四个问题都在同一种输入上触发 —— 普通 VM、vCPU 跑过、然后开启脏页日志 —— 矩阵把它们实验性地分开:

| | 002 | 003 | 004 | **005** |
| --- | --- | --- | --- | --- |
| 需要脏页日志 | 是 | 是 | 是 | 是 |
| 需要大页 | 否 | **是** | 否 | **否 —— 恰恰相反** |
| 需要脏页日志调用**成功** | 否 | 不适用 | 否 | **是** |
| 位置 | 宿主 `mmu.c:190` | EL2 `permissions.rs:147` | 宿主 `mmu.c:1749` | EL2 unmap,经 `mmu.c:325` |
| 来源 | 上游,且上游**已修** | 继承自 `ee88afa47b55` | 三分,见 004 | 继承自 `ee88afa47b55` |
| 可迁移补丁 | 有(`fce886a60207`) | 无 —— 上游把功能删了 | 部分 | **无** —— 同一次删除 |

**它们构成一条互相遮蔽的链**,这正是现在就立案的现实理由:

- **002 遮蔽 003。** 没有 TLB 失效,写保护未必对客户机可见,于是能走到出问题路径的那次缺页不是必然的 —— 这就是 003 在 `#4` 上测得 22/25 而非 25/25 的原因。
- **003 遮蔽 005。** 脏页日志超级调用在 EL2 改动任何状态之前就中止了,那个会失败的 unmap 根本没机会发生。

因此按顺序修复是在**揭示**而不是在解决:修好 002,003 变成 100%;修好 003,**005 会在每一次带脏页日志的销毁中出现**。谁合入 003 的补丁之后看到这条告警,应当把它读作"遮蔽被揭开",而不是回归。

## 上游

2026-08-06 检索,覆盖 `aosp/android15-6.6`、`aosp/android16-6.12`、`aosp/android17-6.18` 与 `torvalds/master`。完整记录见 `evidence/2026-08-06-upstream-provenance.txt`。

**这个缺陷是从 ACK 原样继承来的 —— klinux 没有引入任何东西。** EL2 处理函数是随着创造它的那笔提交一起到来的:

```c
/* ACK ee88afa47b55, 2024-04-03, Vincent Donnefort
   "ANDROID: KVM: arm64: Huge page support for pKVM guest relax perm" */
int __pkvm_host_dirty_log_guest(u64 gfn, struct pkvm_hyp_vcpu *vcpu)
{
	ret = __check_host_unshare_guest(vm, &phys, ipa, 0);
	if (ret)
		goto unlock;

	ret = kvm_pgtable_stage2_map(&vm->pgt, ipa, PAGE_SIZE,
				     phys, KVM_PGTABLE_PROT_RWX,   /* <-- 裸 prot */
				     &vcpu->vcpu.arch.stage2_mc, 0);
```

和 klinux 的 Rust 是同一个 `(gfn, vcpu)` 签名、同一个硬编码 order 0、同一个裸 prot。而那笔提交的**宿主侧**改动,就是 klinux 的 `mmu.c:1749/1755/1762`,逐行相同,连 `logging_active` 分支都一样。

**上游另一套脏页日志设计同样有这个问题。** `android15-6.6` 带的是另一个处理函数 `__pkvm_dirty_log(hyp_vcpu, pfn, gfn)`(`mem_protect.c:2771`),它结尾也是 `kvm_pgtable_stage2_map(..., KVM_PGTABLE_PROT_RWX, ...)`。两套设计都会把状态丢掉。

**上游从未修复它。上游是把这个功能删了。**

| 提交 | 日期 | 对这一行做了什么 |
| --- | --- | --- |
| `ee88afa47b55` | 2024-04-03 | 引入它,裸 prot |
| `6a3d47d01594` | 2025-01-28 | *"Make `__pkvm_host_dirty_log_guest()` upstream-friendly"* —— 重排了**正是这次调用**、改了检查函数名,**裸 prot 原样保留** |
| `6e3ff69cb190` | 2025-01-28 | *"Remove `__pkvm_host_dirty_log_guest()`"* —— 43 行删除,纯移除 |

移除的理由是重新设计,不是修 bug:*"Now that dirty logging for no-guests is done from `user_mem_abort()` with the standard KVM logic, the hypercall is unused."* 从 6.12 起,活下来的 `__pkvm_host_relax_perms_guest()` 用 `kvm_pgtable_stage2_relax_perms()` 做同一件事,而它动不了状态位 —— 所以 6.12、6.18 和 mainline 是**结构上免疫**,而不是被打了补丁。用 `git log --grep` 在两条 ACK 线上搜 `dirty log|dirty_log|mkstate|page state`,搜不到任何修复。

`6a3d47d01594` 值得单独记一笔:一次重写了这次调用、却没看见问题的清理提交。

## 修复状态

> **修复已提交到 klinux,commit `cfaadd217ab6`**:在那次重新映射处改用 `pkvm_mkstate(KVM_PGTABLE_PROT_RWX, PKVM_PAGE_SHARED_BORROWED)`。**仅通过编译**,`disposition: fix-proposed`,未验证。判据见下文的回归测试。

已按上述方式本地修复。**没有可迁移的补丁** —— 上游的解法是删功能,而删功能的前提是把 np-guest 脏页日志整体挪进通用的 `user_mem_abort()`,那是对整条 np-guest 内存路径的重新设计,不是一个 hunk。所以这个得我们自己写。

**建议的修法 —— 把那次映射抹掉的标注补回去。** `permissions.rs:202`,`__pkvm_host_dirty_log_guest()` 内:

```rust
-            KVM_PGTABLE_PROT_RWX,
+            pkvm_mkstate(KVM_PGTABLE_PROT_RWX, PKVM_PAGE_SHARED_BORROWED),
```

同时把 `pkvm_mkstate` 加进 `permissions.rs:28` 的 `use crate::utils::{...}`,把 `PKVM_PAGE_SHARED_BORROWED` 加进 consts 的引入。

`PKVM_PAGE_SHARED_BORROWED` 是正确取值,不是猜的:它就是两条语句之前 `check_unshare()` 刚验证过的那个状态;这次映射本来就不打算改变归属(只改权限和粒度);而且 `guest_complete_share()` 为**同一种** host→guest 共享映射写的正是它(`guest.rs:1116`)。**不能**顺手加上 `PKVM_PAGE_RESTRICTED_PROT` —— 那一位是由 `prot != RWX` 推导出来的,而这次映射是全 RWX。

另一条更贴近上游走向的路,是干脆不在这里用 `stage2_map`,改走 6.12 的 `kvm_pgtable_stage2_relax_perms()`。那个形状更抗未来,但它**拆不开块映射**,所以要等问题 003 的大页问题先解决才谈得上。而这个一行修复与 003 正交,可以先落。

**确认这一关已经过了。** 本项目的规矩是:不在纯代码阅读得出的机制上建补丁。现在它已经不是了 —— 同一页、同一个超级调用上的前后对照记在 `evidence/2026-08-06-reprotect-eperm-and-lost-dirty-page.txt`,5/5,连同它预言的那个用户可见的脏页丢失。**这个补丁可以写了。**

唯一还没做的验证,不是修复的前置条件,而是对修复的一次校核:一次 **EL2 覆盖率捕获**(武装 `pkvm_cov` ring,分别跑 `nohuge` 与 `nodirty`,对 `.rs:line` 取差)能让 `host.rs:1722` 只出现在失败那一侧,把站点**直接读出来**而不是推断出来;在打了补丁的内核上再跑一次,它应当消失。这仍将是本项目第一次需要这个 ring 来推进问题、而不只是测吞吐。

**回归测试。** 打完补丁就跑 `repro/probe-dirtylog-twice.c nohuge`。判据是 **round 3 的 `A=1`**,以及 trace 里那次重新写保护返回 `0` 而不是 `-EPERM`。两者今天都 5/5 失败,所以这是一个真测试,不是同义反复。**不要**把 `B=1` 写进判据 —— 对照页今天只有 3/5 被跟踪,原因与本缺陷无关且尚未查清(见运行记录),拿它当判据会造成假失败。

**修这个不等于修了 003,也不需要先修 003。** 它们是同一个处理函数里两个独立的缺陷:003 是那次 order 0 查表拒绝块映射,005 是那次映射把状态丢掉。两者都继承自 `ee88afa47b55`。

### 具体怎么修

**未经验证。** 这个函数已经有两个从代码推出来的设计在硬件上翻车了,所以在 `probe-dirtylog-twice.c` 说话之前,下面仍然只是假设。这次不同的地方在于:两个分支各自的失败模式现在是**已知**的,而不是假定的。

原语必须按当前粒度来选,因为两种情形的失败方向正好相反:

```rust
    ret = checked_tx.check_unshare();
    if ret != 0 { /* 解锁 */ return ret; }

    // 探测粒度。这就是 003 删掉的那次调用,但这里用作**判别器**而不是关卡 ——
    // -E2BIG 现在用来选择路径,而不是让整个超级调用失败。
    let mut phys: u64 = 0;
    let mut pte: kvm_pte_t = 0;
    let r = ptr_wrapper!(vm).guest_get_valid_pte(&mut phys, guest_addr, 0, &mut pte);

    ret = if r == 0 {
        // 已经是 PAGE_SIZE:这是一次纯权限放宽,而 stage2_map() 拒绝这种更新
        // (且它的 -EAGAIN 会被 walker 吞成 0)。relax_perms 才是指定路径,
        // 而且它是对现有叶子的读改写,软件状态位天然保留 —— 无需回写。
        kvm_pgtable_stage2_relax_perms(&mut vm_ref.pgt, guest_addr,
                                       KVM_PGTABLE_PROT_RWX, 0)
    } else if r == -(E2BIG as i32) {
        // 块映射:stage2_map() 会把块拆开。拆块时目标页那一项被留空(无效),
        // 因为 stage2_map_walk_table_pre() 跳过调用方的范围,于是 old_is_counted
        // 为假、needs-update 过滤不生效,映射照常写入。也正因为那是全新的一项,
        // 状态位必须在这里显式写回。
        kvm_pgtable_stage2_map(&mut vm_ref.pgt, guest_addr, PAGE_SIZE as u64, host_addr,
                               pkvm_mkstate(KVM_PGTABLE_PROT_RWX, PKVM_PAGE_SHARED_BORROWED),
                               mc, 0)
    } else {
        r
    };
```

**为什么两个分支各自站得住。**

- 4 KiB 那支正是死循环的那一支,而 `relax_perms` 恰恰是 `pgtable.c:961-975` 让调用方改用的东西。它在 EL2 也已被证明可用 —— `__pkvm_host_relax_guest_perms()` 就是非日志那一支的兄弟,今天在正常工作。
- 块映射那一支**已经在硬件上跑着**。`#13` 的默认 THP 模式 5/5 通过,走的正是"对块做 `stage2_map(PAGE_SIZE)`"。唯一的改动是给 prot 加上状态位 —— 而这恰恰是必要的,因为拆块之后目标项是全新的,否则会被写成 `PKVM_PAGE_OWNED`。

**这套修法不需要什么。** 不需要移植 `__pkvm_host_split_guest`,不需要 hyp request 管道,不需要动宿主的 `kvm_pinned_page` 树。`stage2_map` 本来就会拆块,而 `pkvm_call_hyp_nvhe_ppage()` 的 `-E2BIG` 重试本来就能应付"宿主的树比 stage-2 粗" —— 这不是推测,`#13` 此刻正在这么跑。上游 android16-6.12 的拆块系列仍然是更整洁的长期形态,但选它的理由是与 ACK 收敛,不是必需。

**判据。** `repro/probe-dirtylog-twice.c nohuge` 的 round 3 必须给出 `A=1`、且重新写保护返回 `0`;同时 `repro/probe-dirtylog-thp.c` 三种模式都必须仍然通过 —— 后半条很重要,因为块映射那一支是目前唯一正常的,不能被改坏。看 `rc`,别只看输出:这些程序挂死时一个字都不打印。

## 残留风险

因为 003 遮着它,任何在 `#4` 这类内核上测得的 005 频率都只是**下界**;而任何不开启脏页日志的测试永远看不到它。迄今为止的 syzkaller 测试都属于后者 —— 2026-07-31 与 2026-08-05 两轮的控制台日志里都没有这个签名。

**而这个缺陷更重的那一半,根本没有签名。** 销毁时的告警至少还是一条消息;脏页丢失什么都不产生 —— 用户态没有错误、`dmesg` 没有一行、`KVM_GET_DIRTY_LOG` 返回成功。任何靠刮控制台日志的测试都找不到它,当前配置下的 syzkaller 也找不到,因为它没有"这个位图本该置位"这样的判据。它是先从代码里**预言**出来、再写一个"若预言为假就会失败"的测试才被抓到的。在判断模糊测试到底买到了什么覆盖时,这一点值得记住。
