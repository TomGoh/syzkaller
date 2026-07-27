# 阶段 3 — 结构化程序：从 KVM 描述到 `Prog` 对象

> 对应学习计划的阶段 3。一手资料：`prog/prog.go`、`prog/types.go`、`prog/target.go`、`prog/size.go`、`docs/program_syntax.md`、`docs/syscall_descriptions_syntax.md`、`sys/linux/dev_kvm.txt`、`sys/linux/dev_kvm_arm64.txt`、`sys/syz-extract/`、`sys/syz-sysgen/` 与 `sys/generated/`。
> 目标：理解 syscall 的规则怎样被编译成 `Target/Syscall/Type`，一条具体测试怎样表示为 `Prog/Call/Arg`，以及指针、结构体、长度和资源关系为什么能支持结构化生成与变异。
> 本文于 2026-07-20 按当前工作树核对。源码更新后若行号发生变化，应优先按类型名或函数名定位。

---

## 一、阶段 3 要解决什么问题

阶段 2 把 `*prog.Prog` 当作 Host 端的一条结构化测试程序，并跟踪了它怎样进入 `queue.Request`、被序列化、发送和执行。本阶段向内展开这个对象，回答以下问题：

1. syzkaller 从哪里知道 KVM 的不同 `ioctl` 分别要求哪一种 fd、命令号和参数结构？
2. `.txt` 中的类型规则与一条具体程序中的参数值是什么关系？
3. 文本中的 `r0`、`r1` 和 `r2` 怎样表示 KVM 控制 fd、VM fd 和 vCPU fd 之间的依赖？
4. `ptr`、`struct`、`flags`、`len` 和 `resource` 最终怎样落到 `Arg` 对象上？
5. 为什么这些结构能帮助生成和变异，但不能保证 KVM 调用一定成功？

本文继续把运行 Go fuzzer、manager 和 RPC server 的一侧称为 **Host**，把运行 executor 与待测内核的一侧称为**目标端**。

### 1. 贯穿全文的 ARM64 KVM 程序

KVM 的用户态接口以 fd 表示不同层次的内核对象：先打开 `/dev/kvm` 得到 KVM 控制 fd，再由它创建 VM fd，最后由 VM fd 创建 vCPU fd。不同层次的 `ioctl` 必须作用在对应类型的 fd 上。

本文使用下面这条 `linux/arm64` 程序：

```text
r0 = openat$kvm(0xffffffffffffff9c, &AUTO='/dev/kvm\x00', 0x2, 0x0)
r1 = ioctl$KVM_CREATE_VM(r0, AUTO, 0x0)
ioctl$KVM_SET_USER_MEMORY_REGION(r1, AUTO, &AUTO={0x0, 0x0, 0x0, AUTO, &(0x7f0000010000/0x1000)=nil})
r2 = ioctl$KVM_CREATE_VCPU(r1, AUTO, 0x0)
ioctl$KVM_ARM_VCPU_INIT(r2, AUTO, &AUTO={0x5, 0x0, ""})
ioctl$KVM_RUN(r2, AUTO, 0x0)
```

这不是 C，而是 syzkaller 保存测试用例的 `.syz` 文本。第一次阅读时，不应从十六进制数和 `AUTO` 开始猜。先看下面这份近似 C。最前面的数据区准备不对应任何一行 `.syz`；标为 1～6 的部分才与六行程序逐项对应。

```c
/* 运行环境准备：不对应任何一行 .syz。
 * executor 已经完成这一步；生成独立 C 时，syz-prog2c 会自动补上。 */
void *data_area = mmap((void *)0x20000000, 0x1000000,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
void *guest_mem = (char *)data_area + 0x10000;

/* 1 */
int kvmfd = openat(AT_FDCWD, "/dev/kvm", O_RDWR, 0);

/* 2 */
int vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);

/* 3 */
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .flags = 0,
    .guest_phys_addr = 0,
    .memory_size = 0x1000,
    .userspace_addr = (uintptr_t)guest_mem,
};
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);

/* 4 */
int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);

/* 5 */
struct kvm_vcpu_init init = {
    .target = KVM_ARM_TARGET_GENERIC_V8,
    .features = {0},
};
ioctl(vcpufd, KVM_ARM_VCPU_INIT, &init);

/* 6 */
ioctl(vcpufd, KVM_RUN, 0);
```

这段 C 用正常变量名和结构体字段说明语义，省略了错误处理；它不是 `syz-prog2c` 的逐字输出。`syz-prog2c` 的实际输出会使用固定映射地址、资源槽和直接 `syscall()`，第十节会展示其中的关键部分。

两种写法可以这样一一对应：

| `.syz` 写法 | C 中的对应物 |
|---|---|
| `r0` | `kvmfd`，打开 `/dev/kvm` 得到的控制 fd |
| `r1` | `vmfd`，`KVM_CREATE_VM` 返回的 VM fd |
| `r2` | `vcpufd`，`KVM_CREATE_VCPU` 返回的 vCPU fd |
| `ioctl$KVM_CREATE_VM(r0, AUTO, 0)` | `ioctl(kvmfd, KVM_CREATE_VM, 0)` |
| `&AUTO={...}` | 自动放置一份结构体，并传入它的地址，类似 C 中的 `&region` 或 `&init` |
| `&(地址/0x1000)=nil` | 从预映射数据区中选择一段大小为 `0x1000` 的 VMA，对应 `guest_mem`；它本身不执行 `mmap` |
| `size=AUTO` | 根据上述 VMA 大小填成 `0x1000`，对应 `.memory_size = 0x1000` |
| `cmd=AUTO` | 根据 `$KVM_*` 变体填入相应命令，例如 `KVM_CREATE_VM` |
| `{0x5, 0x0, ""}` | `target=5`、feature bitmap 为 0，其余保留字段清零 |

这里必须把“准备数据区”和“使用数据区”分开：

```text
executor 或 C reproducer 的初始化代码
    └── 先映射整块 syzkaller 数据区
          本例 C 中为 [0x20000000, 0x21000000)
                         │
                         │ .syz 中的 VMA 偏移为 0x10000
                         ▼
KVM_SET_USER_MEMORY_REGION
    └── 使用其中 [0x20010000, 0x20011000) 这一页
```

`.syz` 为了跨目标稳定地显示地址，使用固定的文本编码基址 `0x7f0000000000`。因此：

```text
文本地址  0x7f0000010000
- 编码基址 0x7f0000000000
------------------------
对象内部偏移      0x10000

C 数据区基址 0x20000000
+ 对象内部偏移 0x00010000
------------------------
实际传入 KVM  0x20010000
```

普通 executor 路径中，`executor.cc` 在解释程序前调用 `os_init()`，Linux 实现在 `executor/executor_linux.h` 中映射整个数据区及两侧保护页。生成独立 C 时，`pkg/csource/csource.go` 会调用 `Target.DataMmapProg()`，把等价的初始化 `mmap` 自动放到正文前。因此，VMA 参数只负责在已映射区域中选择地址和长度，并不向 `Prog.Calls` 增加一条 `mmap`。

各行的作用是：

```text
打开 /dev/kvm
    │ 产生 fd_kvm：KVM 控制对象
    ▼
创建 VM
    │ 产生 fd_kvmvm：一个虚拟机对象
    ├── 注册一页 guest 内存
    └── 创建 vCPU
           │ 产生 fd_kvmcpu：一个虚拟 CPU 对象
           ├── 按 ARM64 参数初始化
           └── 请求运行
```

第一行只是取得 KVM 接口的入口，本文不会再逐项讲解一般文件打开流程。真正要观察的是后五行怎样表达 KVM 的对象层级、`ioctl$...` 变体和嵌套结构。

这条程序是一个适合学习类型模型的**接口骨架**，不是一台完整可运行的虚拟机。它没有装入 guest 指令、设置 PC 和通用寄存器，也没有处理 vCPU 退出。后文会专门说明这一区别。

---

## 二、先分清两种文本和四层对象

### 1. syscall 描述不是测试程序

syzkaller 中容易混淆的两种文本分别是：

| 文本 | 典型位置 | 回答的问题 |
|---|---|---|
| syscall 描述，通常称为 syzlang 描述 | `sys/linux/*.txt` | 某类调用有哪些参数，每个参数的类型、方向和关系是什么 |
| syz 程序文本 | corpus、日志、`sys/linux/test/*` | 这一次具体执行哪些调用，各参数取什么值，调用之间怎样引用资源 |

`sys/linux/dev_kvm.txt:11-35` 中包含以下规则：

```text
resource fd_kvm[fd]
resource fd_kvmvm[fd]
resource fd_kvmcpu[fd]

openat$kvm(... ) fd_kvm
ioctl$KVM_CREATE_VM(fd fd_kvm, cmd const[KVM_CREATE_VM],
                    type intptr[0:64]) fd_kvmvm
ioctl$KVM_SET_USER_MEMORY_REGION(
                    fd fd_kvmvm,
                    cmd const[KVM_SET_USER_MEMORY_REGION],
                    arg ptr[in, kvm_userspace_memory_region])
ioctl$KVM_CREATE_VCPU(fd fd_kvmvm, cmd const[KVM_CREATE_VCPU],
                      id intptr[0:2]) fd_kvmcpu
```

这些行不是一次可直接执行的测试，而是一组接口规则。它们规定：

- `KVM_CREATE_VM` 的第一个参数必须是 `fd_kvm`，返回 `fd_kvmvm`；
- `KVM_SET_USER_MEMORY_REGION` 需要 `fd_kvmvm`，第三个参数指向指定结构体；
- `KVM_CREATE_VCPU` 同样需要 `fd_kvmvm`，返回更专门的 `fd_kvmcpu`；
- 每个 `ioctl$...` 变体的 `cmd` 都是相应 KVM 命令的固定常量。

开头的具体程序则依据这些规则选择了 vCPU ID、内存大小、ARM64 target 和 feature 等实际值。

### 2. 从描述到执行的四层关系

```text
sys/linux/*.txt + 架构相关常量
          │ 编译
          ▼
Target ──包含──► Syscall / Type / ResourceDesc   静态规则层
          │
          │ 解析程序文本，或按规则生成程序
          ▼
Prog ────包含──► Call / Arg                      具体程序层
          │
          │ SerializeForExec()
          ▼
copyin / call / copyout 等低层指令               executor 输入层
          │
          ▼
目标内核中的 openat、ioctl 等实际系统调用         运行层
```

本阶段重点是中间两层：

- 静态规则层回答“对于 `linux/arm64`，可以怎样构造 KVM 调用”；
- 具体程序层回答“这一条程序实际选择了哪些调用和值”；
- 阶段 2 已经说明程序怎样下降为 executor 指令并被执行；
- 阶段 4 再深入这些规则怎样参与随机生成和变异。

---

## 三、静态规则层：`Target`、`Syscall` 与 `Type`

### 1. `Target` 不是目标机

`prog/target.go:20-87` 中的 `Target` 描述一个 OS/架构组合，例如本文的 `linux/arm64`。它不是 VM、物理机或 executor 进程，而是一份供 Host 端使用的接口模型。

其主要内容可以分成两类：

```go
type Target struct {
    OS, Arch, Revision string
    PtrSize, PageSize, NumPages, DataOffset uint64
    BigEndian bool

    Syscalls  []*Syscall
    Resources []*ResourceDesc
    Consts    []ConstValue
    Flags     []FlagDesc
    Types     []Type

    SyscallMap map[string]*Syscall
    ConstMap   map[string]uint64
    FlagsMap   map[string][]string
    // 另有目标相关处理函数和内部索引。
}
```

- `PtrSize`、字节序和页大小等字段描述当前 ABI 与数据区布局；
- `Syscalls`、`Resources`、`Consts`、`Flags` 和 `Types` 是编译后的 syscall 规则；
- `SyscallMap` 等索引让解析、生成和变异代码可以按名称快速查找规则。

同一 Host 进程中的许多 `Prog` 会共享同一个 `Target`。`Prog.Target` 的作用是说明这条程序必须按哪个 OS/架构的系统调用号、参数宽度和内存布局解释。

当前工作树中，实际取得的 `linux/arm64` `Target` 包含 8051 项 `Syscall`、785 项资源描述和 31229 项类型。数量会随描述更新而变化，只用于帮助理解 `Target` 的规模。

### 2. 一个 `Syscall` 保存什么

`prog/types.go:13-29` 中的 `Syscall` 是一项已经编译好的调用规则：

```go
type Syscall struct {
    ID       int
    NR       uint64
    Name     string
    CallName string
    Args     []Field
    Ret      Type
    Attrs    SyscallAttrs
    // 另有资源使用关系等内部字段。
}
```

几个名称和编号不能混为一谈：

| 字段 | 含义 |
|---|---|
| `Name` | syzkaller 使用的完整名称，可以带 `$` 变体，如 `ioctl$KVM_CREATE_VM` |
| `CallName` | 最终的基础调用名；各个 `ioctl$KVM_*` 的 `CallName` 都是 `ioctl` |
| `NR` | 当前架构实际进入内核时使用的系统调用号；本文各个 KVM `ioctl$...` 变体的 `NR` 都是 29 |
| `ID` | 该项在当前 `Target.Syscalls` 中的编号，executor 输入先用它标识调用规则 |
| `Args` / `Ret` | 各参数与返回资源的类型规则 |
| `Attrs` | `disabled`、`timeout`、`no_generate` 等描述属性 |

在当前 `linux/arm64` `Target` 中：

| `Name` | `CallName` | `ID` | `NR` | 返回资源 |
|---|---:|---:|---:|---|
| `openat$kvm` | `openat` | 4732 | 56 | `fd_kvm` |
| `ioctl$KVM_CREATE_VM` | `ioctl` | 1922 | 29 | `fd_kvmvm` |
| `ioctl$KVM_SET_USER_MEMORY_REGION` | `ioctl` | 1963 | 29 | 无资源返回值 |
| `ioctl$KVM_CREATE_VCPU` | `ioctl` | 1921 | 29 | `fd_kvmcpu` |
| `ioctl$KVM_ARM_VCPU_INIT` | `ioctl` | 1902 | 29 | 无资源返回值 |
| `ioctl$KVM_RUN` | `ioctl` | 1951 | 29 | 无资源返回值 |

这里最值得注意的是：五项 KVM `ioctl` 的 `NR` 都是 29，但 `ID`、参数类型和固定 `cmd` 不同。`ID` 是 syzkaller 描述表内部编号，源码更新后可能变化；`NR` 才是该架构进入内核时使用的 syscall number。

### 3. `$` 变体为什么很重要

Linux 的 `ioctl` 在 C ABI 上只有三个通用参数：fd、cmd 和 arg。但不同 `cmd` 对 fd 所代表的对象、arg 的方向和内存布局有完全不同的要求。若只保留一个“任意 fd + 任意整数 cmd + 任意地址”的模型，生成器几乎不知道怎样组成有效 KVM 请求。

syzkaller 用 `$` 后缀把同一个基础调用拆成多个结构化变体：

```text
ioctl$KVM_CREATE_VM
  fd   → ResourceType(fd_kvm)
  cmd  → ConstType(KVM_CREATE_VM)
  arg  → IntType(intptr[0:64])
  ret  → ResourceType(fd_kvmvm)

ioctl$KVM_SET_USER_MEMORY_REGION
  fd   → ResourceType(fd_kvmvm)
  cmd  → ConstType(KVM_SET_USER_MEMORY_REGION)
  arg  → PtrType(in, StructType(kvm_userspace_memory_region))

ioctl$KVM_ARM_VCPU_INIT
  fd   → ResourceType(fd_kvmcpu)
  cmd  → ConstType(KVM_ARM_VCPU_INIT)
  arg  → PtrType(in, StructType(kvm_vcpu_init))
```

因此，`$` 后缀不是内核里的新系统调用名，而是 syzkaller 对同一系统调用不同接口形态的命名。执行时，它们都通过 `CallName=ioctl` 和 `NR=29` 进入内核。

### 4. `Type` 是规则，不是参数值

`prog/types.go:176-203` 中的 `Type` 接口同时包含两类能力：

- 类型名称、大小、对齐、是否可选、是否变长等静态信息；
- 创建默认参数、生成、变异和最小化该类参数的行为。

因此，`Type` 可以理解为“某个参数位置的规则对象”。它不保存本次程序究竟用了哪个 fd、哪一页内存或哪个 feature 值。

例如：

- `ResourceType(fd_kvmvm)` 知道该位置需要一个 VM fd；
- `ConstType(KVM_RUN)` 知道 `cmd` 必须固定为 `KVM_RUN`；
- `PtrType(in, kvm_vcpu_init)` 知道这里是指向输入结构体的指针；
- `StructType(kvm_vcpu_init)` 知道结构体的字段顺序、大小和对齐；
- `FlagsType(kvm_vcpu_feature_bits_arm64)` 知道 ARM64 vCPU feature 的候选位；
- `LenType(addr)` 知道该整数与同一结构体中的 `addr` 区域大小有关。

这些 `Type` 对象属于 `Target` 的静态描述，可以被许多调用和程序共享。

---

## 四、具体程序层：`Prog`、`Call` 与 `Arg`

### 1. `Prog` 到底是什么

`prog/prog.go:15-22` 中的定义很短：

```go
type Prog struct {
    Target   *Target
    Calls    []*Call
    Comments []string
    // 另有反序列化安全模式的内部状态。
}
```

一个 `*prog.Prog` 就是 **Host 内存中的一条完整 syzkaller 测试程序**：

- `Target` 指向它所属的 OS/架构规则；
- `Calls` 按执行顺序保存具体调用；
- `Comments` 保存程序注释，不是核心执行数据。

它不是可读文本，也不是 executor 直接解释的二进制流。程序文本可通过 `Target.Deserialize()` 解析成 `Prog`，`Prog.Serialize()` 又能把它写回文本；执行前还要通过 `SerializeForExec()` 降为低层执行指令。

对本文例子而言，`Prog.Target` 指向 `linux/arm64`，`Prog.Calls` 中按顺序保存六个 `Call`。跨调用的资源引用也包含在这张对象图中。

### 2. 每个 `Call` 把规则与具体值接起来

`prog/prog.go:79-92` 中的 `Call` 包含：

```go
type Call struct {
    Meta    *Syscall
    Args    []Arg
    Ret     *ResultArg
    Props   CallProps
    Comment string
}
```

- `Meta` 指向 `Target` 中共享的静态 `Syscall`；
- `Args` 保存这一次调用选择的具体参数；
- `Ret` 保存调用直接产生的资源返回值，无资源返回值时为 `nil`；
- `Props` 保存 `async`、`fail_nth`、`rerun` 等单次调用属性。

以 `ioctl$KVM_CREATE_VM` 为例：

```text
Call.Meta.Name                 = "ioctl$KVM_CREATE_VM"
Call.Meta.Args[0].Type         = ResourceType(fd_kvm)   静态规则
Call.Args[0]                   = ResultArg(Res=r0)       本次具体引用
Call.Meta.Args[1].Type         = ConstType(0xae01)      静态规则
Call.Args[1]                   = ConstArg(0xae01)        本次具体值
Call.Ret.Type                  = ResourceType(fd_kvmvm)
```

因此，`Call.Meta.Args[i].Type` 是第 `i` 个参数的规则，`Call.Args[i]` 是这一条程序实际使用的值。二者位置对应，但职责不同。

### 3. `Arg` 是具体值，也可以嵌套和引用

`Arg` 是具体参数对象的共同接口。主要实现与对应的 `Type` 如下：

| `Type` 规则 | `Arg` 实例 | 保存的具体内容 |
|---|---|---|
| `ConstType`、`IntType`、`FlagsType`、`LenType` | `ConstArg` | 一个整数值；具体规则决定它是固定常量、一般整数、flag 还是关联长度 |
| `PtrType`、`VmaType` | `PointerArg` | 地址、VMA 大小或指向的参数对象 |
| `BufferType` | `DataArg` | 输入字节，或输出缓冲区大小 |
| `StructType` | `GroupArg` | 按结构体字段排列的一组子参数 |
| `ResourceType` | `ResultArg` | 特殊值，或对另一个资源结果的引用 |

这里只列本文 KVM 程序实际出现的对象类型。`prog` 包还支持其他类型，但如果当前例子没有用到，就不在这里提前展开。

例如，`KVM_SET_USER_MEMORY_REGION` 的第三个参数在内存中不是一个孤立地址，而是下面这棵对象树：

```text
PointerArg                         PtrType(in, kvm_userspace_memory_region)
└── Res → GroupArg                 StructType(kvm_userspace_memory_region)
          ├── ConstArg(0)          FlagsType(kvm_mem_slots)
          ├── ConstArg(0)          FlagsType(kvm_mem_region_flags)
          ├── ConstArg(0)          FlagsType(kvm_guest_addrs)
          ├── ConstArg(0x1000)     LenType(addr)
          └── PointerArg           VmaType(vma64[1:2])
              ├── Address=0x10000
              ├── VmaSize=0x1000
              └── Res=nil          VMA 表示区域，不另带 pointee
```

结构体和数组会继续嵌套 `Arg`；资源引用则会越过调用边界，指向前面调用产生的结果。因此，`Prog` 整体不是单纯的树，而是一张以调用顺序为主干、内部带嵌套和跨调用引用的对象图。

### 4. 文本中的 `r0`、`r1`、`r2` 和 `&AUTO`

程序文本中的：

```text
r1 = ioctl$KVM_CREATE_VM(r0, AUTO, 0x0)
ioctl$KVM_SET_USER_MEMORY_REGION(r1, AUTO, ...)
r2 = ioctl$KVM_CREATE_VCPU(r1, AUTO, 0x0)
```

解析后不会在参数中保存字符串 `"r0"` 或 `"r1"`。例如：

```text
openat$kvm.Call.Ret
    └── KVM_CREATE_VM.Args[0].Res

KVM_CREATE_VM.Call.Ret
    ├── KVM_SET_USER_MEMORY_REGION.Args[0].Res
    └── KVM_CREATE_VCPU.Args[0].Res

KVM_CREATE_VCPU.Call.Ret
    ├── KVM_ARM_VCPU_INIT.Args[0].Res
    └── KVM_RUN.Args[0].Res
```

生产者的 `Call.Ret` 与使用者的参数都是 `ResultArg`，使用者通过 `Res` 指针直接指向生产者。重新序列化时，serializer 才按照这些引用关系分配 `r0`、`r1`、`r2` 等可读名称。

`&AUTO` 也不是传给内核的特殊地址。它表示文本作者不指定具体数据区位置，解析器可以分配一个不冲突的地址。分配后：

- 普通指针用 `PointerArg.Address` 保存数据区偏移，用 `PointerArg.Res` 保存所指对象；
- VMA 参数用 `Address` 和 `VmaSize` 选择预映射数据区中的一段区域，`Res` 为 `nil`；它本身不会产生 `mmap` 调用；
- executor 或生成的 C 最终使用目标数据区基址加偏移得到真实地址。

这里的 `AUTO` 还可以用于由类型确定的常量和长度。严格解析本文程序后，`KVM_CREATE_VM` 的 `cmd` 会成为 `0xae01`，内存区域的 `size` 会成为 `0x1000`。也就是说，`AUTO` 只是让解析器按规则补出值，并不把“自动计算”推迟到内核执行时。

---

## 五、逐项读懂 KVM 描述

### 1. 先建立 KVM 对象层级

`sys/linux/dev_kvm.txt:11-15` 定义：

```text
resource fd_kvm[fd]
resource fd_kvmvm[fd]
resource fd_kvmcpu[fd]
resource fd_kvmdev[fd]
resource fd_kvm_guest_memfd[fd]
```

这些都继承普通 `fd`，但彼此是不同的专用资源：

```text
fd
├── fd_kvm              /dev/kvm 控制对象
├── fd_kvmvm            一个 KVM VM 对象
├── fd_kvmcpu           一个 KVM vCPU 对象
├── fd_kvmdev           VM 内的 KVM 设备对象
└── fd_kvm_guest_memfd  guest memory fd 对象
```

这种区分不是为了给整数换名字，而是为了限制调用组合。要求 `fd_kvmcpu` 的 `KVM_RUN` 不应随意拿一个 VM fd；要求 `fd_kvmvm` 的内存注册也不应拿控制 fd。

### 2. `openat$kvm`：取得入口资源

`sys/linux/dev_kvm.txt:18` 把路径固定为 `/dev/kvm`，把 mode 固定为 0，并声明返回 `fd_kvm`：

```text
openat$kvm(fd const[AT_FDCWD],
           file ptr[in, string["/dev/kvm"]],
           flags flags[open_flags],
           mode const[0]) fd_kvm
```

这一变体的目的很直接：让生成器知道怎样取得后续 KVM 调用所需的控制资源。本文程序选择 `O_RDWR`，得到文本中的 `r0`。

### 3. `KVM_CREATE_VM`：从控制对象创建 VM 对象

```text
ioctl$KVM_CREATE_VM(fd fd_kvm,
                    cmd const[KVM_CREATE_VM],
                    type intptr[0:64]) fd_kvmvm
```

逐项对应本文程序：

```text
r1 = ioctl$KVM_CREATE_VM(r0, AUTO, 0x0)
```

- `fd fd_kvm` → `ResultArg(Res=openat$kvm.Ret)`；
- `cmd const[KVM_CREATE_VM]` → `ConstArg(0xae01)`；
- `type intptr[0:64]` → `ConstArg(0)`；
- 返回 `fd_kvmvm` → `Call.Ret`，序列化时命名为 `r1`。

在 ARM64 上，`type=0` 表示使用默认 IPA 大小。描述中的 `[0:64]` 给生成器一个整数范围，但不表示范围内每个值都一定被当前内核接受。

### 4. `KVM_SET_USER_MEMORY_REGION`：指针、结构体、VMA 与长度关系

调用规则位于 `sys/linux/dev_kvm.txt:35`：

```text
ioctl$KVM_SET_USER_MEMORY_REGION(
    fd fd_kvmvm,
    cmd const[KVM_SET_USER_MEMORY_REGION],
    arg ptr[in, kvm_userspace_memory_region])
```

结构体规则位于 `sys/linux/dev_kvm.txt:545-551`：

```text
kvm_userspace_memory_region {
    slot   flags[kvm_mem_slots, int32]
    flags  flags[kvm_mem_region_flags, int32]
    paddr  flags[kvm_guest_addrs, int64]
    size   len[addr, int64]
    addr   vma64[1:2]
}
```

各字段的含义是：

| 字段 | 本次值 | 结构化规则 |
|---|---:|---|
| `slot` | 0 | 从已知 memory slot 候选中选择 |
| `flags` | 0 | 可组合的内存区域 flag |
| `paddr` | 0 | guest 物理地址起点 |
| `size` | `0x1000` | `LenType`，关联本结构中的 `addr` |
| `addr` | 一页 VMA | executor 预映射数据区中的一页子区域 |

程序中只明确写出了 VMA 大小：

```text
&(0x7f0000010000/0x1000)=nil
```

这里 `/0x1000` 表示区域大小为一页。`size` 写成 `AUTO` 后，解析器沿 `len[addr]` 的路径找到该 VMA，并把 `ConstArg.Val` 计算为 `0x1000`。当前源码中的实际对象树也验证了：

```text
size: ConstArg type=LenType value=0x1000
addr: PointerArg type=VmaType Address=0x10000 VmaSize=0x1000 Res=nil
```

最终生成的 C 会在结构体中写入：

```c
*(uint64_t*)0x20000090 = 0x1000;     /* size */
*(uint64_t*)0x20000098 = 0x20010000; /* userspace addr */
```

这正是“结构化”的具体含义：syzkaller 不只知道第三个参数是一个地址，还知道地址指向 32 字节结构体，结构体中有五个字段，其中一个长度字段与另一个 VMA 字段相关。

### 5. `KVM_CREATE_VCPU`：资源继续向下派生

```text
ioctl$KVM_CREATE_VCPU(fd fd_kvmvm,
                      cmd const[KVM_CREATE_VCPU],
                      id intptr[0:2]) fd_kvmcpu
```

本文程序让它引用 `r1`，选择 vCPU ID 0，并把返回资源保存为 `r2`：

```text
r2 = ioctl$KVM_CREATE_VCPU(r1, AUTO, 0x0)
```

对象关系为：

```text
KVM_CREATE_VM.Ret: ResultArg(fd_kvmvm)
    └── KVM_CREATE_VCPU.Args[0]: ResultArg(Res=前者)

KVM_CREATE_VCPU.Ret: ResultArg(fd_kvmcpu)
```

### 6. `KVM_ARM_VCPU_INIT`：架构专用结构体

通用 KVM 描述放在 `dev_kvm.txt`，ARM64 专用部分放在 `dev_kvm_arm64.txt`。后者只对 `meta arches["arm64"]` 生效。

`sys/linux/dev_kvm_arm64.txt:43-49,70` 中的规则是：

```text
kvm_vcpu_target = ..., KVM_ARM_TARGET_GENERIC_V8

kvm_vcpu_init {
    target   flags[kvm_vcpu_target, int32]
    feature  flags[kvm_vcpu_feature_bits_arm64, int32]
    pad      array[const[0, int32], 6]
}

ioctl$KVM_ARM_VCPU_INIT(
    fd fd_kvmcpu,
    cmd const[KVM_ARM_VCPU_INIT],
    arg ptr[in, kvm_vcpu_init])
```

本文程序中的：

```text
&AUTO={0x5, 0x0, ""}
```

解析为：

```text
PointerArg
└── GroupArg(kvm_vcpu_init, 32 bytes)
    ├── ConstArg(target=5)       KVM_ARM_TARGET_GENERIC_V8
    ├── ConstArg(feature=0)      未启用可选 feature 位
    └── DataArg(24 zero bytes)   六个 32 位保留字段
```

`array[const[0, int32], 6]` 的每个元素都固定为零。因为整段数组内容固定，编译后的具体对象可以用一个 24 字节 `DataArg` 紧凑保存，而不一定表现为六个单独的 `ConstArg`。

### 7. `KVM_RUN`：使用 vCPU 资源

`sys/linux/dev_kvm.txt:60`：

```text
ioctl$KVM_RUN(fd fd_kvmcpu,
              cmd const[KVM_RUN],
              arg const[0])
```

它要求第一项是 `fd_kvmcpu`，因此本文程序中的参数引用 `KVM_CREATE_VCPU.Ret`。`cmd` 固定为 `0xae80`，第三个参数固定为 0。

这项规则只表达“如何调用 `KVM_RUN`”，不会自动建立一台完整 guest。实际 KVM 程序通常还要设置寄存器和 guest 代码，并通过描述中的 `mmap$KVM_VCPU` 映射 `struct kvm_run` 来读取退出原因。帮助 syzkaller 准备更完整 guest 状态的机制放到本阶段边界处再介绍。

---

## 六、资源如何在调用间流转

### 1. 资源不是 fd 数字的别名

`resource` 建模的是带类型的生产者—使用者关系。编译后，每类资源由 `ResourceDesc` 保存静态信息：

- `Name` 是资源名；
- `Kind` 是包含继承关系的类型链；
- `Values` 是无需生产者的特殊值；
- `Ctors` 记录能够生产该资源的调用。

当前 `linux/arm64` 目标中的实际结果为：

```text
fd          Kind=[fd]             Values=[-1]
fd_kvm      Kind=[fd, fd_kvm]     Values=[-1]
fd_kvmvm    Kind=[fd, fd_kvmvm]   Values=[-1]
fd_kvmcpu   Kind=[fd, fd_kvmcpu]  Values=[-1]
```

`fd_kvm`、`fd_kvmvm` 和 `fd_kvmcpu` 都能用于要求一般 `fd` 的位置，但三个专用类型彼此不能随意替换。这个类型层级直接塑造生成器可选择的调用依赖。

### 2. 本文程序的完整资源图

```text
openat$kvm.Ret : fd_kvm                          文本名 r0
        │
        └── KVM_CREATE_VM.Args[0]
                    │
                    ▼
           KVM_CREATE_VM.Ret : fd_kvmvm          文本名 r1
                    ├── KVM_SET_USER_MEMORY_REGION.Args[0]
                    └── KVM_CREATE_VCPU.Args[0]
                                  │
                                  ▼
                         KVM_CREATE_VCPU.Ret : fd_kvmcpu   文本名 r2
                                  ├── KVM_ARM_VCPU_INIT.Args[0]
                                  └── KVM_RUN.Args[0]
```

如果这些位置都只是普通整数，syzkaller 不会知道：

- `KVM_CREATE_VM` 前面通常需要一个能产生 `fd_kvm` 的调用；
- 注册内存和创建 vCPU 应引用同一个 VM 对象；
- 初始化与运行应引用创建出来的 vCPU 对象；
- 删除某个生产者时，后续哪些参数会失去来源。

`ResultArg` 还在生产者一侧维护反向使用集合。克隆、删除调用或替换资源时，`prog` 包可以找到并维护这些引用。

### 3. 资源也可以由输出结构体产生

资源不一定来自系统调用返回寄存器。KVM 自己就有一个例子：

```text
ioctl$KVM_CREATE_DEVICE(fd fd_kvmvm,
                        cmd const[KVM_CREATE_DEVICE],
                        arg ptr[inout, kvm_create_device])

kvm_create_device {
    type   flags[kvm_device_type, int32]   (in)
    fd     fd_kvmdev                       (out)
    flags  flags[kvm_device_flags, int32]  (in)
}
```

该 `ioctl` 的普通返回值只表示成功或错误，真正的设备 fd 由内核写入 `arg->fd`。具体程序中，这个结构体是 `GroupArg`，其中 `fd` 字段是输出方向的 `ResultArg`；后续 KVM device `ioctl` 可以引用它。

因此，资源常见的产生方式至少有两种：

```text
系统调用直接返回资源       KVM_CREATE_VM → fd_kvmvm
内核写入输出内存中的资源   KVM_CREATE_DEVICE.arg.fd → fd_kvmdev
```

二者都能形成后续调用可引用的 `ResultArg`，只是 executor 取值的位置不同。

### 4. 生产者失败时会怎样

对象图记录的是“后一个参数应使用前一个结果”，但不能保证生产调用成功。生成的 C 会先把资源槽初始化为 `0xffffffffffffffff`；只有系统调用返回值不是 `-1` 时，才用真实 fd 覆盖资源槽。

所以，如果目标机没有 `/dev/kvm`、权限不足或 VM 创建失败，后续调用会拿到无效资源并正常返回错误。资源关系提高了程序进入有意义状态的概率，却不是调用成功的证明。

---

## 七、先汇总本例中的结构，再看它们怎样影响变异

前六节是按照 KVM 调用顺序展开的，所以各类 `Type` 和 `Arg` 分散在不同调用中。讨论变异之前，先把本文确实见过的对象放到一张表中：

| 静态规则 `Type` | 具体对象 `Arg` | 本文中的实例 |
|---|---|---|
| `ResourceType` | `ResultArg` | `fd_kvm`、`fd_kvmvm`、`fd_kvmcpu` 的产生与引用 |
| `ConstType` | `ConstArg` | 各个 KVM `ioctl` 固定的 `cmd` |
| `IntType` | `ConstArg` | `KVM_CREATE_VM` 的 VM type、`KVM_CREATE_VCPU` 的 vCPU ID |
| `FlagsType` | `ConstArg` | memory slot、内存 flags、vCPU target 和 feature bitmap |
| `PtrType` | `PointerArg` | 指向 `kvm_userspace_memory_region` 和 `kvm_vcpu_init` |
| `StructType` | `GroupArg` | 上述两个结构体及其字段列表 |
| `VmaType` | `PointerArg` | guest RAM 对应的数据区子区域 |
| `LenType` | `ConstArg` | `size=len[addr]` 得到的 `0x1000` |
| `BufferType` | `DataArg` | 固定的 `/dev/kvm\0` 字节和 vCPU init 中的 24 字节零填充 |

接下来只讨论这些已经出现的节点。阶段 4 才会进入具体变异函数和选择概率。

### 1. 资源引用决定哪些调用能够接在一起

本文已经建立了：

```text
fd_kvm → fd_kvmvm → fd_kvmcpu
```

因此，当程序中存在多个 VM 或 vCPU 资源时，资源参数可以改为引用另一个**类型兼容**的生产结果；它不能随意拿普通整数或错误种类的 fd 代替。若删除 `KVM_CREATE_VM`，还必须处理引用其返回值的内存注册和 vCPU 创建调用。

这里谈的是资源结构对变异施加的边界。如何选择另一项资源、何时插入生产调用以及删除后怎样修复程序，留到阶段 4 结合实现代码讨论。

### 2. 指针使变异能够进入结构体内部

本文已经看到两棵具体对象树：

```text
PointerArg
└── GroupArg(kvm_userspace_memory_region)
    ├── slot
    ├── flags
    ├── paddr
    ├── size
    └── addr

PointerArg
└── GroupArg(kvm_vcpu_init)
    ├── target
    ├── feature
    └── pad
```

所以，变异不必把第三个 `ioctl` 参数当成一个任意地址。它可以保留外层指针和结构体布局，只选择其中的 `feature`、`slot` 或 `paddr` 等字段修改；也可以把整个指针换成 `NULL` 等特殊值，以测试内核的错误处理路径。

这两项主线调用都使用 `ptr[in, ...]`，意思是 executor 在调用前准备结构体内容，内核读取它。第六节额外见过的 `KVM_CREATE_DEVICE` 使用 `ptr[inout, ...]`：调用前结构体中已有输入字段，调用后内核还会把设备 fd 写回输出字段。本文没有使用只写不读的 `ptr[out, ...]` 实例，因此这里不继续扩展。

### 3. 都保存为 `ConstArg`，不表示规则相同

下面几种值在具体程序里都是 `ConstArg`，但它们背后的 `Type` 不同，因而变异含义也不同：

| 位置 | 静态规则 | 对变异的含义 |
|---|---|---|
| `KVM_CREATE_VM.cmd` | `ConstType(KVM_CREATE_VM)` | 在这个 syscall 变体内是固定值，不能随意改成另一个 KVM 命令 |
| `KVM_CREATE_VCPU.id` | `IntType(intptr[0:2])` | 是一般整数；正常生成和数值变异会按描述中的范围处理它 |
| `kvm_vcpu_init.feature` | `FlagsType(kvm_vcpu_feature_bits_arm64)` | 可以优先增删描述中已知的 feature 位 |
| `kvm_userspace_memory_region.size` | `LenType(addr)` | 正常情况下与 `addr` 的区域大小联动 |

例如，把 `feature` 从 0 改为 `0x8`，只是修改 `GroupArg` 中对应的 `ConstArg`；外层指针、结构体布局、target、pad 和 vCPU 资源引用都可以保持不变。

`FlagsType` 提供的是优先尝试的已知语义值，不是“程序中绝不允许出现其他整数”的验证规则。fuzzer 仍然需要测试异常 flag，但它不必从完全无意义的 32 位空间开始探索。

### 4. `LenType` 与 `VmaType` 形成可维护的关系

本文的具体关系是：

```text
kvm_userspace_memory_region.size ──len[addr]──► addr.VmaSize
             0x1000                              0x1000
```

`prog/size.go:17-181` 中的大小计算逻辑可以沿 `addr` 路径找到 VMA。正常生成或调整 VMA 大小时，可以重新计算 `size`。

但 syzkaller 也要测试内核对错误长度的处理。`prog/mutation.go:376` 中的 `LenType.mutate()` 可以故意生成不符合描述关系的长度。因此，`len[addr]` 的准确含义是“syzkaller知道并能维护这项关系”，而不是“每次执行时两者都必须相等”。

### 5. 固定数据不会因为位于内存中就任意变化

`/dev/kvm\0` 在对象中是 `DataArg`，但其静态规则是固定字符串 `string["/dev/kvm"]`。同样，`kvm_vcpu_init.pad` 在描述中是六个固定为零的 32 位元素，编译后可紧凑保存为 24 字节 `DataArg`。它们位于内存中，不表示普通变异就应把每个字节任意改写。

本例没有出现 `UnionType/UnionArg`。因此本阶段不讨论 union 怎样选择分支；等后续遇到一项真正包含 union 的 syscall 描述时，再结合那项调用引入。

结构化变异的核心到这里可以概括为：它针对当前节点背后的 `Type` 修改 `Arg`，并尽量维护已经描述出来的资源、布局和长度关系；同时保留少量故意破坏关系、测试错误路径的机会。

---

## 八、结构化描述保证什么，不保证什么

### 1. 能提供的能力

- 知道调用属于哪个 OS/架构，以及正确的 ABI 宽度和系统调用号；
- 把不同 KVM `ioctl` 区分为各自的 fd、cmd 和 arg 规则；
- 按描述构造本文已经看到的指针、VMA、结构体和固定数据；
- 区分输入内存、输出内存与双向内存；
- 建立长度字段与目标对象的关系；
- 建立资源生产、引用、继承和特殊值关系；
- 让生成、变异和最小化按语义节点工作；
- 把同一对象图稳定地序列化成 syz 文本或 executor 指令。

### 2. 不能保证的事情

- syscall 描述本身一定完整或完全正确；
- `/dev/kvm` 在目标机上存在且当前进程有权限访问；
- 当前内核支持所选 VM 类型、vCPU target 或 feature；
- 每个资源生产调用都成功；
- 长度、flag 和指针每次都严格合法，因为 fuzzer 会有意测试异常输入；
- 单靠类型就能表达 KVM 的全部状态机和调用先后条件；
- 结构正确就一定产生新覆盖率。

本文程序很好地说明了边界。它的 ABI、指针布局、长度关系和 fd 对象层级都成立，但它仍没有：

- 向 guest RAM 写入可执行指令；
- 设置 vCPU 的 PC、栈和通用寄存器；
- 建立处理特定中断所需的 irqchip；
- 映射并读取 `struct kvm_run` 以处理 VM exit；
- 根据目标机能力选择一定受支持的 vCPU feature。

因此，`KVM_RUN` 可能失败，也可能很快退出；这不与结构化描述矛盾。syzlang 主要描述参数形状和一部分显式关系，不是完整的 KVM 正确性证明。

到这里才需要引入一个新的概念：**伪调用**。名字以 `syz_` 开头的调用通常不是同名 Linux 系统调用，而是由 executor 中的辅助函数实现。一个伪调用可以整理输入、准备复杂状态，或者在内部执行多个实际操作。它与 `ioctl$KVM_CREATE_VM` 这种 `$` 变体不同：后者最终仍是一次普通 `ioctl(2)`，前者没有可以直接进入内核的同名 syscall number。

仓库中的 `syz_kvm_setup_syzos_vm$arm64`、`syz_kvm_add_vcpu$arm64` 和旧式 `syz_kvm_setup_cpu$arm64` 就是 KVM 相关伪调用。它们帮助构造更有执行意义的 guest 状态，但涉及 guest 指令、选项数组和更多 KVM 细节，不适合在第一次理解 `Prog/Type/Arg` 时作为主例，后续研究 KVM fuzzing 实现时再展开。

---

## 九、描述怎样生成并加载为 `Target`

### 1. 为什么还需要 `.const`

`.txt` 可以写 `KVM_CREATE_VM`、`KVM_ARM_TARGET_GENERIC_V8` 等符号，但这些符号的值取决于 OS、架构和内核头。结构描述与数值来源因此被分开：

```text
sys/linux/*.txt       参数和类型规则，以及引用了哪些符号
sys/linux/*.const     各架构上这些符号的具体数值
```

例如，当前 `dev_kvm_arm64.txt.const` 中记录：

```text
KVM_ARM_TARGET_GENERIC_V8 = arm64:5
KVM_ARM_VCPU_PMU_V3_BIT   = arm64:8
```

这就是程序中的 `target=0x5` 和后续实验中的 `feature=0x8` 的数值来源。

### 2. `syz-extract`：从内核环境取得符号值

入口位于 `sys/syz-extract/extract.go:72`。它先解析 syscall 描述，找出所需 include、常量和系统调用号，再针对所选架构生成辅助 C 源码。

`sys/syz-extract/fetch.go` 显示，辅助源码会由目标编译器处理；工具随后根据目标提取器的实现，运行生成程序或从 ELF 特定节读取数值。读取 ELF 的方式也避免了直接运行其他架构程序。结果按描述文件和架构写入 `sys/<os>/*.const`。

较准确的过程是：

```text
分析 .txt 所需符号
    → 生成包含相应内核头的 C
    → 编译
    → 运行辅助程序或读取 ELF
    → 写入分架构 .const
```

`make extract SOURCEDIR=<kernel>` 需要内核源码或构建目录，通常只在更新描述引用的常量、系统调用或架构信息时运行。

### 3. `syz-sysgen`：编译规则

入口位于 `sys/syz-sysgen/sysgen.go:75`。对每个 OS，它会：

1. 用 `pkg/ast` 解析全部 `.txt`；
2. 读取 `.const`；
3. 按架构调用 `pkg/compiler.Compile()`；
4. 得到 `Syscall`、`ResourceDesc`、`Type`、常量和 flags；
5. 生成 Host 与 executor 分别需要的产物。

当前主要产物是：

| 产物 | 用途 |
|---|---|
| `sys/gen/<os>_<arch>.gob.flate` | 压缩保存 Host 端的 `Syscall/Resource/Type` 描述 |
| `sys/register.go` | 嵌入描述文件并登记所有 OS/架构 `Target` |
| `executor/syscalls.h` | executor 的调用名称、系统调用号和伪调用函数表 |
| `executor/defs.h` | executor 使用的目标布局和属性定义 |

`sys/generated/generated.go` 提供压缩描述的序列化、解码与注册逻辑。目录名虽然是 `generated`，但它是加载支持包，不应与 `sys/gen/*.gob.flate` 这一生成数据目录混为一谈。

`make generate` 使用仓库中已有的 `.const`，不依赖一份现场内核源码；它最终调用 `syz-sysgen` 重新生成各架构的描述和 executor 表。

### 4. 运行时怎样得到 `Target`

Host 端工具通常会导入：

```go
import _ "github.com/google/syzkaller/sys"
```

这个空白导入不是为了直接调用包函数，而是触发 `sys/register.go` 的 `init()`。它为每个 OS/架构调用 `generated.Register()`，登记一个包含 ABI 基本信息和延迟填充函数的 `Target`。

随后：

```go
target, err := prog.GetTarget("linux", "arm64")
```

第一次取得目标时，`prog/target.go:103-115` 的 `GetTarget()` 会触发延迟初始化：

```text
解压 sys/gen/linux_arm64.gob.flate
    → 填入 Syscalls / Resources / Consts / Flags / Types
    → 为 Type 恢复内部引用
    → 分配 Syscall.ID
    → 建立 SyscallMap / ConstMap / FlagsMap
    → 计算资源生产者等辅助索引
    → 执行 Linux 目标专用初始化
```

至此，解析器、生成器和变异器才得到可直接使用的 `linux/arm64` 规则集合。

### 5. 完整生成链

```text
                      需要内核源码或构建目录
内核头文件 ───────────────┐
                         │ syz-extract
sys/linux/*.txt ─────────┴────────► sys/linux/*.const
       │                                  │
       └──────────────┬───────────────────┘
                      │ syz-sysgen + pkg/compiler
                      ▼
        ┌────────────────────────────────────────┐
        │ sys/gen/linux_arm64.gob.flate          │ Host 规则
        │ sys/register.go                        │ Target 注册
        │ executor/syscalls.h / defs.h           │ executor 调用表
        └────────────────────────────────────────┘
                      │
                      │ 导入 sys；GetTarget()
                      ▼
          Target / Syscall / Type
                      │
                      │ 解析、生成、变异
                      ▼
              Prog / Call / Arg
```

这条链也说明 Host 与 executor 为什么要检查 description revision。这个 revision 是编译后描述数据的哈希标识；两侧不一致时，同一个 call ID 可能表示不同调用，因此不能继续执行。

---

## 十、动手实验：从 KVM syz 程序到 C

### 1. 准备程序

把本文开头的程序保存为 `/tmp/stage3-kvm-arm64.syz`，然后执行：

```bash
./bin/syz-prog2c \
    -prog /tmp/stage3-kvm-arm64.syz \
    -os linux \
    -arch arm64 \
    -strict
```

这里必须显式选择 `arm64`，因为 `ioctl$KVM_ARM_VCPU_INIT` 只属于 ARM64 `Target`。`-strict` 让反序列化在调用名或参数无法按描述解析时直接失败；非严格模式会尽量用默认值修补旧程序，适合兼容描述更新后的 corpus，但不适合本次核对。

转换成功说明：这段文本能被当前 `linux/arm64` 描述严格解析，并能生成对应 C。它不等于已经在 ARM64 KVM 环境中运行成功。

### 2. 观察生成 C 中的资源和结构体

生成 C 的关键部分可缩写为：

```c
uint64_t r[3] = {
    0xffffffffffffffff,
    0xffffffffffffffff,
    0xffffffffffffffff,
};

/* syz-prog2c 自动加入的数据区初始化，不来自原 Prog.Calls。 */
syscall(__NR_mmap, 0x1ffff000ul, 0x1000ul, 0,
        0x32ul, (intptr_t)-1, 0);             /* 左保护页 */
syscall(__NR_mmap, 0x20000000ul, 0x1000000ul, 7,
        0x32ul, (intptr_t)-1, 0);             /* 数据区 */
syscall(__NR_mmap, 0x21000000ul, 0x1000ul, 0,
        0x32ul, (intptr_t)-1, 0);             /* 右保护页 */

memcpy((void*)0x20000040, "/dev/kvm\000", 9);
res = syscall(__NR_openat, 0xffffffffffffff9cul,
              0x20000040ul, 2, 0);
if (res != -1)
    r[0] = res;

res = syscall(__NR_ioctl, r[0], 0xae01, 0);
if (res != -1)
    r[1] = res;

*(uint32_t*)0x20000080 = 0;          /* slot */
*(uint32_t*)0x20000084 = 0;          /* flags */
*(uint64_t*)0x20000088 = 0;          /* guest physical address */
*(uint64_t*)0x20000090 = 0x1000;     /* len[addr] */
*(uint64_t*)0x20000098 = 0x20010000; /* userspace VMA */
syscall(__NR_ioctl, r[1], 0x4020ae46, 0x20000080ul);

res = syscall(__NR_ioctl, r[1], 0xae41, 0);
if (res != -1)
    r[2] = res;

*(uint32_t*)0x200000c0 = 5;          /* target */
*(uint32_t*)0x200000c4 = 0;          /* feature */
memset((void*)0x200000c8, 0, 24);    /* pad */
syscall(__NR_ioctl, r[2], 0x4020aeae, 0x200000c0ul);

syscall(__NR_ioctl, r[2], 0xae80, 0);
```

可以直接观察到：

- 三条初始化 `mmap` 由 C reproducer 生成器自动添加，原 `.syz` 的六个 `Call` 中没有它们；
- 三类资源被保存到不同槽位，并由后续调用引用；
- 所有 KVM `ioctl$...` 最终都变成 `__NR_ioctl`；
- 各变体的固定 `cmd` 已变成相应常量值；
- `GroupArg` 的各个字段按 ARM64 ABI 写入连续内存；
- VMA 大小 `0x1000` 被写进关联的 `size` 字段；
- 若生产调用失败，槽位保留无效资源值，后续调用可正常失败。

### 3. 只修改一个嵌套结构化参数

复制程序，只把 `kvm_vcpu_init.feature` 从 0 改为 `0x8`：

```diff
-ioctl$KVM_ARM_VCPU_INIT(r2, AUTO, &AUTO={0x5, 0x0, ""})
+ioctl$KVM_ARM_VCPU_INIT(r2, AUTO, &AUTO={0x5, 0x8, ""})
```

在当前 ARM64 常量表中，`0x8` 是 `KVM_ARM_VCPU_PMU_V3_BIT`。再次运行 `syz-prog2c`，生成 C 只有对应字段发生实质变化：

```diff
-*(uint32_t*)0x200000c4 = 0;
+*(uint32_t*)0x200000c4 = 8;
```

外层指针、结构体大小、target、保留字段和全部资源引用都保持不变。这是结构化参数变异的一个最小模型：定位 `PointerArg → GroupArg → feature ConstArg`，只修改有明确语义的节点。

这仍不保证目标内核支持 PMU v3。类型描述给出了值得尝试的 feature 位，运行结果再由内核能力和当前状态决定。

### 4. 检查实际 Go 对象

本次还用临时 Go 程序执行了：

```go
target, _ := prog.GetTarget("linux", "arm64")
p, _ := target.Deserialize(data, prog.Strict)
```

递归打印 `p.Calls` 和各类 `Arg` 后确认：

```text
KVM_CREATE_VM.Args[0]
    = ResultArg(fd_kvm, Res=openat$kvm.Ret)
KVM_CREATE_VM.Ret
    = ResultArg(fd_kvmvm, out)

KVM_SET_USER_MEMORY_REGION.Args[2]
    = PointerArg
      → GroupArg(kvm_userspace_memory_region)
        → ConstArg(size=0x1000, type=LenType)
        → PointerArg(Address=0x10000, VmaSize=0x1000, type=VmaType)

KVM_CREATE_VCPU.Args[0]
    = ResultArg(fd_kvmvm, Res=KVM_CREATE_VM.Ret)
KVM_CREATE_VCPU.Ret
    = ResultArg(fd_kvmcpu, out)

KVM_ARM_VCPU_INIT.Args[2]
    = PointerArg
      → GroupArg(kvm_vcpu_init)
        → ConstArg(target=5)
        → ConstArg(feature=0)
        → DataArg(24 zero bytes)

KVM_RUN.Args[0]
    = ResultArg(fd_kvmcpu, Res=KVM_CREATE_VCPU.Ret)
```

这同时验证了三件事：文本资源名最终是对象引用，结构体是嵌套 `Arg`，`AUTO` 长度已在 Host 端变成具体值。

### 5. 测试结果

在当前工作树执行：

```bash
GOCACHE=/tmp/stage3-gocache /usr/local/go/bin/go test ./prog
```

`prog` 包测试通过。该测试覆盖范围远大于本文例子，但它只说明当前 `prog` 包的解析、生成、变异等测试整体通过，不等于已经在 VM 中验证了这条 ARM64 KVM 程序的运行结果。

---

## 十一、现在应能独立读懂一段 `.txt`

以后看到一项 syscall 描述，可以按以下顺序阅读：

```text
调用名
  → 是否有 $ 变体，基础 CallName 是什么
  → 每个参数的外层类型
  → 指针所指对象和方向
  → 指针所指结构体或其他复合对象的内部布局
  → len 指向哪个字段
  → 输入和输出 resource
  → 返回资源类型
  → 调用属性
```

以这项规则为例：

```text
ioctl$KVM_SET_USER_MEMORY_REGION(
    fd fd_kvmvm,
    cmd const[KVM_SET_USER_MEMORY_REGION],
    arg ptr[in, kvm_userspace_memory_region])
```

应该能说出：

1. 它是基础系统调用 `ioctl` 的一个 syzkaller 变体；
2. 第一个参数是输入资源，必须与 `fd_kvmvm` 兼容；
3. 第二个参数不是任意命令，而是固定 `KVM_SET_USER_MEMORY_REGION`；
4. 第三个参数是指向输入结构体的指针；
5. 具体程序中的外层是 `PointerArg`，结构体是 `GroupArg`，各字段继续对应子 `Arg`；
6. `size len[addr]` 表明该字段能根据 `addr` VMA 的大小计算；
7. 调用没有声明资源返回类型，所以 `Call.Ret == nil`；这不表示 Linux `ioctl(2)` 没有普通整数返回值，executor 仍会记录执行结果；
8. 该规则能帮助生成形状和依赖合理的请求，但不保证目标机接受该内存布局。

### 阶段 3 的出师问题

#### `resource` 在变异时意味着什么？

该参数不是任意整数，而应优先引用一项类型兼容的已有资源；缺少资源时，生成过程可能需要插入相关生产调用。替换或删除生产者还要维护所有使用关系。`fd_kvmvm` 和 `fd_kvmcpu` 虽然都继承 `fd`，但不能在要求专用类型的位置互换。

#### `ptr` 在变异时意味着什么？

它是带指向类型和方向的内存对象。变异可以深入 `kvm_userspace_memory_region` 或 `kvm_vcpu_init` 的字段，同时重新安排内存；也可以有意尝试 `NULL` 等特殊指针。

#### `flags` 在变异时意味着什么？

该整数位置有一组已知语义值及组合方式。变异可以优先增删或替换这些位，例如改变 vCPU feature bitmap，而不是只做无差别整数扰动。

#### `len` 是否保证长度永远正确？

不保证。`LenType` 让正常生成和结构修改能够计算关联对象的大小，也允许专门的长度变异故意制造不一致，以测试内核的边界检查。

#### `Type` 与 `Arg` 的根本区别是什么？

`Type` 是由 syscall 描述编译得到、可被许多程序共享的规则；`Arg` 是某一条程序中依照该规则建立的具体值。`Call.Meta.Args[i].Type` 与 `Call.Args[i]` 分别位于这两个层次。

#### `Prog` 为什么不只是 `[]Call`？

因为它还必须绑定一个 `Target`，而调用内部包含嵌套参数、资源引用和调用属性。资源又会在不同调用间建立引用，所以完整程序是一张有调用顺序的对象图。

#### 这条 KVM 程序为什么“结构成立”却不一定“能运行 guest”？

类型描述已经保证 fd 层级、命令号、结构体布局和长度关系可被正确表达；但创建可运行 guest 还涉及内核能力、寄存器、guest 指令、退出处理等更高层状态。这些不可能仅由当前几项参数类型自动推导。

---

## 十二、本阶段结论与下一阶段边界

阶段 3 可以压缩成两条链：

```text
描述链：syzlang + 常量
        → Syscall / Type / ResourceDesc
        → Target

程序链：Target 的规则
        → Prog / Call / Arg
        → 可读 syz 文本或 executor 指令
```

`Target/Syscall/Type` 是静态接口模型，`Prog/Call/Arg` 是一次测试的具体对象。KVM 例子清楚展示了这种分工：

- `$` 变体把同一个 `ioctl` 拆成不同参数模型；
- `fd_kvm → fd_kvmvm → fd_kvmcpu` 表达内核对象的生产和依赖；
- `PointerArg → GroupArg → 子 Arg` 保存嵌套结构；
- `LenType` 关联 VMA 大小；
- 结构化变异可以只修改一个 feature 字段，同时保留其他关系。

本阶段已经回答“程序是什么、规则从哪里来”。阶段 4 将沿用这些对象，继续追踪：

- `Target.Generate()` 怎样从空状态选择调用和构造 `Arg`；
- `ChoiceTable` 怎样影响调用组合；
- `Prog.Mutate()` 怎样插入、删除、替换和递归修改参数；
- 变异资源或指针时，怎样自动增加调用和修复依赖；
- 为什么某些长度会联动更新，而另一些会被故意改错。

---

## 源码导航

| 主题 | 位置 |
|---|---|
| `Prog`、`Call`、各类 `Arg` | `prog/prog.go` |
| `Syscall`、`Type`、各具体 `Type` | `prog/types.go` |
| `Target` 注册、延迟初始化和索引 | `prog/target.go` |
| `len` 路径解析与大小计算 | `prog/size.go` |
| syz 程序文本解析与序列化 | `prog/encoding.go` |
| executor 指令序列化 | `prog/encodingexec.go` |
| 可读程序语法 | `docs/program_syntax.md` |
| syscall 描述语法 | `docs/syscall_descriptions_syntax.md` |
| 通用 KVM 资源、ioctl 和结构体 | `sys/linux/dev_kvm.txt` |
| ARM64 KVM 调用、vCPU 结构和 feature | `sys/linux/dev_kvm_arm64.txt` |
| 架构常量提取 | `sys/syz-extract/` |
| 描述编译和 executor 表生成 | `sys/syz-sysgen/` |
| 压缩描述的加载与注册 | `sys/generated/generated.go`、`sys/register.go` |
