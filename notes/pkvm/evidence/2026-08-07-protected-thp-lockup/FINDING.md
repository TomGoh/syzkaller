# 受保护 VM + THP 背衬 → CPU 挂死在 EL2(N90,内核 #25)

日期：2026-08-07
板子：N90（10.42.27.17），8 核
内核：`6.6.103+ #25`，`Source Version: a89de9c463bc`（= klinux `pkvm-tlb-flush-range-fix`，含 issue 006 修复）
发现者：新加的 `syz_kvm_run_protected_guest$arm64` 复合调用

---

## 1. 已证实（硬件、单变量隔离、两次复现）

三个程序**只差一个变量**，都在同一次开机、同一个 syz-executor 沙箱下跑：

| 步骤 | 程序 | 耗时 | coverage | 板子 |
| --- | --- | --- | --- | --- |
| 1（对照） | `syz_kvm_dirty_log_cycle(hp_mode=0)` —— **普通 VM** | ~1 s | 84318 | 干净 |
| 2 | `syz_kvm_run_protected_guest(hp_mode=0)` —— 受保护 VM，`MADV_NOHUGEPAGE` | ~1 s | 69566 | 干净 |
| 3 | `syz_kvm_run_protected_guest(hp_mode=1)` —— 受保护 VM，**保留 THP** | **46 s** | **0** | **挂死** |

第 2、3 步之间**唯一的差别是那一行 `madvise(MADV_NOHUGEPAGE)`**。46 秒是 executor 的超时时长，即该调用从未返回、是被杀掉的。

### 挂死的形态

```
rcu: INFO: rcu_sched detected stalls on CPUs/tasks:
rcu:     6-...0: (1 GPs behind) idle=db74/1/0x4000000000000000 softirq=22666/22676 fqs=6527
rcu:     (detected by 2, t=15002 jiffies, g=27013, q=7446 ncpus=8)
Sending NMI from CPU 2 to CPUs 6:        <- 之后没有 CPU 6 的任何输出
```

**CPU 6 既不上报 RCU 静止态，也不响应 NMI。** 之后是纯粹的下游塌方：

```
cleanup_net → hsr_dellink → hsr_del_port → synchronize_rcu()   （持有 rtnl_lock，永不返回）
  ├─ addrconf_dad_work  → rtnl_lock       （D）
  ├─ linkwatch_event    → rtnl_lock       （D）
  ├─ hwinfo(ethtool)    → rtnl_lock       （D）
  └─ Qt bearer ×3       → netlink_dump    （D）
sshd → do_seccomp → bpf_int_jit_compile → __text_poke → kick_all_cpus_sync   （soft lockup）
```

ping 通、ssh 死，是因为 sshd 建连必须过 seccomp→BPF JIT→`kick_all_cpus_sync`，而 CPU 6 不会应答。

### 排除项

- **不是内核 #25 本身**：第一次事故前板子已干净运行 62 分钟，第一条 soft lockup 打印时仍是 `Not tainted`；第 1 步对照也证明同一沙箱下普通 VM 无恙。
- **不是"跑受保护 VM"本身**：第 2 步跑了受保护客户机，完全正常。更早的独立探针（`probe-reclaim-dying.c`，只换入代码页、NOHUGEPAGE）也跑过两次受保护 VM，板子无恙。
- **不是 syzkaller 沙箱的 netns 折腾**：第 1 步做了完全相同的 netns/bridge/bond 建拆。

---

## 2. 已证实（续）：挂死的准确位置

### 2.1 在 `close(vm)` 里，不在捐赠端

`probe-thp-where.c` 在各阶段把标记写进 `/dev/kmsg`（**不是 stderr** —— 挂死时 ssh 会断，stderr 就丢了；`/dev/kmsg` 会被既有的 console 采集实时收走）。THP arm 的输出：

```
RPGPROBE: A before KVM_RUN
RPGPROBE: B after KVM_RUN rr=0 exit=6      <- 捐赠端走通了，order-9 映射没问题
RPGPROBE: C before close(vcpu)
RPGPROBE: D before close(vm)
                                           <- E 永远没有打印
```

对照 arm（thp=0）A→F 全部走完。**所以 (a) 捐赠端排除，是 (b) 拆除回收端。**

### 2.2 卡在第一发 hypercall，不是主机侧循环

`function_graph` 追踪 `pkvm_destroy_hyp_vm` 的调用图，限定在探针那一个 pid 上，`trace_pipe` 逐行写进 `/dev/kmsg`。

对照（thp=0）：
```
pkvm_call_hyp_nvhe_ppage() {
  __reclaim_dying_guest_page_call() {
    pkvm_cov_begin();
+ 12.520 us   |  }          <- 12.5 µs 正常返回
```

THP（thp=1）：
```
pkvm_call_hyp_nvhe_ppage() {
  __reclaim_dying_guest_page_call() {
    pkvm_cov_begin();
                            <- 追踪到此为止，再无任何输出
```

**结论：主机对 `__pkvm_reclaim_dying_guest_page` 发出的第一次 HVC（order=9）进入 EL2 后再没返回。** 主机侧那个 `pkvm_call_hyp_nvhe_ppage` 的 while 循环已逐分支验证会终止（成功路径 `pins` 归零即返回；`-E2BIG` 只能把 order 9→0 一次；`-ENOENT` 每轮都减 `size`），与此一致。

归档：`ftrace-control-thp0.txt`、`ftrace-thp1-hang.txt`

---

## 3. 与 ACK 参考实现的偏离（代码对照，均未经实验确认与本挂死的因果）

klinux 的基线是 **ACK `android16-6.12`**（签名与 `phys != __phys` 检查一致）。`__pkvm_host_reclaim_page` 有四处偏离，**C 与 Rust 两份实现同步偏离**，说明是移植时就有的：

| ACK android16-6.12 | klinux |
| --- | --- |
| `check_shl_overflow(PAGE_SIZE, order, &page_size)` 守卫 | 缺失 |
| `if (!pkvm_hyp_vm_is_protected(vm)) return -EPERM;` | 缺失 |
| `hyp_poison_page(phys, page_size)` | `hyp_poison_page(phys)` —— **只清首个 4 KiB** |
| `SHARED_OWNED` 检查失败 → `return -EBUSY` | 只 `WARN_ON` 后继续 |
| stage-2 unmap 在状态判断**之后** | 在**之前** |

第三条是独立的**机密性缺陷**：order=9 时 2 MiB 里有 2044 KiB 客户机内存未擦除就归还给 host。与挂死无关，应单独跟踪。

另有一处 Rust 独有偏离，在 `drain_hyp_pool`：

```c
/* ACK/C: reclaim_hyp_pool() */
order = page->order;                                   /* 读出真实 order */
page->order = 0;
push_hyp_memcache(host_mc, p, hyp_virt_to_phys, order);
WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(p), 1 << order));
```
```rust
/* klinux Rust: 三处写死 */
ptr_wrapper!(page).hyp_page_ref_dec();                 /* 未先清 page->order */
ptr_wrapper!(mc).push_hyp_memcache(addr as *mut u64, 0);
warn_on!(__pkvm_hyp_donate_host(hyp_virt_to_pfn(addr), 1) != 0);
```

复合页只归还了首页，尾部 `(1<<order)-1` 页留在 hyp 手里 → 泄漏 + 与 host 记账不符。**但这不是挂死**：`hyp_page_ref_dec()` 只做减一，回收入池走的是 `hyp_put_page`（`page_alloc.rs:294→279→__hyp_attach_page`），这个 `while let` 每轮都从空闲链表摘走一项且无人放回，一定终止。

### 已排除：EL2 KCOV 桥不是原因（硬件实验）

这个怀疑是有依据的：`function_graph` 里挂死前最后一条正是 `pkvm_cov_begin()`，覆盖率包装宏又是全局包住每个 `kvm_call_hyp_nvhe` 的，而 order=9 要遍历 2 MiB，EL2 侧 SanCov 回调数量比 order=0 高几个数量级，很可能打满 16381 槽的 ring —— 一个天然只在大页发作的动机。

判别只需一个变量。停掉 `pkvm-cov-arm.service`、`echo 0 > enable`，确认 `armed_cpus 0` 后跑同一个探针：

```
armed_cpus          0
RPGPROBE: A before KVM_RUN
RPGPROBE: B after KVM_RUN rr=0 errno=- exit=6
RPGPROBE: C before close(vcpu)
RPGPROBE: D before close(vm)      <- 与武装时完全相同的位置，E 依旧没有
```

板子照样挂死。**覆盖率桥排除。**

推论：ring_dump 作为**仪器**仍然成立 —— 插桩不改变控制流，覆盖率既非病因，武装它去观察 EL2 走到哪里就没有循环论证的问题。

### 已排除的 EL2 候选（读代码）

- `guest_get_valid_pte` —— 与 C 逐行等价，无循环；order=9 时 `size == PMD_SIZE` 检查通过
- `psci_mem_protect_dec(512)` —— 只调计数器，`wrapping_add_signed`，无循环
- `host_stage2_try!` 宏 —— 与 C 一致，`-ENOMEM` 后只重试一次

### 仍未排除

`kvm_pgtable_stage2_unmap(pgt, ipa, 2 MiB)`、`__host_check_page_state_range(phys, 2 MiB, ...)`、`host_stage2_set_owner_locked(phys, 2 MiB, ...)` —— 三者都要遍历 2 MiB 区间。

**不再靠读代码往下猜。** 用 `kvm/pkvm_cov/ring_dump`（新增的只读 debugfs）从健康 CPU 读走卡死 CPU 的 EL2 coverage ring：`pkvm_cov_end()` 没跑过，PC 原封不动留在 host 内存里，这是 EL2 挂死时唯一的现场。结果见 §4。

---

## 4. 根因（已定位，硬件证据 + 地址反查）

### 4.1 抓到的现场

`ring_dump` 在挂死后从健康 CPU 读走卡死 CPU 的 ring：

```
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381   <- t=69s
cpu7 flags=0x1 count=57 hits=57 dropped=0 nested=0 capacity=16381   <- t=89s，纹丝不动
```

`flags=0x1` = 窗口仍是 ENABLED，`pkvm_cov_end()` 从未运行 —— 正是那个没返回的 HVC。

**`count` 冻结在 57，20 秒不变**，这一条直接推翻了"EL2 在死循环"的方向：若在插桩代码里循环，SanCov 每个基本块都会回调，`hits` 会一路把 16381 槽打满并置 OVERFLOW。它停住了。

57 个 PC 的末尾：

```
53  __hyp_put_page+0x24
54  __hyp_put_page+0x134
55  __pkvm_reclaim_dying_guest_page+0x55c/0x560     <- 函数最末尾（Rust 把 panic 冷路径排在尾部）
56  rust_begin_unwind+0x8/0xc                       <- Rust panic handler
```

### 4.2 反查到源码行

```
$ addr2line -e vmlinux -f -i -C 0x...bfd4
xhypervisor::mem_protect::host::__pkvm_host_reclaim_page
  arch/arm64/kvm/hyp/xhypervisor/src/bindings/mod.rs:300     <- bug_on! 宏里的 panic!
__pkvm_reclaim_dying_guest_page
  arch/arm64/kvm/hyp/xhypervisor/src/pkvm.rs:1298            <- 内联进来的调用点
```

`__pkvm_host_reclaim_page` 里**只有一处 `bug_on!`**（其余全是 `warn_on!`）：页状态 `match` 的 `_ =>` 兜底分支。

### 4.3 缺陷 A —— 触发者：`pte` 写成了 `prot`

```c
/* ACK android16-6.12，guest_get_page_state() —— 测的是 prot */
prot = kvm_pgtable_stage2_pte_prot(pte);
if (kvm_pte_valid(pte) && ((prot & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX))
        state = PKVM_PAGE_RESTRICTED_PROT;
```
```rust
/* klinux xhypervisor/src/mem_protect/guest.rs —— 测的是 pte */
let prot = kvm_pgtable_stage2_pte_prot(pte);
// 前面已经判断了页表项有效，这里应该就不用再次判断了
// pte 只是 u64 类型，其值也不会改变
if (pte & KVM_PGTABLE_PROT_RWX) != KVM_PGTABLE_PROT_RWX {
    state = PKVM_PAGE_RESTRICTED_PROT;
}
```

移植时为了删掉冗余的 `kvm_pte_valid(pte)`（注释正是在解释这件事），**把后半段的 `prot` 一并换成了 `pte`**。`prot` 是 `kvm_pgtable_stage2_pte_prot()` 提取出来的权限位，`pte` 是 64 位页表项，低位含义完全不同。

`KVM_PGTABLE_PROT_X=BIT(0) / W=BIT(1) / R=BIT(2)`，故 `RWX = 0b111 = 7`。stage-2 描述符低三位：

| | bit0 `KVM_PTE_VALID` | bit1 `KVM_PTE_TYPE` | bit2（MemAttr 低位，普通内存为 1） | `pte & 7` |
| --- | --- | --- | --- | --- |
| 4 KiB page（末级） | 1 | **1** = `TYPE_PAGE` | 1 | `0b111` = 7 |
| 2 MiB block（L2） | 1 | **0** = `TYPE_BLOCK` | 1 | `0b101` = 5 |

- **块映射**：`(pte & 7) != 7` **恒为真** → `PKVM_PAGE_RESTRICTED_PROT` 无条件置上。客户机 RAM 页本身是 `PKVM_PAGE_OWNED`（值 0），最终 `state == PKVM_PAGE_RESTRICTED_PROT` 单独出现 —— 四个分支（`OWNED` / `SHARED_BORROWED` / `SHARED_BORROWED|RESTRICTED_PROT` / `SHARED_OWNED`）**一个都不匹配** → `_ => bug_on!(true)`。
- **4 KiB 页**：bit1 与 bit2 凑出 7，条件为假，状态为 `OWNED`，命中第一个分支，一路正常。

**这就精确解释了 hp_mode=0 正常、hp_mode=1 挂死的不对称**，无需任何额外假设。

### 4.4 缺陷 B —— 放大者：EL2 的 panic handler 是 `loop {}`

```rust
/* arch/arm64/kvm/hyp/xhypervisor/src/lib.rs:75 */
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
```

`bug_on!(cond)` → `panic!()` → `rust_begin_unwind` → **`loop {}`，在 EL2、中断已屏蔽的上下文里原地空转**。于是：

- 该 CPU 永不返回 host，HVC 永不完成
- 不再执行任何插桩基本块（`count` 冻结在 57）
- 不响应 IPI、不上报 RCU 静止态、连 NMI 都不应
- 全机所有需要全核同步的操作（`synchronize_rcu`、`kick_all_cpus_sync`、`rcu_barrier`）永久阻塞 → 级联 soft lockup → 整板失联，只能断电

**这是比缺陷 A 更系统性的问题**：Rust hypervisor 里**任何**一次 `bug_on!` 触发，都会从"一条可报告的 hyp panic"退化成"整板静默挂死、无任何诊断信息"。C 版 nvhe 的 `BUG_ON` 走 `hyp_panic()`，会向 host 报告。

本次排查的绝大部分成本（数小时、五次断电）都由缺陷 B 造成，而不是缺陷 A。

---

## 5. 结论与建议处置

| | 缺陷 | 性质 |
| --- | --- | --- |
| A | `guest_get_page_state()` 用 `pte` 代替 `prot` 做 RWX 判断 | 移植笔误；任何块映射的受保护 VM 拆除必然踩中 |
| B | `#[panic_handler] { loop {} }` | 系统性；把任何 EL2 BUG 变成不可诊断的整板挂死 |
| C | `hyp_poison_page(phys)` 丢失长度参数 | 机密性：order=9 时 2044 KiB 客户机内存未擦除即归还 host |
| D | Rust `drain_hyp_pool` 把 order 写死 0/1 页 | 复合页尾部泄漏 + 与 host 记账不符 |
| E | 缺 `check_shl_overflow` / `pkvm_hyp_vm_is_protected` 守卫，`SHARED_OWNED` 失败只 WARN 不返回 `-EBUSY`，unmap 与状态判断次序与 ACK 相反 | 与 ACK 的其余偏离，未逐条验证影响 |

A 与 B 应分别修；**B 优先**，因为在它修好之前，任何其他 EL2 缺陷都同样只会表现为整板挂死。

C/D/E 为本次顺带发现，与本挂死无因果，建议单独跟踪。

---

## 3. 对 fuzzing 的直接影响

**这个 bug 不能留给 campaign 撞。** 它不产生 crash 报告，而是让整块板子失联，每次都要人去按电源 —— 对 fuzzing 是纯负收益。

处置：把描述里的 `hp_mode` 收紧成只允许 0，THP 那一路作为 **issue 007 的复现件**单独保留。等 EL2 修好再放开。

---

## 4. 归档

这份文件是**工作记录**——包含被杀掉的假设和走过的弯路。定案后的权威记录是
[`issues/007-protected-block-mapping-el2-panic/`](../../../../issues/007-protected-block-mapping-el2-panic/)，
共用的证据只在那里保存一份，本目录不复制。

只留在这里的：

| 文件 | 内容 |
| --- | --- |
| `FINDING.md` | 本文：完整过程，含两个被推翻的方向 |
| `console-lockup-cascade.txt` | **第一次**事故（1259 行，boot #1）。issue 收录的是第二次、单变量隔离的那份，这份记录的是最初撞上时的样子 |
| `probe-reclaim-dying.c` | 受保护 vs 普通 VM 的**可达性**差分探针。它属于复合调用那件事（证实 id 35 可达），不是 007 的复现件——它用 NOHUGEPAGE，从不触发本 bug |
| `rpg.prog` | 触发用的 syzkaller 程序 |

在 issue 目录里的：`ring-dump-cpu7-hang.txt`、`ftrace-{control-thp0,thp1-hang}.txt`、
`console-boot2-thp-isolated.txt`、`repro/probe-thp-kcov.c`、`repro/catch-ring.sh`、
`repro/ring-dump.patch`。

复现条件：

```
kernel:  6.6.103+ #25 @ a89de9c463bc，cmdline kvm-arm.mode=protected
prog:    r0 = openat$kvm(0xffffffffffffff9c, &(0x7f0000000000), 0x2, 0x0)
         syz_kvm_run_protected_guest$arm64(r0, 0x1)
run:     ./syz-execprog -executor=./syz-executor -procs=1 -threaded=0 -cover=1 -slowdown=10 <prog>
```
