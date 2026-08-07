# 007 —— 2 MiB 块映射让 EL2 触发 bug_on!(),而 panic 处理器是 loop{},所以板子直接死了

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

在添加 `syz_kvm_run_protected_guest$arm64`（本 fork 里第一个跑**保护型**客户机的复合调用）时发现。当晚手工复现,根因定位到一个单符号的移植错误,通过读取已经死掉的 CPU 的 EL2 程序计数器锁定位置。

**两个独立的缺陷。** 一个让 EL2 触发 `bug_on!`;另一个把任何 EL2 `bug_on!` 变成静默的全板死机。第二个更严重,也正是它让这个问题花了五次硬重启才诊断出来。

---

## 背景知识

### 保护型客户机与普通客户机的区别

在 pKVM 架构下,客户机分两种:

- **普通客户机**（`KVM_CREATE_VM`）:宿主内核可以自由访问客户机的内存。销毁时,`kvm_flush_shadow_all()` 先跑,`pkvm_unmap_range()` 在 unmap 阶段就通过 `__pkvm_host_unmap_guest` 把固定页面逐个排空了。它仍然要进 EL2,只是走的是 unmap 那条路,等销毁真正开始时 pinned_pages 已经空了,`__pkvm_reclaim_dying_guest_page` 的循环一次都不执行。
- **保护型客户机**（`KVM_VM_TYPE_ARM_PROTECTED`）:客户机的内存对宿主不可见。EL2 维护着这些页面的所有权状态,销毁时必须走 `__pkvm_host_reclaim_page()` 这条 EL2 回收路径,由 EL2 检查页面状态后把所有权交还宿主。本缺陷就在这条回收路径上。

### stage-2 页表项（PTE）的结构

ARM64 的 stage-2 页表项是一个 64 位的描述符。它的低几位编码了描述符的类型和属性:

| 位 | 含义 |
|----|------|
| bit 0 | Valid 位（`KVM_PTE_VALID`）:1 = 有效 |
| bit 1 | Type 位:1 = 页描述符（`KVM_PTE_TYPE_PAGE`,4 KiB 映射）,0 = 块描述符（`KVM_PTE_TYPE_BLOCK`,2 MiB 映射）|
| bit 2 | MemAttr 低位:1 = Normal Memory |

而 `enum kvm_pgtable_prot` 是**另一套完全独立的抽象标志**,不是 PTE 上的任何位:

| `enum kvm_pgtable_prot` 的位 | 含义 |
|----|------|
| bit 0 | `KVM_PGTABLE_PROT_X`:可执行 |
| bit 1 | `KVM_PGTABLE_PROT_W`:可写 |
| bit 2 | `KVM_PGTABLE_PROT_R`:可读 |

关键在于:**这两者不是"同一个 64 位里的不同偏移",而是两个不同的命名空间。** `KVM_PGTABLE_PROT_R/W/X` 是一个小枚举的第 0/1/2 位,PTE 里根本没有它们;硬件真正存放权限的地方是 **S2AP(bit 6–7)** 和 **XN(bit 53–54)**。`kvm_pgtable_stage2_pte_prot()` 的作用是把这些硬件字段**翻译**成那个枚举:

```c
enum kvm_pgtable_prot kvm_pgtable_stage2_pte_prot(kvm_pte_t pte)
{
	enum kvm_pgtable_prot prot = pte & KVM_PTE_LEAF_ATTR_HI_SW;
	...
	if (pte & KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R)   prot |= KVM_PGTABLE_PROT_R;
	if (pte & KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W)   prot |= KVM_PGTABLE_PROT_W;
	switch (FIELD_GET(KVM_PTE_LEAF_ATTR_HI_S2_XN, pte)) { case 0: prot |= KVM_PGTABLE_PROT_X; ... }
	return prot;
}
```

正因为是两个命名空间而不是两个偏移,把 `prot` 写成 `pte` 之后,`& 0b111` 落到的是 PTE 最低三位——Valid、Type、MemAttr——与权限**毫无关系**。这也是为什么这个笔误不会退化成"权限判断得不准",而是变成"描述符类型决定了状态标志"。本缺陷的根因正在于此。

### `bug_on!` 和 Rust 的 panic 处理

`bug_on!(cond)` 是内核里常见的断言宏——条件为真时触发一个不可恢复的错误。在 C 的 nVHE 管理程序里,`BUG_ON` 走 `hyp_panic()`,会向宿主报告;但在本树的 Rust EL2 里,`bug_on!` 展开成 `panic!("BUG_ON condition failed")`（`bindings/mod.rs:300`）,而 panic 处理器是:

```rust
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

`loop {}` 意味着 CPU 在 EL2 级别原地打转,中断被屏蔽,永远不会返回宿主。对宿主而言,这个 CPU 就消失了——不响应 IPI、不参与 RCU、不回答 NMI。

### 透明大页与块映射

本板的透明大页（THP）对匿名内存是开启的:探针并没有调用 `MADV_HUGEPAGE`,只要不主动用 `MADV_NOHUGEPAGE` 关掉,一段 2 MiB 对齐的匿名映射就会被 2 MiB 大页支持（探针打印的 `aligned2M=1` 确认了对齐条件成立）。在 stage-2 页表里,4 KiB 映射对应页描述符（level 3,PTE type = 1）,2 MiB 映射对应块描述符（level 2,PTE type = 0）。这个 type 位的差别,正是触发条件。

---

## 症状

一个 memslot 保留 THP 资格的保护型客户机,永远不会完成销毁。`close(vm)` 不返回;进程在执行器的 46 秒超时时被杀。然后一个 CPU 停止报告 RCU 静止状态,停止回答 NMI:

```
rcu: INFO: rcu_sched detected stalls on CPUs/tasks:
rcu:     6-...0: (1 GPs behind) idle=db74/1/0x4000000000000000 softirq=22666/22676 fqs=6527
rcu:     (detected by 2, t=15002 jiffies, g=27013, q=7446 ncpus=8)
Sending NMI from CPU 2 to CPUs 6:                 <- 没有回溯跟上来
```

之后全是连锁反应。`cleanup_net` 在 `synchronize_rcu()` 中永久阻塞（此时它持有 `rtnl_lock`）,所有碰网络的任务堆积在它后面;`sshd` 在 `kick_all_cpus_sync()` 中软死锁,因为 seccomp 的 BPF JIT 调了 `__text_poke()`。板子能 ping 通但拒绝 ssh。只有硬重启能恢复。

完整连锁记录:[evidence/console-boot2-thp-isolated.txt](evidence/console-boot2-thp-isolated.txt)。

---

## 触发条件

单变量隔离,一次启动,一个沙箱,三个程序:

| 程序 | 耗时 | 覆盖率 | 板子 |
| --- | --- | --- | --- |
| `syz_kvm_dirty_log_cycle(hp_mode=0)` —— **普通**客户机 | ~1 s | 84318 | 正常 |
| `syz_kvm_run_protected_guest(hp_mode=0)` —— 保护型,`MADV_NOHUGEPAGE` | ~1 s | 69566 | 正常 |
| `syz_kvm_run_protected_guest(hp_mode=1)` —— 保护型,THP 不禁用 | **46 s** | **0** | **卡死** |

第 2 行和第 3 行的唯一区别是一条 `madvise(MADV_NOHUGEPAGE)` 调用。保护型客户机是必要条件（普通客户机的固定页面在 `kvm_flush_shadow_all()` 阶段就已被 `pkvm_unmap_range()` 排空,轮到回收循环时无页可回收）,块映射也是必要条件。

不需要 pvmfw。已有的保护型复合调用里,会真正运行客户机的那两个（`syz_kvm_run_fw_fault` 与 `syz_kvm_run_fw_fault_gen`）都在 `firmware_size == 0` 的 INFO 门处返回（本板没有 pvmfw 预留）,连 vCPU 都没建就退出了,所以从未触达这条路径;其余几个是纯配置路径,本来就不运行客户机。`pvmfw_load_addr` 留为无效时,EL2 的 `pkvm_vcpu_init_psci()` 直接从宿主 vCPU 寄存器取复位 PC,所以一个普通的 `KVM_SET_ONE_REG(pc)` 就能启动客户机。

---

## 根因

### 缺陷 A —— 写了 `pte` 而本该写 `prot`

`arch/arm64/kvm/hyp/xhypervisor/src/mem_protect/guest.rs`, `guest_get_page_state()`:

```rust
let prot = kvm_pgtable_stage2_pte_prot(pte);
// 前面已经判断了页表项有效，这里应该就不用再次判断了
// pte 只是 u64 类型，其值也不会改变
if (pte & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX {
    state = PKVM_PAGE_RESTRICTED_PROT;
}
```

对比 ACK `android16-6.12` 的 C 原版（`arch/arm64/kvm/hyp/nvhe/mem_protect.c`）:

```c
prot = kvm_pgtable_stage2_pte_prot(pte);
if (kvm_pte_valid(pte) && ((prot & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX))
    state = PKVM_PAGE_RESTRICTED_PROT;
```

注释显示了移植者的意图:去掉冗余的 `kvm_pte_valid(pte)` 检查（前面确实已经判断过了）。但在去掉它的同时,条件的后半段把 `prot` 也丢掉了——变成了用原始 64 位页表项 `pte` 做掩码,而不是 `kvm_pgtable_stage2_pte_prot()` 翻译出来的 `prot` 枚举值。

`KVM_PGTABLE_PROT_X = BIT(0)`,`W = BIT(1)`,`R = BIT(2)`,所以 `KVM_PGTABLE_PROT_RWX == 0b111`。而一个有效的 stage-2 描述符的低三位:

| | bit 0 `KVM_PTE_VALID` | bit 1 `KVM_PTE_TYPE` | bit 2（MemAttr 低位;Normal Memory 为 1）| `pte & 7` |
| --- | --- | --- | --- | --- |
| 4 KiB 页,末级 | 1 | **1** = `KVM_PTE_TYPE_PAGE` | 1 | `0b111` = 7 |
| 2 MiB 块,level 2 | 1 | **0** = `KVM_PTE_TYPE_BLOCK` | 1 | `0b101` = 5 |

对于一个**块描述符**,`(pte & 7) != 7` 恒成立,所以 `PKVM_PAGE_RESTRICTED_PROT` 被无条件置位。而保护型客户机的 RAM 页面状态是 `PKVM_PAGE_OWNED`（值为 0）,所以 `guest_get_page_state()` 返回的是 `PKVM_PAGE_RESTRICTED_PROT` 单独一个标志——它不匹配 `__pkvm_host_reclaim_page()` 四个 match 臂中的任何一个:

```rust
match page_state as u32 {
    PKVM_PAGE_OWNED                                            => { ... }
    PKVM_PAGE_SHARED_BORROWED                                  => { ... }
    s if s == (PKVM_PAGE_SHARED_BORROWED | PKVM_PAGE_RESTRICTED_PROT) => { ... }
    PKVM_PAGE_SHARED_OWNED                                     => { ... }
    _ => { bug_on!(true); }          // <- 这个函数里唯一的 bug_on!
}
```

对于一个**页描述符**,bit 0、bit 1、bit 2 三者都为 1,凑成 7,条件为假,状态保持 `OWNED`,第一个臂运行——这就是为什么 `hp_mode=0` 正常而 `hp_mode=1` 死机,不需要任何进一步假设。

### 缺陷 B —— EL2 的 panic 处理器是无限循环

`arch/arm64/kvm/hyp/xhypervisor/src/lib.rs:75`:

```rust
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

`bug_on!(cond)` 展开为 `panic!("BUG_ON condition failed")`（`bindings/mod.rs:300`）,执行到这里。CPU 在 EL2 打转,中断被屏蔽:永远不返回宿主,HVC 永远不完成,不执行任何插桩代码,不响应 IPI、RCU、NMI。宿主上所有需要全部 CPU 参与的操作——`synchronize_rcu()`、`rcu_barrier()`、`kick_all_cpus_sync()`——全部永久阻塞。

C 的 nVHE 管理程序的 `BUG_ON` 走 `hyp_panic()` 并向宿主报告。**在 Rust 管理程序里,每个 `bug_on!` 都从"一个可报告的 hyp panic"退化为"板子没了,而且没有任何诊断信息"。** 缺陷 B 独立于 A,对任何 EL2 bug 都会起同样作用。

---

## 硬件确认

定位过程,按顺序,每一步缩小前一步的范围:

| 问题 | 手段 | 答案 |
| --- | --- | --- |
| 是捐赠阶段还是拆解阶段? | 写 `/dev/kmsg` 的阶段标记（ssh 会断,stderr 丢失） | 拆解阶段:`KVM_RUN` 返回 `KVM_EXIT_MMIO`,`close(vm)` 不返回 |
| 宿主循环还是单次超级调用? | 对 `pkvm_destroy_hyp_vm` 跑 `function_graph`,单 pid,流到 `/dev/kmsg` | **第一次** `__reclaim_dying_guest_page_call` 就没回来;对照组同一处 12.5 µs 正常返回 |
| 在转还是在停? | 从健康 CPU 读 `kvm/pkvm_cov/ring_dump` | **停了** |
| 具体在哪? | 对最后的 PC 跑 `addr2line` | `bindings/mod.rs:300`,内联自 `__pkvm_host_reclaim_page` |

第三行是扭转整个调查方向的那一步。卡死的 CPU 的 EL2 覆盖率环相隔二十秒读出来还是一样的:

```
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381     <- t=69 s
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381     <- t=89 s
```

`flags=0x1` 表示 `pkvm_cov_begin()` 拉开了窗口,而 `pkvm_cov_end()` 从未运行——那个 HVC 没有回来。冻结的 `count` 排除了插桩过的循环（循环会驱动 `hits` 到 16381 上限并置 `OVERFLOW`）。最后三个 PC:

```
54  __kvm_nvhe___hyp_put_page+0x134
55  __kvm_nvhe___pkvm_reclaim_dying_guest_page+0x55c/0x560
56  __kvm_nvhe__RNvCs..._rustc17rust_begin_unwind+0x8/0xc
```

```
$ aarch64-linux-gnu-addr2line -e vmlinux -f -i -C 0xffff80008145bfd4
xhypervisor::mem_protect::host::__pkvm_host_reclaim_page
  arch/arm64/kvm/hyp/xhypervisor/src/bindings/mod.rs:300
__pkvm_reclaim_dying_guest_page
  arch/arm64/kvm/hyp/xhypervisor/src/pkvm.rs:1298
```

产物:[ring dump](evidence/ring-dump-cpu7-hang.txt) · [hang trace](evidence/ftrace-thp1-hang.txt) · [control trace](evidence/ftrace-control-thp0.txt) · [console cascade](evidence/console-boot2-thp-isolated.txt)。

`ring_dump` 在本 issue 之前不存在;它是覆盖率环的只读 debugfs 导出,因为卡死的 EL2 没有任何其他通道——它不能打印,宿主也不能中断它。补丁:[repro/ring-dump.patch](repro/ring-dump.patch)。

### 已排除的假设

- **EL2 KCOV 桥接。** 之所以怀疑,是因为卡死前最后追踪到的宿主调用是 `pkvm_cov_begin()`,而且 order 9 比 order 0 多好几个数量级的 SanCov 回调,所以环溢出是一个看起来合理的 order-only 触发条件。停了 `pkvm-cov-arm.service`,写 `0` 到 `enable`,确认 `armed_cpus 0`,重跑:**板子在同一个标记处卡死。**
- **宿主侧循环。** `pkvm_call_hyp_nvhe_ppage()` 的 `while` 每个分支都终止:成功路径 `pins` 归零时返回,`-E2BIG` 最多把 `order` 从 9 降为 0 一次,`-ENOENT` 每次迭代递减 `size`。trace 也独立显示它从未完成第一次调用。
- **EL2 无限循环。** 冻结的 `count`,见上。

---

## 爆炸半径

任何能打开 `/dev/kvm` 的进程都能卡死机器:创建 `KVM_VM_TYPE_ARM_PROTECTED` 的 VM,注册一个 THP-eligible 的 memslot,跑一条指令,退出。不需要 `/dev/kvm` 之外的权限,不需要特殊硬件——特别是不需要 pvmfw。`repro/probe-thp-kcov.c` 约 180 行,不到一秒就卡死。

因为故障是管理程序级别的无限循环而不是宿主故障,宿主的常规缓解手段全部失效:任务杀不掉,CPU 下线不了,`sysrq` 够不着它。

---

## 修复状态

`open`。管理程序没有任何改动——本条目仅记录诊断。

两个缺陷都要修,而且 **B 应该先修**:在 panic 处理器能报告而不是卡死之前,任何其他 EL2 缺陷都同样不可诊断,而这个问题花了五次硬重启正是这个原因。

---

## 残余风险

**已处理。** `syz_kvm_run_protected_guest$arm64` 的 `hp_mode` 已由 `int8[0:1]` 收紧为 `const[0]`（`sys/linux/dev_kvm_arm64.txt`,理由与放开条件写在该处的注释里）,所以模糊测试撞不到 THP 那一路;THP 情形保留在 `repro/probe-thp-kcov.c`。

判断依据:这个 bug **不产生崩溃报告**,只让整板失联、每次命中要一次硬重启——对模糊测试是纯负收益,修好之前留着有害无益。007 达到 `fix-verified` 后再放回 `int8[0:1]`。

---

## 同一函数里发现的相邻缺陷（非本卡死的直接原因）

记录在此以免丢失;每一条都需要自己的验证,没有一条是板子卡死的原因。

| | klinux | ACK `android16-6.12` |
| --- | --- | --- |
| `hyp_poison_page()` | `hyp_poison_page(phys)` 只清零一个 `PAGE_SIZE`(4 KiB) | `hyp_poison_page(phys, page_size)` 按 `page_size` 清零 |
| `drain_hyp_pool()`（仅 Rust）| push order `0`,捐 1 页 | 读 `page->order`,清零它,捐 `1 << order` 页 |
| 溢出保护 | 无 | `check_shl_overflow(PAGE_SIZE, order, &page_size)` |
| 保护型客户机守卫 | 无 | `if (!pkvm_hyp_vm_is_protected(vm)) return -EPERM;` |
| `SHARED_OWNED` 检查失败 | `warn_on!` 然后继续 | `ret = -EBUSY; goto unlock;` |
| stage-2 unmap 顺序 | 在页面状态切换之前 | 在它之后 |

第一条是一个独立的**机密性缺陷**:在 order 9 时只毒化了 2 MiB 块的前 4 KiB,于是 2044 KiB 的保护型客户机内存未经擦除就回到了宿主。

注意它**不是**继承自上游:ACK 的原型是 `void hyp_poison_page(phys_addr_t phys, size_t page_size)`,而 klinux 的 `hyp/include/nvhe/mm.h:22` 是 `void hyp_poison_page(phys_addr_t phys)` —— 长度参数在 klinux 本地被删掉了,**C 与 Rust 两份实现同步偏离**。所以它既不是上游缺陷,也不是 Rust 移植独有的产物。

工作记录,含两个被排除的假设:[notes/pkvm/evidence/2026-08-07-protected-thp-lockup/FINDING.md](../../notes/pkvm/evidence/2026-08-07-protected-thp-lockup/FINDING.md)。
