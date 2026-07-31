# pKVM 模糊测试问题汇总 —— dmesg 输出与复现用例对照

**测试环境**

| 项目 | 内容 |
| --- | --- |
| 内核 | `6.6.103+`，源码版本 `82c426336042`（klinux 仓库，分支 `pkvm-el2-kcov`，基线 `klad-v11-next`） |
| 系统 | 银河麒麟 V11 SP1（ostree），aarch64 |
| 硬件 | N90（Great Wall N90F3 / GWMNDB1GL1）、D3000（Phytium D3000） |
| 启动参数 | `kvm-arm.mode=protected`（pKVM 保护模式，EL2 为 Rust 实现） |
| 测试工具 | syzkaller，配置 `workdir-v11-fleet/v11-fleet.cfg`，启用 62 个 KVM 相关系统调用 |
| 测试时间 | 2026-07-31 09:55 启动，10:01 两块板子同时挂死 |

**原始日志**：`n90-dmesg-full.txt`（10044 行）、`d3000-dmesg-full.txt`（11101 行）

**总体结果**：约 20 分钟内，两块板子出现**完全相同的三类问题**。没有 Oops、BUG 或 panic
（`grep -cE "BUG:|Oops|Unable to handle kernel"` 结果为 0），但**问题一导致两块机器最终都失去响应，需要断电重启**。

| 问题 | N90 次数 | D3000 次数 | 严重程度 | 复现用例状态 |
| --- | --- | --- | --- | --- |
| 一、`account_locked_vm` 自死锁（任务挂起） | 2 个任务 | 2 个任务 | **严重：机器卡死** | 已验证 |
| 二、`kvm_tlb_flush_vmid_range` 告警 | 257 | 278 | 中：仅告警 | 已验证 |
| 三、`pend_sync_exception` 告警 | 4 | 1 | 中：仅告警 | **实测不能复现** |

---

## 问题一：`pkvm_unmap_guest()` 中 mmap_lock 自死锁（导致机器卡死）

### dmesg 输出（N90，D3000 完全相同）

```
[ 1158.886327] INFO: task syz.1.1519:46333 blocked for more than 122 seconds.
[ 1158.886339]       Tainted: G        W         ------- ----  6.6.103+ #2
[ 1158.886348] task:syz.1.1519      state:D stack:0     pid:46333 tgid:46333 ppid:30531
[ 1158.886361] Call trace:
[ 1158.886364]  __switch_to+0x10c/0x158
[ 1158.886380]  __schedule+0x3c0/0x12e8
[ 1158.886390]  schedule+0x28/0x120
[ 1158.886400]  schedule_preempt_disabled+0x14/0x28
[ 1158.886410]  rwsem_down_write_slowpath+0x234/0x710
[ 1158.886418]  down_write+0x60/0x78
[ 1158.886425]  account_locked_vm+0x4c/0x108        <== 申请 mmap_lock 写锁
[ 1158.886436]  __unmap_stage2_range+0x230/0x2e8
[ 1158.886448]  stage2_unmap_vm+0x178/0x298         <== 已持有 mmap_lock 读锁
[ 1158.886458]  kvm_arch_vcpu_ioctl+0x41c/0xc30
[ 1158.886468]  kvm_vcpu_ioctl+0x5e4/0xac8
[ 1158.886477]  __arm64_sys_ioctl+0x108/0x128
```

同一任务在 122/143/163/184 秒反复上报，说明**永不返回**。

### syzkaller 侧记录

```
2026/07/31 10:01:56 VM 0: crash: INFO: task hung in __unmap_stage2_range
2026/07/31 10:01:56 VM 1: crash: INFO: task hung in __unmap_stage2_range
```

崩溃目录：`workdir-v11-fleet/crashes/f260f6991637ea9284ba440d365fb26f0cc24ade/`
（含 `description`、`log0`/`log1` 控制台日志、`report0`/`report1` 解析报告）

**注意**：syzkaller 没能自动生成最小化复现程序（`repro.prog`），因为挂死的进程处于 **D 状态无法杀死**，
管理器无法重启虚拟机——日志中持续出现
`scp: /root/syzkaller-fuzz/syz-executor: Text file busy`，测试从 10:17 起完全停滞。

### 根本原因（已确认）

`stage2_unmap_vm()` 在整个 unmap 过程中持有 `mmap_lock` 的**读锁**，
而 `pkvm_unmap_guest()` 调用的 `account_locked_vm()` 要申请同一把锁的**写锁**：

```c
/* arch/arm64/kvm/mmu.c:337-340（当前 klinux 代码，未打补丁） */
write_unlock(&kvm->mmu_lock);
account_locked_vm(mm, 1 << ppage->order, false);   /* -> mmap_write_lock(mm) 永久阻塞 */
write_lock(&kvm->mmu_lock);
```

现有代码释放的是 `mmu_lock`（因为 `account_locked_vm()` 可能睡眠），
但**阻塞的并不是 `mmu_lock`**。读锁持有者无法再申请写锁，因此这是**无条件自死锁，不是竞态**。

### 触发条件与复现用例

对已经运行过的 vCPU **第二次**调用 `KVM_ARM_VCPU_INIT`，且 CPU 不支持 `ARM64_HAS_STAGE2_FWB`。
N90 与 D3000 都满足条件。对应系统调用 `ioctl$KVM_ARM_VCPU_INIT` 正在本次测试的启用列表中。

**独立复现程序**：`../finding-pkvm-unmap-selfdeadlock-2026-07-29/repro-deadlock.c`
（另有已编译的 `repro-deadlock.aarch64`）

```
aarch64-linux-gnu-gcc -O2 -static -o repro-deadlock repro-deadlock.c
```

> ⚠️ 该程序在有问题的内核上**会挂死整台机器**，请只在可以断电重启的机器上运行。
> 修复后的内核应输出 `PASS`。

### 已有修复（重要）

该缺陷此前已定位并修复，补丁位于 `futlab-fixes` 分支：

```
commit d7c317637aa2
KYLIN: KVM: arm64: pkvm: defer RLIMIT_MEMLOCK accounting out of mmap_lock
作者：Haoze Wu <wuhaoze@kylinos.cn>   日期：2026-07-29
```

该补丁改为把递减操作延后（累加到每个 VM 的原子计数，在各自的锁释放之后再结算），
提交说明中记录**已在 Phytium D3000 与 N90（6.6.30）上验证**：原本必挂的复现程序在打补丁后返回 0。

补丁已合入 `android/common` 的 `2030/bug930` 分支，但**尚未移植到 klinux（`klad-v11-next`）**，
因此本次 V11 内核仍是未修复状态。**建议优先移植此补丁**——
其余两个问题的严重程度与之相比可以忽略。

### 影响

每个挂死的任务会**永久持有 `mmap_lock` 和 `kvm->srcu` 读侧临界区**，并且不断累积。
后果是新进程（包括 sshd 派生的登录 shell）也会阻塞，机器逐步失去响应：
本次测试中两块板子在 10:01 挂死，10:25 后 ssh 已无法在 90 秒内完成登录，只能断电重启。
这很可能也是此前 D3000 内存耗尽问题的机制——永不结束的 srcu 读者会阻止其所持内存的回收。

---

## 问题二：`kvm_tlb_flush_vmid_range` 告警（数量最多）

### dmesg 输出

```
[  947.249993] WARNING: CPU: 0 PID: 42378 at arch/arm64/kvm/hyp/pgtable.c:654 kvm_tlb_flush_vmid_range+0x74/0xe0
[ 1135.036919] Call trace:
[ 1135.036923]  kvm_tlb_flush_vmid_range+0x74/0xe0
[ 1135.036936]  kvm_arch_flush_remote_tlbs_range+0x34/0x48
[ 1135.036950]  kvm_flush_remote_tlbs_memslot+0x2c/0xa0
[ 1135.036962]  kvm_arch_commit_memory_region+0x2e4/0x338
[ 1135.036976]  kvm_set_memslot+0x1f0/0x728
[ 1135.036986]  __kvm_set_memory_region+0x46c/0x550
[ 1135.036996]  kvm_vm_ioctl+0xf08/0x1ca8
```

N90 上 257 次告警**全部**来自这一条调用路径，无一例外（按栈帧统计 257/257）。

### syzkaller 侧记录

**无**。本次配置中 `"ignores": ["WARNING:"]`，所有 WARNING 都被管理器忽略，
因此不会生成崩溃记录。若需要 syzkaller 自动最小化此类问题，需去掉该过滤项。

对应系统调用：`ioctl$KVM_SET_USER_MEMORY_REGION`（在启用列表中）。

### 机制说明

`pgtable.c:654` 不是判断语句，而是超级调用本身：

```c
if (!system_supports_tlb_range()) {
        kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);   /* 第 654 行 */
        return;
}
```

告警来自宏内部、被归属到调用点：`kvm_call_hyp_nvhe()`（`kvm_host.h:1257`）以
`WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` 结尾。即 **EL2 返回了非成功状态**。

**与 KCOV 插桩无关**：我们新增的 `KVM_PKVM_COV_HVC` 宏（`kvm_host.h:1237-1245`）只是把
完全相同的 `arm_smccc_1_1_hvc(...)` 调用包在 `pkvm_cov_begin/end` 之间，不触碰 `res`；
该 `WARN_ON` 是上游原有代码。

### EL2 为何拒绝——**该结论尚未经实测确认，仅为代码分析推测**

`kvm_asm.h:55-74` 用注释把超级调用枚举明确分成两段，而 `__kvm_tlb_flush_vmid` 位于前一段：

```c
/* Hypercalls available only prior to pKVM finalisation */
  ... __kvm_flush_vm_context, __kvm_tlb_flush_vmid_ipa, __kvm_tlb_flush_vmid,
      __kvm_tlb_flush_vmid_range, __kvm_flush_cpu_context, ...
  __pkvm_prot_finalize,
/* Hypercalls available after pKVM finalisation */
  ... __kvm_vcpu_run, ...
```

而 Rust 分发器 `hyp_main.rs:1710-1752` 在保护模式初始化完成后把 `hcall_min` 抬到
`__pkvm_prot_finalize`，低于它的 id 一律返回 `SMCCC_RET_NOT_SUPPORTED`。

按此推测，这是**宿主侧缺陷**：`kvm_tlb_flush_vmid_range()` 中
`!system_supports_tlb_range()` 的回退分支没有考虑 pKVM，调用了管理程序在 finalisation
之后有意停用的超级调用。

> **确认方法**：在告警处抓取 `res.a0` 的实际值（kprobe 或临时 `WARN_ONCE` 打印），
> 确认其等于 `SMCCC_RET_NOT_SUPPORTED`，并确认此刻 `kvm_protected_mode_initialized` 为真。
> 在此之前请勿基于该推测直接改代码。
>
> 另注：此前记录过的 `__kvm_flush_vm_context` VMID 回绕问题是**不同的函数**，
> 但两者的 id 同处 finalisation 之前的区段，这正是怀疑二者同源的原因。

### 复现用例（已在 D3000 实测验证）

**`repro-tlbflush-warn.c`**（本目录）

```
aarch64-linux-gnu-gcc -O2 -static -o repro-tlbflush repro-tlbflush-warn.c
```

原理：先创建一个不带脏页跟踪的内存槽，再对**同一个槽**打开 `KVM_MEM_LOG_DIRTY_PAGES`
（即 `KVM_MR_FLAGS_ONLY` 变更），从而进入写保护 + TLB 刷新路径。不需要 vCPU，也不需要客户机代码。

前提：不要启用 `KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2`，否则内核会提前返回、改由 CLEAR ioctl 处理。

**实测结果**（D3000，2026-07-31 10:18）：

```
pid=469390 done -- check dmesg for pgtable.c:654 with this pid
[ 3524.339978] CPU: 0 PID: 469390 Comm: repro-tlbflush Tainted: G  W  ------- ----  6.6.103+ #2
```

告警头部的 PID 与程序自报 PID 一致（469390），`Comm` 为 `repro-tlbflush`，
可与测试进程产生的告警明确区分。运行一次产生一条告警。该程序**不会破坏系统**。

---

## 问题三：`pend_sync_exception` 告警

### dmesg 输出

```
[  989.224073] WARNING: CPU: 0 PID: 43221 at arch/arm64/kvm/inject_fault.c:22 pend_sync_exception+0x110/0x188
[  989.224774] Call trace:
[  989.224778]  pend_sync_exception+0x110/0x188
[  989.224786]  inject_abt64+0x38/0x108
[  989.224794]  kvm_inject_dabt+0x64/0x80
[  989.224802]  __kvm_arm_vcpu_set_events+0x94/0xd0
[  989.224813]  kvm_arch_vcpu_ioctl+0x7d4/0xc30
[  989.224825]  kvm_vcpu_ioctl+0x5e4/0xac8
[  989.224835]  __arm64_sys_ioctl+0x108/0x128
```

### syzkaller 侧记录

**无**，原因同问题二（WARNING 被过滤）。
对应系统调用：`ioctl$KVM_SET_VCPU_EVENTS`（在启用列表中）。

### ⚠️ 重要修正（2026-07-31，据上游 syzbot 复现器订正）

**先前把该告警解释为"HVC 预递增 PC 跨用户态遗留"，这个解释不成立，已撤回。**

关键事实是 `INCREMENT_PC` 与异常目标位**共用同一组 bit，且是有意为之**
（`kvm_host.h:935-942`，注释原文 "Overlaps with EXCEPT_MASK on purpose"）：

```c
#define PENDING_EXCEPTION  __vcpu_single_flag(iflags, BIT(0))
#define INCREMENT_PC       __vcpu_single_flag(iflags, BIT(1))
#define EXCEPT_MASK        __vcpu_single_flag(iflags, GENMASK(3, 1))
```

`__EXCEPT_SHIFT = ctz(GENMASK(3,1)) = 1`，所以异常目标的编码值左移 1 位后落进该字段：

| 目标 | 编码值 | 实际置位 | 会被读成 `INCREMENT_PC` |
| --- | --- | --- | --- |
| `EXCEPT_AA64_EL1_SYNC` | 0 | `0b0000` | 否 |
| `EXCEPT_AA64_EL1_IRQ` | 1 | `0b0010` | **是** |
| `EXCEPT_AA64_EL1_SERR` | 3 | `0b0110` | **是** |
| `EXCEPT_AA64_EL2_IRQ` | 5 | `0b1010` | **是** |
| `EXCEPT_AA32_IABT` | 1 | `0b0010` | **是** |

**结论：`vcpu_get_flag(v, INCREMENT_PC)` 为真并不等于"有待执行的 PC 递增"**，
它同样可能只是上一次待注入异常的目标编码。因此该 `WARN` 不需要任何 `KVM_RUN`
或 HVC 参与，先前的解释是错的。

### 但这条上游机制在【本树】上复现不出来（已实测）

写了一个**完全不调用 `KVM_RUN`** 的程序，在 N90 上试了 4 种组合，
每种独立 VM/vCPU，逐次统计 `inject_fault.c:22` 的告警增量：

| 组合 | 结果 |
| --- | --- |
| `dabt` → `dabt` | 0 条 |
| `serror`(无 esr) → `dabt` | 0 条 |
| 单次调用同时置 `serror_pending` + `ext_dabt_pending` | 0 条 |
| `serror`(带 esr) → `dabt` | 0 条 |

原因在本树代码里很清楚：

1. 64 位客户机的 `kvm_inject_dabt()` 走 `pend_sync_exception()` →
   `EXCEPT_AA64_EL1_SYNC`，编码值 **0**，写进字段反而会**清掉** BIT(1)；
2. 本树的 `kvm_set_sei_esr()`（`inject_fault.c:233`）**根本不碰 flag 机制**，
   它直接设 `vcpu_set_vsesr()` + `HCR_VSE`，走硬件虚拟 SError 通道，
   因此 `kvm_inject_vabt()` 不会置上 `EXCEPT_AA64_EL1_SERR`。

上游 mainline 后来把 SError 注入改造进了 flag 机制，在那种树上组合 2/3 就会命中——
**上游复现器是对的，只是这棵 6.6.103 Kylin 树还是旧实现。**

### 因此本树上的触发源仍未确定

dmesg 里的告警是真实的（N90 4 次、D3000 1 次），入口 ioctl 从调用栈可确定是
`KVM_SET_VCPU_EVENTS`。但究竟是什么置上了 BIT(1)，目前**两种可能都没被证实**：

- 某条 `kvm_pend_exception()` 用了奇数编码的目标（`EL1_IRQ` / `EL1_SERR` /
  `EL2_IRQ` / `AA32_IABT`），残留到下一次注入；
- 或者确实是 `kvm_incr_pc()` 留下的真·PC 递增（我原先的猜测，同样未被证实）。

要定位，最省事的办法仍是去掉配置里的 `"ignores": ["WARNING:"]`，
让 syzkaller 自己最小化出触发序列——见文末建议。

### 原机制说明（已被上述修正推翻，保留供对照）

`inject_fault.c:22` 是 `kvm_pend_exception(vcpu, EXCEPT_AA64_EL1_SYNC)`，
该宏（`kvm_emulate.h:718-723`）第一行就是：

```c
#define kvm_pend_exception(v, e)					\
	do {								\
		WARN_ON(vcpu_get_flag((v), INCREMENT_PC));		\
		vcpu_set_flag((v), PENDING_EXCEPTION);			\
		vcpu_set_flag((v), e);					\
	} while (0)
```

即：用户态通过 `KVM_SET_VCPU_EVENTS` 请求注入数据异常时，vCPU 上**仍挂着一个待执行的 PC 递增**。
注入异常与递增 PC 是互斥的——异常必须在出错指令处发生，而不是在其之后。

`INCREMENT_PC` 能跨越"返回用户态"这一步存活，是 arm64 KVM 对 HVC/SMC 的**有意设计**，
`handle_exit.c:66-76` 的注释写得很明确：

> We need to advance the PC after the trap, as it would otherwise return to the
> same address. Furthermore, **pre-incrementing the PC before potentially exiting
> to userspace** maintains the same abstraction for both SMCs and HVCs.

### 复现用例 —— **尚未验证，请谨慎使用**

**`repro-vcpuevents-warn.c`**（本目录）

思路：让客户机执行 `HVC` 发起 PSCI `SYSTEM_OFF`。KVM 在 `handle_exit.c:76` 先设置
`INCREMENT_PC`，随后 PSCI 把它转成 `KVM_EXIT_SYSTEM_EVENT` 返回用户态，此时标志位仍然置位；
紧接着调用 `KVM_SET_VCPU_EVENTS(ext_dabt_pending=1)` 即可命中告警。

> **状态说明：已在 N90 上实测，该程序当前【不能】复现告警。**
>
> 首次尝试（10:21）因程序缺少超时保护而卡住：`KVM_RUN` 一直不返回，整条命令超时，
> 没有拿到任何输出。补上 `alarm(3)` 后于 10:45 在 N90 重测，结果明确：
>
> ```
> entering KVM_RUN...
> KVM_RUN ret=-1 errno=4 exit_reason=10 (expect 24 = SYSTEM_EVENT)
>   NOTE: interrupted by SIGALRM -- the guest never reached the HVC,
>         so INCREMENT_PC is NOT pending and the WARN will not fire.
> KVM_SET_VCPU_EVENTS ret=0 errno=0
> ```
>
> `errno=4` 即 `EINTR`，`exit_reason=10` 即 `KVM_EXIT_INTR`——是闹钟把 `KVM_RUN` 打断的，
> 说明**客户机根本没有执行到 `HVC`**，`KVM_RUN` 在 3 秒内没有自行返回。
> `dmesg` 中没有任何 `Comm: repro-vcpuev` 的告警（计数为 0）。
>
> 已排除的原因：客户机指令编码是正确的，反汇编确认为
> `mov x0,#0x8` / `movk x0,#0x8400,lsl #16` / `hvc #0`，即 `x0=0x84000008`（PSCI SYSTEM_OFF）。
> 尚不清楚客户机为何没有推进到 HVC（`repro-deadlock.c` 只依赖取指缺页，
> 并不能证明客户机指令真的执行完毕，所以不能拿它作为反证）。
>
> **结论**：问题三的触发路径目前**仍只是代码分析的推断，没有独立复现程序佐证**。
> dmesg 里的告警本身是真实的（N90 4 次、D3000 1 次），
> 触发的系统调用可从调用栈确定为 `KVM_SET_VCPU_EVENTS`，但"`INCREMENT_PC` 因 HVC 预递增
> 而跨用户态存活"这一步尚未被独立复现验证。
> 若需要可靠用例，建议把配置中的 `"ignores": ["WARNING:"]` 去掉，让 syzkaller 自己最小化。

---

## 建议的处理顺序

1. **移植 `d7c317637aa2` 到 klinux 并重新编译部署**。这是唯一会导致机器卡死的问题，
   而且补丁已经写好并验证过。在此之前继续跑模糊测试没有意义——两块板子会在 10 分钟内再次挂死。
2. 若希望 syzkaller 自动收集问题二、问题三的最小化复现程序，
   把配置里的 `"ignores": ["WARNING:"]` 去掉后重跑。
3. 问题二在动手改代码之前，先按上文方法实测确认 `res.a0` 的实际返回值。
4. 问题三补上超时保护后验证复现程序。

## 当前设备状态

**两块板子都需要断电重启**：仍能 ping 通，但 ssh 无法在 90 秒内完成登录，
这正是问题一补丁说明中描述的最终状态（"machine hangs and needs a power cycle"）。
模糊测试的管理器已停止，语料库 `corpus.db` 已保留。

时间线（说明卡死原因与复现程序无关）：

| 时间 | 事件 |
| --- | --- |
| 09:55 | 测试启动，覆盖率正常增长至 12505 |
| 10:01:56 | **两块板子同时上报 `task hung in __unmap_stage2_range`** |
| 10:01 之后 | 管理器反复尝试重启虚拟机失败：`scp: Text file busy`（挂死进程处于 D 状态杀不掉） |
| 10:17 | 执行计数停滞在 3011，测试实际已停止 |
| 10:18 | 问题二复现程序在 D3000 上验证成功 |
| 10:21 | 问题三复现程序运行，此时板子已处于降级状态 |
| 10:25 之后 | 两块板子 ssh 均无法登录 |
