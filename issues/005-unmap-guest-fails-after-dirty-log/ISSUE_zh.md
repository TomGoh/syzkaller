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

**究竟是哪一处触发,没有实测,而且从 EL1 根本测不到。** 两处都在管理程序内部、在 `hvc` 之后。kretprobe 之所以奏效,是因为 `pkvm_call_hyp_nvhe_ppage()` 是宿主代码;**kprobe 无法跨越异常级**。当年定下问题 002 的手法(在 WARN 处读寄存器)和定下这一半的手法(在边界挂探针),撞的是同一堵墙。

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

未修,而且诊断还不足以提出修法。错误码现在已经知道了;剩下的是**两处 EL2 检查中究竟是哪一处**触发,而这**从 EL1 回答不了**。

因此下一步是一次 **EL2 覆盖率捕获**,而这套工具本项目已经有了:武装 `pkvm_cov` ring,分别跑 `nohuge` 和 `nodirty`,对 `.rs:line` 覆盖集合取差。只在失败那一侧出现的,是 `host.rs:1722` 还是 `___host_check_page_state_range()` 里的比较,就是那个站点。这正是这个 ring 当初被造出来的用途,也是本 tracker 里第一次需要它来**推进**问题、而不只是用来测吞吐。

**本问题的上游检索尚未做。** 需要注意的是,检索会被 003 已经确立的事实所塑造:`__pkvm_host_dirty_log_guest` 在上游**根本不存在** —— ACK 用的是 `__pkvm_dirty_log(pfn, gfn)`,而且会事先把块拆开 —— 所以这条告警所抱怨的那个状态,在上游可能压根不会出现。若如此,005 会和 003 一样属于本地缺陷而非继承缺陷;但这一点尚未核实。

## 残留风险

因为 003 遮着它,任何在 `#4` 这类内核上测得的 005 频率都只是**下界**;而任何不开启脏页日志的测试永远看不到它。迄今为止的 syzkaller 测试都属于后者 —— 2026-07-31 与 2026-08-05 两轮的控制台日志里都没有这个签名。
