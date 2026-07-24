# pKVM syzkaller：当前状态、缺口与后续路线

> 状态快照：2026-07-23。本文不是对既有设计或实现记录的替换，而是把已经验证的 Host 侧结果、Stage-2 真机 smoke，以及仍未完成的工作放在同一张图里。本文所说的“已验证”以 `pkvm-lifecycle-fuzzing@b04ed35e0` 的证据和 `/home/jose/common-stage2mvp` 中与该证据哈希一致的构建为准。

> **⚠️ 2026-07-24 更新：§6.1 与 §7「第一步」提出的度量已经做完，本文若干结论被实测推翻。**
> 记录见 `2026-07-24-pkvm-stage2-step1-feedback-measurement.md`，数据见 `evidence/step1-measurement-2026-07-24/`。
> **下列具体表述现已过时，保留仅作为「2026-07-23 当时的认知」，不要再据以决策：**
>
> | 本文表述 | 位置 | 实测结论 |
> |---|---|---|
> | 「87 个 EL2 PC、其中 77 个解析到 Rust 源码」 | §4.4、§5 | **是串口洪水挤出来的假象。真值 132 个唯一 EL2 PC**；被挤掉的 45 个恰是 donation 状态机 |
> | 「`_prog1.1` 恰有 524287 项 …… task KCOV buffer 也已经构成实际瓶颈」 | §6.1 | **推翻。** 该 coverfile 84% 是 pl011 串口驱动自旋，由 bridge 自己 4 行 overflow printk 触发。去掉 hot-path printk 后同一 smoke 只用掉 area 的 49%，**KCOV 侧丢失恒为 0** |
> | 「双层容量截断」是第一优先级 | §6.1 | **只剩上游一层。** 下游那层是测量仪器假象；上游 ring 实测丢失 5.02% |
> | 「EL1 drain 去重仍是很有价值的后续减压阀」及其预分配 / per-`KVM_RUN` session 设计 | §6.1、§7 第二步(3) | **建议放弃。** `kcov_requested == kcov_accepted`，没有需要缓解的 KCOV 压力；这省掉第二步最复杂的一块 |
> | `PKVM_COV_RING_PCS 511`、「一页 8 字节 header + 511 个 PC」 | §4.2、§6.1 | **现为 509**：header 扩到 24 字节以容纳每次 #23 的 producer 计数（1B） |
> | 「`kcov_add_pcs()` …… 当前不报告丢失计数」 | §4.4 | **已实现**：返回实际写入条数或 `-ENODEV`；全部计数经 `kvm/pkvm_cov/stats` 导出 |
> | 1A+ 建议「增至 2M，必要时 4M」 | §7 1A+ | **实际做到 1M 即止**：executor 输出共享内存 `ConstMaxOutputSize = 14 MiB` 才是先到的天花板，且翻倍 area **没有换来任何新的 unique EL2 PC** |
>
> §7 的整体次序（先度量、不先开 campaign）是对的，并已按此执行完毕。

## 1. 结论

项目已经跨过了两个重要里程碑，但还不能称为“可长期运行的 EL2 coverage-guided campaign”。

| 工作线 | 状态 | 含义 |
|---|---|---|
| Stage 1 / 1.5：pKVM Host 侧 fuzzing | 已完成 | syzkaller 能经由真实 `/dev/kvm` ioctl 驱动 protected-VM 生命周期，并从 EL1 Host KVM 代码获得 KCOV feedback。 |
| Stage 2：EL2 coverage 最小闭环 | 已完成 smoke | Rust hyp 的实际 PC 已经能够经由共享 ring、EL1 drain 和 KCOV 回到 syzkaller，并映射到 Rust `.rs:line`。 |
| Stage 2：manager campaign | 未完成 | 当前存在 ring/KCOV 截断、单 CPU 归属约束和无可变异 EL2 输入面的缺口。 |
| 深度 protected-guest / HVC fuzzing | 未完成 | SyzOS 目前不能直接跨越 protected guest 的内存隔离；Rust hyp 的 guest-HVC 面也较薄。 |

因此，当前正确的定位是：**Host 侧已经是一条可用 fuzzing 线；EL2 已从设计变为真机跑通的原型，但尚不是 campaign 级基础设施。**

Stage 2 的近期大方向也随之收敛为：先让反馈信号的来源、容量和丢失都可度量、可解释，再让 executor 以正确的串行方式执行可变异的 Host 侧 pKVM 序列。不能因为已经看到了 Rust `.rs:line`，就跳过“这些 signal 是否完整、是否归属于正确程序”的验证。

这条“先度量”的工作分成三个互补的小步骤：现有内核上的 debugfs A/B（1A）、仅放大 executor KCOV area 的下游容量探针（1A+），以及内核侧端到端丢失计数（1B）。它们不是把同一件事拆成三份，而是分别观察用户态可见的结果、task KCOV 的下游容量，以及 EL2 producer 侧在到达 KCOV 前已经发生的丢失。

## 2. 要解决的原始问题

这项工作分为两个不同层次，不能混为一谈。

1. **EL1 / Host KVM 层。** syzkaller 是否能创建 protected VM、配置 vCPU 和内存、运行和销毁它，并收集 Linux Host KVM 的普通 KCOV。
2. **EL2 / Rust hyp 层。** Rust pKVM 在 EL2 内部实际执行的基本块，能否成为 syzkaller 的 coverage signal。

普通 KCOV 只能直接处理第一层。EL2 没有 Linux 的 `current`，且运行在不同映射中；直接让 EL2 使用普通 `__sanitizer_cov_trace_pc()` 会错误地访问 Linux task/KCOV 状态。Stage 2 的目的就是建立一个安全的桥接路径，而不是简单地打开 `CONFIG_KCOV_INSTRUMENT_ALL`。

当前的数据流如下：

```text
syz 程序
  -> executor 中的 pKVM helper / KVM ioctl
  -> EL1 Linux KVM Host 代码                 <- 普通 KCOV
  -> host -> hyp hypercall
  -> Rust EL2 pKVM hyp                       <- Rust SanCov
  -> per-CPU host-shared ring
  -> EL1 指定边界处 drain
  -> kcov_add_pcs() 写入当前 executor 线程的 KCOV
  -> syz-executor / syz-manager 的 signal、corpus、覆盖率
```

EL2 ring 不携带 task id。归属在回到 EL1 后恢复：在当前 MVP 中，`pkvm_mem_abort()` 仍同步运行在发起 `KVM_RUN` 的 executor 线程中；此时 `kcov_add_pcs()` 看到的 `current` 就是正确的测试线程。

## 3. 已完成的 Host 侧工作（Stage 1 / 1.5）

### 3.1 pKVM ABI 与生命周期建模

syzkaller 已增加或扩展了下列受控 pKVM 调用面：

- protected VM 创建：强制 `KVM_CREATE_VM` 的 protected bit（bit 31）；
- protected VM、vCPU、安全 `VCPU_INIT` 和显式 close 的资源模型；
- `syz_kvm_vcpu_run_immediate$arm64`：首次 `KVM_RUN` 仍跨越 host->hyp 创建路径，但以 `immediate_exit=1` 返回，避免没有有效 guest payload 时的 busy loop；
- memslot delete/move/dirty/readonly 拒绝路径；
- `KVM_ENABLE_CAP` 的 INFO、`SET_FW_IPA` 与 post-run `-EBUSY` 路径；
- 固定的 firmware-fault smoke helper `syz_kvm_run_fw_fault$arm64`。

其中最重要的设计是 **composite pseudo-call**。syzkaller 的 resource subtype 不能表达“此 VM 已经 run 过”这样的时序状态：一个父类型 resource 仍可能被传给子类型参数。对于带有时间前提的状态机，正确的做法是在 executor C helper 中完整构造状态：

```text
创建 protected VM -> 注册 memslot -> 创建 vCPU -> safe init -> run -> 目标操作
```

这比继续堆叠 resource subtype 更可靠，也能避免随机组合落入 BUG1/BUG2 等不属于当前受控 slice 的路径。

### 3.1.1 为什么 EL1 会执行这些 pKVM 函数：ioctl 是起点，但不是一对一关系

`/dev/kvm` 是 userspace 到 KVM 的 ABI。executor 在 EL0 发出的 ioctl 先进入 EL1 的通用 KVM file operation（例如 `kvm_vm_ioctl()`、vCPU fd 的 ioctl handler），再分派到 arm64 KVM 代码。protected VM 并不意味着 EL1 不参与；EL1 仍负责处理 ABI、验证参数、分配/管理 Host KVM 对象、管理 fd 和用户页。pKVM 的区别是：涉及 protected guest 的关键 ownership/map 状态，EL1 不能自行决定，必须通过 host-hypercall 请求 EL2，由 EL2 作为最终执行/约束方。

因此，正确因果链不是“一个 ioctl 对应一个 pKVM 函数”，而是：

| userspace 操作 | 当下的 EL1 工作 | 随后可能发生的 pKVM/EL2 工作 |
|---|---|---|
| `KVM_CREATE_VM(bit31)` | `/dev/kvm` ioctl -> `kvm_create_vm()` -> `kvm_arch_init_vm()`；Host 分配 `struct kvm`，共享 Host KVM metadata，并调用 `pkvm_init_host_vm(kvm, type)`。 | `pkvm_init_host_vm()` 检查 protected mode 并将 `kvm->arch.pkvm.enabled = true`；它标记“这是一台 pVM”，但尚未创建完整 hyp VM。 |
| `KVM_ENABLE_CAP(...SET_FW_IPA...)` | EL1 的 `pkvm_vm_ioctl_set_fw_ipa()` 检查状态。 | 这里只写 `pvmfw_load_addr` 字段；没有立即 donation/load firmware。真正映射发生在后来的 guest fault。 |
| `KVM_SET_USER_MEMORY_REGION` | EL1 注册 memslot，维护 Host 内存元数据。 | 对 pVM 而言，普通用户页不会在这一步全部直接交给 EL2；实际 page pin/donation/map 可延迟到 guest 首次访问。 |
| `KVM_CREATE_VCPU` | EL1 创建 `struct kvm_vcpu`、run page、vCPU fd，并调用架构 vCPU 初始化代码。 | 这仍主要是 Host-side object setup；完整 hyp VM/vCPU 对象尚未由首次运行路径建立。 |
| `KVM_ARM_VCPU_INIT` | EL1 从 userspace 复制并校验 `kvm_vcpu_init`，建立初始 CPU 状态与 feature 配置。 | 它为运行做准备；当前实现中 full `pkvm_create_hyp_vm()` 在首次 run preparation，而不是这个 ioctl 本身。 |
| 首次 `KVM_RUN` | vCPU fd ioctl -> `kvm_arch_vcpu_ioctl_run()`；首次运行准备会检查 timer、PMU、VGIC 等。 | `pkvm_create_hyp_vm()` 在此创建 hyp VM handle、调用 `__pkvm_init_vm`，并为各 vCPU 建立 hyp 侧对象。 |
| guest 缺页 | `KVM_RUN` 尚未返回 userspace；EL1 的 `pkvm_mem_abort()` pin/检查当前 guest 页。 | EL1 发 #23 `__pkvm_host_map_guest`，EL2 donation/map 该 guest page；这是当前 EL2 coverage MVP 的采集边界。 |
| 关闭 vCPU/VM fd | EL1 处理 fd refcount 与 KVM object 销毁；最后一个 VM 引用释放后进入 `kvm_arch_destroy_vm()`。 | `pkvm_destroy_hyp_vm()` 触发 hyp VM teardown；它发生在 close 路径，不是某个普通配置 ioctl 的立即结果。 |

最重要的直觉是：**ioctl 启动了状态机；真正的 pKVM 工作会按资源生命周期和 guest 的实际行为延迟发生。** `SET_FW_IPA` 只是记下 firmware 应放在哪里，首次 guest fault 才导致 #23；`KVM_CREATE_VM` 只是创建 Host-side VM，首次 `KVM_RUN` 才导致 `pkvm_create_hyp_vm()`；close fd 才导致 destroy。

#### 从 executor 到 EL2 再到 close：一台 pVM 的完整因果链

下面把一次受控 pVM 测试按真正的发生顺序展开。这里的重点不是记住每个函数名，而是区分三件事：**哪一个 userspace ioctl 触发了事情、EL1 当下做了什么、以及 EL2 是否在这一步实际运行。**

**0. executor 先打开 `/dev/kvm`。** `openat$kvm()` 返回 `fd_kvm`。它只是 KVM 设备的控制 fd；此时没有 VM、没有 vCPU，也没有任何 EL2 pVM 对象。

**1. `KVM_CREATE_VM` 创建的是 Host 的 VM 壳子。** executor 对 `fd_kvm` 发出带 bit 31 的 `KVM_CREATE_VM`。通用 KVM 层从设备 ioctl 进入 `kvm_create_vm()`，随后调用 arm64 的 `kvm_arch_init_vm()`。这一步在 EL1：它分配并初始化 `struct kvm`，建立 stage-2 的 Host-side 管理结构，并调用：

```c
ret = kvm_share_hyp(kvm, kvm + 1);
ret = pkvm_init_host_vm(kvm, type);
```

`pkvm_init_host_vm()` 识别 protected bit，检查机器已在 protected KVM 模式，然后把 `kvm->arch.pkvm.enabled` 置为真。于是 Host 知道“这是一台 pVM”。这里可能有把 Host metadata 共享给 hyp 的小型 host-hypercall，但**完整的 hyp VM 尚未创建**；所以不能把 `KVM_CREATE_VM` 简化成“立即在 EL2 创建一台 VM”。

**2. firmware 设置和 memslot 注册先建立 EL1 状态，通常不立刻执行 firmware。** `KVM_ENABLE_CAP` 的 `SET_FW_IPA` 进入 `pkvm_vm_ioctl_set_fw_ipa()`，只是在允许的时机把 firmware IPA 记入 `pvmfw_load_addr`。同样，`KVM_SET_USER_MEMORY_REGION` 先在 EL1 注册 memslot：哪个 guest IPA 范围对应哪一段 Host userspace memory。此时并没有因为“登记了 4 MiB memslot”就把 4 MiB 全部 donation 到 EL2。pVM 的页可以按需处理，实际 map 会等 guest 真正访问该 IPA。

这正是 firmware smoke 中 `SET_FW_IPA(0x7fc00000)` 的意义：它不是把 pvmfw 装入 EL2，而是告诉之后的 fault path，“当 guest 第一次取这个 IPA 时，应该按 pvmfw 的规则处理”。

**3. `KVM_CREATE_VCPU` 和 `KVM_ARM_VCPU_INIT` 让 Host 有一个可运行的 vCPU。** 前者创建 `struct kvm_vcpu`、vCPU fd 和 userspace 可 `mmap` 的 `struct kvm_run` 页；arm64 的 `kvm_arch_vcpu_create()` 也会共享 vCPU 的 Host metadata。后者把 userspace 提供的 `struct kvm_vcpu_init` 复制、校验并写入 vCPU 初始状态，最终标记 `VCPU_INITIALIZED`。它们仍主要在 EL1 做 ABI 和对象准备。尤其要注意：安全的 `VCPU_INIT` 是为了避开已知的共享 arm64-KVM warning；它不是“已经让 guest 在 EL2 跑起来”。

**4. 第一次 `KVM_RUN` 才触发完整的 hyp VM/vCPU 建立。** vCPU fd 的 `KVM_RUN` 先经过通用 KVM，再进入 `kvm_arch_vcpu_run_pid_change()`。它会检查 vCPU 已初始化、准备 timer/PMU/VGIC 等首次运行所需状态；对于 protected KVM，首次运行还会调用：

```c
ret = pkvm_create_hyp_vm(kvm);
```

这个函数创建 hyp VM handle，并通过 host-hypercall 调用 EL2 的 `__pkvm_init_vm`，再为已有 vCPU 建立 hyp 侧对象。因此，这才是“Host-side KVM 对象变成实际 pVM hyp 对象”的关键边界。

`syz_kvm_vcpu_run_immediate$arm64` 利用了一个很有用的顺序：`pkvm_create_hyp_vm()` 在 `immediate_exit` 检查之前。因此即使它随后以 `-EINTR` 快速返回，依然能覆盖首次运行的 Host→hyp 创建路径；但 guest 没有真正执行，也就不会触发后面的缺页和 #23。

**5. 只有 real `KVM_RUN` 让 guest 执行后，才会发生 #23。** `syz_kvm_run_fw_fault$arm64` 与 immediate helper 的区别就在这里：它令 `run->immediate_exit = 0`，让 pvmfw 的第一条执行路径真正开始。guest 首次取 firmware IPA `0x7fc00000` 时，页面还没有映射，硬件异常回到 EL2/Host 的 pKVM fault handling；EL1 随后进入 `pkvm_mem_abort()`。

`pkvm_mem_abort()` 先确认 fault IPA、pin 对应的 Host page、决定权限；然后通过包装函数发起编号为 **#23** 的 Host hypercall：

```c
ret = pkvm_host_map_guest(pfn, gfn, nr_pages, prot);
/* 内部：kvm_call_hyp_nvhe(__pkvm_host_map_guest, ...) */
```

这里的 #23 是 Rust hyp `HOST_HCALL` 分发表中 `__pkvm_host_map_guest` 的编号。它**不是**第 23 个 syscall、不是第 23 次 fault，也不是 guest 主动发出的 HVC。它表示“EL1 请求 EL2 把已经验证/pin 的 Host page 按 pVM 规则映射给 guest”。这个 #23 正是当前 Stage-2 MVP 选择的 EL1→EL2 覆盖边界。

**6. #23 执行期间，EL2 只产生日志式 PC，不认识 Linux task。** EL2 的 Rust pKVM 代码在处理 `__pkvm_host_map_guest`、firmware donation、页表和 TLB 工作时，SanCov 插桩会调用 `__sanitizer_cov_trace_pc()`。callback 将原始返回地址追加到 owner CPU 的一页 shared ring。EL2 没有 Linux `current`，也不需要记录“这是哪一个 syzkaller program”。

这里有两种很容易混淆的 PFN：传给 #23 的 `pfn` 是**要映射给 guest 的内容页**；coverage ring 的 PFN 是一页专门由 Host 分配、share 给 EL2 的**观测缓冲区**。两者完全不同，不能理解成“EL1 返回时把 guest page 写成覆盖率数据”。

**7. #23 返回 EL1 后，EL1 把 ring 数据写入当前线程的 KCOV。** 当前 MVP 在 `pkvm_mem_abort()` 只包住上面那一次 #23 调用：调用前清空 `count` 并打开 ring；调用返回后关闭 ring，acquire 读取 PC 数组。随后 `pkvm_cov_runtime_to_link()` 把运行时 hyp VA 还原为 `vmlinux` 中的 link address，`kcov_add_pcs()` 再把这些 PC 追加到此刻 `current` 的 KCOV area。

因为 #23 是同步的：同一个 executor worker 进入 `KVM_RUN`，在同一调用栈上执行 `pkvm_mem_abort()`，等待 EL2 返回后立即 drain，所以这里的 `current` 仍是发起该次 `KVM_RUN` 的 executor worker。测试归属在 EL1 恢复，不靠 EL2 的 task id。这也是 MVP 强制单 CPU、串行 smoke 的原因：先保证一个 ring 在一个明确的 worker/CPU 上没有混入其他调用。

**8. `KVM_RUN` 终于返回 executor，syzkaller 才读取 KCOV。** 对 pvmfw smoke，EL2/guest 继续执行并最终产生 `KVM_EXIT_MMIO`，因此 `KVM_RUN` 返回 0。executor 现在可以从该线程的 KCOV mmap area 取走普通 EL1 PC 和刚刚由 `kcov_add_pcs()` 追加的 EL2 PC；syzkaller 对 arm64 PC 使用“返回地址减 4”回到 SanCov `bl` call site，再借同一个 debuginfo `vmlinux` 将 `__kvm_nvhe_` 地址解析为 Rust `.rs:line`。这就是 smoke 中“87 个 EL2 PC、其中 77 个解析到 Rust 源码”的完整来历。

**9. vCPU fd 和 VM fd 的 close 导致 teardown。** vCPU fd 持有对 VM 的引用，因此测试必须先 `munmap` run page、关闭所有 vCPU fd，最后再关闭 protected VM fd。最后一个 VM 引用释放后，EL1 才进入 `kvm_arch_destroy_vm()`，继而 `pkvm_destroy_hyp_vm()`；它通过一组 teardown host-hypercall 释放/reclaim hyp 侧状态。这也是为什么“某个 `close()` 返回成功”本身不足以证明 pVM 销毁，之前才用 probe 验证最终 destroy 确实被调用。

可以把整条链压缩为：

```text
ioctl 创建 Host 对象
  -> 首次 KVM_RUN 创建 hyp VM/vCPU
  -> guest 真跑并访问未映射 IPA
  -> EL1 pkvm_mem_abort
  -> #23 进入 EL2 做 page map/donation，同时写 PC 到 coverage ring
  -> 返回 EL1 drain 到发起线程的 KCOV
  -> KVM_RUN 返回 executor，syzkaller 读取并符号化
  -> close 最后一个 VM 引用，进入 hyp teardown
```

当前 ring 只在 #23 前后打开和 drain。因此不应说“所有 pKVM hypercall 都已经有 EL2 KCOV”：创建、首次 init、teardown 等路径可以进入 EL2，但暂时只有 #23 的 EL2 执行被送回 syzkaller。以后若要覆盖其他边界，需要为每个边界单独定义清空、采集、归属和 teardown 规则。

### 3.2 Host campaign 的证据

受控 Host campaign 已在 N90 上完成：

- corpus：`0 -> 93`；
- manager host KCOV signal：`0 -> 6407`；
- 执行量：`300k+`，约 `39 exec/s`；
- 约两小时，无 crash。

`6407` 是 syzkaller 的 Host KCOV signal，不是 EL2 Rust 行覆盖。它包含 executor 和 Host KVM 的信号；目标侧已覆盖 `pkvm_init_host_vm`、`pkvm_create_hyp_vm`、`pkvm_destroy_hyp_vm` 等 EL1 Host 函数。

### 3.3 已知 Host warning 的处理方式

- BUG1 是通用 arm64 KVM timer/PMU_V3 路径的 warning；当前通过安全 `VCPU_INIT`、allowlist 和 fresh corpus 避开，未宣称已修复。
- BUG2 最终关联 normal VM dirty logging 的 TLB flush 路径，而不是 protected VM 的 EL2 缺陷；当前 composite 只构造 bit-31 protected VM，避免污染受控 slice。

这两个问题保留为发现和证据，不是当前 pKVM Host lifecycle 里程碑的阻塞项。

## 4. 已完成的 EL2 最小闭环（Stage 2 MVP）

实验 kernel 增加了 `CONFIG_PKVM_EL2_COV`。它只面向 fuzz kernel，默认关闭，并要求：

- `KVM && X1_RMS && KCOV`；
- `!RANDOMIZE_BASE`：EL2 runtime hyp VA 必须能还原为 vmlinux link address；
- `!PREEMPT_RT`：当前 RCU/`mmu_lock` 生命周期假设依赖非 RT 的 preempt-off 语义。

实现分为四部分：

1. Rust hyp 加入 `-C debuginfo=2` 和 SanCov trace-pc；
2. EL2 的 `__sanitizer_cov_trace_pc()` 将原始返回地址写进 per-CPU shared ring；
3. EL1 在 `pkvm_mem_abort()` 包围 #23 `__pkvm_host_map_guest` 的一次调用，arm ring、执行 hypercall、随后 drain；
4. drain 使用 `pkvm_cov_runtime_to_link()` 还原地址，再用 `kcov_add_pcs()` 写入当前 task 的 KCOV。

下面按“为什么需要、代码怎样做、它保证什么、它还不保证什么”展开这四部分。

### 4.1 Rust hyp 的 DWARF 与 SanCov：让 EL2 产生可解释的 PC

**为什么需要它。** 普通 Host KCOV 的编译器回调只覆盖 EL1 Linux 代码。Rust hyp 是单独构建、单独链接到 nVHE image 的 crate；如果不单独给这个 crate 加插桩，EL2 即使实际执行，也不会产生任何 coverage PC。如果只有插桩而没有 DWARF，最终虽然能看到地址，却只能得到 Rust codegen unit 名称或 `?:?`，不能解释为 `pkvm.rs:line`。

实际入口是 `arch/arm64/kvm/hyp/nvhe/rust/build_rust.sh`。它先保留原有的 freestanding/release 编译选项：

```sh
export RUSTFLAGS="-C relocation-model=static -C code-model=small -C opt-level=3 \
  -C panic=abort -C force-unwind-tables=no -C target-feature=-neon"
```

只有在 kernel build 导出的 `CONFIG_PKVM_EL2_COV` 非空时，才追加：

```sh
export RUSTFLAGS="$RUSTFLAGS \
  --cfg=CONFIG_PKVM_EL2_COV \
  -C debuginfo=2 \
  -C passes=sancov-module \
  -C llvm-args=-sanitizer-coverage-level=3 \
  -C llvm-args=-sanitizer-coverage-trace-pc"
export CARGO_PROFILE_RELEASE_DEBUG=2
```

这些参数各自有不同职责：

- `-C debuginfo=2` 与 `CARGO_PROFILE_RELEASE_DEBUG=2` 让 release Rust 对象携带可用于 addr2line 的 DWARF 行表；两者共同构成已经验证的当前构建配方。
- `-C passes=sancov-module` 显式启用 LLVM 的 SanCov module pass。只有 `llvm-args` 而没有这个 pass，不会产生 trace-pc 调用。
- `-sanitizer-coverage-trace-pc` 令被插桩的 Rust 基本块在入口处调用 trace-pc callback；在 AArch64 中最终是类似 `bl __sanitizer_cov_trace_pc` 的调用点。
- `--cfg=CONFIG_PKVM_EL2_COV` 不只是装饰。Rust 的 `#[cfg(CONFIG_PKVM_EL2_COV)]` dispatch 和 extern 声明也依赖它；漏掉它会造成“C side 已经有 callback/对象，但 Rust 中 setup hypercall 根本没有被编进来”的半成品。

这个功能被 Kconfig 严格限制：

```text
config PKVM_EL2_COV
    depends on KVM && X1_RMS && KCOV
    depends on !RANDOMIZE_BASE
    depends on !PREEMPT_RT
    default n
```

`hyp/nvhe/Makefile` 也只有在该 config 为 `y` 时才将 `cov.o` 链入 hyp：

```make
hyp-obj-$(CONFIG_PKVM_EL2_COV) += cov.o
```

所以 config-off kernel 不携带额外 callback、`kcov_add_pcs()` 或新 hypercall；它不是对普通 pKVM kernel 的无条件改造。

**这里保证什么。** Rust EL2 基本块可以产生 PC，最终 vmlinux 中保留 Rust `.rs:line` 信息。实际 smoke 已证明 `handle___pkvm_host_map_guest` 可以解析到 `hyp_main.rs:1069`。

**这里还不保证什么。** 编译期插桩本身不等于 PC 能回到 Linux；它只解决“EL2 有可记录的 PC”和“PC 将来可解释”两个前提。PC 的跨异常级传递由后面三部分完成。

### 4.2 EL2 callback 与共享 ring：在没有 `current` 的地方只记录 PC

**为什么需要它。** EL2 不能安全地调用普通 Linux KCOV callback。普通 callback 的设计前提是能访问 Linux task 的 `current->kcov_area`；EL2 不具备这个 task 上下文，地址空间也不同。因此 EL2 producer 的职责必须收缩到最小：只把“我刚刚经过了哪个 EL2 PC”写到一块双方都能访问的内存，绝不在 EL2 识别测试程序或操作 Linux task。

这里要特别区分：**EL1 和 EL2 都有名为 `__sanitizer_cov_trace_pc()` 的 callback，但它们是两份不同实现。** 名字相同不是复用同一个函数，而是因为两边的编译器 SanCov/KCOV 插桩都约定调用这个 ABI 名称；EL1 kernel image 与 nVHE hyp image 是独立链接的代码镜像，nVHE 链接时会给 hyp 的全局符号加前缀，因此不会冲突。实验 kernel 的最终 `vmlinux` 中实际同时存在：

```text
__sanitizer_cov_trace_pc              # EL1：kernel/kcov.c
__kvm_nvhe___sanitizer_cov_trace_pc   # EL2：arch/arm64/kvm/hyp/nvhe/cov.c
```

它们的差别如下：

| 项目 | EL1 普通 KCOV callback | EL2 pKVM callback |
|---|---|---|
| 定义位置 | `kernel/kcov.c` | `arch/arm64/kvm/hyp/nvhe/cov.c` |
| 谁调用它 | 被 KCOV 插桩的普通 Linux kernel 基本块 | 被 SanCov 插桩的 Rust hyp 基本块 |
| 能否使用 `current` | 能；它是当前 executor worker 对应的 Linux task | 不能；EL2 没有 Linux task / `current` |
| 直接写到哪里 | `current->kcov_area` | 当前 CPU 的 Host-EL2 shared ring |
| 何时进入 syzkaller 的 KCOV | callback 当场写入 | 返回 EL1 后，由 `pkvm_cov_end()` 调 `kcov_add_pcs()` 追加 |

因此 `kcov_add_pcs()` 也不是第三个 SanCov callback。它是本实验新增的 **EL1 bridge helper**：EL2 callback 已经结束后，EL1 从 ring 取出一批 PC，再由它按 KCOV 的安全写入规则补进当前 task 的 KCOV area。最终 syzkaller 看到的是两类 PC 的并集：普通 EL1 callback 直接记录的 Host PC，加上这个 bridge 补入的 EL2 PC。

共享布局在 `arch/arm64/include/asm/kvm_pkvm_cov.h`：

```c
#define PKVM_COV_RING_PCS 511

struct pkvm_cov_ring {
    __u32 count;
    __u32 flags;
    __u64 pcs[PKVM_COV_RING_PCS];
};
_Static_assert(sizeof(struct pkvm_cov_ring) == 4096, ...);
```

它目前严格是一页：8 字节 header 加 511 个 64 位 PC。名称叫 ring，但 MVP 实际上是**追加式、有上限、不会回绕覆盖旧数据的 buffer**：满了以后丢弃新 PC，并置 `PKVM_COV_FLAG_OVERFLOW`。这也是当前 511 条容量问题的来源。

EL2 callback 位于 `arch/arm64/kvm/hyp/nvhe/cov.c`：

```c
void notrace __sanitizer_cov_trace_pc(void)
{
    struct pkvm_cov_ring *r = __this_cpu_read(pkvm_cov_ring);

    if (!r || !(smp_load_acquire(&r->flags) & PKVM_COV_FLAG_ENABLED))
        return;
    if (__this_cpu_read(pkvm_cov_in_cb))
        return;
    __this_cpu_write(pkvm_cov_in_cb, true);

    count = READ_ONCE(r->count);
    if (count < PKVM_COV_RING_PCS) {
        r->pcs[count] = (u64)__builtin_return_address(0);
        smp_store_release(&r->count, count + 1);
    } else {
        smp_store_release(&r->flags,
            READ_ONCE(r->flags) | PKVM_COV_FLAG_OVERFLOW);
    }
    __this_cpu_write(pkvm_cov_in_cb, false);
}
```

这里有四个设计点：

1. `notrace`：callback 自己不能再被 SanCov 插桩，否则会递归调用自己。当前 `cov.c` 所在 C hyp build 本身也不启用 SanCov，形成双重保护。
2. `__builtin_return_address(0)`：记录的不是 callback call-site 本身，而是 `bl` 指令之后的返回地址。这个选择与普通 KCOV 的约定一致，后面由 syzkaller 在 ARM64 上减 4 回到真正的 cover point；EL1 不应提前减 4。
3. `pkvm_cov_in_cb`：若异常或嵌套路径再次进入已插桩 Rust 代码，宁可丢一个 PC，也不让两个 producer 并发修改 `count`/`pcs[]`。
4. release/acquire：EL2 先写 `pcs[count]`，再 release-store 更新 `count`；EL1 acquire-load 到新 count 后，才能安全读取相应的 PC。这不是可有可无的 barrier，在 ARM 内存模型下它保证“看见 count 就看见该 PC”。

EL2 如何拿到这块页也不是直接把任意 Host 指针拿来用。EL1 先通过公开 API `kvm_share_hyp()` 将一页共享给 hyp，随后在 owner CPU 上发 config-gated `__pkvm_cov_setup(pfn)` hypercall。Rust dispatch 对这个新 id 采取“固定表之前显式判断”的方式，避免改变现有 66 项 `HOST_HCALL` 表或 unit-test id：

```rust
if id == __KVM_HOST_SMCCC_FUNC___pkvm_cov_setup {
    let pfn = declare_reg::<u64>(self, 1);
    let ret = pkvm_cov_setup(pfn);
    self.regs.regs[0] = SMCCC_RET_SUCCESS as u64;
    self.regs.regs[1] = ret as i64 as u64;
    return;
}
```

EL2 的 `pkvm_cov_setup()` 再把 PFN 转为 hyp virtual address，调用 `hyp_pin_shared_mem()`，并把该页记录到本 CPU 的 `DEFINE_PER_CPU(pkvm_cov_ring)` 指针中。拆除时先清 per-CPU 指针，再 unpin，避免 callback 在 unpin 后仍使用旧指针。

**这里保证什么。** EL2 有一个不依赖 Linux `current` 的、每 CPU 的 PC producer；Host 和 hyp 对同一页的 ownership/pin 生命周期有明确顺序。

**这里还不保证什么。** 当前只配置了一页、一个 CPU，且不做 producer 去重；满后直接丢 PC。它也不提供多 CPU 或多测试程序之间的完整覆盖率服务。

### 4.3 #23 `pkvm_mem_abort()` hook：限定采样窗口并在 EL1 恢复归属

**为什么选 #23。** #23 是 `__pkvm_host_map_guest`。protected guest 在实际运行时对尚未映射的 guest IPA 发生 fault，EL1 的 `pkvm_mem_abort()` 处理它，随后通过该 hypercall 请 EL2 donation/map 这页 guest memory。N90 的 pvmfw smoke 已证明 firmware IPA `0x7fc00000` 会稳定到达这条路径，并能干净地以 MMIO exit 返回。

这比全局包住 `kvm_call_hyp_nvhe()` 更准确。全局包裹会混入 boot、非 fuzz 线程和无关 hypercall；而在唯一的 Host 调用点前后采样，EL2 PC 能归到具体的 `KVM_RUN` 调用。

这里有两个很容易混淆的概念：

- **#23 是 host-hypercall 编号。** 它在 Host SMCCC/hypercall ABI 中表示 `__pkvm_host_map_guest`；不是系统调用号、不是第 23 次 fault，也不是 guest HVC 编号。
- **coverage ring PFN 与 #23 的 guest-page PFN 不同。** 前者是启用 coverage 时 EL1 专门分配的一页共享页，传给 `__pkvm_cov_setup(ring_pfn)`，EL2 将它保存为本 CPU 的 coverage buffer；后者是 `pkvm_host_map_guest(pfn, gfn, ...)` 的参数，表示当前要为 guest fault donation/map 的那一页普通 guest memory。EL2 callback 始终向前者写 coverage PC，绝不向正在映射的 guest 页写 coverage。

#23 也不是每一次 `KVM_RUN` 都一定发生。只有 guest 实际访问了尚未被 pKVM 映射的 IPA 才会触发它；`immediate_exit=1` 在 guest 执行前返回，因此不会走到 #23。选择它的原因是 pvmfw smoke 已经证明：在这个受控、会返回的 firmware-fault 场景中，它可靠到达 Rust donation/map 路径，且它给出了一个窄而可归属的 EL1->EL2 边界。

`mmu.c` 的实际改动位于 `pkvm_mem_abort()`，在拿到 `write_lock(&kvm->mmu_lock)` 后：

```c
struct pkvm_cov_ring *cov = pkvm_cov_begin();

ret = pkvm_host_map_guest(pfn, *fault_ipa >> PAGE_SHIFT,
                          page_size >> PAGE_SHIFT, KVM_PGTABLE_PROT_R);
if (cov)
    pkvm_cov_end(cov);
```

按时间顺序，coverage 数据不是“等 #23 返回后才由 EL2 写到 PFN 里”，而是：

```text
启用 coverage 时：EL1 分配并共享 ring page，EL2 保存它的 per-CPU 指针

本次 #23 前：EL1 begin() 清 count、置 ENABLED
本次 #23 内：Rust EL2 callback 将 PC 写到已经共享的 ring page
本次 #23 返回 EL1 后：EL1 end() 读取同一 ring page，转换 PC，再复制到 current->kcov_area
```

如果 #23 没有返回，例如底层执行挂住，则 `pkvm_cov_end()` 无法执行，coverage 也不会作为本次 syscall 的结果交给 syzkaller；这正是选择“已验证会干净返回”的 pvmfw 路径作为 MVP smoke 的原因。

还要区分“coverage 目标”和“采集边界”。coverage 的目标不是 hypercall 编号本身，而是 hypercall 执行期间经过的 **Rust EL2 基本块**；#23 只是当前选择的、可精确归属的开门/关门位置。当前 Rust hyp 虽然整体插入了 SanCov，但 ring 只在 #23 前后被打开，因此当前 EL2 coverage 只包含 #23 执行窗口内的 Rust 路径。#21、#34/#35、#36-#38 等其他 host-hypercall 尚未有各自的采集窗口。

数据也不是在 #23 返回时由 syzkaller 直接读取。准确顺序为：

```text
EL2 执行 #23 时：callback 写 shared ring
#23 返回 EL1 后：pkvm_cov_end() 在 kernel 内 drain ring -> current->kcov_area
整个 KVM_RUN ioctl 返回 executor 后：syz-executor 从自己的 KCOV mmap 区读取本次调用 coverage
executor 完成结果上报后：syz-manager 才在控制机收到 coverage/signal
```

普通 EL1 KCOV 与这批 EL2 PC 最终共存在同一个 executor task 的 KCOV area 中：前者由普通 kernel KCOV callback 自动写入，后者由 `kcov_add_pcs()` 在 #23 返回后补写。

#### 一次真实 pvmfw `KVM_RUN` 的时间线

下面以 `syz_kvm_run_fw_fault` 为例。要先分清两种时间尺度：ring 的**建立/拆除**通常每次 smoke 一次；ring 的**清空/开启/读取**则在每一次 #23 map hypercall 周围重复。

| 时刻 | EL1 Host（含 executor/kernel） | EL2 Rust hyp | shared ring / KCOV |
|---|---|---|---|
| A. 启用前 | executor worker 建立自己的 KCOV mmap，随后各 syscall 可在 trace-PC 模式下执行。 | 尚无 ring 指针。 | 无 EL2 coverage 数据。 |
| B. `echo 1 > .../pkvm_cov/enable` | EL1 分配一页 ring，`kvm_share_hyp()`，并在当前 CPU 发 `__pkvm_cov_setup(ring_pfn)`。 | `pkvm_cov_setup()` 将 ring PFN 转为 hyp VA，pin 该页，存入这个 CPU 的 per-CPU 指针。 | ring 已存在，但 `ENABLED` 尚未为某次 #23 打开。 |
| C. helper 的前半段 | executor 经 ioctl 创建 protected VM、注册 firmware window、`SET_FW_IPA`、创建/init vCPU。普通 EL1 KCOV 可记录这些 Host 路径。 | 生命周期相关 EL2 handler 可能执行，但当前 ring 尚未 arm，因此它们的 Rust PC 不被这套 MVP 收集。 | task KCOV 已有普通 Host PC；ring 仍关闭。 |
| D. 进入 `KVM_RUN` | executor worker 发 ioctl；Host KVM 准备运行 vCPU。 | hyp 进入/运行 vCPU，guest/pvmfw 开始执行。 | ring 仍关闭。 |
| E. firmware 首次缺页 | Host 发现 guest IPA 尚未映射，进入 `pkvm_mem_abort()`，准备 guest-page PFN/GFN，获取 `write_lock(&kvm->mmu_lock)`。 | 尚未执行这一次 #23 map handler。 | ring 已存在但尚未打开。 |
| F. #23 前 | `pkvm_cov_begin()` 检查当前 CPU 是 owner 且当前 executor task 开着 trace-PC KCOV；随后清 `count`、release-store `ENABLED`。 | 看到同一页的 `ENABLED` 后才会记录。 | 本次 #23 的临时 PC 列表从空开始。 |
| G. #23 内 | EL1 调 `pkvm_host_map_guest(guest_page_pfn, gfn, ...)`，即 Host hypercall #23。 | Rust `handle___pkvm_host_map_guest` 及其 donation/map 路径执行；每个被插桩基本块 callback 将 PC 追加到 ring。 | EL2 写 `pcs[]`，最后 release-store `count`。满 511 条后丢新 PC 并标 overflow。 |
| H. #23 返回 | HVC 返回到同一个 EL1 调用栈；`pkvm_cov_end()` 先关闭 `ENABLED`，再 acquire-load `count` 和 `pcs[]`。 | 本次 #23 不再向 ring 写入。 | EL1 将 runtime PC 转为 link PC，并通过 `kcov_add_pcs()` 追加到**当前 executor worker** 的 KCOV。 |
| I. guest 继续 | EL1 完成这页的 Host bookkeeping，然后让 guest 继续。若 pvmfw 又访问未映射页，回到 E，产生下一次 #23。 | 再次执行新的 map/donation 工作。 | 每一轮 E-H 都清空和 drain 一次 ring；同一个 `KVM_RUN` 的 task KCOV 累积所有轮次的 EL1/EL2 PC。 |
| J. `KVM_RUN` 结束 | pvmfw 这次最终以预期 MMIO exit 返回；helper 收尾 vCPU、VM。 | guest 不再执行。 | executor 读取 KCOV mmap，获得本次 syscall 的普通 EL1 PC 加已 drain 的 EL2 PC。 |
| K. 上报/关闭 | executor 将执行结果和 coverage 交给 syz-execprog/manager。之后 `echo 0` 会走 RCU + EL2 unregister + checked unshare 的安全 teardown。 | `pkvm_cov_setup(0)` 清除 per-CPU ring 指针、unpin 页。 | ring 不再可写；只有确认 EL2 与 Host 都放弃它后才 free。 |

N90 smoke 中同一个 `KVM_RUN` 出现了 78 次 `pkvm_mem_abort`，因此 E-H 重复了多次；这解释了为什么单次 KVM_RUN 可以积累大量 EL2 PC，也解释了 ring 和 task KCOV 两层容量都可能打满。

这个位置有意满足四个条件：

- ring 只在 `pkvm_host_map_guest()` 的执行窗口内开启；
- `pkvm_cov_end()` 位于调用之后，因此 success 和 error 返回都被 drain；
- 原有 `ret` 不被 coverage 逻辑改写；
- 在非 RT kernel 上，`mmu_lock` 的 preempt-off 性质同时固定 CPU，并把 `begin() -> end()` 覆盖为 RCU reader window。

`pkvm_cov_begin()` 的核心判断是：

```c
ring = rcu_dereference_check(pkvm_cov_host_ring, !preemptible());
if (!ring)
    return NULL;
if (smp_processor_id() != pkvm_cov_owner_cpu)
    return NULL;
if (!kcov_current_trace_pc())
    return NULL;

WRITE_ONCE(ring->count, 0);
smp_store_release(&ring->flags, PKVM_COV_FLAG_ENABLED);
```

它说明“没有 KCOV consumer 时不付 EL2 callback 成本”“错误 CPU 不采样而不是错归属”“每个 #23 调用从空 buffer 开始”。随后 EL2 callback 在同一 CPU 追加 PC；HVC 返回后 `pkvm_cov_end()` 先 acquire flags、disarm，再 acquire count、读取 PC 并分批交给 KCOV。

为什么这里能使用 `current`？调用顺序是：executor worker 发出 `KVM_RUN` -> guest fault -> `pkvm_mem_abort()` -> EL2 map hypercall -> 返回同一个 EL1 调用栈 -> `pkvm_cov_end()`。因此 `kcov_add_pcs()` 执行时的 `current` 仍是发起该 `KVM_RUN` 的 executor worker。EL2 从头到尾都不需要、也不应该知道这个身份。

ring 启停也需要专门处理，因为这是 Host 与 hyp 共同拥有的一页：

```text
disable producer flag
-> rcu_assign_pointer(NULL)
-> synchronize_rcu()
-> 在 owner CPU 上 __pkvm_cov_setup(0)
-> checked unshare
-> free page
```

若 IPI、EL2 unregister 或 unshare 不能确认成功，代码会保持 disabled 并故意泄漏该页，而不是把仍可能被 EL2 映射的页交还给 page allocator。这是内存所有权安全优先于一页内存回收的选择。

**这里保证什么。** 对 #23 这个同步边界而言，EL2 PC 能精确归入发起它的 `KVM_RUN` 调用；错误 CPU 会 skip，而不是把别的程序的覆盖率混入当前调用。

**这里还不保证什么。** 当前只有 #23 被包围。创建/share、首次 run 创建 hyp VM/vCPU、teardown 等 boundary 的 EL2 内部 PC 仍没有采集窗口。单 CPU owner 也意味着普通 manager 并发执行时会丢 coverage。

### 4.4 runtime PC、link address、KCOV 与 syzkaller symbolization：把“EL2 地址”变成“Rust 源码行”

EL2 callback 写进 ring 的 PC 是 **运行时 hyp VA**。而 `vmlinux`、ELF cover point 和 addr2line 使用的是 nVHE 链接地址。二者不能直接混用；更不能用 `hyp_physvirt_offset`，因为它描述的是 hyp VA 与物理地址的关系，而不是 hyp code runtime VA 与 link address 的关系。

Host helper `pkvm_cov_runtime_to_link()` 使用整段连续 hyp `.text` 的锚点做反变换：

```c
unsigned long link_start = (unsigned long)__hyp_text_start;
unsigned long hyp_start =
    (unsigned long)kern_hyp_va(lm_alias((unsigned long)__hyp_text_start));
unsigned long text_size =
    (unsigned long)__hyp_text_end - (unsigned long)__hyp_text_start;

if (pc < hyp_start || pc - hyp_start >= text_size)
    return 0;
return link_start + (pc - hyp_start);
```

`lm_alias()` 很关键：`__hyp_text_start` 是 kernel-image mapping 下的符号，而 `kern_hyp_va()` 需要的是同一物理页的 linear-map alias。范围检查也不是装饰；只有 hyp `.text` 内的 PC 才能用这一常量偏移反推。范围外的地址直接丢弃，不能伪装成一个看似有效的 vmlinux 地址。

KASLR 被 Kconfig 禁止，是因为离线 symbolizer 必须使用与运行 image 对应的 link address。换言之，drain 和 symbolizer 必须对应同一次带 DWARF/SanCov 的构建；不能部署一个 kernel，却用另一次链接的 vmlinux 做符号化。

随后 `pkvm_cov_end()` 每次取到一个有效 link PC，就批量调用：

```c
kcov_add_pcs(batch, nb);
```

`kernel/kcov.c` 中的实现刻意复用普通 KCOV callback 的写入顺序：

```c
if (!check_kcov_mode(KCOV_MODE_TRACE_PC, current))
    return;
pos = READ_ONCE(area[0]) + 1;
if (pos >= current->kcov_size)
    break;
WRITE_ONCE(area[0], pos);
barrier();
area[pos] = pcs[i];
```

这里没有调用 `canonicalize_ip()`，因为传入的已是还原后的 nVHE **link address**；也没有在 kernel 中减 4。两点都关系到 syzkaller 的 cover-point 约定。

编译器产生的是：

```text
call-site:     bl __kvm_nvhe___sanitizer_cov_trace_pc
callback sees: return address = call-site + 4
ring/KCOV:     raw return address
syzkaller:     PreviousInstructionPC(ARM64) = raw - 4
report point:  原始 bl call-site
```

syzkaller 的 `pkg/cover/backend/elf.go` 专门把
`__kvm_nvhe___sanitizer_cov_trace_pc` 识别为 trace-pc callback，因此 ELF scanner 会把指向它的 `bl` 指令当作 CoverPoint。之后 `PreviousInstructionPC()` 对 ARM64 减去 4，`addr2line -afi -e vmlinux` 再将 CoverPoint 映射为 Rust `.rs:line`。

`pkg/cover/backend/pkvm_cov_test.go` 不只测试一个函数入口地址；它会从真实 debuginfo vmlinux 的 hyp `.text` 中解码一条实际 `bl callback` 指令，构造 `call-site + 4` 的 raw PC，模拟 runtime tag，执行 runtime->link 还原，然后验证 syzkaller 的 `-4` 精确回到原 call-site 并得到 Rust 源码行。这样，如果未来 kernel 端“好心”地预先减 4，测试会直接失败。

**这里保证什么。** 当前真机 smoke 已证明 EL2 runtime PC 能通过这条链路进入 coverfile，并有 77 个唯一 EL2 PC 映射到 Rust `.rs:line`。

**这里还不保证什么。** `kcov_add_pcs()` 在 task KCOV 区满时会直接停止追加；它当前不报告丢失计数。加上 511-PC EL2 buffer，这正是 Stage-2 campaign 前必须解决的双层容量问题。

关键源码位置：

- `/home/jose/common-stage2mvp/arch/arm64/kvm/pkvm_cov.c`：EL1 ring 生命周期、地址还原、drain；
- `/home/jose/common-stage2mvp/arch/arm64/kvm/hyp/nvhe/cov.c`：EL2 SanCov callback；
- `/home/jose/common-stage2mvp/kernel/kcov.c`：向 current task 的 KCOV 区批量追加 PC；
- `/home/jose/common-stage2mvp/arch/arm64/kvm/mmu.c`：#23 hook；
- `/home/jose/common-stage2mvp/arch/arm64/kvm/Kconfig`：实验配置约束。

ring 的安全生命周期是：Host 分配并 `kvm_share_hyp()` -> EL2 pin/register -> EL1 drain；关闭时先 disarm 和 RCU 置空，再等待 reader，确认 EL2 unregister，确认 unshare，最后才 free。任何远端状态不能确认的分支都宁可泄漏一页，也不释放仍可能被 hyp 映射的内存。

## 5. N90 Stage-2 smoke 实际证明了什么

`pkvm-lifecycle-fuzzing@b04ed35e0` 中的 smoke evidence 证明的不是“编译通过”，而是如下真实硬件闭环：

```text
executor KVM_RUN
  -> firmware IPA 0x7fc00000 的 guest fault
  -> pkvm_mem_abort / __pkvm_host_map_guest
  -> Rust EL2 SanCov
  -> shared ring
  -> EL1 drain / current task KCOV
  -> syz-execprog coverfile
  -> addr2line / Rust .rs:line
```

已满足的验收信号：

- 78 次 `pkvm_mem_abort` fault，第一笔 IPA 为 `0x7fc00000`，全部返回 0；
- pvmfw 实际运行并通过预期 MMIO exit 返回；
- owner CPU 为 0，测试线程在 CPU 0 上运行；
- coverfile 中有 87 个唯一 hyp-text PC；
- 其中 77 个映射到 `rust/src/*.rs:line`；
- 包含 #23 handler：`handle___pkvm_host_map_guest -> hyp_main.rs:1069`；
- 没有 BUG、owner-CPU skip、teardown-unconfirmed 或 unshare-unconfirmed。

因此可以明确说：**EL2 Rust 覆盖率到 syzkaller 的闭环已在真机通过。**

## 6. 当前主要缺口

### 6.1 两层容量截断

这是当前第一优先级。

EL2 ring 当前固定为一页：8 字节 header + 511 个 `u64` PC。smoke dmesg 至少记录了 4 次 `count=511` 的 ring overflow；后续 EL2 PC 会丢弃。

同时，`_prog1.1` coverfile 恰有 `524287` 项，等于 executor `kCoverSize = 512 << 10` 的上限；去重后仅有 5010 个唯一 raw PC。结合 `kcov_add_pcs()` 满 buffer 后直接停止追加的实现，这说明 task KCOV buffer 也已经构成实际瓶颈。

这两层问题必须分开处理：

- 增大 EL2 ring 能减少 producer 侧丢失；
- 增大 ring 后会更快给 task KCOV 施压；
- EL1 drain 去重只能保护 KCOV，不能恢复 ring 已丢的 PC；
- 当前已经每次 #23 返回后 drain，单次 hypercall 内超过 511 的问题不能靠“更频繁 drain”解决。

这里要区分 **raw PC** 和 **unique cover point**。syzkaller 最终真正关心的是 unique signal；`524287 -> 5010` 的聚合结果也说明整段执行存在大量重复。但当前 EL2 producer 是“先 append raw PC、再由 EL1 处理”的模型：如果 ring 在第 511 条后溢出，后面可能恰好有一个此前未出现的 unique PC。因此，只要保持 EL2 的简单 append-only producer，ring 容量首先仍要按单次 #23 的 **raw callback 高水位** 留出余量；不能只按事后看到的 unique PC 数决定容量。

同理，task KCOV 被打满的原因尚不能只归因于 EL2 bridge。普通 EL1 KCOV callback 与 `kcov_add_pcs()` 共用同一个 `current->kcov_area`；78 次 fault 中的 Host KVM 页表和 fault 路径本身也会持续写入。现有证据证明“整体重复很多”，但没有证明“KCOV 的主要压力来自哪一侧”。这需要下一步 A/B 测量，而不是先假定 EL1 drain 去重就能解决容量问题。

证据目前只能严格说明“至少四次记录到的 drain 饱和”。由于 overflow 日志是 ratelimited，不能据此断言 78 次 fault 每次都溢出。

从数据流看，这是两个彼此独立的截断层：

1. **上游 EL2 ring 截断。** 每次 #23 内，SanCov callback 先写一页、511 项的 ring；满后 PC 在进入 EL1 和 KCOV 之前已经丢失。增大 task KCOV 完全无法恢复这些 PC，只有 ring 容量或 producer 设计能处理它。
2. **下游 task KCOV 截断。** `kcov_add_pcs()` 与普通 EL1 KCOV 共用 `current->kcov_area`；executor 当前请求的 `kCoverSize = 512 << 10` 是这次 smoke 的 524287 项上限来源，而不是 KCOV ABI 的固定 512K 上限。增大 executor 请求的 area 可以探测这一层，但看不见上游 ring 已丢的 PC。

EL1 drain 去重仍是很有价值的后续减压阀，但它必须是 **预分配** 的。`pkvm_cov_end()` 位于 `kvm->mmu_lock` write-lock 区间，约束是 no-alloc/no-sleep；不能在这里临时 `kmalloc()` 或建立普通 hash table。正确形态是：在 `KVM_RUN` 入口或 coverage enable 阶段准备有界 scratch，以 generation 标记代替整表清零；drain 中只做无睡眠查找/插入。并且要新增真正的 per-`KVM_RUN` session 边界，否则现有的 per-#23 begin/end 只能做到 per-map 去重。

### 6.2 单 CPU、手工串行验证，不等于 manager campaign

MVP 只有一个 ring 和一个 owner CPU。手工 smoke 用：

```text
taskset -c 0
-procs=1
-threaded=0
```

这保证了测试正确，但 syz-manager 还没有 Stage-2 专用执行模式。仅设置 `procs=1` 不能阻止 executor 线程迁移 CPU，也不能自动关闭 threaded/collide 相关行为。

当前 owner guard 在错误 CPU 上选择 skip，因此优点是不会错归属；缺点是 campaign 会丢 signal。故当前阶段适合 smoke，不适合 manager 长跑。

### 6.3 当前只采集 #23 donation/map 路径

Rust hyp 虽然整体已经插入 SanCov，ring 只在 #23 `__pkvm_host_map_guest` 前后 arm。因此可见的是 firmware fault/donation/map 路径，而非所有 host->hyp boundary。

创建/share 的 #21、首次 run 的 #34/#35、关闭时的 #36-#38 等 EL2 路径，在 Host 侧已经走到，但尚没有各自可归属的 EL2 采集点。

### 6.4 smoke helper 不是 fuzzing 输入面

`syz_kvm_run_fw_fault$arm64` 标为 `no_generate`，这是刻意且正确的。它是固定、完整、可解释的闭环 smoke，不应该自动进入 corpus。

当前阶段的可安全变异输入面，应首先理解为 **Host 侧受约束的 KVM 配置和状态序列**，而不是任意 guest code。pvmfw 是固定 firmware，protected-guest 隔离也使普通 VM 的 SyzOS 内存通道不能直接复用。可探索的维度包括经过验证的 memslot 布局、IPA 范围、`SET_FW_IPA` 的合法取值，以及触发 donation/map 的次数和顺序。

这不等于把现有 `no_generate` smoke 直接去掉限制。固定 smoke 继续作为闭环回归测试；真正的 fuzz primitive 应另行建模为受控 composite，保持 protected-VM 生命周期、firmware window、错误清理与资源关闭顺序等前提。深度 protected-guest/HVC fuzzing 仍是后续独立工作。

### 6.5 文档与源码治理

- 主实现记录仍把 Stage 2 写成 deferred/dark，并写有“EL2 image carries 0 SanCov callbacks”的历史结论；这已被真机 smoke 推翻，必须补一段当前状态。
- Stage-2 kernel 原型仍是 `/home/jose/common-stage2mvp` 的未提交工作树；虽然 canonical patch、`stage2.config`、构建哈希和 MANIFEST 已能功能性复现 smoke build，但在准备长期维护或提交前，仍应整理为明确的 kernel 分支/提交。
- N90 的 ACPI/EFI 环境不适合原先基于 DT ramoops 的恢复设想；无人值守 campaign 前还需要解决 efi-pstore 与 isolated backend crash-log 约定的差异。

## 7. 推荐的后续顺序

### 第一步：两层反馈完整性度量，不开 EL2 manager campaign

补齐主实现记录，明确：

```text
Stage 1/1.5 Host campaign：已完成
Stage 2 MVP closed loop：已在 N90 通过
Stage 2 manager campaign：未完成
当前 EL2 coverage：存在 ring 与 task-KCOV 截断
```

这一阶段不启动 manager campaign，也不先落地最终 ring 扩容或 dedup 方案。它分为 1A、1A+、1B；三者共同回答“哪里先截断、谁在填满 KCOV、下一步应该改哪一层”。

#### 1A：当前内核上的 debugfs A/B 基线

现有 debugfs `kvm/pkvm_cov/enable` 就是 bridge 的运行时开关：关闭后 Host ring 为 `NULL`，`pkvm_cov_begin()` 返回 `NULL`，#23 仍执行、普通 EL1 KCOV 仍记录，但不会执行 EL2 drain/注入。因此这一步不需要重编 kernel。

每组 A/B 都必须满足：

- 用同一 build、同一固定 smoke、同一个 CPU；
- 写 `enable=1` 的进程也固定到该 CPU，因为 setup 时的 `get_cpu()` 决定 `owner_cpu`；
- `syz-execprog` 同样固定到 owner CPU，并保持 `-procs=1 -threaded=0`；
- 做多组配对运行，记录 raw 条目数、unique 条目数、hyp-text PC 数及是否达到 KCOV 上限。

OFF 组本身能回答“普通 EL1 路径是否已单独打满 KCOV”。ON/OFF 差值表示 **启用 bridge 后的总额外 KCOV 压力**，其中既包括注入的 EL2 PC，也包括 `pkvm_cov_begin/end()`、地址转换和批量追加本身的 EL1 footprint；它不是纯 EL2 PC 数。若两组都到达 KCOV 上限，coverfile 大小或 unique 集合都只能给出下界，不能据此完成 bridge 归因。

#### 1A+：仅重编 executor 的下游容量 probe

`kCoverSize` 由 executor 定义，并通过 `KCOV_INIT_TRACE64` 请求给 kernel；当前 kernel 的 KCOV 实现按请求大小 `vmalloc_user()` 分配，并没有 512K 的固定 ABI 上限。因此可以先把实验 executor 的 `kCoverSize` 从 `512 << 10` 增至 2M，必要时再增至 4M，重编并部署 executor，而不重编 kernel。

在相同 pinned smoke 下，这能观察下游 task KCOV 在旧 524287 项之后还会产生多少 raw PC、hyp-text PC 与 unique PC，并帮助判断“仅增大 area 是否足够”还是“必须同时做 dedup”。它仍不能替代 1B：EL2 ring 在 #23 内丢掉的 PC 不会因为 executor area 变大而重新出现。大 buffer 也会提高每个 KCOV task 的 vmalloc 和 executor 地址空间占用，因此只用于 `-threaded=0`、`procs=1` 的受控 smoke，不直接作为 campaign 默认值。

#### 1B：内核侧 loss accounting 与来源归因

只有内核计数才能看见 coverfile 截断点之后的量。计数必须是累计、可导出的，不能依赖 ratelimited dmesg。至少区分：

- EL2 callback 命中数；
- 每次 #23 的 raw callback 数、最大值、分位数和 overflow 次数；
- 成功写 ring 数与 ring 满后丢弃数；
- drain 次数；
- runtime->link 转换成功数与丢弃数；
- `kcov_add_pcs()` 请求写入数、实际写入数、KCOV 满导致的丢弃数。

当前 4 KiB ring 的布局不能无代价加入这些 EL2 统计字段；较自然的实现是单独的 host-shared stats page。若采用它，同一个 setup HVC 需要传入 ring PFN 与 stats PFN，并让 share、EL2 pin/register、unregister、unshare 和“失败则保守泄漏”的生命周期同时覆盖两页。

stats 的语义应以每次 #23 为单位：Host 在 begin 中先清零 per-#23 stats，再 release 地置 `ENABLED`；EL2 记录 callback hit 与满后 drop；Host 在 end 中 disarm 并 snapshot。这里不能只依赖现有 `flags` 的 acquire：在没有 overflow 的正常路径上，EL2 不会写 flags。EL2 必须为 stats 提供明确的 release publication，Host 用对应 acquire 读取，或提供等价的发布序列，才能保证看到与 PC/count 一致的统计值。

`kcov_add_pcs()` 当前为 `void`，且 KCOV 满后静默 `break`。这一步应让它或一个仅供 `CONFIG_PKVM_EL2_COV` 使用的 sibling helper 返回实际接受条目数，并区分“trace mode 已失效”和“area 已满”等原因；drain 才能准确累计 request/accepted/dropped。

本步的产物不是“unique PC 已经完整”的宣称，而是一张能决定后续方案的数据表：单次 #23 的 raw high-water、EL2 bridge 的净写入量、task KCOV 的来源拆分，以及每层实际丢失量。若 ring 仍会截断，就不能从现有样本精确推断 overflow 之后的 unique PC 数；这是下一步容量 probe 要解决的限制。

主实现记录中关于“Stage 2 deferred/dark”或“EL2 image 没有 SanCov callback”的历史表述，也应在这一步同步改正，以免它与已通过的真机 smoke 证据互相矛盾。

### 第二步：设计并实现双层容量方案

不要只把 `PKVM_COV_RING_PCS` 调大，也不要把 EL1 去重当作万能解药。根据第一步数据决定：

1. 保持 EL2 append-only 时，采用多少页 per-CPU shared buffer 才能覆盖单次 #23 的 raw high-water，并留出合理裕量；
2. task KCOV 是否需要 Stage-2 专用更大 buffer；
3. 是否采用 EL1 的预分配、per-`KVM_RUN` link-PC 去重，以减少 bridge 对 task KCOV 的重复压力；
4. 如果数据表明 raw high-water 高得不合理，再谨慎评估 EL2 的极简压缩；在没有数据前，不把复杂 dedup 放进 EL2。

EL1 去重表的键应是 runtime->link 转换后的 link PC，这才是跨 fault 可比较、也是 syzkaller 真正消费的地址。它必须在 `KVM_RUN` session 外层预分配，并在 drain 热路径中保持无分配、无睡眠；表满时应显式统计并退化为直通，而不是悄悄丢覆盖率。虽然它的逻辑归属应随 active KCOV task/descriptor 走，而不是永久绑定某个 CPU，但不能简单地在 `KCOV_ENABLE` 中分配：该 ioctl 的正常路径持有 KCOV spinlock。需要选择一个锁外的预分配点，或在 KCOV descriptor 初始化阶段准备 scratch，再在 `KVM_RUN` begin/end 中推进 generation。

下一次手工 smoke 的验收应当是：ring 不饱和、task KCOV 不打满、仍能看到 Rust `.rs:line`，并且丢失统计为零或有明确、可解释的上界。

### 第三步：并行准备 campaign 的两个前置条件

这两条工作可以并行开发，但在进入 manager campaign 前必须同时完成。

#### 3A. Stage-2 专用串行执行模式

需要同时做到：

- manager `procs=1`；
- 不启用 `ExecFlagThreaded`；
- 不生成 collide 执行；
- executor 中承载 KVM 调用的线程固定到 owner CPU；
- CPU 不匹配时显式报告而非静默少 coverage。

#### 3B. 可安全生成的 Host 侧输入面

在保持固定 `syz_kvm_run_fw_fault` 作为回归 smoke 的同时，设计新的 generateable composite。它只允许已验证的 protected-VM 创建、firmware window、memslot、IPA 与 fault-trigger 组合，并在内部完成资源清理。目标是让 syzkaller 能变异 Host KVM 序列并得到不同的 #23 / EL2 signal，而不是错误地把固定 pvmfw 或任意 guest code 当作输入。

完成 3A 和 3B 后，单 CPU MVP 才能从手工 smoke 进入 manager 可控运行。

### 第四步：做小规模 supervised EL2 campaign

这轮 campaign 的目标不是立刻寻找大量 bug，而是验证：

- manager corpus 确实会因 Rust EL2 signal 增长；
- 每个 signal 可用同一 build 的 vmlinux 映射到 Rust 源码；
- 没有 ring/KCOV 截断；
- 没有 owner-CPU skip；
- 目标机重启、日志收集和恢复路径可控。

### 第五步：扩大输入与 boundary

在 campaign 基础稳定后，再逐项为 #21、#34/#35、#36-#38 等 boundary 建立独立的 arm/drain/归属规则。不要全局包裹 `kvm_call_hyp_nvhe()`，否则会混入 boot、非 fuzz 线程和无关 hypercall，破坏 coverage attribution。

Host 侧工作可以继续并行推进，但应扩展到新的安全状态机 slice，而不是在已饱和的小 surface 上无限延长稳定性测试。

## 8. 当前阶段的准确表述

可以用下面这句话概括当前成果：

> Host 侧 pKVM fuzzing 已经是可用工具；EL2 coverage 已经从理论设计变成真机跑通的原型。下一目标不是立即开 campaign，而是先以 loss accounting 和 A/B smoke 证明反馈信号完整、可归因、可解释；之后再补齐串行归属与可变异的 Host 侧输入面。
