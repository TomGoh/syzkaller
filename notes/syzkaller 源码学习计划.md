# syzkaller 源码学习计划（自顶向下 · 源码级）

> 目标：从"目的与要解决的问题"入手，先走通一条端到端数据流（脊柱），再逐模块深挖源码。范围：纯 syzkaller 本身（暂不掺入 pKVM/EL2）。风格：源码级 + 动手实验。
> 创建于 2026-07-16。所有 `file:line` 均对着当时的工作树核对过，读之前若行号对不上，以就近的函数名为准。

## 怎么用这份计划

三条贯穿始终的原则：

1. **以"一个程序的一生"（阶段 2）为脊柱**。读任何模块时都问一句："它是这条链上的哪一棒？"——这是自顶向下不迷路的关键。
2. **`_test.go` 是最好的 API 文档**。读 `prog/mutation_test.go`、`pkg/fuzzer/fuzzer_test.go` 往往比读实现更快建立正确心智模型。
3. **能画出来 / 能复述才算懂**。每个阶段末尾的"出师标准"就是自检点，勾掉它再往下走。

依赖关系：阶段 0 → 1 → 2 是地基，**不能跳**；之后 3、5、6 是重点，4 / 7 / 8 可按兴趣微调顺序。

图例：`- [ ]` 是可勾选的进度项，读完 / 做完就打勾；每个阶段留了 **我的笔记** 空位，边学边填。

> **笔记约定**：每阶段单独成文（`notes/阶段N-*.md`）。笔记内容要**完整、自包含**——把讲解的完整版（含图、表、推理、源码 `file:line` 依据）都写进去，**不做压缩/省略**，回看笔记时不需要再翻聊天记录补全。图里的 ASCII 连接线要对齐。**纠正/深化时，直接用正确版替换掉旧的不完整版，不要留"简化版 + 补正"两个矛盾的东西并存误导人。**

---

## 总进度

- [x] 阶段 0 — 目的与要解决的问题 → 笔记：`notes/阶段0-目的与要解决的问题.md`
- [x] 阶段 1 — 项目结构：全景地图 → 笔记：`notes/阶段1-项目结构全景地图.md`
- [x] 阶段 2 — 端到端主线：一个程序的一生 ★脊柱 → 笔记：`notes/阶段2-端到端主线-一个程序的一生.md`
- [x] 阶段 3 — 程序到底是什么：prog + sys → 笔记：`notes/阶段3-结构化程序-Prog与syzlang.md`
- [x] 阶段 4 — 生成与变异：程序怎么来的 → 笔记：`notes/阶段4-生成与变异-程序怎么来的.md`
- [ ] 阶段 5 — 反馈闭环：signal → corpus ★核心
- [ ] 阶段 6 — 执行器深潜：executor.cc
- [ ] 阶段 7 — 覆盖率处理与符号化：pkg/cover
- [ ] 阶段 8 — 编排层：manager / RPC / VM / 崩溃
- [ ] 阶段 9 — 综合贯通（capstone）

预估节奏：约 8–12 个半天，按精读程度伸缩。

---

## 阶段 0 — 目的与要解决的问题（半天）

**目标**：两句话说清 syzkaller 是什么、凭什么强过随机 fuzzing。

**读什么**

- [x] `README.md` 顶部
- [x] `docs/internals.md`
- [x] `docs/coverage.md`
- [x] `docs/research.md`（论文 / talk 索引，挑一个 talk 看）

**主线问题**：syzkaller 的**两根支柱**是什么，为什么缺一不可？

参考答案：① **结构化 / 系统调用感知**（它懂 syscall 的参数结构，不是瞎填字节）；② **覆盖率引导**（你已吃透的 KCOV 反馈）。只有①会像 random 一样浅，只有②不懂 syscall 结构会卡在参数校验。

**动手**

- [x] 读 README 顶部 + 看一张 `docs/process_structure.png`

**出师**

- [x] 对着白纸画出 `syz-manager(host)` ↔ `syz-executor(VM 内)` ↔ `瞬态测试子进程` 三层，说出各自职责

**我的笔记**

→ 详见 `notes/阶段0-目的与要解决的问题.md`


---

## 阶段 1 — 项目结构：全景地图（半天）

**目标**：把顶层目录映射到职责，建立"东西都在哪"的空间感。

**读什么**

- [x] 顶层 `ls`
- [x] `Makefile` 的 `target` / `executor` / `manager` / `execprog` 目标（114–207 行），看清哪些二进制怎么来

**目录速记表**（背下来）：

| 目录 | 职责 |
|---|---|
| `prog/` | 跨 OS 复用的程序表示与生成/变异算法；具体数据受 `prog.Target` 约束 |
| `sys/` | 系统调用描述（syzlang `.txt`）+ 代码生成（`syz-extract`/`syz-sysgen`） |
| `executor/` | C++ 执行器（跑在 VM 里） |
| `pkg/fuzzer/` | 反馈闭环（signal→corpus→变异） |
| `pkg/signal/` `pkg/corpus/` | 覆盖率信号类型 / 语料库 |
| `pkg/rpcserver/` `pkg/flatrpc/` | manager↔executor 的 RPC |
| `pkg/cover/` | PC→file:line 符号化 + 报告 |
| `vm/` | VM 后端（qemu/gce/isolated/...） |
| `syz-manager/` | 顶层编排 |
| `pkg/report/` `pkg/repro/` | 崩溃解析与复现 |

**动手**

- [x] 装好 Go + C 工具链后 `make`，看 `bin/` 里生成了哪些二进制（本环境实测：host→`bin/`，target→`bin/linux_amd64/`，executor 为 static-pie；详见笔记第五节）

**出师**

- [x] 随口说一个功能（"变异一个程序""把 PC 变成行号"），能立刻指出目录

**我的笔记**

→ 详见 `notes/阶段1-项目结构全景地图.md`

---

## 阶段 2 — 端到端主线：一个程序的一生（1 天）★脊柱

**目标**：钻进任何单模块**之前**，先把一条完整数据流走通。先有脊柱，再挂器官。

**主线**（照着追接力棒，只求"看到传递"，不抠细节）：

```
syz-manager（Host）
  genFuzz：从零生成，或从 corpus 选择程序并变异
        │
        ▼
  *prog.Prog → queue.Request（Host 进程内的调度对象）
        │ rpcserver.Runner.sendRequest
        │ Prog.SerializeForExec()
        ▼
  FlatRPC ExecRequest{Data: copyin/call/copyout 字节流}
        │ FlatRPC over TCP
        ▼
syz-executor（Target）
  runner → Proc → syz-executor exec / fork server
        │ fork 瞬态测试子进程
        ▼
  execute_one → execute_call → KCOV → CallInfo/ProgInfo
        │ FlatRPC ExecResult
        ▼
syz-manager（Host）
  queue.Result → processResult → triageProgCall
        │ 有潜在新 signal
        ▼
  重复执行去抖 → minimize 并重执行验证 → Corpus.Save
        │
        └──► corpus.db，并成为后续变异素材
```

**读什么**（每一棒扫一眼，看到接力即可）

- [x] `pkg/fuzzer/fuzzer.go:285` `genFuzz` + `pkg/fuzzer/job.go:53` — 选择从零生成或 corpus 变异
- [x] `prog/generation.go:12` `Target.Generate` — 从零生成 `Prog`
- [x] `prog/encodingexec.go:69` `Prog.SerializeForExec` — 降为 executor 能解释的字节流
- [x] `pkg/rpcserver/runner.go:141`、`:306` — 取请求、封装并发送 `ExecRequest`
- [x] `executor/executor_runner.h` + `executor/executor.cc:582` — runner、`Proc` 与 `exec` 进程接力
- [x] `executor/common.h:609` — fork server 为一次请求启动瞬态测试子进程
- [x] `executor/executor.cc:933`、`:1225`、`:1377` — 解释指令、形成 signal/cover 和调用结果
- [x] `pkg/rpcserver/runner.go:426` — 把 `ExecResult` 交回原 `queue.Request`
- [x] `pkg/fuzzer/fuzzer.go:231` `triageProgCall` — 判断是否存在潜在新 signal
- [x] `pkg/fuzzer/job.go` + `pkg/corpus/corpus.go:130` — 去抖、最小化并保存
- [x] `syz-manager/manager.go:991` — 把新程序持久化到 `corpus.db`

**动手（无需 VM）**

- [x] `go test ./pkg/fuzzer/ -run Test -v`（本环境通过 `TestFuzz` 和 `TestDeflake`，本地 executor 无需 VM）
- [x] `./bin/syz-prog2c -help`，并把 `sys/linux/test/file` 的四条 syz 调用转换成 C

**出师**

- [x] 不看代码复述"生成→序列化→RPC→执行→采集→回传→triage→入库"这条链，并说出每一棒在哪个文件

**我的笔记**

→ 详见 `notes/阶段2-端到端主线-一个程序的一生.md`

---

## 阶段 3 — 程序到底是什么：prog + sys（1–2 天）

**目标**：理解"结构化"支柱——一个 `Prog` 是什么、syscall 如何被描述。

**读什么**

- [x] `prog/prog.go`、`prog/types.go` — `Prog`/`Call`/`Arg`/`Type` 数据模型
- [x] `prog/target.go` — `Target`（某 OS/arch 的全部 syscall 元信息）
- [x] `docs/program_syntax.md`、`docs/syscall_descriptions_syntax.md`
- [x] `sys/linux/*.txt` 挑几个读（本阶段精读 `dev_kvm.txt` 与 `dev_kvm_arm64.txt` 中的资源、`ioctl$` 变体、结构体和长度关系）
- [x] 生成链：`sys/syz-extract` 生成架构相关 `.const`，`sys/syz-sysgen` 编译 `.txt` 与 `.const`；主要产物是 `sys/gen/*.gob.flate`、`sys/register.go` 和 executor 头文件，`sys/generated/` 是加载支持包

**主线问题**：同一个 `ioctl(2)` 怎样通过 `$` 变体获得不同的 fd、cmd 和结构体规则？KVM 控制 fd、VM fd 与 vCPU fd 怎样建模为 `fd_kvm → fd_kvmvm → fd_kvmcpu` 的资源依赖？`kvm_userspace_memory_region` 中的指针、VMA 和 `len[addr]` 又怎样变成可递归修改的 `Arg` 对象？

**动手**

- [x] 手写一条 ARM64 KVM 接口骨架：打开 `/dev/kvm` → 创建 VM → 注册 guest 内存 → 创建并初始化 vCPU → `KVM_RUN`；用 `syz-prog2c -os linux -arch arm64 -strict` 查看 C，再把 `kvm_vcpu_init.feature` 从 `0` 改为 `KVM_ARM_VCPU_PMU_V3_BIT` 对应的 `0x8` 并比较差异

**出师**

- [x] 读懂一段 `.txt` 描述，解释 `resource`/`ptr`/`flags` 在变异时各意味着什么

**我的笔记**

→ 详见 `notes/阶段3-结构化程序-Prog与syzlang.md`

---

## 阶段 4 — 生成与变异：程序怎么来的（1–2 天）

**目标**：理解程序如何被创造和演化——这决定 fuzzer 探索空间的形状。

**读什么**

- [x] `prog/generation.go:12` `Target.Generate` — 从零生成
- [x] `prog/mutation.go:28` `Prog.Mutate` — 各类变异算子（insert/remove/mutate-arg/splice/squash）
- [x] `prog/rand.go:48` `newRand` / `randGen` — 随机参数、资源选择
- [x] `prog/prio.go` `ChoiceTable` — 哪些 syscall 组合更可能有意义
- [x] `prog/minimization.go` — 反向：缩到最小仍保留信号
- [x] 重点读 `prog/mutation_test.go`

**主线问题**：`ChoiceTable` 与资源依赖如何让“创建 VM 后创建 vCPU”比“对任意 fd 发出随机 KVM ioctl”更容易被生成？

**动手**

- [x] 运行相关 `prog` 变异测试（`GOCACHE=/tmp/stage4-gocache go test ./prog -run 'TestMutationFlags|TestChooseCall|TestMutateArgument|TestMutateTable|TestMutateRandom|TestMutateCorpus|TestSizeMutateArg' -v`）
- [x] 用临时 Go 实验观察一次删除调用和参数变异的结果（未改动生产源码）

**出师**

- [x] 列出主要变异算子，解释 `ChoiceTable`/优先级为何是"结构感知"的关键

**我的笔记**

→ 详见 `notes/阶段4-生成与变异-程序怎么来的.md`


---

## 阶段 5 — 反馈闭环：signal → corpus（1–2 天）★核心

**目标**：把你已懂的覆盖率接到 fuzzer 决策上——"coverage-guided"真正发生的地方。

**读什么**

- [ ] `pkg/signal/signal.go` — `Signal = map[pc]prio`，`Diff`/`Merge`（"什么叫新信号"）
- [ ] `pkg/fuzzer/fuzzer.go:285` `genFuzz` — 每步"生成 vs 变异"的抉择
- [ ] `pkg/fuzzer/fuzzer.go:231` `triageProgCall` — 发现新 signal 触发 triage
- [ ] `pkg/fuzzer/job.go:87` `triageJob`（验证信号稳定→最小化→入库）
- [ ] `pkg/fuzzer/job.go:448` `smashJob`（对好种子加大变异）
- [ ] `pkg/fuzzer/cover.go` — `maxSignal` 记账
- [ ] `pkg/corpus/corpus.go`、`minimize.go`、`prio.go`

**主线问题**：一个程序触发新边后，要经过哪些步（去抖 / 最小化 / 入库）才成为语料？**为什么要 triage 而不是直接入库**？

**动手**

- [ ] `go test ./pkg/fuzzer/ -v` 跟 `fuzzer_test.go`
- [ ] 跑起 manager 后开 web dashboard 看 corpus/coverage 增长曲线

**出师**

- [ ] 解释"新 signal → triage → minimize → corpus → 变成变异素材"这个正反馈，以及 `maxSignal` 去重的作用

**我的笔记**


---

## 阶段 6 — 执行器深潜：executor.cc（1 天）

**目标**：C++ 运行时如何在 VM 里安全、可复现地执行程序并采集覆盖率。KCOV 接缝已精读，这里补齐其余。

**读什么**

- [ ] `executor/executor.cc:582` `main` → `receive_execute`/`reply_execute` 请求循环
- [ ] 沙箱：`do_sandbox_none`/`setuid`/`namespace`（680+）
- [ ] 线程化执行 + 超时/挂起处理（960–1184 附近）
- [ ] 回传序列化：`write_signal`(1226) / `write_cover`(1261) / `write_comparisons`(1278)
- [ ] `executor/executor_linux.h`（KCOV 消费，已精读）

**主线问题**：为什么每个程序在 fork 出的瞬态测试子进程里跑？**threaded 模式**解决什么问题（阻塞型 syscall）？

**动手**

- [ ] 在 `write_signal` 加一行打印 `nsig`，`make executor` 重编，观察不同程序的信号量

**出师**

- [ ] 说清 executor 执行模型（子进程隔离、threaded、collide）与输出缓冲区布局

**我的笔记**


---

## 阶段 7 — 覆盖率处理与符号化：pkg/cover（半天–1 天）

**目标**：PC 如何还原成 file:line 并渲染成报告——你 KCOV 笔记里 `addr2line` 那步的工业实现。

**读什么**

- [ ] `pkg/cover/report.go` — 报告主流程
- [ ] `pkg/cover/backend/` — 读 ELF/DWARF，建立 PC↔行映射
- [ ] `pkg/cover/canonicalizer.go` — 跨 VM/模块偏移的 PC 归一化
- [ ] `pkg/cover/html.go`、`heatmap.go` — 可视化

**主线问题**：一个原始 PC 要经过哪些步（归一化→查 DWARF→映射源码行）？**为什么需要 canonicalizer**？

**动手**

- [ ] 用带覆盖率的 manager 生成一次 HTML 报告打开看
- [ ] 对着 `backend/` 的 DWARF 解析读一遍

**出师**

- [ ] 解释 syzkaller 如何把一堆 `uint64` 变成网页上标绿的源码行

**我的笔记**


---

## 阶段 8 — 编排层：manager / RPC / VM / 崩溃（1–2 天）

**目标**：补上外圈——谁管 VM、怎么发程序、崩溃怎么抓怎么复现。

**读什么**

- [ ] `syz-manager/manager.go:66` `Manager`、`:264` `RunManager`、`:1307` `fuzzerLoop`
- [ ] `pkg/rpcserver/rpcserver.go`、`runner.go`（每 VM 一个 runner）
- [ ] `vm/vmimpl/`（后端接口）+ `vm/qemu/`（最常用）；知道 `vm/isolated/`、`vm/gce/` 的存在
- [ ] `pkg/report/`（内核 oops/KASAN → 崩溃描述）、`pkg/repro/`（复现与最小化）
- [ ] `docs/configuration.md`

**主线问题**：一次崩溃从"内核 console 输出"到"`workdir/crashes` 里一条去重记录"经过了什么？

**动手**

- [ ] 读一个 mgr 配置样例
- [ ] 看 `pkg/report` 的正则如何识别一类崩溃

**出师**

- [ ] 画出完整外圈——manager 启动 N 个 VM → 每 VM 一个 executor → RPC 分发/回收 → 崩溃解析 → 复现

**我的笔记**


---

## 阶段 9 — 综合贯通（capstone，择一）

**目标**：把前面的碎图拼成一张。

- [ ] 在真实 QEMU + 内核镜像上跑一次完整 `syz-manager`（`docs/setup.md`），开 dashboard 看它自己跑
- [ ] 或挑一个跨层的真实 commit（改 signal / 改描述的 PR），顺着读懂"改一处牵动哪些层"
- [ ] 或自出题："我想让 fuzzer 更偏向某类 syscall，该改哪几个文件？"——能答出来就算贯通

**我的笔记**


---

## 附录 A — 常用命令速查

```bash
make                     # 构建全部（需 Go + C/C++ 工具链）
make executor            # 只重编 C++ 执行器
go test ./prog/... -v    # prog 单测（无需 VM，读测试即读 API）
go test ./pkg/fuzzer/ -v # 反馈闭环单测（进程内跑闭环）
./bin/syz-prog2c -help   # 把 syz 程序翻成 C（理解程序模型的利器）
./bin/syz-execprog -help # 在目标上执行程序并看覆盖率（需内核/VM）
```

## 附录 B — 参考文档清单

- 架构：`docs/internals.md`、`docs/coverage.md`、`docs/research.md`
- 描述语言：`docs/program_syntax.md`、`docs/syscall_descriptions.md`、`docs/syscall_descriptions_syntax.md`
- 运维：`docs/setup.md`、`docs/configuration.md`、`docs/reproducing_crashes.md`

## 附录 C — 随时可回看的两条主心骨

1. **数据流脊柱**：生成 → 序列化 → RPC → 执行 → 采集 → 回传 → triage → 入库 →（回到生成）。
2. **可移植性分界线**：`prog/` 与 OS 无关（纯核心），`sys/` 才是某 OS 特定——理解这条线是读懂 syzkaller 结构的钥匙。
