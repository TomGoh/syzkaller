---
id: 001
slug: pkvm-unmap-selfdeadlock
title: pkvm_unmap_guest() 在 stage2_unmap_vm() 持有 mmap_lock 读锁期间请求写锁导致自死锁
class: kernel-defect
signature: 'INFO: task hung in __unmap_stage2_range'
hazard: wedges-target
diagnosis: root-caused
disposition: fix-verified
repro: repro/repro-deadlock.c
observations:
  - target: 'klinux 6.6.103+ #3 @39ee2e725c12'
    state: reproduced
    run: 2026-08-05-deadlock-repro
    evidence: evidence/2026-08-05-task-stack.txt
  - target: 'klinux 6.6.103+ #4 @348c94763cc6'
    state: not-observed
    run: 2026-08-05-deadlock-fix-verify
---

# 001 — `pkvm_unmap_guest()` 在 `mmap_lock` 上的自死锁

任何拥有 `/dev/kvm` 访问权限的进程都可以让未打补丁的板子永久卡死，且不需要模糊测试器的帮助。当前跟踪的最高严重性问题：它是唯一一个会让整台机器不可用的缺陷。

## 现象

一个任务进入 `KVM_ARM_VCPU_INIT` 的不可中断睡眠后永远不返回。卡死时从 `/proc/79770` 读取的实时状态（`evidence/2026-08-05-task-stack.txt`）：

```
PID=79770  state=D  wchan=account_locked_vm
[<0>] account_locked_vm+0x4c/0x108
[<0>] __unmap_stage2_range+0x230/0x2e8
[<0>] stage2_unmap_vm+0x178/0x298
[<0>] kvm_arch_vcpu_ioctl+0x41c/0xc30
[<0>] kvm_vcpu_ioctl+0x5e4/0xac8
[<0>] __arm64_sys_ioctl+0x108/0x128
```

hung-task 看门狗随后在 122 / 143 / 225 秒重复报告同一调用栈（`evidence/2026-08-05-dmesg-live.txt`），确认该任务始终未完成。

这些栈帧偏移与 2026-07-31 测试活动在 **N90 和 D3000** 上产生的卡死任务字节完全一致（`notes/pkvm/evidence/v11-campaign-n90-2026-07-31/FINDING.md`），这将该活动中的卡死和此复现程序识别为同一缺陷，而非两个相似的不同缺陷。

---

## 背景知识

要理解这个死锁，需要先了解几个概念及其之间的关系。

### ARM64 KVM 的两级地址翻译

在 ARM64 的虚拟化架构中，内存地址翻译分为两级：

- **Stage-1**：Guest 虚拟地址 → Guest 物理地址，由 Guest 自己的页表控制，VM 运行时由硬件自动完成翻译。
- **Stage-2**：Guest 物理地址 → Host 物理地址，由 Hypervisor 管理。这确保了 Guest 只能访问分配给它的物理内存，无法越界。

KVM 为每个 VM 维护一棵 Stage-2 页表。当 Guest 访问一段尚未映射的物理内存时，触发 Stage-2 缺页异常，KVM 的缺页处理函数（`user_mem_abort()` 或 pKVM 模式下的 `pkvm_mem_abort()`）负责将对应的 Host 物理页映射进 Stage-2 页表。

### pKVM（protected KVM）是什么

pKVM 是 Android/ACK 对 KVM 的扩展，其核心思想是将部分 Hypervisor 代码运行在 EL2（更高的异常级别），使其对 Host 内核本身也不可信任。在 pKVM 模式下：

- Host 内核仍然管理 Guest 的内存分配，但不能直接修改 Stage-2 页表——这由 EL2 端的 pKVM 代码完成。
- Host 通过 hypercall 请求 EL2 执行页表操作（映射、解除映射等），而不是自己直接操作。
- **被固定的页面（pinned pages）**：在 pKVM 模式下，每当地址翻译需要一个新页面，Host 必须先通过 `pin_user_pages()` 锁定该用户页面（防止被回收），然后通过 hypercall `__pkvm_host_map_guest` 请求 EL2 映射它。这些被锁定的页面用 `kvm_pinned_page` 结构追踪，存放在 VM 的 `pinned_pages` 区间树/枫树中。

主机通过启动参数 `kvm-arm.mode=protected` 启用 pKVM。

### RLIMIT_MEMLOCK 与 `account_locked_vm()`

`pin_user_pages()` 将用户页面锁定在物理内存中，防止它被换出或迁移。Linux 通过 `RLIMIT_MEMLOCK` 限制每个进程能锁定的内存量，防止恶意或错误的程序耗尽物理内存。

`account_locked_vm()`（`mm/util.c`）就是操作这个计数器的函数：`inc=true` 时递增 `mm->locked_vm`（锁定页面时），`inc=false` 时递减（解除锁定时）。因为 `mm->locked_vm` 是进程地址空间的核心状态，修改它需要持有该进程的 `mmap_lock` **写锁**：

```c
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc)
{
    if (pages == 0 || !mm)
        return 0;

    mmap_write_lock(mm);    /* 修改 locked_vm 需要写锁 */
    ret = __account_locked_vm(mm, pages, inc, current, capable(CAP_IPC_LOCK));
    mmap_write_unlock(mm);
    return ret;
}
```

### `mmap_lock` 是什么

`mmap_lock`（`struct mm_struct->mmap_lock`）是保护进程地址空间核心数据结构的读写信号量（rwsem）：

- **读锁**：用于遍历 VMA 链表/树——例如缺页处理、`/proc/<pid>/maps` 读取等只需观察但不修改的场景。
- **写锁**：用于修改地址空间——例如 `mmap`/`munmap`、修改 `locked_vm` 计数器等会改变进程内存布局或状态的场景。

Linux rwsem 的关键语义：**一个已经持有读锁的任务不能再获取同一把锁的写锁**（读→写升级不可递归）。这不是竞争条件——即使没有其他竞争者，同一任务的读→写升级也必然死锁。

### Stage-2 FWB（Force Write-Back）

FWB 是 ARMv8.4 引入的硬件特性。当 CPU 支持 Stage-2 FWB 时，硬件保证 Stage-2 页表可以强制所有 RAM 访问为 cacheable，这意味着不需要在重置 vCPU 时刷新整个 Stage-2 映射——只需失效 I-cache 即可。因此支持 FWB 的 CPU 永远不走 `stage2_unmap_vm()` 路径，自然不会触发此 bug。N90 和 D3000 的 CPU 不支持 FWB，所以一定会走有问题的路径。

### `KVM_ARM_VCPU_INIT` 做什么

这是 KVM 的 ioctl 命令，用于初始化或重置一个 vCPU。用户态代码可以**多次**对同一个 vCPU 调用此 ioctl——第一次是初始化，后续调用则重置 vCPU 到初始状态。当 vCPU 已经运行过（`vcpu_has_run_once()`），重置意味着 Guest 之前建立的内存映射可能已经过时，需要在重新运行前清除 Stage-2 映射，让 Guest 重新按需缺页建立干净的映射。这正是 `stage2_unmap_vm()` 被调用的场景。

---

## 机制：为什么自死锁

**置信度：已确认。** 锁的同一性不是从源码推断的——`wchan` 直接命名 `account_locked_vm` 为阻塞函数，其上方一帧即为 `stage2_unmap_vm`，即持有冲突锁的函数。

### 死锁的两个角色

| 层级 | 函数 | 做了什么 | 对 `mmap_lock` 的操作 | 锁的对象 |
|------|------|----------|----------------------|----------|
| 上层调用者 | `stage2_unmap_vm()` | 清除 VM 的全部 Stage-2 映射 | `mmap_read_lock(current->mm)` — 获取读锁 | `current->mm` |
| 下层被调者 | `pkvm_unmap_guest()` → `account_locked_vm()` | 解除 pin 并归还 RLIMIT_MEMLOCK 配额 | `mmap_write_lock(mm)` — 请求写锁 | `kvm->mm` |

`stage2_unmap_vm()` 需要读锁是因为它要遍历进程的 VMA 来找到属于每个 memslot 的地址范围；`pkvm_unmap_guest()` 里的 `account_locked_vm()` 需要写锁是因为它要修改 `mm->locked_vm` 计数器。

在绝大多数情况下，发起 ioctl 的进程就是创建 VM 的进程，因此 `current->mm == kvm->mm`，这是**同一把 rwsem**。Linux rwsem 不可递归：一个已经持有读锁的任务不可能再获取同一把锁的写锁。不存在任何调度交错能让它成功，所以这是一个**确定性死锁**，而非竞争条件。

只有当 ioctl 由不同 mm 的任务发起时（fork 的子进程继承了 fd，或通过 `SCM_RIGHTS` 传递了 fd），`current->mm` 和 `kvm->mm` 才是两把不同的 rwsem，此时不会死锁。但这是小众场景。

### 旧代码释放了错误的锁

修复前的代码确实在睡眠调用前后释放了一个锁，但它释放的是 `kvm->mmu_lock`：

```c
/* klinux 修复前 (348c94763cc6 的父提交) — pkvm_unmap_guest() */
write_unlock(&kvm->mmu_lock);
account_locked_vm(mm, 1 << ppage->order, false);   /* 内部调 mmap_write_lock(mm) */
write_lock(&kvm->mmu_lock);
```

```c
/* `2030/bug930` 世系（参考检出 ~/common）修复前 — pkvm_unmap_range() */
write_unlock(&kvm->mmu_lock);
account_locked_vm(mm, cnt, false);                  /* 内部调 mmap_write_lock(mm) */
write_lock(&kvm->mmu_lock);
```

这两段代码的注释都只说"account_locked_vm may sleep"来解释释放 `mmu_lock` 的必要性（`mmu_lock` 是自旋锁，不可在持有期间睡眠），但**完全没有提及 `mmap_lock`**。释放 `mmu_lock` 确实解决了"在自旋锁中睡眠"的问题，但真正阻塞当前任务的锁是 `mmap_lock`，而不是 `mmu_lock`——释放一个不冲突的锁对解决另一个锁的冲突毫无帮助。

这里需要理解 `mmu_lock` 和 `mmap_lock` 的角色差异：

- **`kvm->mmu_lock`**：保护 KVM Stage-2 页表自身的数据结构（页表项、pinned pages 树等），是自旋锁，持有期间不可睡眠。
- **`current->mm->mmap_lock`**：保护进程地址空间的布局和状态（VMA 树、`locked_vm` 计数器等），是读写信号量，允许睡眠。

旧代码的作者意识到了在自旋锁中调用可睡眠函数的问题并释放了 `mmu_lock`，但没有意识到更外层的 `mmap_lock` 读锁才是真正的阻塞者。

---

## 完整调用链：从用户态 ioctl 到死锁

以下是自上而下的完整调用链，每一步都标注了源码位置、函数职责和锁状态：

```
用户态: ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, ...)
    │
    │  用户态 QEMU/测试程序对已创建的 vCPU 调用初始化 ioctl。
    │  如果 vCPU 已经运行过，内核需要重置其状态。
    │
    ▼
kvm_vcpu_ioctl()                              [virt/kvm/kvm_main.c]
    │  KVM 的通用 vCPU ioctl 分发层，根据命令号路由到架构相关处理。
    │
    ▼
kvm_arch_vcpu_ioctl()                         [arch/arm64/kvm/arm.c]
    │  ARM64 架构的 vCPU ioctl 处理。对于 KVM_ARM_VCPU_INIT，
    │  调用 kvm_arch_vcpu_ioctl_vcpu_init()。
    │
    │  条件: vcpu_has_run_once(vcpu) == true
    │    → vCPU 之前已经运行过，需要清除旧的 Stage-2 映射
    │  条件: !cpus_have_final_cap(ARM64_HAS_STAGE2_FWB)
    │    → CPU 不支持 FWB，必须手动 unmap（FWB 硬件只需刷 I-cache）
    │
    ▼
stage2_unmap_vm(vcpu->kvm)                    [arch/arm64/kvm/mmu.c:1207]
    │
    │  作用：遍历 VM 的所有 memslot，清除每个 memslot 对应的
    │  Stage-2 映射。这是让 Guest 重新从干净状态按需缺页的前提。
    │  在整棵源码树中只有上述一处调用者。
    │
    │  ┌──────────────────────────────────────────────────────────────┐
    │  │  mmap_read_lock(current->mm)   ← 获取 mmap_lock 读锁       │
    │  │  为什么需要读锁：unmap 需要遍历进程 VMA 来确定每个 memslot   │
    │  │  对应的用户地址范围，这是只读遍历，所以只需读锁。但读锁会在  │
    │  │  整个 unmap 操作期间一直持有。                               │
    │  │                                                              │
    │  │  write_lock(&kvm->mmu_lock)    ← 获取 mmu_lock 写锁          │
    │  │  为什么需要写锁：要修改 Stage-2 页表和 pinned pages 树。      │
    │  └──────────────────────────────────────────────────────────────┘
    │
    ▼
stage2_unmap_memslot(kvm, memslot)            [arch/arm64/kvm/mmu.c]
    │  对单个 memslot 的地址范围执行 unmap。
    │
    ▼
__unmap_stage2_range()                        [arch/arm64/kvm/mmu.c:445]
    │  Stage-2 unmap 的中间层，处理保护 VM 的快速返回。
    │
    │  条件: is_protected_kvm_enabled() == true (pKVM 主机)
    │  条件: kvm->arch.pkvm.enabled == false (非保护 VM)
    │  → 保护 VM 不需要 Host 端 unmap（其页表完全由 EL2 管理），
    │    直接 return。普通 VM 则继续。
    │
    ▼
___unmap_stage2_range()                       [arch/arm64/kvm/mmu.c:437]
    │  Stage-2 unmap 的最底层分发。
    │
    │  条件: is_protected_kvm_enabled() == true
    │  → pKVM 主机上，普通 VM 的 unmap 需要通过 hypercall 让 EL2
    │    解除映射，走 pkvm_unmap_range() 分支。
    │  → 非 pKVM 主机走传统的 kvm_pgtable_stage2_unmap()，
    │    不涉及 pinned pages，不调 account_locked_vm()。
    │
    ▼
pkvm_unmap_range(kvm, start, end)            [arch/arm64/kvm/mmu.c]
    │
    │  作用：在指定 IPA 范围内，遍历 VM 的 pinned pages 树，
    │  对每个 pinned page 调用 pkvm_unmap_guest() 解除映射。
    │  这是 pKVM 特有的逻辑——因为普通 KVM 不跟踪 pinned pages。
    │
    ▼
pkvm_unmap_guest(kvm, ppage)                 [arch/arm64/kvm/mmu.c]
    │
    │  作用：解除单个 pinned page 的映射。具体步骤：
    │  1. 通过 hypercall __pkvm_host_unmap_guest 让 EL2 清除页表项
    │  2. unpin_user_pages() 解除页面的物理锁定
    │  3. 从 pinned_pages 树中移除该节点
    │  4. 归还 RLIMIT_MEMLOCK 配额 ← 这里出问题
    │
    │  旧代码:
    │    write_unlock(&kvm->mmu_lock);               ← 释放 mmu_lock（解决了自旋锁中不可睡眠的问题）
    │    account_locked_vm(mm, ..., false);           ← 归还 RLIMIT_MEMLOCK 配额
    │      └→ mmap_write_lock(mm)                    ← ★ 但这里请求了 mmap_lock 写锁！
    │         当前任务已持有同一 mm 的 mmap_lock 读锁
    │         读写锁不可递归升级，永远阻塞
    │    write_lock(&kvm->mmu_lock);
    │
    ▼
  *** 任务进入 D 状态（不可中断睡眠），永远不返回 ***
```

### 为什么 `stage2_unmap_vm()` 要持有 `mmap_lock` 读锁

这个函数需要遍历 VM 的所有 memslot，并对每个 memslot 调用 `stage2_unmap_memslot()`。后者内部需要查找 VMA（通过 `hva_to_memslot` 等路径），而查找 VMA 要求 `mmap_lock` 至少被读锁保护——否则并发的 `mmap`/`munmap` 可能正在修改 VMA 树，导致遍历结果不一致。读锁保证了在整个 unmap 期间 VMA 布局不变。

### 为什么 pKVM 的 unmap 涉及 `account_locked_vm()`

普通 KVM 的 unmap 只是清除 Stage-2 页表项（`kvm_pgtable_stage2_unmap()`），不涉及锁定/解锁用户页面。但在 pKVM 模式下，映射一个页面时通过 `pin_user_pages()` 锁定了物理页并计入了 `RLIMIT_MEMLOCK`（在 `pkvm_mem_abort()` → `account_locked_vm(mm, ..., true)` 中完成），所以解除映射时必须配对地递减计数（`account_locked_vm(mm, ..., false)`）。正是这个配对计费引入了对 `mmap_lock` 写锁的需求。

---

## 触发条件

第二次对已运行过的 vCPU 发起 `KVM_ARM_VCPU_INIT`，在**不支持 Stage-2 FWB 的 CPU** 上，且 VM **至少有一个 pinned page** 需要解除映射。

`kvm_arch_vcpu_ioctl_vcpu_init()`（`arch/arm64/kvm/arm.c:1592-1596`）是触发入口：

```c
if (vcpu_has_run_once(vcpu)) {
    if (!cpus_have_final_cap(ARM64_HAS_STAGE2_FWB))
        stage2_unmap_vm(vcpu->kvm);
    else
        icache_inval_all_pou();
}
```

`stage2_unmap_vm()` 在整棵源码树中只有这一个调用者。支持 FWB 的硅片走 `icache_inval_all_pou()` 分支，永远不会到达缺陷代码，这很可能是该问题在上游一直未被发现的原因。N90 和 D3000 都缺少 FWB。

还有两个前置条件必须同时满足：

- **主机必须以 `kvm-arm.mode=protected` 启动。** 否则 `___unmap_stage2_range()` 走 `kvm_pgtable_stage2_unmap()` 分支，这是传统 KVM 的 unmap，只清除页表项，根本不经过 `pkvm_unmap_range()`，因此不会接触 `account_locked_vm()`。
- **Guest 必须不是保护 VM。** `__unmap_stage2_range()` 在 `is_protected_kvm_enabled() && kvm->arch.pkvm.enabled` 时立即返回——保护 VM 的 Stage-2 页表完全由 EL2 管理，Host 不参与 unmap。而 `pkvm.enabled` 仅在创建 VM 时指定 `KVM_VM_TYPE_ARM_PROTECTED` 才会设置（`pkvm.c:462-469`）。

因此唯一的受影响组合是 **pKVM 主机上的普通 `KVM_CREATE_VM` guest**——正是复现程序创建的那种。这种 VM 仍然拥有 pinned pages：在保护主机上，**每个** VM 的缺页都经过 `pkvm_mem_abort()`（而非 `user_mem_abort()`），在 `mmu.c:1844` 对 `RLIMIT_MEMLOCK` 计费并在 `mmu.c` 中插入 ppage，无论该 VM 本身是否为保护 VM。

至少需要一个 pinned page，但原因并非 `account_locked_vm()` 的早期返回——其参数 `1 << ppage->order` 永远非零。真正的原因是 `for_ppage_node_in_range` 在没有 pinned page 时迭代零次，`account_locked_vm()` 根本不会被调用。一个从未运行过（因而不曾缺页映射任何内存）的 VM 是安全的。

**总结——四个条件缺一不可：**

| # | 条件 | 含义 | 不满足时的后果 |
|---|------|------|----------------|
| 1 | 主机以 `kvm-arm.mode=protected` 启动 | pKVM 模式使 unmap 走 `pkvm_unmap_range()` | `___unmap_stage2_range()` 走普通 `kvm_pgtable_stage2_unmap()`，不调 `account_locked_vm()` |
| 2 | VM 为普通 VM（非 `KVM_VM_TYPE_ARM_PROTECTED`） | 普通 VM 的 unmap 不会被跳过 | 保护 VM 在 `__unmap_stage2_range()` 中直接 return |
| 3 | CPU 不支持 `ARM64_HAS_STAGE2_FWB` | 缺少 FWB 就必须手动 unmap | FWB 硅片走 `icache_inval_all_pou()`，不调 `stage2_unmap_vm()` |
| 4 | VM 至少有一个 pinned page | unmap 循环才会执行循环体 | 循环体不执行，`account_locked_vm()` 不被调用 |

---

## 复现

`repro/repro-deadlock.c`——编译与运行：

```
aarch64-linux-gnu-gcc -O2 -static -o repro-deadlock repro/repro-deadlock.c
```

复现程序做的事很简单：创建一个普通 KVM VM，创建一个 vCPU，先运行一次（让 vCPU 进入 `has_run_once` 状态，同时 Guest 的缺页会创建 pinned page），然后再次对该 vCPU 调用 `KVM_ARM_VCPU_INIT`——这就走入了 `stage2_unmap_vm()` → `pkvm_unmap_guest()` → `account_locked_vm()` 的死锁路径。

确定性复现：在 klinux `6.6.103+ #3` 上首次运行即卡死，无需对为 `common` 6.6.30 世系编写的版本做任何适配（`evidence/2026-08-05-repro-stdout.txt`）。打了补丁的内核返回 0。

> **此程序会卡死运行它的板子。** 可以通过 ssh 恢复而无需物理访问，但只能通过 sysrq——参见[运行记录](../runs/2026-08-05-deadlock-repro.md#recovery)。不要在你无法重启的板子上运行。

---

## 爆炸半径

一个卡住的任务就能拖垮整台机器，这是实测结果而非假设。

要理解为什么，需要知道 Linux rwsem 的公平性语义：当有写者在等待队列中时，后续新来的读者不能"插队"——即使当前读锁已经被持有，新读者也必须排在写者后面。这是因为如果允许读者持续插队，写者可能被饿死（starvation）。

在我们的场景中，死锁任务在 `mmap_lock` 的等待队列中占了一个写者位，而这个写者永远无法被授予（因为它自己持有读锁且不能升级）。此后，所有对同一 `mm` 的 `mmap_read_lock()` 调用都要排在它后面，也就跟着永远阻塞。

从正常的 shell 读取被毒化的 PID 的不同文件：

| 读取 | 是否需要 `mmap_lock` | 结果 |
|------|---------------------|------|
| `/proc/79770/stat` | 否 | 正常 |
| `/proc/79770/cmdline` | 读锁 | 永久阻塞 |
| `/proc/79770/maps` | 读锁 | 永久阻塞 |

`sysrq-w` 显示了附带损害，全部卡在 `__access_remote_vm` → `down_read_killable`：

```
task:OptiDaemon     state:D  pid:1965      <- Kylin 系统自带服务，与模糊测试无关
task:repro-deadlock state:D  pid:79770     <- 最初的受害者
task:ps             state:D  pid:82448
task:pgrep          state:D  pid:83814
```

`OptiDaemon` 按自己的时间表轮询 `/proc`，无需我们帮忙就卡死了。因此板子会随时间自然衰亡——一个接一个的守护进程扫过 `/proc` 就会被拖下水。未打补丁的机器不需要密集的模糊测试就会死，一条 `ps` 命令就够了。

两个操作后果随之而来：

1. **syz-manager 无法恢复目标。** 阻塞的任务处于 `D` 状态且不可杀死（`SIGKILL` 对 `D` 状态无效），syz-manager 在重新部署时失败，报 `scp: /root/syzkaller-fuzz/syz-executor: Text file busy`，测试活动无限期卡死而非重启。
2. **内存泄漏。** 每个卡住的任务永远持有 `kvm->srcu` 的读侧，这阻止了它所固定的一切资源被回收——这是之前在 D3000 上观测到的未解释内存耗尽的首要候选机制，尽管该关联尚未证实。

---

## 修复方案

**已修复并验证**——`348c94763cc6` 在 klinux 分支 `pkvm-unmap-deadlock-fix`，*"KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock"*，`+52/-7` 涉及 `mmu.c` 和 `kvm_host.h`。在 N90 上由 [2026-08-05-deadlock-fix-verify](../runs/2026-08-05-deadlock-fix-verify.md) 验证：同一个确定性卡死板子的二进制文件现在 10 次全部通过，内核与失败版本仅差此补丁。checkpatch：0 错误。

### 设计原理

修复的核心原理是：**不在 `mmap_lock` 持有期间调用 `account_locked_vm()`，而是将计数延迟到所有相关锁释放之后结算。**

这和 ACK 6.12 自然消解此缺陷的方式是同一原理——6.12 把计费从 unmap 路径中移除，放到回收路径上（详见下文"ACK 6.12 展示了什么"），而我们的修复在 6.6 形态的树上用最小改动实现了同样的"计费与 `mmap_lock` 持有区域分离"。

### 具体实现

**1. 新增 per-VM 原子计数器**

在 `struct kvm_protected_vm`（`arch/arm64/include/asm/kvm_host.h:267`）中新增：

```c
atomic_long_t pending_unaccount;
```

这个计数器暂存 unmap 路径中应归还但无法立即归还的页面数。因为 unmap 可能在 `mmu_lock` 保护下并发执行，原子操作保证了计数安全。

**2. `pkvm_unmap_guest()` / `pkvm_unmap_range()` 不再内联计费，改为累加计数**

klinux（interval tree，per-ppage 计数）修复后：

```c
static int pkvm_unmap_guest(struct kvm *kvm, struct kvm_pinned_page *ppage)
{
    ...
    unpin_user_pages_dirty_lock(&ppage->page, 1, ppage->dirty);
    kvm_pinned_pages_remove(ppage, &kvm->arch.pkvm.pinned_pages);

    /*
     * 不再调用 account_locked_vm()，也不再释放 mmu_lock。
     * 原因：account_locked_vm() 内部调 mmap_write_lock()，
     * 而 stage2_unmap_vm() 已持有 mmap_read_lock()，写锁请求会死锁。
     * 将计数暂存，等 mmap_lock 释放后再结算。
     */
    atomic_long_add(1 << ppage->order, &kvm->arch.pkvm.pending_unaccount);

    kfree(ppage);
    return 0;
}
```

`2030/bug930` 世系（maple tree，批量 `cnt` 计数）修复后：

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
{
    unsigned long cnt = 0;
    ...
    mt_for_each(&kvm->arch.pkvm.pinned_pages, entry, index, end - 1) {
        ...
        ret = pkvm_unmap_guest(kvm, ppage);
        if (ret)
            break;
        cnt += nr_pages;
    }

    /* 同上：不在持有 mmap_lock 时结算 */
    atomic_long_add(cnt, &kvm->arch.pkvm.pending_unaccount);

    return ret;
}
```

**3. `pkvm_flush_unaccount()` 在锁释放后结算**

```c
static void pkvm_flush_unaccount(struct kvm *kvm)
{
    long n = atomic_long_xchg(&kvm->arch.pkvm.pending_unaccount, 0);

    if (n > 0)
        account_locked_vm(kvm->mm, n, false);
}
```

`atomic_long_xchg` 原子地取出整个待结算计数并将其归零，所以：
- 可以无条件调用——没有待结算计数时什么都不做。
- 可以从多条路径调用——并发的 unmap 不可能重复计费，`xchg` 保证了每次结算都独占一个计数快照。
- 只有减量被延迟——`pkvm_mem_abort()` 中的增量仍在 `mmap_lock` 之外内联计费——所以页面总是先被计费后才被减计费，`locked_vm` 总数不会变负。

**4. 三个调用点在各自锁释放后调用 `pkvm_flush_unaccount()`**

以 `stage2_unmap_vm()` 为例：

```c
void stage2_unmap_vm(struct kvm *kvm)
{
    idx = srcu_read_lock(&kvm->srcu);
    mmap_read_lock(current->mm);          /* 获取读锁 */
    write_lock(&kvm->mmu_lock);

    ...  /* unmap 操作，只累加 pending_unaccount，不再调 account_locked_vm() */

    write_unlock(&kvm->mmu_lock);
    mmap_read_unlock(current->mm);        /* 释放读锁 */
    srcu_read_unlock(&kvm->srcu, idx);

    /* 安全：mmap_lock 和 mmu_lock 均已释放，可以调 account_locked_vm() */
    pkvm_flush_unaccount(kvm);
}
```

另外两个调用点（`kvm_uninit_stage2_mmu`——VM 拆解时、`kvm_arch_flush_shadow_memslot`——memslot 变更时）也在各自的 `mmu_lock` 和 `mmap_lock` 释放后调用。

### 额外收益：堵住 use-after-free

移除睡眠调用同时移除了释放 `mmu_lock` 的理由，这堵住了同一循环中的第二个漏洞。

`for_ppage_node_in_range` 宏在循环体内缓存了后继节点（`__tmp = kvm_pinned_pages_iter_next(...)`），以便在循环迭代时安全地删除当前节点。旧代码在循环体内释放 `mmu_lock`，这打开了一个窗口：并发的 `MEM_RELINQUISH` 操作（由 Guest 通过 SMCCC hypercall 发起）可以在锁释放期间 `kfree` 这个缓存的后继节点。循环回到迭代条件时访问已释放的节点——use-after-free。

修复后不再释放 `mmu_lock`，unmap 在锁保护下原子完成，消除了这个窗口。一个只延迟计费但保留 `mmu_lock` 释放的移植版本会修复死锁但留下 use-after-free——所以这个设计选择不是任意的。

---

## 上游无修复

在编写本补丁之前搜索了 Android-Common 和 mainline。两个看起来相关的提交实际上不是：

- `78adeb53eea1` *"Fix account_locked_mm() call in non-preemptible section"* (2024-05)——将计费批量化到 `pkvm_unmap_range()` 内部的 `write_unlock`/`write_lock` 之间。这个补丁解决的是"在 `mmu_lock` 自旋锁持有期间调用可睡眠函数"的问题，仅绕开了 `mmu_lock`，完全没有意识到更外层的 `mmap_lock` 读锁冲突。
- `246414094770` *"Don't do account_locked_vm() while atomic"* (2024-11)——逐页 `write_unlock`/`account`/`write_lock`。同样只绕开了 `mmu_lock`。**本仓库已包含其等价物**（适配大页），正是发现死锁的代码——说明这个补丁的作者也只看到了 `mmu_lock` 问题。

两者均未触及 `mmap_lock`。ACK `android15-6.6` 在 HEAD（2026-08-04）仍搭载此 bug。Mainline 仅因 v6.14 将 guest stage-2 移入 EL2 的重构（`fce886a60207` 及其系列）而免受影响——该重构将 Stage-2 页表管理从 Host 内核完全移到了 EL2，Host 端不再有 `account_locked_vm()` 调用，但这是全新基础设施，不可回移到 6.6。另经检查确认无关的提交：`ed14b491ec76`/`9a13ca20af8d`（THP 计费算术）、`3ae13572d106`（THP 与 balloon 的回收）、`04512258010d`（回收计费改用 `kvm->mm` 而非 `current->mm`——另一个缺陷，不在本 issue 范围内）。

*搜索注意事项：* `lore.kernel.org` 的验证码阻止了自动化查询，所以可能存在未合并的邮件列表帖子未被看到。Mainline 和 ACK 的合并状态已从源码验证。

---

## 血缘图：哪些树携带此缺陷，为什么

此缺陷属于 **ACK pKVM guest-stage-2 系列**，而非任何单一产品树。导入该系列的一切都继承了它；未导入的则自然免疫。以下逐行列出使得"上游无修复"成为有界断言而非乐观期望的依据。

Android-Common 的 LTS 家族为 **6.6 (android15) → 6.12 (android16) → 6.18 (android17)**。**没有 ACK 6.14**——6.14 不是 LTS，ACK 跳过了它。专用的 `pkvm-experimental` 分支只到 6.6（`android14-5.15-pkvm-experimental`、`android14-6.1-pkvm-experimental`、`android15-6.6-pkvm_experimental`）；从 6.12 起不再有，因为该工作已合入主线 ACK 分支。

代码通过以下系列引入，从旧到新：

```
af919d2384e8  Handle guest stage-2 page-tables entirely at EL2   引入 pinned pages + account_locked_vm
8061523a64f0  Implement MEM_RELINQUISH SMCCC hypercall
b66e27a61e1f  Unshare pages from __unmap_stage2_range()          引入 pkvm_unmap_range()
78adeb53eea1  Fix account_locked_mm() call in non-preemptible section
5d9808b9071c  THP support for pKVM guests
e56d181356a4  Convert kvm_pinned_pages to an interval-tree       ← klinux 有此转换，2030/bug930 仍用枫树
```

这是 `aosp/android15-6.6` 上 `arch/arm64/kvm/` 下涉及 `account_locked_vm` 的完整历史——六个提交，无一解决 `mmap_lock`。

两个下游树以不同路径继承：

- **`android/common` `2030/bug930`**（参考检出 `~/common`）**直接**继承自 `android15-6.6-pkvm_experimental`——`git merge-base --is-ancestor` 确认，合并基点*就是*该分支的 HEAD `e70cae0cbb35`。它停在 `e56d181356a4` 之前，所以仍然通过枫树（maple tree）批量化单个 `cnt`，计费代码在 `pkvm_unmap_range()` 中。
- **klinux `klad-v11-next`** 将同一 ANDROID 系列导入 V11 6.6.103 内核——272 个 `ANDROID: KVM: arm64` 提交——*包括*区间树转换（klinux 以独立世系重新应用了该转换，不共享 `e56d181356a4` 对象，但从源码可确认代码等价）。因此 `pkvm_unmap_guest()` 中是 per-ppage 计费。两个树不共享对象存储，它们的此代码副本差一个 ACK 提交的等价改动，这就是一个补丁无法同时服务两者的全部原因。

### `android15-6.6` 与 `android15-6.6-pkvm_experimental` 是分叉的分支

一个自然的疑问是：`2030/bug930` 继承自 `android15-6.6-pkvm_experimental`，而后者是 ACK 的分支，为什么它的代码会和 ACK `android15-6.6` 不一样？答案是这两个 ACK 分支在 `e70cae0cbb35` 之后就**分叉**了，不再同步。

- **`android15-6.6`** 是 ACK 的主分支，持续接收上游的修复和变更。
- **`android15-6.6-pkvm_experimental`** 是 pKVM guest-stage-2 的实验性分支，停在 `e70cae0cbb35` 不再与主线合并。

结果是 ACK 主线上后续的修复进入了 `android15-6.6` 但**没有**进入 `pkvm_experimental`。以 THP 计费修复为例：同一个 Change-Id (`I20498c42fcba6efa2c80cb63057bbada6ad8fa87`) 在两个分支上有不同的 cherrypick——`ed14b491ec76` 在 `aosp/android15-6.6` 上，`9a13ca20af8d` 在 `review/8707` 上。两个 cherrypick 的改动内容相同，但 git 对象不同（上下文行和 hunk 偏移不同），且后者是基于 `pkvm_experimental` 的代码树重新应用的。

### 三个状态，而非两个

`2030/bug930` 分支本身**没有** cherry-pick 任何 ACK 主线修复，也**没有**合入任何本地 Kylin 补丁——它就是 `pkvm_experimental` 的原样延续。验证方式（参考检出 `~/common`，`da966ce9a047`）：

```
git show 2030/bug930:arch/arm64/kvm/mmu.c   # 第 337-339 行仍是未修复的 account_locked_vm(mm, cnt, false)
```

因此在本 issue 的范围内只有两个状态：

| 树 | 基点 | 状态 |
|------|------|------|
| `2030/bug930`（`~/common`） | `pkvm_experimental` 的完整继承 | 未修复——仍然包含原始死锁代码 |
| klinux `klad-v11-next` | 独立导入 ACK 系列 | 已修复——`348c94763cc6` |

> **树的取用范围。** 按 [`issues/README.md`](../README.md) 的参考树规则，比较只针对**未经本地修改**的树：`~/common`（未改动的 `2030/bug930`）、`~/kernel-refs/ack`（纯净 ACK 6.6 / 6.12 / 6.18）、`~/kernel-refs/linux`（纯净 mainline），以及我们的工作树 klinux `klad-v11-next`。带本地补丁的检出一律不作为比较依据。
>
> 这条规则的由来：某个检出的**工作树**里有未提交的修复，而它的**分支**没有——直接读磁盘上的 `mmu.c` 会看到 `atomic_long_add(cnt, ...)`，据此得出的"该分支已修复"结论是错的。确认状态必须用 `git show <ref>:<path>`，不能读工作树。

各树状态（均从源码读取而非推断）：

| 树 | unmap 路径中的计费方式 | 受影响 |
|---|------|------|
| mainline Linux `v6.19.14` | 无——该 tag 下 `arch/arm64` 没有任何 `account_locked_vm` | 否 |
| mainline Linux `torvalds/master` @ `c21bb4193` | `account_locked_vm` 已回归（`mmu.c:1724`、`:1773`；`pkvm.c:355`），但均位于 fault 和 reclaim 路径，不在 unmap 路径中；`stage2_unmap_vm()` 仍持 `mmap_read_lock` 但不再嵌套写锁请求 | 否 |
| ACK `android-mainline` | 无 | 否 |
| ACK `android15-6.6` @ `742616e`（`aosp/android15-6.6`） | `___unmap_stage2_range` → `pkvm_unmap_range` → `account_locked_vm(mm, cnt, false)` | **是，至今仍有** |
| ACK `android15-6.6-pkvm_experimental` | 同上 | **是** |
| `2030/bug930` @ `da966ce9a047`（`~/common`） | 同形（批量化 `cnt`，枫树），`mmu.c:337-339` | **是**——分支本身未修复 |
| klinux `klad-v11-next` | per-ppage，区间树 | **是**——由 `348c94763cc6` 修复 |
| ACK `android16-6.12` | **无——`pkvm_unmap_range`/`pkvm_unmap_guest` 不存在** | 否 |
| ACK `android17-6.18` | 无 | 否 |

---

## ACK 6.12 展示了什么，以及为什么它在这里重要

6.12 是有趣的一个，因为它没有修复此缺陷——它**消解**了此缺陷，而其消解方式独立地确认了本修复选择的设计。

6.12 中的 `stage2_unmap_vm()` 仍然在整个 unmap 期间持有 `mmap_read_lock(current->mm)`，和 6.6 完全一样。锁没变。变化的是计费**离开了 unmap 路径**：`___unmap_stage2_range()` 现在只是 `KVM_PGT_FN(kvm_pgtable_stage2_unmap)(...)`——纯粹的页表清除，没有任何 `account_locked_vm()` 调用。整棵树中仅有的两个减计费点都在 `pkvm.c` 中，位于**回收**路径上，且都在 `mmu_lock` 和 `mmap_lock` 之外——

- `pkvm.c:417`，VM 拆解循环中遍历 `__pkvm_reclaim_dying_guest_page`，循环后批量计费；
- `pkvm.c:762`，`pkvm_host_reclaim_page()`，在 `write_unlock(&host_kvm->mmu_lock)` 之后。

所以 ACK 生产线收敛到了与本修复相同的原理——**减计费不能在持有 `mmap_lock` 的 unmap 中执行**——并通过结构重组跨主版本达到了这一点，作为 `__pkvm_pages_to_ppages` / `__pkvm_host_donate_guest` 重构的一部分。通过 per-VM 原子变量延迟结算是该原理在 6.6 形态的树上的最小表达，也是它成为正确携带形态而非本地发明的原因。

*本对比的注意事项：* 6.12 和 6.18 是通过 gitiles 从分支 tip 读取的，不是从已获取的历史。代码的当前缺席已验证；移除它的提交未被识别，因此未引用提交 id。

同一阅读中的两个附注。`892713e97ca1` *"Sidestep stage2_unmap_vm() on vcpu reset when S2FWB is supported"* (2020) 解释了为什么上游没有人踩到这个：FWB 硅片从不进入该路径。6.12 的 `pkvm_host_reclaim_page()` 对 `host_kvm->mm` 计费，而 klinux 仍使用 `current->mm`（`pkvm.c:311`、`:534`）——这是另一个缺陷，对应上文的 `04512258010d`，单独跟踪，不在本 issue 范围内。

---

## 为什么不能从上游打补丁

一个自然的问题是：既然这是 ACK 引入的缺陷，为什么不直接从上游 ACK 或者 Linux mainline cherry-pick 一个修复？答案是：**上游不存在可以 cherry-pick 的修复**，而且每一个"看起来可以"的来源都因为结构性原因无法应用。以下逐条分析。

### ACK `android15-6.6`：有 bug，无修复

`android15-6.6` 是受影响的 ACK 分支，它有这个 bug，但它**没有修复这个 bug**。ACK 上仅有的两个相关提交（`78adeb53eea1` 和 `246414094770`）解决的是不同的问题——"在 `mmu_lock` 自旋锁持有期间调用可睡眠函数"，而不是"在 `mmap_lock` 读锁持有期间请求写锁"。本仓库已经包含了它们的等价物，正是在此基础上发现了 `mmap_lock` 死锁。所以没有什么可以从 ACK cherry-pick 过来。

### ACK `android16-6.12` / `android17-6.18`：代码已经不存在了

6.12 对 pKVM 的 guest stage-2 做了结构性重构（`__pkvm_pages_to_ppages` / `__pkvm_host_donate_guest` 系列），计费从 unmap 路径中完全移除了。这不是一个"修复补丁"——这是一个架构变更，涉及数十个提交，改变了 `pkvm_unmap_range()` / `pkvm_unmap_guest()` / `pkvm_host_reclaim_page()` 等函数的存在形式、调用关系和数据结构。

cherry-pick 这个重构意味着将 6.12 的 pKVM guest-stage-2 子系统整体移植到 6.6，而非挑选单个补丁。这是一次重大移植工程，而非 `git cherry-pick`，其风险和工作量远大于在 6.6 形态上做最小化修复。

### Linux mainline：代码形态完全不同

Mainline 的免疫来自 v6.14 的 `fce886a60207` 系列，将 guest stage-2 页表管理从 Host 内核完全移到了 EL2。此后的 mainline 代码中，Host 端不再有 `pkvm_unmap_range()` / `pkvm_unmap_guest()` / `account_locked_vm()` 在 unmap 路径中的任何调用——不是修复了死锁，而是整个代码路径在 Host 端消失了。

即使在 `torvalds/master`（`c21bb4193`）上 `account_locked_vm` 已回归到 `arch/arm64/kvm/`（`mmu.c:1724`、`:1773`；`pkvm.c:355`），这些调用也全部位于 fault 和 reclaim 路径，不在 unmap 路径中。`stage2_unmap_vm()` 仍然持有 `mmap_read_lock`，但它不再嵌套任何写锁请求。

cherry-pick `fce886a60207` 系列同样意味着将 EL2 stage-2 管理这一全新基础设施移植到 6.6——这不是一个回移目标，而是一次内核版本升级。

### `2030/bug930` 世系 ↔ klinux：同一设计，不同实现

两个下游树各自写了独立的修复（`d7c317637aa2` 和 `348c94763cc6`），设计相同但 hunk 不兼容，原因有三层：

1. **缺陷代码在不同的函数中**。klinux 的计费在 `pkvm_unmap_guest()` 内（per-ppage，区间树），`2030/bug930` 世系的计费在 `pkvm_unmap_range()` 内（批量 `cnt`，枫树）。修复一个重写了前者，修复另一个重写了后者——没有匹配的上下文行，`git cherry-pick` 必然冲突。

2. **数据结构不同**。klinux 用区间树遍历（`for_ppage_node_in_range`），`2030/bug930` 世系用枫树遍历（`mt_for_each`）。循环体、迭代宏、节点访问方式完全不同。

3. **`struct kvm_arch` 布局偏移**。修复向 `struct kvm_protected_vm` 添加了 `pending_unaccount` 字段，该结构是 `struct kvm_arch` 的成员，而 `kvm_arch` 在 EL2 通过共享内存被 Rust nVHE 绑定按偏移量读取。两个世系的 `kvm_arch` 布局不同（klinux 多了区间树字段等），所以字段偏移不同。即使 hunk 碰巧能应用，对象级兼容也不成立——必须完整重建包括 EL2 hypervisor。

### 为什么只能本地写

综合以上四条，每一条上游来源都因为结构性原因不可 cherry-pick：

| 来源 | 为什么不能 cherry-pick |
|------|----------------------|
| ACK `android15-6.6` | 没有修复可取——只有绕开 `mmu_lock` 的补丁，本仓库已有其等价物 |
| ACK `android16-6.12` | 计费的消失是架构重构的结果，不是独立补丁，cherry-pick 等于移植整个子系统 |
| Linux mainline | 免疫来自 EL2 stage-2 管理这一全新基础设施，不可回移 |
| `2030/bug930` 世系 ↔ klinux | 函数、数据结构、结构体布局均不同，hunk 不兼容且对象级不兼容 |

唯一可传递的是**设计**——"计费与 `mmap_lock` 持有区域分离"这一原则，以及在 6.6 形态上的最小化表达：per-VM 原子计数器 + 延迟结算。两个修复刻意保持了字段名（`pending_unaccount`）、函数名（`pkvm_flush_unaccount`）和注释措辞的一致，使得未来整合时可以识别为同一修复。

---

## 残余风险

**禁用 `ioctl$KVM_ARM_VCPU_INIT` 不能关闭此问题。** 2026-08-05 的普查在移除该 syscall 后运行了 10,356 次执行且未命中死锁，但 `syz_kvm_setup_cpu$arm64` 在 C 辅助函数内部（`executor/common_kvm_arm64.h:225` → `:196`，声明于 `sys/linux/dev_kvm_arm64.txt:156`）对调用者提供的 `fd_kvmcpu` 发起 `KVM_ARM_VCPU_INIT`。所以序列 `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` 能到达相同状态且可被生成。

将此缓解措施视为降低概率而非保护。任何针对未打补丁内核的测试活动仍可能丢失板子。
