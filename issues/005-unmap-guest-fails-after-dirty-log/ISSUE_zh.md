# 005 —— 客户机 unmap 在销毁时失败,但只发生在脏页日志成功之后

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

> **本文行号以 `klinux @348c94763cc6`(`#4`)为准** —— 即观测所在的构建,可用 `git show 348c94763cc6:<path>` 复核。

一个脏页日志超级调用**成功过**的 VM,销毁时无法干净地拆掉:stage-2 unmap 返回错误,`__unmap_stage2_range()` 在 `exit_mmap()` 内部就此告警。它目前是隐形的,因为问题 003 让那个超级调用在**每一个大页支撑的客户机**上都失败 —— 而默认情况下,每个客户机都是大页支撑的。

现在就立案而不是以后,正是因为这层依赖:**修好 003 之后,它会在每一次带脏页日志的销毁中出现**,看上去像是 003 补丁引入的回归。它不是 —— 它比那个补丁更老,只是被它遮住了。

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

**置信度:假设。** 是哪个调用失败已经确定;**为什么**失败尚未确定。

**失败的是 EL2 的客户机 unmap 超级调用。** `pkvm_unmap_range()`、`___unmap_stage2_range()` 和 `stage2_apply_range()` 全部被内联进 `__unmap_stage2_range()`,所以告警的 `brk` 是由那次失败的调用直接到达的(`evidence/2026-08-05-warn-site-disasm.txt`):

```
0x8c4  bl   pkvm_call_hyp_nvhe_ppage      /* mmu.c:325,__pkvm_unmap_guest_call */
0x8c8  cbz  w0, +0x1f4                    /* w0 就是错误码 */
0x8cc  bl   __sanitizer_cov_trace_pc
0x8d0  b    +0x268  ->  +0x26c 处的 brk    /* 告警 */
```

**错误码无法从寄存器转储里恢复** —— 这一点值得写明,因为同样的手法在问题 002 上是奏效的。`w0` 在 `0x8c8` 处确实是错误码,但 `0x8cc` 的 SanCov 调用会在到达 `brk` 之前破坏 `x0`;`#4` 的转储里 `x0 : 0` 正是这个缘故。要拿到这个值,需要在 `pkvm_call_hyp_nvhe_ppage()` 上挂 kretprobe,或临时加一条打印 `err` 的 `WARN_ONCE`。

**不是内层的范围检查。** `pkvm_unmap_range()` 自己在 `mmu.c:364` 有一条 `WARN_ON(end < ppage->ipa + (PAGE_SIZE << ppage->order))`,若是它触发会单独打印。所有捕获里都没有它,所以错误是**从 EL2 回来的**,不是宿主自己走查出来的。

**候选错误路径。** `pkvm_call_hyp_nvhe_ppage()`(`pkvm.c:240-296`)会吸收掉大部分失败,能到达调用方的只有四个返回:

| 返回 | 条件 |
| --- | --- |
| `-EINVAL` `pkvm.c:263` | EL2 返回 `-E2BIG` 而 `order` 已经是 0 —— "something is really wrong" |
| `-EINVAL` `pkvm.c:272` | EL2 返回 `-ENOENT`,而该页 `order` 为 0 —— "不该丢掉一个 PAGE_SIZE 固定页的记录" |
| `-EINVAL` `pkvm.c:281` | `page_size > size` |
| `err` `pkvm.c:293` | 其他任何 EL2 错误,原样上抛 |

**为什么反相关很有提示性。** `nohuge` 下页是 4 KiB,即 `ppage->order == 0` —— 而上表前两行恰好就是"当 order 已经为 0 时,把一个**本可恢复**的 EL2 回答变成硬 `-EINVAL`"的两条臂。在 default 模式下,同样的 EL2 回答会被重试或 fallthrough 掉。这是对"告警为何随页大小变化"的一个合理解读,但**尚未坐实**:它假定 EL2 的错误是 `-E2BIG` 或 `-ENOENT`,而那恰恰是没能读到的那个值。

**成功的脏页日志调用留下了什么。** 当它走通时,`__pkvm_host_dirty_log_guest` 会执行 `check_unshare()`,然后把该页以 `KVM_PGTABLE_PROT_RWX` **重新映射**(`permissions.rs:135-211`)。也就是说它改动了 EL2 的页状态和客户机 stage-2 表项,而随后对同一页的 unmap 就失败了。当 003 用 `-E2BIG` 把这次调用打断时,这些改动一件都不会发生 —— 这正是遮蔽关系的形状,也是它此前不可能被看到的原因。

## 复现

`repro/probe-dirtylog-thp.c` 的 `nohuge` 模式:

```
aarch64-linux-gnu-gcc -O2 -static -o probe-dirtylog-thp repro/probe-dirtylog-thp.c
./probe-dirtylog-thp nohuge     # KVM_RUN 成功,退出时随即出现本告警
./probe-dirtylog-thp            # 大页:改为 003 触发,本告警不出现
./probe-dirtylog-thp nodirty    # 两者都不出现
```

该告警在进程退出时经 `exit_mmap` 打印。读 `dmesg` 要带沉降 —— 程序返回后立刻读会间歇性漏掉。

## 影响面

unmap 走查在失败处中止。`stage2_apply_range()` 返回第一个错误,所以该范围内**位于失败点之后的固定页不会被 unmap、不会被解除固定、也不会被记账** —— 而这是一个正在被销毁的 VM。这是否构成真正的泄漏,取决于随后的 `__pkvm_finalize_teardown_vm` 与 `drain_hyp_pool` 能回收多少,尚未核查。

本问题没有做内存损失测量。问题 004 的教训适用:`MemFree` 在这个量级上被噪声主导,与其称量机器,不如去追销毁路径的代码。

## 与其他问题的关系

四个问题都在同一种输入上触发 —— 普通 VM、vCPU 跑过、然后开启脏页日志 —— 矩阵把它们实验性地分开:

| | 002 | 003 | 004 | **005** |
| --- | --- | --- | --- | --- |
| 需要脏页日志 | 是 | 是 | 是 | 是 |
| 需要大页 | 否 | **是** | 否 | **否 —— 恰恰相反** |
| 需要脏页日志调用**成功** | 否 | 不适用 | 否 | **是** |
| 位置 | 宿主 `mmu.c:190` | EL2 `permissions.rs:147` | 宿主 `mmu.c:1749` | EL2 unmap,经 `mmu.c:325` |

**它们构成一条互相遮蔽的链**,这正是现在就立案的现实理由:

- **002 遮蔽 003。** 没有 TLB 失效,写保护未必对客户机可见,于是能走到出问题路径的那次缺页不是必然的 —— 这就是 003 在 `#4` 上测得 22/25 而非 25/25 的原因。
- **003 遮蔽 005。** 脏页日志超级调用在 EL2 改动任何状态之前就中止了,那个会失败的 unmap 根本没机会发生。

因此按顺序修复是在**揭示**而不是在解决:修好 002,003 变成 100%;修好 003,**005 会在每一次带脏页日志的销毁中出现**。谁合入 003 的补丁之后看到这条告警,应当把它读作"遮蔽被揭开",而不是回归。

## 修复状态

未修,而且诊断还不足以提出修法。下一步就是那个错误码,距离一条 kretprobe 之遥。

**本问题的上游检索尚未做。** 需要注意的是,检索会被 003 已经确立的事实所塑造:`__pkvm_host_dirty_log_guest` 在上游**根本不存在** —— ACK 用的是 `__pkvm_dirty_log(pfn, gfn)`,而且会事先把块拆开 —— 所以这条告警所抱怨的那个状态,在上游可能压根不会出现。若如此,005 会和 003 一样属于本地缺陷而非继承缺陷;但这一点尚未核实。

## 残留风险

因为 003 遮着它,任何在 `#4` 这类内核上测得的 005 频率都只是**下界**;而任何不开启脏页日志的测试永远看不到它。迄今为止的 syzkaller 测试都属于后者 —— 2026-07-31 与 2026-08-05 两轮的控制台日志里都没有这个签名。
