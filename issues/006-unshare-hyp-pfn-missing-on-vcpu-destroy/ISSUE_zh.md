# 006 —— 被挂起的 vCPU 泄漏 EL2 固定引用,而且泄漏是永久的

English version: [ISSUE.md](ISSUE.md) —— 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

由 2026-08-06 全覆盖模糊测试轮次发现——这是本项目第一个不在脏页日志路径上的发现。模糊测试期间出现两次(05:22:32、07:30:12),syzkaller 都没能化简出复现器。当天上午手工根因定位并复现。

**这是一个继承自上游的缺陷,ACK 已修复,且任何能打开 `/dev/kvm` 的进程都能触发。**

完整工作记录(含三个被排除的假设):[evidence/2026-08-07-what-was-ruled-out.txt](evidence/2026-08-07-what-was-ruled-out.txt)。

---

## 背景知识

### pKVM 中的"固定"(pin)是什么

在 pKVM 架构下,EL2(管理程序)和宿主内核(EL1)是两个独立的特权级。EL2 不能随意访问宿主的内存——宿主必须显式地把一块内存"共享"(share)给 EL2,EL2 才能读写它。这个共享操作在 EL2 侧维护着一个**引用计数**:每次 `hyp_pin_shared_mem()` 把一块内存**固定**给 EL2,计数加一;每次 `hyp_unpin_shared_mem()` 解除固定,计数减一。计数归零时,EL2 才真正释放对这块内存的访问权。

注意这个"固定"拦住的是什么:它**不碰 Linux 的内存管理**,不阻止回收也不阻止换出(`hyp_pin_shared_mem()` 全部的动作就是两次页状态校验加一次 `hyp_page_ref_inc()`)。它拦住的是**归属转移**——EL2 侧只要 `is_range_refcounted()` 为真,任何想把这块内存从 EL2 手里拿回去的转移都会被拒绝。本缺陷失败的正是这道关口。

这里的"pin"是"固定/钉住"的意思——EL2 声明"这块内存我还在用,别拿走",而不是芯片的"引脚"。

这与宿主侧的 `kvm_share_hyp()` / `kvm_unshare_hyp()` 是**两套独立的机制**:宿主侧用一棵红黑树(`hyp_shared_pfns`)跟踪哪些物理页共享给了 EL2。EL2 侧则用**两样东西**跟踪——页面所有权状态(`OWNED` / `SHARED_OWNED` / `SHARED_BORROWED`)和这个固定计数,两者互相独立。区分它们很重要:issue 005 坏的是前者(状态被覆盖),本 issue 坏的是后者(计数没归零)。两边必须配对——如果 EL2 侧的固定没解除,宿主侧的 unshare 就会被 EL2 拒绝。

### vCPU 的创建与销毁

在 pKVM 下,一个 vCPU 的生命周期大致是:

1. **创建**(`KVM_CREATE_VCPU`):宿主分配 `struct kvm_vcpu`,调用 `kvm_share_hyp()` 把它共享给 EL2
2. **首次运行**(`KVM_RUN`):触发 `kvm_arch_vcpu_run_pid_change()` → `pkvm_create_hyp_vm()`,此时 EL2 通过 `__pkvm_init_vcpu` 创建自己的 vCPU 副本,并把宿主的 `struct kvm_vcpu` **固定**到 EL2(这样 EL2 可以安全地读取宿主 vCPU 的状态)
3. **销毁**(关闭 fd → `kvm_destroy_vm`):宿主调用 `kvm_unshare_hyp()` 解除共享,EL2 侧的固定引用应当已经被释放

本缺陷发生在第 2 步的 EL2 初始化中,但在第 3 步才表现为告警——而且造成的损害是永久的。

### `KVM_MP_STATE` 是什么

`KVM_MP_STATE` 是 vCPU 的多处理器状态,表示 vCPU 当前的运行状态:`RUNNABLE`(0,可运行)、`STOPPED`(5,已停止)、`SUSPENDED`(10,挂起)等。VMM 可以通过 `KVM_SET_MP_STATE` ioctl 设置它。在 ARM64 上,宿主接受 `SUSPENDED` 状态,但 EL2 只认 `RUNNABLE` 和 `STOPPED`——这个不一致就是触发条件。

---

## 症状

```
WARNING: CPU: 0 PID: 474382 at arch/arm64/kvm/mmu.c:728 kvm_unshare_hyp+0x12c/0x140
Comm: syz.0.6493   6.6.103+ #16   Source Version: cba248683e5c

 kvm_unshare_hyp+0x12c/0x140        arch/arm64/kvm/mmu.c:728
 kvm_arm_vcpu_destroy+0x34/0xf8     arch/arm64/kvm/reset.c:160   <- vCPU 结构体
 kvm_arch_vcpu_destroy+0x5c/0xb0    arch/arm64/kvm/arm.c:536
 kvm_destroy_vcpus -> kvm_arch_destroy_vm -> kvm_destroy_vm -> kvm_vm_release -> __fput
```

`struct kvm_vcpu` 每跨一页产生一条 WARN(本构建上是三条)。非致命——板子没挂,模糊测试继续跑。

---

## 根因

**置信度:已根因定位。** 固定顺序、错误路径、清理函数的不对称,都直接从源码逐行读过。

`init_pkvm_hyp_vcpu()`(`arch/arm64/kvm/hyp/nvhe/pkvm.c:574`,Rust 移植版在 `hyp/xhypervisor/src/pkvm.rs:1793`)先把宿主 vCPU 结构体固定到 EL2,再做校验,最后才记录那个"解除固定时需要的指针":

```c
// arch/arm64/kvm/hyp/nvhe/pkvm.c —— init_pkvm_hyp_vcpu()
if (hyp_pin_shared_mem(host_vcpu, host_vcpu + 1))   // 582行:固定已取
    return -EBUSY;

hyp_vcpu->vcpu.arch.hyp_reqs = kern_hyp_va(host_vcpu->arch.hyp_reqs);  // 585行:hyp_reqs 字段已赋值
if (hyp_pin_shared_mem(hyp_vcpu->vcpu.arch.hyp_reqs, ...)) {           // 586行:hyp_reqs 固定已取
    hyp_unpin_shared_mem(host_vcpu, host_vcpu + 1);
    return -EBUSY;
}

if (host_vcpu->vcpu_idx != vcpu_idx) {              // 592行:校验1
    ret = -EINVAL;
    goto done;                                       // 594行:跳到清理
}

mp_state = READ_ONCE(host_vcpu->arch.mp_state.mp_state);  // 597行:读 mp_state
if (mp_state != KVM_MP_STATE_RUNNABLE && mp_state != KVM_MP_STATE_STOPPED) {  // 598行:校验2
    ret = -EINVAL;
    goto done;                                       // 600行:跳到清理
}

hyp_vcpu->host_vcpu = host_vcpu;                    // 603行:指针在这里才记录——太晚了
```

`goto done` 调用 `unpin_host_vcpu()`(第 449 行),它通过**字段**来解除固定:

```c
// arch/arm64/kvm/hyp/nvhe/pkvm.c:449
static void unpin_host_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
    struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;    // 451行:在 592/598 路径上仍为 NULL
    void *hyp_reqs = hyp_vcpu->vcpu.arch.hyp_reqs;       // 452行:已在 585 行赋值

    if (host_vcpu)                                        // 454行:NULL,跳过
        hyp_unpin_shared_mem(host_vcpu, host_vcpu + 1);
    if (hyp_reqs)                                         // 456行:非 NULL,执行
        hyp_unpin_shared_mem(hyp_reqs, hyp_reqs + 1);
}
```

**整个 bug 就是这个不对称**:`hyp_reqs` 的固定能释放,因为它的字段在校验之前就赋值了;`host_vcpu` 的固定释放不了,因为它的字段在校验之后才赋值。

Rust 移植版(`hyp/xhypervisor/src/pkvm.rs`)的结构完全相同——固定在 1816 行,`hyp_reqs` 字段在 1823 行,`vcpu_idx` 校验在 1836 行,`mp_state` 校验在 1843 行,`self.host_vcpu = host_vcpu` 在 1850 行。`unpin_host_vcpu()`(1525 行)同样先查 `self.host_vcpu`(为 null 就跳过),再查 `self.vcpu.arch.hyp_reqs`(已赋值就解除固定)。

---

## 触发条件是一个合法的宿主操作

`KVM_MP_STATE_SUSPENDED` 是 **10**(见 `include/uapi/linux/kvm.h:560`),而 `kvm_arch_vcpu_ioctl_set_mpstate()`(`arm.c:724`)接受它:

```c
case KVM_MP_STATE_SUSPENDED:
    kvm_arm_vcpu_suspend(vcpu);    // arm.c:739
        -> WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_SUSPENDED);  // arm.c:706
```

EL2 只接受 `RUNNABLE(0)` 和 `STOPPED(5)`(见上面 `init_pkvm_hyp_vcpu()` 的 598 行)。所以在 vCPU 首次 `KVM_RUN` 之前把它挂起——这是 VMM 的常规操作——就走上了泄漏路径。不需要 `/dev/kvm` 之外的任何权限。

### 完整触发序列

1. `open("/dev/kvm")` → `KVM_CREATE_VM` → `KVM_CREATE_VCPU` → `KVM_ARM_VCPU_INIT`
2. `KVM_SET_MP_STATE(SUSPENDED)` —— 宿主接受,`mp_state` 写入 10
3. `KVM_RUN` —— 触发 `kvm_arch_vcpu_run_pid_change()`(`arm.c:838`)→ `pkvm_create_hyp_vm()`(`arm.c:894`)→ `__pkvm_create_hyp_vm()`(`pkvm.c:379`)→ `__pkvm_create_hyp_vcpu()`(`pkvm.c:202`)→ `kvm_call_refill_hyp_nvhe(__pkvm_init_vcpu, ...)`(`pkvm.c:223`)→ EL2 的 `__pkvm_init_vcpu()`(`pkvm.c:790`)→ `init_pkvm_hyp_vcpu()`(`pkvm.c:574`)
4. EL2 在 598 行发现 `mp_state=10` 不是 0 也不是 5,走 600 行 `goto done`,泄漏 `host_vcpu` 的固定引用,返回 `-EINVAL`
5. `-EINVAL` 沿原路返回:`__pkvm_init_vcpu` → `__pkvm_create_hyp_vcpu`(返回)→ `__pkvm_create_hyp_vm`(`goto destroy_vm`)→ `pkvm_create_hyp_vm` → `kvm_arch_vcpu_run_pid_change` → `KVM_RUN` 返回 `-1`,`errno=EINVAL`
6. 关闭 fd → `kvm_arch_vcpu_destroy()`(`arm.c:523`)→ `kvm_arm_vcpu_destroy()`(`reset.c:156`)→ `kvm_unshare_hyp(vcpu, vcpu + 1)`(`reset.c:160`)
7. `kvm_unshare_hyp()`(`mmu.c:718`)逐页调 `unshare_pfn_hyp()`,后者调 `__pkvm_host_unshare_hyp` 让 EL2 解除共享——但 EL2 发现该页还被固定着(固定引用没释放),拒绝 unshare,返回非零
8. `mmu.c:728` 的 `WARN_ON(unshare_pfn_hyp(pfn))` 触发

---

## 硬件确认

`repro/probe-mpstate-suspended-pin-leak.c`,N90,内核 `#16 @cba248683e5c`。从持续控制台捕获中读取(**不是** dmesg,原因见下文的方法注记)。

| 操作 | `KVM_RUN` 返回 | 销毁时的 WARN |
| --- | --- | --- |
| 基线,不运行探针 | — | 0 |
| 对照组,`mp_state = RUNNABLE` | `-EINTR`(我们的 3 秒闹钟) | **0** |
| 实验组,`mp_state = SUSPENDED` | **`-EINVAL`** | **3 条**,`Comm: probe-mpstate` |

跨多次运行、不同 PID 均复现。`KVM_RUN` 的 `-EINVAL` 正是 EL2 的返回值经 `__pkvm_create_hyp_vcpu` → `pkvm_create_hyp_vm` → `kvm_arch_vcpu_run_pid_change` 原样传播上来。

---

## 泄漏是永久的——这才是更严重的一半

固定引用泄漏在**物理页**上,这些页随后归还给 slab 分配器。反复运行最终会导致:

```
FATAL: KVM_CREATE_VCPU: Invalid argument
```

因为后来的 vCPU 如果落在了被毒化的页上,就无法共享给 hyp——`kvm_share_hyp()`(`arm.c:512`,在 `kvm_arch_vcpu_create()` 内部)会失败,于是 **`KVM_CREATE_VCPU` 本身开始失败**。这是无权限可达的资源耗尽:每个触发的 VM 毒化 vCPU slab 的另外几页,除了重启没有任何东西能恢复它们。

这也回答了两个此前悬而未决的问题:

- **为什么单独运行是间歇性的**——一次运行只在 slab 分给它全新页时才 WARN。如果拿到的是已毒化的页,`KVM_CREATE_VCPU` 一开始就失败,根本没到销毁那一步,自然没有 WARN。
- **为什么过夜发生率在爬升**——前 13.5 小时零次,后 3.5 小时两次,正是毒化页在累积。运行记录曾把这个形态标为未验证的猜测;现在它有了机制。

---

## 上游

ACK 已修复,`Bug: 357781595`:

| 提交 | 相关性 |
| --- | --- |
| `2b4d43af6` ANDROID: KVM: arm64: Fix cleanup on partially-initialised pKVM vCPU init failure | **正是这个 bug** |
| `fe4e0e499` BACKPORT: UPSTREAM: KVM: arm64: Fix pin leak and publication ordering in `__pkvm_init_vcpu()` | 相邻路径的固定引用泄漏;cherry-pick 自 `73b9c1e5da84`,`Fixes: 49af6ddb8e5c`,`Cc: stable` |

`2b4d43af6` 的提交信息逐字点名了我们这条路径——*"leak the host_vcpu pin via the mp_state-failure path, which jumps to 'done:' before hyp_vcpu->host_vcpu is recorded"*——修复方式是把赋值提前:

```c
/* Set before mp_state check so 'done:' cleanup can unpin host_vcpu. */
hyp_vcpu->host_vcpu = host_vcpu;
hyp_vcpu->vcpu.kvm = &hyp_vm->kvm;
```

`2b4d43af6` 还带了同一函数中相邻路径的三个进一步泄漏(hyp-pool SVE 分配在 `pkvm_vcpu_init_psci()` 失败时泄漏;从未固定过的 SVE 页被解除固定;`pvmfw_entry_vcpu` 被悬空)。**这三个在本板上均未确认**——N90 没有 SVE,所以 SVE 相关的在本板不可达——但它们在同一个函数里,应当一起迁移,而不是只 cherry-pick 那一行提前。

---

## 不是 Rust 移植的产物

Rust EL2 是修复前 C 代码的忠实移植,本树的 `arch/arm64/kvm/hyp/nvhe/pkvm.c` 也带着同样的 bug。这也排除了我们未提交的 `CONFIG_PKVM_EL2_COV` 插桩可能是元凶的担忧:触发它的是一个 60 行的 C 程序,从不碰覆盖率环。

---

## 两条值得留存的方法注记

**dmesg 在模糊测试期间不可用于本板。** `vm/isolated/isolated.go:340` 对每个实例会话执行 `dmesg >> dmesg-history.log; dmesg -C; dmesg -w`,所以缓冲区被主动清空——实测只保留 42 秒窗口。本 issue 早期的一条注记把这个现象归因于环形缓冲区回绕并算了流失率;那是错的,增大 `log_buf_len` 也救不了。用持续捕获。

**运行这个探针会污染模糊测试的崩溃计数器,** 因为 syzkaller 的控制台读取器也会看到我们的 WARN。模糊测试在 09:20 报告的"第 3 次出现"就是我们的探针,不是 fuzzer 发现的。

---

## 下一步

把 `2b4d43af6`(并考虑 `fe4e0e499`)迁移到本树的 Rust EL2 和 C 两侧,然后重跑 `repro/probe-mpstate-suspended-pin-leak.c`,要求:对照组 0 条 WARN,实验组 0 条 WARN,且反复迭代后 `KVM_CREATE_VCPU` 仍成功。注意当前板子上的毒化页是永久的——**验证需要重启**,否则既有的毒化会产生与修复无关的失败。
