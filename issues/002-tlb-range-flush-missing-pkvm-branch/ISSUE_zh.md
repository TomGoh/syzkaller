# 002 —— 范围 TLB 刷新请求的是一个 EL2 已经停用的超级调用

English version: [ISSUE.md](ISSUE.md) — 该文件带 YAML front-matter,是 tracker 的权威记录;本文是等价中文版,不含 front-matter。

本项目迄今出现最频繁的一类签名:2026-07-31 二十分钟内 N90 上 257 次、D3000 上 278 次;2026-08-05 那轮 census 的最后 3.5 分钟里 270 次。它只是一条告警,不是崩溃 —— 机器毫发无损。它的分量在于:告警所报告的那个操作**不是失败了,而是根本没有发生**,而调用方是脏页日志的写保护路径。

## 症状

```
WARNING: CPU: 0 PID: 92660 at arch/arm64/kvm/hyp/pgtable.c:654 kvm_tlb_flush_vmid_range+0x74/0xe0
CPU: 0 PID: 92660 Comm: repro-tlbflush Not tainted 6.6.103+ #4
Call trace:
 kvm_tlb_flush_vmid_range+0x74/0xe0
 kvm_arch_flush_remote_tlbs_range+0x34/0x48
 kvm_flush_remote_tlbs_memslot+0x2c/0xa0
 kvm_arch_commit_memory_region+0x2e4/0x338
 kvm_set_memslot+0x1f0/0x728
 __kvm_set_memory_region+0x46c/0x550
 kvm_vm_ioctl+0xf08/0x1ca8
```

只有这一条调用路径,没有例外:2026-07-31 那次 N90 上 257 次全部逐帧一致。入口 ioctl 是 `KVM_SET_USER_MEMORY_REGION`。

`pgtable.c:654` 不是判断语句,它就是超级调用本身:

```c
	if (!system_supports_tlb_range()) {
		kvm_call_hyp(__kvm_tlb_flush_vmid, mmu);   /* 第 654 行 */
		return;
	}
```

`WARN_ON` 在宏内部、被归属到调用点:`kvm_call_hyp_nvhe()` 以 `WARN_ON(res.a0 != SMCCC_RET_SUCCESS)` 结尾。即 **EL2 返回了非成功状态**。

**与我们的插桩无关。** `KVM_PKVM_COV_HVC` 把**完全相同的** `arm_smccc_1_1_hvc(...)` 包在 `pkvm_cov_begin`/`pkvm_cov_end` 之间,从不触碰 `res`;反汇编也能看到 `pkvm_cov_begin` 在 `hvc` 之前、`pkvm_cov_end` 在 `res.a0` 已被取走之后。该 `WARN_ON` 是上游原有代码。

## 机制

**置信度:已确证。** "EL2 是**拒收**而不是**执行失败**"这个判断,依据的是实测到的寄存器值,不是对派发器的代码阅读。

**哪个寄存器装着状态码。** 函数起点 `ffff8000800bbc20`,告警报的 `+0x74` 即 `ffff8000800bbc94`,反汇编显示那正是 `cbz x19` 之后紧跟的 `brk`。`hvc` 前两条指令 `mov x0, #0xa` 选定 hypercall id 10;`hvc` 之后紧接着 `mov x19, x0` 取回状态码。所以告警处的 `x19` 就是 `res.a0`(`evidence/2026-08-05-warn-site-disasm-and-ids.txt`)。

**这个寄存器里是什么。** 2026-08-05 census 的每一条该告警中,`x19` 都是 `0xffffffffffffffff`,**270 次中 270 次**,零方差(`evidence/2026-08-05-res-a0-distribution.txt`)。`SMCCC_RET_NOT_SUPPORTED = -1`,`SMCCC_RET_SUCCESS = 0`(`include/linux/arm-smccc.h:339-340`)。随后在下一个内核上做的定向复现得到同样的值,而且告警与反汇编取自**同一个二进制**。

**为什么 `-1` 唯一地指向拒收路径。** Rust 派发器(`hyp_main.rs:1707-1765`)在保护模式初始化完成后把 `hcall_min` 抬到 `__pkvm_prot_finalize`:

```rust
if unlikely(id < hcall_min || (id as usize) >= HOST_HCALL.len()) {
    self.regs.regs[0] = SMCCC_RET_NOT_SUPPORTED;   // :1751 —— 在任何 handler 之前
    return;
}
if let Some(hfn) = HOST_HCALL[id as usize] {
    self.regs.regs[0] = SMCCC_RET_SUCCESS;          // :1758 —— 在调用它之前
    hfn(self);
}
```

成功路径是**先写 SUCCESS 再调 handler**,所以任何 handler 都不可能留下 `-1`。`-1` 只能来自 `:1751`。

**id 也对得上。** 取自 EL2 crate 实际编译所依据的 bindgen 输出,本 config 下:`__kvm_tlb_flush_vmid` = **10**,`__pkvm_prot_finalize` = **18**,`__pkvm_tlb_flush_vmid` = **39**。发出的 id(10,从反汇编读出)低于 `hcall_min`(18),而基于 handle 的刷新(39)高于它。手数枚举会得到不同数字 —— 里面有条件编译项 —— 所以以生成的常量为准。

**因此 TLB 失效从未执行。** 不是"失败":EL2 在派发之前就拒收了这个 id,没有任何 handler 运行过。

## 触发条件

对一个**已经存在**的内存槽追加 `KVM_MEM_LOG_DIRTY_PAGES`(即 `KVM_MR_FLAGS_ONLY` 变更),宿主以 `kvm-arm.mode=protected` 启动。创建时就带该标志是另一条路径;并且不能启用 `KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2`,否则内核提前返回、改由 `CLEAR` ioctl 处理。

没有 `FEAT_TLBIRANGE` 的硬件(飞腾 D3000 与长城 N90 都是)永远走 `pgtable.c:654` 的 `!system_supports_tlb_range()` 分支,即 id 10。有 TLBIRANGE 的机器走 `:661` 的 `__kvm_tlb_flush_vmid_range`,id 11 —— 同样位于 finalisation 之前的区段,被以同样方式拒收。

不需要 vCPU,也不需要客户机代码。

## 复现

`repro/repro-tlbflush-warn.c`:

```
aarch64-linux-gnu-gcc -O2 -static -o repro-tlbflush repro/repro-tlbflush-warn.c
```

运行一次产生一条告警,可通过 PID 与 `Comm` 明确归属。**无害** —— 不会影响板子,与问题 001 的复现程序不同。

## 影响面

告警本身是噪音,它所报告的事情不是。

调用方是脏页日志的写保护路径,而 EL2 的 `__pkvm_wrprotect` **按设计不做 TLB 失效**,它依赖这次刷新。刷新被拒之后,客户机可以继续通过残留的可写 stage-2 TLB 表项写入,而这些写**不会**被记入 dirty bitmap —— 对迁移或快照而言是静默的数据丢失。**这一层后果是从源码推导的,尚未在本树上实测证明**;已实测的部分是"失效没有执行"。

而且没有任何东西会暴露这次失败:`kvm_arch_flush_remote_tlbs_range()` 无条件 `return 0`,把通用层"失败就退化为全量 flush"的兜底也一并关掉了。而它本该退化过去的那条全量路径 `kvm_arch_flush_remote_tlbs()` **是有** pKVM 分支的,本来会成功。

对测试本身的代价是吞吐与盲区:该签名在管理器编译期的 ignore 列表里,于是一轮测试会在 `dmesg` 里堆出几百条,而报告显示一切正常。

## 修复状态

**补丁已提交,尚未运行时验证** —— klinux 分支 `pkvm-tlb-flush-range-fix` 上的 `3608223e5012`(基于承载问题 001 修复的那个分支),*"KYLIN: KVM: arm64: pkvm: route range TLB flush through the VM handle"*,`mmu.c` 中 `+7/-2`。`mmu.o` 编译通过,checkpatch 0 errors、0 warnings。

### 这一个上游有

与问题 001 不同,上游存在修复。`kvm_arch_flush_remote_tlbs_range()` 的 pKVM 分支来自 mainline **`fce886a6020734d6253c2c5a3bc285e385cc5496`** —— *"KVM: arm64: Plumb the pKVM MMU in KVM"*,Quentin Perret,经 kvmarm/next 进入 v6.14 —— 并作为 **`73a4f4ffe584`** 回合到 ACK `android16-6.12`(2024-12-18,`Change-Id: I58367c5b21366b9bc973efe2f36dc82d3b6dfc23`)。

这**正是问题 001 里引用的那个提交** —— 当时用来说明 mainline 为何对那个缺陷免疫。一次上游重构同时关掉了两个:把固定页记账移出 unmap 路径(001),并给范围刷新补上 pKVM 分支(002)。本树停在它之前。

该提交无法 cherry-pick:`mmu.c` 与 `kvm_mmu.h` 合计 `+96/-226`,引入 `KVM_PGT_FN()` 间接层、把非保护客户机的 stage-2 整体交给 pKVM —— 本树没有这套基础设施,ACK 自己的 backport 说明也写着 "Small conflicts all over the place."。但它针对这一个函数的 hunk 是自包含的,只依赖本树已有的符号,已原样取用。

ACK `android15-6.6` —— 本血统所派生自的分支 —— 在其 HEAD 上**至今仍没有**这个修复,这是直接对着分支 tip 查的,不是查本地镜像。

**查找方法**值得复用:`android.googlesource.com` 允许匿名读**文件内容**,但历史页面要求登录,所以无法从网页确定提交;本地 `kernel-refs/ack` 是 `--depth=1`,有内容无历史。两次 `git fetch --shallow-since` 把 `android16-6.12` 加深到分支起点附近,此时边界提交已**不含**该修复 —— 证明改动落在区间内。对 29,877 个提交做 pickaxe 因按需拉 blob 被杀;改为对触及 `mmu.c` 的 67 个提交二分,6 次探测命中。

### 复现程序无法验证这个修复

由评审发现,并已对照源码确认。打上补丁后,这个复现程序会**不再告警,但什么也证明不了**:

`kvm->arch.pkvm.handle` 只在 `__pkvm_create_hyp_vm()`(`pkvm.c:412`)中写入,而该函数经 `arm.c:879` 在**首次 vCPU 运行**时才到达。复现程序不创建 vCPU,所以 handle 仍是 0。`kvm_flush_remote_tlbs_memslot()` 没有 `pkvm_is_hyp_created()` 守卫,新分支照样执行;到了 EL2,`get_vm_by_handle()` 把 handle 0 映射为越界索引并返回 null(`pkvm.rs:1208-1224`),`handle___pkvm_tlb_flush_vmid` 于是**不刷新、也不触碰 `a0`** 就返回 —— 而 `a0` 早已被派发器置为 `SMCCC_RET_SUCCESS`。

净效果:无告警,也无刷新。这个行为是**正确的** —— 没有 hyp VM 就没有 EL2 stage-2 表项需要失效 —— 但告警消失是因为调用变成了 no-op。

顺带一提:正是那个边界检查让 handle 0 只是"无用"而非"危险"。如果没有 `idx >= KVM_MAX_PVMS`,宿主传入的 handle 0 就会变成一次 EL2 越界读。

**因此有意义的验证需要另写一个程序**,与暴露缺陷的那个不同:建 VM → **真正跑一次 vCPU**(使 hyp VM 存在、页面已映射)→ 开启脏页日志 → 让客户机写入 → 确认这些写进入 dirty bitmap。"告警没了"不构成证据。这也是上面 `repro:` 字段只指向触发程序的原因 —— 验证程序是另一件产物,目前尚不存在。

## 残留风险

修复之后,handle 为 0 的情形会"报告成功却什么都没做"。今天这是对的,但它同时也**抹掉了唯一的信号** —— 修复之前,被拒的刷新至少还会告警。若将来出现某条路径能在客户机存活的情况下带着零 handle 进到这个函数,它将静默失败。

`kvm_arch_flush_remote_tlbs_range()` 及其兄弟函数仍然无条件 `return 0`,所以对所有调用方而言,通用层的"退化为全量 flush"兜底依旧是关闭的。

pKVM 分支刷新整个 VMID、忽略 `addr`/`size`,因此脏页日志类负载下每次内存槽操作都会触发一次全量 stage-2 刷新,比非 pKVM 路径更粗。这与 mainline 一致,不值得为此偏离上游。
