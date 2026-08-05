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

## 机制：为什么自死锁

**置信度：已确认。** 锁的同一性不是从源码推断的——`wchan` 直接命名 `account_locked_vm` 为阻塞函数，其上方一帧即为 `stage2_unmap_vm`，即持有冲突锁的函数。

### 死锁的两个角色

| 层级 | 函数 | 对 `mmap_lock` 的操作 | 锁的对象 |
|------|------|----------------------|----------|
| 上层调用者 | `stage2_unmap_vm()` | `mmap_read_lock(current->mm)` — 获取读锁 | `current->mm` |
| 下层被调者 | `pkvm_unmap_guest()` → `account_locked_vm()` | `mmap_write_lock(mm)` — 请求写锁 | `kvm->mm` |

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
/* ksrc-pkvmfix / common-stage2mvp 修复前 (d7c317637aa2 的父提交) — pkvm_unmap_range() */
write_unlock(&kvm->mmu_lock);
account_locked_vm(mm, cnt, false);                  /* 内部调 mmap_write_lock(mm) */
write_lock(&kvm->mmu_lock);
```

旧代码的注释解释了为什么释放 `mmu_lock`（"account_locked_vm may sleep"），但**完全没有提及 `mmap_lock`**。释放一个不冲突的锁对解决另一个锁的冲突毫无帮助。

### `account_locked_vm()` 的写锁获取

`account_locked_vm()`（`mm/util.c:547-560`）的入口处无条件请求写锁：

```c
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc)
{
    if (pages == 0 || !mm)
        return 0;

    mmap_write_lock(mm);    /* ← 死锁发生在这里 */
    ret = __account_locked_vm(mm, pages, inc, current, capable(CAP_IPC_LOCK));
    mmap_write_unlock(mm);

    return ret;
}
```

其底层实现 `__account_locked_vm()` 甚至有一个断言 `mmap_assert_write_locked(mm)`，确认调用时必须持有写锁。

---

## 完整调用链：从用户态 ioctl 到死锁

以下是自上而下的完整调用链，每一步都标注了源码位置和锁状态：

```
用户态: ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, ...)
    │
    ▼
kvm_vcpu_ioctl()                              [virt/kvm/kvm_main.c]
    │
    ▼
kvm_arch_vcpu_ioctl()                         [arch/arm64/kvm/arm.c]
    │
    │  条件: vcpu_has_run_once(vcpu) == true
    │  条件: !cpus_have_final_cap(ARM64_HAS_STAGE2_FWB)
    │
    ▼
stage2_unmap_vm(vcpu->kvm)                    [arch/arm64/kvm/mmu.c:1207]
    │
    │  ┌─────────────────────────────────────────────────────────┐
    │  │  mmap_read_lock(current->mm)   ← 获取 mmap_lock 读锁  │
    │  │  write_lock(&kvm->mmu_lock)    ← 获取 mmu_lock 写锁    │
    │  └─────────────────────────────────────────────────────────┘
    │
    ▼
stage2_unmap_memslot(kvm, memslot)            [arch/arm64/kvm/mmu.c]
    │
    ▼
__unmap_stage2_range()                        [arch/arm64/kvm/mmu.c:445]
    │
    │  条件: is_protected_kvm_enabled() == true (pKVM 主机)
    │  条件: kvm->arch.pkvm.enabled == false (非保护 VM)
    │  → 不提前返回，继续执行
    │
    ▼
___unmap_stage2_range()                       [arch/arm64/kvm/mmu.c:437]
    │
    │  条件: is_protected_kvm_enabled() == true
    │  → 走 pkvm_unmap_range() 分支
    │
    ▼
pkvm_unmap_range(kvm, start, end)            [arch/arm64/kvm/mmu.c]
    │
    │  遍历 pinned pages 区间树/枫树
    │  对每个 pinned page:
    │
    ▼
pkvm_unmap_guest(kvm, ppage)                 [arch/arm64/kvm/mmu.c]
    │
    │  旧代码:
    │    write_unlock(&kvm->mmu_lock);               ← 释放 mmu_lock（无关）
    │    account_locked_vm(mm, ..., false);
    │      └→ mmap_write_lock(mm)                    ← ★ 死锁！
    │         当前任务已持有同一 mm 的 mmap_lock 读锁
    │         读写锁不可递归升级，永远阻塞
    │
    ▼
  *** 任务进入 D 状态，永远不返回 ***
```

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

- **主机必须以 `kvm-arm.mode=protected` 启动。** 否则 `___unmap_stage2_range()` 走 `kvm_pgtable_stage2_unmap()` 分支，根本不经过 `pkvm_unmap_range()`。
- **Guest 必须不是保护 VM。** `__unmap_stage2_range()` 在 `is_protected_kvm_enabled() && kvm->arch.pkvm.enabled` 时立即返回，而 `pkvm.enabled` 仅在 `KVM_VM_TYPE_ARM_PROTECTED` 时设置。

因此唯一的受影响组合是 **pKVM 主机上的普通 `KVM_CREATE_VM` guest**——正是复现程序创建的那种。这种 VM 仍然拥有 pinned pages：在保护主机上，每个 VM 的缺页都经过 `pkvm_mem_abort()`，在 `mmu.c:1839` 计费并插入 ppage。

至少需要一个 pinned page，但原因并非 `account_locked_vm()` 的早期返回——其参数 `1 << ppage->order` 永远非零。真正的原因是 `for_ppage_node_in_range` 在没有 pinned page 时迭代零次，调用根本不会发生。

**总结——四个条件缺一不可：**

| # | 条件 | 不满足时的后果 |
|---|------|----------------|
| 1 | 主机以 `kvm-arm.mode=protected` 启动 | `___unmap_stage2_range()` 走普通 `kvm_pgtable_stage2_unmap()`，不调 `account_locked_vm()` |
| 2 | VM 为普通 VM（非 `KVM_VM_TYPE_ARM_PROTECTED`） | 保护 VM 在 `__unmap_stage2_range()` 中直接 return |
| 3 | CPU 不支持 `ARM64_HAS_STAGE2_FWB` | FWB 硅片走 `icache_inval_all_pou()`，不调 `stage2_unmap_vm()` |
| 4 | VM 至少有一个 pinned page | 循环体不执行，`account_locked_vm()` 不被调用 |

---

## 复现

`repro/repro-deadlock.c`——编译与运行：

```
aarch64-linux-gnu-gcc -O2 -static -o repro-deadlock repro/repro-deadlock.c
```

确定性复现：在 klinux `6.6.103+ #3` 上首次运行即卡死，无需对为 `common` 6.6.30 世系编写的版本做任何适配（`evidence/2026-08-05-repro-stdout.txt`）。打了补丁的内核返回 0。

> **此程序会卡死运行它的板子。** 可以通过 ssh 恢复而无需物理访问，但只能通过 sysrq——参见[运行记录](../runs/2026-08-05-deadlock-repro.md#recovery)。不要在你无法重启的板子上运行。

---

## 爆炸半径

一个卡住的任务就能拖垮整台机器，这是实测结果而非假设。从正常的 shell 读取被毒化的 PID 的不同文件：

| 读取 | 是否需要 `mmap_lock` | 结果 |
|------|---------------------|------|
| `/proc/79770/stat` | 否 | 正常 |
| `/proc/79770/cmdline` | 读锁 | 永久阻塞 |
| `/proc/79770/maps` | 读锁 | 永久阻塞 |

阻塞的写者坐在 rwsem 等待队列中，而 Linux rwsem 是公平的，所以该 `mm` 的每一个后续**读者**都要排在一个永远无法被授予的写者后面。`sysrq-w` 显示了附带损害，全部卡在 `__access_remote_vm` → `down_read_killable`：

```
task:OptiDaemon     state:D  pid:1965      <- Kylin 系统自带服务，与模糊测试无关
task:repro-deadlock state:D  pid:79770     <- 最初的受害者
task:ps             state:D  pid:82448
task:pgrep          state:D  pid:83814
```

`OptiDaemon` 按自己的时间表轮询 `/proc`，无需我们帮忙就卡死了。因此板子会随时间自然衰亡——一个接一个的守护进程扫过 `/proc` 就会被拖下水。未打补丁的机器不需要密集的模糊测试就会死，一条 `ps` 命令就够了。

两个操作后果随之而来。阻塞的任务处于 `D` 状态且不可杀死，所以 syz-manager 无法恢复目标：它在重新部署时失败，报 `scp: /root/syzkaller-fuzz/syz-executor: Text file busy`，测试活动无限期卡死而非重启。并且每个卡住的任务永远持有 `kvm->srcu` 的读侧，这阻止了它所固定的一切资源被回收——这是之前在 D3000 上观测到的未解释内存耗尽的首要候选机制，尽管该关联尚未证实。

---

## 修复方案

**已修复并验证**——`348c94763cc6` 在 klinux 分支 `pkvm-unmap-deadlock-fix`，*"KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock"*，`+52/-7` 涉及 `mmu.c` 和 `kvm_host.h`。在 N90 上由 [2026-08-05-deadlock-fix-verify](../runs/2026-08-05-deadlock-fix-verify.md) 验证：同一个确定性卡死板子的二进制文件现在 10 次全部通过，内核与失败版本仅差此补丁。checkpatch：0 错误。

### 设计原理

修复的核心原理是：**不在 `mmap_lock` 持有期间调用 `account_locked_vm()`，而是将计数延迟到所有相关锁释放之后结算。**

### 具体实现

**1. 新增 per-VM 原子计数器**

在 `struct kvm_protected_vm`（`arch/arm64/include/asm/kvm_host.h:267`）中新增：

```c
atomic_long_t pending_unaccount;
```

**2. `pkvm_unmap_guest()` / `pkvm_unmap_range()` 不再内联计费，改为累加计数**

klinux（interval tree，per-ppage 计数）修复后：

```c
static int pkvm_unmap_guest(struct kvm *kvm, struct kvm_pinned_page *ppage)
{
    ...
    unpin_user_pages_dirty_lock(&ppage->page, 1, ppage->dirty);
    kvm_pinned_pages_remove(ppage, &kvm->arch.pkvm.pinned_pages);

    /* 不再调用 account_locked_vm()，也不再释放 mmu_lock */
    atomic_long_add(1 << ppage->order, &kvm->arch.pkvm.pending_unaccount);

    kfree(ppage);
    return 0;
}
```

ksrc-pkvmfix / common-stage2mvp（maple tree，批量 `cnt` 计数）修复后：

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

    /* 不再调用 account_locked_vm()，也不再释放 mmu_lock */
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

`xchg` 原子地取出整个待结算计数，所以可以无条件调用、从多条路径调用；并发的 unmap 不可能重复计费，没有待结算计数的调用者什么都不做。

只有减量被延迟——`pkvm_mem_abort()` 中的增量仍在 `mmap_lock` 之外内联计费——所以页面总是先被计费后才被减计费，总数不会变负。

**4. 三个调用点在各自锁释放后调用 `pkvm_flush_unaccount()`**

以 `stage2_unmap_vm()` 为例：

```c
void stage2_unmap_vm(struct kvm *kvm)
{
    idx = srcu_read_lock(&kvm->srcu);
    mmap_read_lock(current->mm);          /* 获取读锁 */
    write_lock(&kvm->mmu_lock);

    ...  /* unmap 操作，只累加 pending_unaccount */

    write_unlock(&kvm->mmu_lock);
    mmap_read_unlock(current->mm);        /* 释放读锁 */
    srcu_read_unlock(&kvm->srcu, idx);

    /* 安全：mmap_lock 已释放，可以调 account_locked_vm() */
    pkvm_flush_unaccount(kvm);
}
```

另外两个调用点（`kvm_uninit_stage2_mmu`、`kvm_arch_flush_shadow_memslot`）也在各自的 `mmu_lock` 和 `mmap_lock` 释放后调用。

### 额外收益：堵住 use-after-free

移除睡眠调用同时移除了释放 `mmu_lock` 的理由，这堵住了同一循环中的第二个漏洞：`for_ppage_node_in_range` 在循环体内缓存了后继节点（`__tmp = kvm_pinned_pages_iter_next(...)`），旧代码释放 `mmu_lock` 打开的窗口中，并发的 `MEM_RELINQUISH` 可以 `kfree` 这个后继节点。一个只延迟计费但保留 `mmu_lock` 释放的移植版本会修复死锁但留下 use-after-free。

---

## 上游无修复

在编写本补丁之前搜索了 Android-Common 和 mainline。两个看起来相关的提交实际上不是：

- `78adeb53eea1` *"Fix account_locked_mm() call in non-preemptible section"* (2024-05)——将计费批量化到 `pkvm_unmap_range()` 内部的 `write_unlock`/`write_lock` 之间。仅绕开了 `mmu_lock`。
- `246414094770` *"Don't do account_locked_vm() while atomic"* (2024-11)——逐页 `write_unlock`/`account`/`write_lock`。仅绕开了 `mmu_lock`。**本仓库已包含其等价物**（适配大页），正是发现死锁的代码。

两者均未触及 `mmap_lock`。ACK `android15-6.6` 在 HEAD（2026-08-04）仍搭载此 bug。Mainline 仅因 v6.14 将 guest stage-2 移入 EL2 的重构（`fce886a60207` 及其系列）而免受影响——这是全新基础设施，不可回移。另经检查确认无关的提交：`ed14b491ec76`/`9a13ca20af8d`（THP 计费算术）、`3ae13572d106`（THP 与 balloon 的回收）、`04512258010d`（回收计费改用 `kvm->mm` 而非 `current->mm`）。

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
e56d181356a4  Convert kvm_pinned_pages to an interval-tree       ← klinux 有此提交，2030/bug930 没有
```

这是 `aosp/android15-6.6` 上 `arch/arm64/kvm/` 下涉及 `account_locked_vm` 的完整历史——六个提交，无一解决 `mmap_lock`。

两个下游树以不同路径继承：

- **`android/common` `2030/bug930`**（futlab；`common-stage2mvp` 和 `ksrc-pkvmfix` 检出的树）**直接**继承自 `android15-6.6-pkvm_experimental`——`git merge-base --is-ancestor` 确认，合并基点*就是*该分支的 HEAD `e70cae0cbb35`。它停在 `e56d181356a4` 之前，所以仍然通过枫树批量化单个 `cnt`。
- **klinux `klad-v11-next`** 将同一 ANDROID 系列导入 V11 6.6.103 内核——272 个 `ANDROID: KVM: arm64` 提交——*包括*区间树转换。因此 `pkvm_unmap_guest()` 中是 per-ppage 计费。两个树不共享对象存储，它们的此代码副本差一个 ACK 提交，这就是一个补丁无法同时服务两者的全部原因。

各树状态（均从源码读取而非推断）：

| 树 | unmap 路径中的计费方式 | 受影响 |
|---|------|------|
| mainline Linux `v6.19.14` | 无——该 tag 下 `arch/arm64` 没有任何 `account_locked_vm` | 否 |
| ACK `android-mainline` | 无 | 否 |
| ACK `android15-6.6` @ 已获取的 tip `9f24219bc984` | `___unmap_stage2_range` → `pkvm_unmap_range` → `account_locked_vm(mm, cnt, false)` | **是，至今仍有** |
| ACK `android15-6.6-pkvm_experimental` | 同上 | **是** |
| futlab `2030/bug930` | 同形（批量化 `cnt`，枫树） | **是**——由 `d7c317637aa2` 本地修复 |
| klinux `klad-v11-next` | per-ppage，区间树 | **是**——由 `348c94763cc6` 修复 |
| ACK `android16-6.12` | **无——`pkvm_unmap_range`/`pkvm_unmap_guest` 不存在** | 否 |
| ACK `android17-6.18` | 无 | 否 |

---

## ACK 6.12 展示了什么，以及为什么它在这里重要

6.12 是有趣的一个，因为它没有修复此缺陷——它**消解**了此缺陷，而其消解方式独立地确认了本修复选择的设计。

6.12 中的 `stage2_unmap_vm()` 仍然在整个 unmap 期间持有 `mmap_read_lock(current->mm)`，和 6.6 完全一样。锁没变。变化的是计费**离开了 unmap 路径**：`___unmap_stage2_range()` 现在只是 `KVM_PGT_FN(kvm_pgtable_stage2_unmap)(...)`，整棵树中仅有的两个减计费点都在 `pkvm.c` 中，位于**回收**路径上，且都在 `mmu_lock` 和 `mmap_lock` 之外——

- `pkvm.c:417`，VM 拆解循环中遍历 `__pkvm_reclaim_dying_guest_page`，循环后批量计费；
- `pkvm.c:762`，`pkvm_host_reclaim_page()`，在 `write_unlock(&host_kvm->mmu_lock)` 之后。

所以 ACK 生产线收敛到了与本修复相同的原理——**减计费不能在持有 `mmap_lock` 的 unmap 中执行**——并通过结构重组跨主版本达到了这一点，作为 `__pkvm_pages_to_ppages` / `__pkvm_host_donate_guest` 重构的一部分。通过 per-VM 原子变量延迟结算是该原理在 6.6 形态的树上的最小表达，也是它成为正确携带形态而非本地发明的原因。

*本对比的注意事项：* 6.12 和 6.18 是通过 gitiles 从分支 tip 读取的，不是从已获取的历史。代码的当前缺席已验证；移除它的提交未被识别，因此未引用提交 id。

同一阅读中的两个附注。`892713e97ca1` *"Sidestep stage2_unmap_vm() on vcpu reset when S2FWB is supported"* (2020) 解释了为什么上游没有人踩到这个：FWB 硅片从不进入该路径。6.12 的 `pkvm_host_reclaim_page()` 对 `host_kvm->mm` 计费，而 klinux 仍使用 `current->mm`（`pkvm.c:311`、`:534`）——这是另一个缺陷，对应上文的 `04512258010d`，单独跟踪，不在本 issue 范围内。

---

## 三个仓库的修复对比

### 代码结构差异

klinux 和 ksrc-pkvmfix/common-stage2mvp 继承了 ACK 系列的不同阶段，导致缺陷代码位于**不同的函数、不同的单元、不同的数据结构**之上：

| 仓库 | pinned pages 数据结构 | 计费位置 | 计费粒度 |
|------|----------------------|----------|----------|
| klinux (`~/klinux`) | 区间树 (`interval_tree`) | `pkvm_unmap_guest()` 内 | per-ppage：`1 << ppage->order` |
| ksrc-pkvmfix (`~/ksrc-pkvmfix`) | 枫树 (`maple tree`) | `pkvm_unmap_range()` 内 | 批量化：循环累计 `cnt`（含 THP `nr_pages`） |
| common-stage2mvp (`~/common-stage2mvp`) | 枫树 (`maple tree`) | `pkvm_unmap_range()` 内 | 批量化：循环累计 `cnt` |

### 修复前代码对比

**klinux——`pkvm_unmap_guest()` 内逐页计费**（`348c94763cc6` 的父提交）：

```c
static int pkvm_unmap_guest(struct kvm *kvm, struct kvm_pinned_page *ppage)
{
    struct mm_struct *mm = kvm->mm;
    ...
    write_unlock(&kvm->mmu_lock);
    account_locked_vm(mm, 1 << ppage->order, false);   /* → mmap_write_lock(mm) → 死锁 */
    write_lock(&kvm->mmu_lock);
    kfree(ppage);
    return 0;
}
```

**ksrc-pkvmfix / common-stage2mvp——`pkvm_unmap_range()` 内批量计费**（`d7c317637aa2` 的父提交）：

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
{
    struct mm_struct *mm = kvm->mm;
    unsigned long cnt = 0;
    ...
    mt_for_each(&kvm->arch.pkvm.pinned_pages, entry, index, end - 1) {
        ...
        cnt++;
    }

    /* account_locked_vm may sleep */
    write_unlock(&kvm->mmu_lock);
    account_locked_vm(mm, cnt, false);                  /* → mmap_write_lock(mm) → 死锁 */
    write_lock(&kvm->mmu_lock);
    return ret;
}
```

两段代码的注释都只说"account_locked_vm may sleep"来解释释放 `mmu_lock` 的必要性，均未提及 `mmap_lock`。

### 修复后代码对比

三个仓库的修复原理相同——延迟到 `pkvm_flush_unaccount()`——但因为计费位置和数据结构不同，hunk 完全不兼容，不能互相 cherry-pick。

**klinux——`pkvm_unmap_guest()` 内累加**（`348c94763cc6`）：

```c
static int pkvm_unmap_guest(struct kvm *kvm, struct kvm_pinned_page *ppage)
{
    ...
    unpin_user_pages_dirty_lock(&ppage->page, 1, ppage->dirty);
    kvm_pinned_pages_remove(ppage, &kvm->arch.pkvm.pinned_pages);

    /*
     * Do NOT call account_locked_vm() here: it takes mmap_lock for write,
     * and stage2_unmap_vm() -- one of our callers -- already holds that same
     * lock for read across the whole unmap. ...
     */
    atomic_long_add(1 << ppage->order, &kvm->arch.pkvm.pending_unaccount);
    kfree(ppage);
    return 0;
}
```

**ksrc-pkvmfix——`pkvm_unmap_range()` 内累加**（`d7c317637aa2`）：

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
{
    unsigned long cnt = 0;
    ...
    mt_for_each(...) {
        unsigned long nr_pages = 1UL << ppage->order;
        ret = pkvm_unmap_guest(kvm, ppage);
        if (ret)
            break;
        cnt += nr_pages;
    }

    atomic_long_add(cnt, &kvm->arch.pkvm.pending_unaccount);
    return ret;
}
```

**common-stage2mvp——`pkvm_unmap_range()` 内累加**（`d7c317637aa2`，与 ksrc-pkvmfix 同一提交）：

```c
static int pkvm_unmap_range(struct kvm *kvm, u64 start, u64 end)
{
    unsigned long cnt = 0;
    ...
    mt_for_each(...) {
        ret = pkvm_unmap_guest(kvm, ppage);
        if (ret)
            break;
        cnt++;
    }

    atomic_long_add(cnt, &kvm->arch.pkvm.pending_unaccount);
    return ret;
}
```

注意 ksrc-pkvmfix 和 common-stage2mvp 的 `cnt` 累加逻辑有细微差异：ksrc-pkvmfix 累加 `nr_pages = 1UL << ppage->order`（支持大页的完整页数），而 common-stage2mvp 只做 `cnt++`（逐页计数，尚无大页支持）。

三个仓库的 `pkvm_flush_unaccount()` 实现完全相同。

### 为什么不能互相 cherry-pick

klinux 的补丁重写了 `pkvm_unmap_guest()`，ksrc-pkvmfix/common-stage2mvp 的补丁重写了 `pkvm_unmap_range()`——函数不同、上下文不同、hunk 没有匹配的上下文行。而且变更向 `struct kvm_protected_vm` 添加了字段，会偏移 `struct kvm_arch` 中的偏移量，且该结构在 EL2 被读取，所以必须完整重建包括 Rust nVHE 绑定——对象级编译测试无法证明其正确性。

传递的是设计：字段名、函数名和注释保持一致，以便在任何人整合两树时能识别出它们是同一修复。

---

## 残余风险

**禁用 `ioctl$KVM_ARM_VCPU_INIT` 不能关闭此问题。** 2026-08-05 的普查在移除该 syscall 后运行了 10,356 次执行且未命中死锁，但 `syz_kvm_setup_cpu$arm64` 在 C 辅助函数内部（`executor/common_kvm_arm64.h:225` → `:196`，声明于 `sys/linux/dev_kvm_arm64.txt:156`）对调用者提供的 `fd_kvmcpu` 发起 `KVM_ARM_VCPU_INIT`。所以序列 `syz_kvm_add_vcpu$arm64` → `ioctl$KVM_RUN` → `syz_kvm_setup_cpu$arm64` 能到达相同状态且可被生成。

将此缓解措施视为降低概率而非保护。任何针对未打补丁内核的测试活动仍可能丢失板子。
