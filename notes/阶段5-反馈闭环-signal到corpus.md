# 阶段 5：反馈闭环——signal 如何变成 corpus

> 本阶段的目标：理解 syzkaller 怎样把一次执行得到的反馈，变成下一轮更有方向的测试。这里的关键不是“看到覆盖率就保存”，而是先判断它是否新、是否稳定、是否值得保留；只有通过这些筛选的程序，才会成为 corpus 中可再次变异的种子。
>
> 本阶段承接阶段 2 的端到端链路和阶段 4 的生成/变异。阶段 6 会再回到 executor，细看原始 KCOV 数据怎样变成 `CallInfo.Signal`；阶段 7 会细看 PC 的符号化和覆盖率报告。本文从 Host 上 fuzzer 收到 `Signal` 开始。

---

## 一、这一阶段在整条链路中的位置

阶段 4 回答的是“下一份 `prog.Prog` 怎样造出来”。但如果每一份程序都只执行一次、随后丢弃，fuzzer 就不会从历史中学习。阶段 5 处理的是执行后的选择：哪些结果应该让 fuzzer 停下来多验证几次，哪些程序值得留下，留下后又怎样影响未来的变异。

```text
已有 corpus ─┐
             ├─► 生成 / 变异一份 prog.Prog
从零生成 ────┘                │
                               ▼
                    executor 执行，返回每个调用的 Signal
                               │
                               ▼
                 maxSignal：这个反馈点以前见过吗？
                               │
                    否（或以更高优先级首次见到）
                               ▼
               triage：重跑、去抖、最小化、确认目标调用
                               │
                    有新的稳定 signal
                               ▼
                  Corpus.Save：保存为新的长期种子
                     │                       │
                     │                       ├─► 动态 ChoiceTable 更新
                     ▼                       │
             按 signal 数量加权抽取 ───────────┘
                     │
                     └─► 再次变异，形成正反馈
```

这里有一个容易混淆的边界：`maxSignal` 与 `corpus` 都会积累 signal，但它们不是同一个东西。

| 名称 | 生命周期 | 作用 | 是否一定稳定 |
| --- | --- | --- | --- |
| 本次 `CallInfo.Signal` | 一次调用结果 | executor 回传的原始反馈元素列表 | 否 |
| `maxSignal` | 当前 fuzzer 实例 | “已经观察过的最高优先级 signal”去重账本 | 否；注释明确说包含 flakes |
| `triageCall.stableSignal` | 一项 triage 任务 | 多次执行后达到出现次数门槛的 signal | 是，按当前门槛 |
| corpus 中 `Item.Signal` | 长期 | 这份种子保留了哪些稳定 signal | 是，来自 triage |

因此，“`maxSignal` 增长”不等于“corpus 已增加一条程序”。前者首先是避免对同一发现反复花钱；后者要求经过稳定性验证和最小化。

---

## 二、先分清 coverage、signal 和 priority

### 2.1 `Signal` 是 fuzzer 用来决策的紧凑集合

`pkg/signal/signal.go` 定义：

```go
type Signal map[elemType]prioType
```

可将它理解为：

```text
反馈元素（通常来自覆盖率） → 这个元素目前的最高优先级
```

在普通 KCOV fuzzing 中，元素通常是覆盖率派生的 PC；但 `pkg/signal` 本身只是一个通用集合，并不规定元素必须是某一种 PC。具体 executor 怎样采集、过滤和回传，留到阶段 6；PC 如何对应源码，留到阶段 7。

`Signal` 的三个基本操作是：

```text
FromRaw([a, b, c], prio)
    把一次回传的列表变成 map：a/b/c 都带这个 prio。

known.DiffRaw([a, b, c], prio)
    只保留 known 中不存在，或 known 的优先级低于 prio 的元素。

known.Merge(delta)
    把每个元素的优先级更新为两边较高者。
```

例如已有：

```text
known = { 0x1000: 1, 0x2000: 3 }
本次  = [ 0x1000, 0x2000, 0x3000 ]，prio = 2
```

则 `DiffRaw` 的结果是：

```text
delta = { 0x1000: 2, 0x3000: 2 }
```

`0x2000` 以前已以优先级 3 出现，不新；`0x1000` 虽然不是新元素，但这次处于更好的执行条件，所以它仍是“新的更高质量 signal”。

### 2.2 priority 衡量的是这次观察的上下文

`pkg/fuzzer/fuzzer.go` 的 `signalPrio` 当前使用两个 bit：

| 条件 | bit | 含义 |
| --- | ---: | --- |
| 目标调用返回 `Error == 0` | `2` | 成功调用通常比失败调用更有价值 |
| 目标调用不含 `ANY` | `1` | 未被 squash 的结构化程序通常更可读、更适合作为种子 |

因此普通调用可得到 0 到 3 的优先级。例如，成功且没有 `ANY` 的调用得到 3；失败但没有 `ANY` 得到 1。`Extra` 覆盖（调用编号为 `-1`）当前固定为 0，因为它不对应某一条普通 syscall。

这不是在给内核边本身打分，也不是说失败路径没有价值。它只是让同一 feedback element 在“成功、结构较完整”的上下文再次出现时，仍有机会触发后续处理。

### 2.3 `cover` 不是 `signal` 的同义词

本阶段会同时看到 `Signal`、`Cover` 和 `RawCover`：

- `Signal` 用于“是否值得继续”的反馈和 corpus 选择；
- `Cover` 是完整覆盖集合的 Host 端表示，triage 时合并多次执行结果；
- `RawCover` 是可选保留的原始覆盖列表，只有 `FetchRawCover` 开启时才从 triage 结果取走。

普通 fuzz 的首次请求通常只带 `ExecFlagCollectSignal`。发现候选 signal 后，triage 请求才同时带 `CollectSignal | CollectCover`，并在 `ReturnAllSignal` 中列出关心的调用。这要求 executor 回传那些调用的完整 signal，Host 才能做多次运行的交集和最小化判定。这里的“只先取 signal、需要时再取完整 cover”是在控制每次 RPC 的数据量；并不是普通 fuzz 完全没有 KCOV。

---

## 三、第一次发现：`maxSignal` 决定是否启动 triage

`Fuzzer.processResult` 遍历 `ProgInfo.Calls`，并额外处理 `ProgInfo.Extra`。每一项都交给 `triageProgCall`：

```text
一次 Request 的结果
  │
  ├─ CallInfo[0] ─┐
  ├─ CallInfo[1] ─┼─► signalPrio
  ├─ ...         ─┤
  └─ Extra       ─┘
                        │
                        ▼
              Cover.addRawMaxSignal(raw, prio)
                        │
              DiffRaw(maxSignal, raw, prio)
                        │
        ┌───────────────┴────────────────┐
        │ delta 为空                       │ delta 非空
        ▼                                 ▼
   这项结果到此结束                 记录 triageCall，启动 triageJob
```

`Cover.addRawMaxSignal` 在锁内完成三件事：计算与全局 `maxSignal` 的差、合并差值到 `maxSignal`、再把差值放入 `newSignal`。后者会由 manager 周期性同步给其他 executor，帮助目标侧尽早过滤已经见过的 signal；这一点在阶段 2 已见过，这里只需要知道它是跨实例的去重同步，不是 corpus 保存。

注意顺序：代码**先**把元素记入 `maxSignal`，**再**检查 `NewInputFilter` 是否允许把这个调用当作新输入。也就是说，`maxSignal` 是“观察过”的账本，不是“已经保存”的账本。它故意包含 flake，避免一个偶发元素不停触发昂贵的 triage。

如果一次程序的多条调用都出现新 signal，它们会被收进同一个 `triageJob.calls` map。这个任务保存的是整份程序副本和若干“目标调用编号”，不是为每个调用拆一份互不相关的程序。

---

## 四、为什么不能立刻入库：triage 先验证发现

一次执行的覆盖率可能受调度、内存布局、设备状态或内核异步行为影响。若首次看见一个元素就直接入库，corpus 会迅速积累只能偶尔复现、也无法稳定贡献 feedback 的程序。因此 triage 的职责是：

1. 重跑原程序，确认候选 signal 不是偶发噪声；
2. 收集该程序的完整覆盖；
3. 在仍保留“新的稳定 signal”的条件下最小化程序；
4. 将验证后的种子交给 corpus，并在附近加大探索。

### 4.1 去抖（deflake）到底在做什么

`triageJob.deflake` 的默认规则可以概括为：

```text
首次执行已看到候选 signal
       │
       ▼
重复执行，收集每轮 signal
       │
       ▼
至少出现在 3 次执行中的元素 → stableSignal
stableSignal ∩ 初始/后续发现的新元素 → newStableSignal
```

对普通新 fuzz 程序，当前常量是“最多 5 次中至少 3 次”。这里的“3 次”包含触发 triage 的首次执行；稳定时通常只需再跑两次，噪声较大时才会继续，最多凑够五次。对已在 corpus 中的候选，策略较宽松：至少 2 次，最多 6 次，并可能继续到 20 次以吸收已有种子的 flake；snapshot 模式有自己的 2 次规则。

实现不是保存五份完整集合再最后求交，而是维护一个 `signals` 数组。每一轮的 signal 与较低门槛集合相交后合并，使 `signals[n-1]` 表示“至少出现在 n 次中的元素”。最后：

```go
stableSignal    = signals[needRuns-1]
newStableSignal = newSignal.Intersection(stableSignal)
```

只有 `newStableSignal` 非空，`handleCall` 才继续。这一点非常重要：一份程序即使有大量稳定覆盖，只要没有保住自己最初带来的新内容，也不会因为“覆盖很多”而进入 corpus。

### 4.2 为什么要避开首次执行的 executor

triage 最初把触发发现的 `ExecutorID` 放入 `Request.Avoid`。`ExecutorID` 是 `(VM, Proc)` 的 Host 侧标识；阶段 2 的 runner/`Proc` 会据此挑选执行位置。该限制是软限制：有其他可用位置时优先换一个，只有一个位置或长时间无法调度时仍可使用原位置。

目的不是保证跨 VM 的完全确定性，而是避免“只在某一个 executor 状态中成立”的假阳性轻易通过验证。由于 `Avoid` 会随每次结果继续追加，triage 尽量在不同执行位置重现同一反馈。

### 4.3 最小化的判定条件比“还能跑”更强

阶段 4 已介绍 `prog.Minimize`：它反复提出更小的候选，交给外部 predicate 决定是否接受。这里 predicate 的标准是：

1. 目标调用仍被执行；
2. 如果原目标调用成功，候选不能把它最小化成失败；
3. 最多三次重跑的合并 signal 仍覆盖全部 `newStableSignal`。

所以 triage 最小化不是只追求“程序不崩”或“还有任意覆盖”，而是保留这一次真正新且已稳定的反馈。这也解释了为何 `ReturnAllSignal` 必须指定目标调用：最小化需要知道删改后保住的是不是同一条调用的目标 signal。

---

## 五、通过 triage 后发生什么：保存、smash、hints

当某个目标调用的 `newStableSignal` 非空，`triageJob.handleCall` 按下面的顺序工作：

```text
稳定的新 signal
      │
      ├─► prog.Minimize（除非该程序已带 ProgMinimized）
      │
      ├─► 启动 smashJob：以最小化后的程序为中心做 25 次变异
      │       ├─► 可选 hintsJob：收集 KCOV comparisons，再据此变异
      │       └─► 可选 faultInjectionJob：枚举指定调用的失败注入点
      │
      └─► Corpus.Save(NewInput{Prog, Call, stableSignal, Cover, RawCover})
```

**smash** 不是“再做一次 triage”，而是对刚证明有价值的种子进行集中探索。它克隆这个程序、调用阶段 4 的 `Prog.Mutate`，当前固定执行 25 次。这里的想法是：刚越过一个新边界时，邻近参数和调用序列往往还藏着更多路径，值得立刻多投入一点预算。

**comparison hints** 与 **fault injection** 是可选的后续任务，不是普通 signal triage 的前提。hints 依赖 KCOV comparison 记录，将比较两端的值转为更有针对性的变异；fault injection 则逐步改变某个目标调用的第 N 次内部失败点。两者会在后续阶段分别深入，本阶段只需知道它们也共享这份已验证的种子。

---

## 六、corpus：不是程序日志，而是可再次抽样的种子集

`pkg/corpus.Corpus` 保存的是“已知能覆盖当前前沿的一组程序”。一项 `Item` 至少包含：

```text
Prog       这份结构化程序
Call       本次保存关联的目标调用
Signal     该种子稳定贡献的 signal
Cover      合并后的覆盖集合，用于统计和报告
HasAny     是否包含 squash 后的 ANY
Updates    同一文本程序后续观察到的附加信息
```

### 6.1 `Corpus.Save` 如何区分“新种子”和“旧种子更新”

保存时，代码先对 `Prog.Serialize()` 求哈希；这个哈希是 corpus item 的身份。因此同一份文本程序再次因为别的调用或别的 signal 被保存时，不会产生重复程序：

```text
第一次看到该程序文本
  → 创建 Item，加入 ProgramsList，记录 signal/cover

再次看到相同文本
  → 合并 Signal 和 Cover，保留至多 32 条 ItemUpdate
  → 不重复加入 ProgramsList
```

这也说明 `Item.Call` 不是“这份程序唯一相关的调用”。一个程序可以对多个调用有价值；`Updates` 用于记录其后续关联。manager 只把真正新文本写入 `corpus.db`，同文本的更新留在内存数据结构和统计中。

### 6.2 corpus 怎样影响下一次变异

`Corpus.ChooseProgram` 从 `ProgramsList` 中按权重抽取程序。当前权重是：

```text
weight = len(Item.Signal)
```

保留更多 stable signal 的种子有更高抽样概率；但它不是必然被选中，仍是加权随机。若配置了 focus area，先按 area 的权重选择一个非空区域，再在该区域的程序列表中抽样。

回到 `genFuzz`：启用真实 coverage 时，fuzzer 以 95% 概率尝试从 corpus 取种子并变异；corpus 为空或这次未走变异时，退回 `Target.Generate` 从零生成。没有真实 coverage 时，这个变异比例降为 50%，因为弱反馈不值得过度围绕现有种子打转。

corpus 还会逐步影响阶段 4 的 `ChoiceTable`。`Fuzzer.ChoiceTable` 发现 corpus 比当前表多出一定数量的程序后，会异步用 `Corpus.Programs()` 重建动态优先级。因此 corpus 的影响有两条：它既提供“从哪份程序开始改”，也逐渐改变“下一条 syscall 更倾向接什么”。

### 6.3 corpus 自己也要最小化

单程序的 triage 最小化解决“这份程序是否有多余调用和参数”；corpus 最小化解决“多份程序之间是否有冗余”。二者对象不同。

`Corpus.Minimize` 将每个 item 的 `Signal` 交给 `signal.Minimize`，只保留仍然承担某些 signal 最高优先级覆盖责任的程序。开始前会优先排序：不含 `ANY` 的程序优先，调用数更少的程序优先；在 signal 责任相同的情况下，这能偏向更结构化、更快的种子。

当前实现的 `Corpus.Minimize(cover bool)` 实际按 **signal** 选择，参数 `cover` 在这个函数体内没有参与决策；不要把它误读为“按完整 source coverage 做集合覆盖最小化”。manager 会等已加载的 corpus triage 完成，并在 corpus 规模比上次最小化大约增长 3% 后才调用它。

---

## 七、队列：candidate、triage、smash 分别在排什么

这些名字都出现在 `execQueues` 中，但它们不是几个等价的“程序池”。当前调度源的轮询顺序是：

```text
triageCandidateQueue
  → candidateQueue
  → triageQueue
  → Alternate(smashQueue, 3)
  → genFuzz 回调
```

`queue.Order` 每次从前向后找第一项非空来源，所以排在前面的类别会先获得执行机会。

| 队列 | 放入什么 | 为什么排在这里 |
| --- | --- | --- |
| `triageCandidateQueue` | 外来 candidate 首次运行后发现潜在新 signal 的 triage 任务 | 先确认启动/恢复时带来的种子 |
| `candidateQueue` | 由 manager 加入的待验证 candidate 程序，例如已有 corpus 或其他来源提供的程序 | 在普通 fuzz 前完成候选验证；无 triage 时可能重试，处理其覆盖不稳定性 |
| `triageQueue` | 新生成/新变异程序产生的新 signal 所对应的 triage 任务 | 尽快确认真正的新发现，避免它被后续工作淹没 |
| `smashQueue` | 已验证种子的 smash，以及可选 hints / fault injection 后续任务 | 深挖好种子，但不能长期挤占广泛探索 |
| `genFuzz` | 不保存请求的回调；即时生成或变异普通 fuzz 程序 | 所有更紧急工作为空时，持续维持主探索流 |

`triageCandidateQueue` 与 `triageQueue` 使用 `DynamicOrderer`。每个新 triage job 取得一个自己的 `Append()` 入口，较早建立的任务优先，避免后来的大量发现把正在确认的任务完全饿死。`candidateQueue` 和 `smashQueue` 是普通 FIFO 队列。

`Alternate(smashQueue, 3)` 不是“smash 总是第三优先级”。它每第三次被查询时故意返回空，让 `Order` 继续落到 `genFuzz`；在没有更高优先级任务时，大致每两次 smash 会让一次普通探索通过。这是“集中深挖”与“扩大搜索面”之间的预算分配。

---

## 八、用一个小例子串起完整闭环

假设从 corpus 选出一份 KVM 程序并变异后得到：

```syz
r0 = openat$kvm(AT_FDCWD, &AUTO="/dev/kvm\x00", O_RDWR, 0)
r1 = ioctl$KVM_CREATE_VM(r0, AUTO, 0)
r2 = ioctl$KVM_CREATE_VCPU(r1, AUTO, 0)
ioctl$KVM_RUN(r2, AUTO, 0)
```

这只是理解反馈流程的例子，不表示真实 KVM vCPU 已满足所有初始化条件。

1. 普通 fuzz 请求只要求 `CollectSignal`。假设 `KVM_CREATE_VCPU` 这一项回传一个以前没有的 feedback element `0xabc`，且调用成功、程序不含 `ANY`，优先级是 3。
2. `maxSignal.DiffRaw` 发现 `{0xabc: 3}`，于是立即把它记入全局账本，并为调用编号 2 建立 `triageCall`。
3. triage 在尽量不同的 executor 上重跑。若 `0xabc` 在首次加两次复跑中都出现，它满足 3/5 门槛，成为 `stableSignal`，同时也是 `newStableSignal`。
4. `prog.Minimize` 尝试删除 `openat`、创建 VM、创建 vCPU 或简化参数；每个候选都必须仍让目标调用执行成功，并保住 `0xabc`。若删掉创建 VM 后资源变成无效 fd、目标调用失败，这个删减会被拒绝。
5. 最小化后的程序连同稳定 signal 和覆盖集合交给 `Corpus.Save`。以后普通 fuzz 有机会按权重抽到它并继续变异；同时会启动一小轮 smash，立即探索它的邻域。
6. manager 将新的 `maxSignal` delta 同步给其他 executor；corpus 逐渐增长后，动态 `ChoiceTable` 也会用新种子更新调用组合偏好。

这就是 coverage-guided 的实际含义：覆盖率不是停在报表上的数字，而是经过“新颖性 → 稳定性 → 压缩 → 再抽样”变成下一轮搜索分布的一部分。

---

## 九、这套设计解决什么问题，又故意不保证什么

它解决的主要问题：

- 不把每一次偶发的 signal 都当成长期种子；
- 不对已经见过的相同反馈反复 triage；
- 让较多稳定 signal 的种子更常被变异；
- 用单程序最小化和 corpus 最小化控制执行成本；
- 在发现新区域后立即 smash，同时保留从零生成的探索能力。

它不保证：

- corpus 中每条程序都能 100% 复现同一覆盖；门槛只是对 flake 的务实折中；
- `maxSignal` 与 corpus signal 始终相等；前者包含已经观察到但最终未通过 triage 的元素；
- signal 多的程序一定更容易触发漏洞；这里的权重是探索启发式，不是漏洞风险评分；
- 最小化后仍保留原程序的所有覆盖；它只承诺保住本次需要的新稳定 signal；
- corpus 最小化保留所有原始 `Cover` 中的每个 PC；当前选择依据是 `Signal`。

---

## 十、本阶段实际检查

本次针对当前工作树执行了：

```bash
env GOCACHE=/tmp/syzkaller-stage5-gocache \
  /usr/local/go/bin/go test ./pkg/signal ./pkg/corpus \
  -run 'Test(IntersectsWith|CorpusOperation|CorpusCoverage)$' -v

env GOCACHE=/tmp/syzkaller-stage5-gocache \
  /usr/local/go/bin/go test ./pkg/fuzzer \
  -run 'Test(Fuzz|Deflake)$' -v
```

结果均通过：

- `TestIntersectsWith` 验证 signal 的优先级交集规则；
- `TestCorpusOperation`、`TestCorpusCoverage` 验证保存、去重与覆盖增量；
- `TestDeflake` 覆盖多轮 signal/cover 合并；
- `TestFuzz` 跑通测试 executor、发现 signal、启动 triage 并最终得到非空 corpus。

第一次在受限沙箱中运行 `TestFuzz` 时，测试创建本地 TCP listener 被拒绝；允许本机监听后同一测试通过。这是运行环境的 socket 限制，不是该测试的断言或 fuzzer 流程失败。

---

## 十一、阶段 5 的出师问题

### `signal`、`maxSignal` 和 corpus 分别是什么？

`signal` 是一次或一组反馈元素及其优先级；`maxSignal` 是当前 fuzzer 已观察过的全局最高优先级账本，包含不稳定发现；corpus 是只保留通过 triage 的程序种子及其稳定 signal 的长期集合。

### 为什么不直接把新 signal 对应的程序入库？

一次覆盖可能是 flake。triage 会跨执行位置重跑，要求候选 signal 达到稳定门槛，再最小化到仍能保住新稳定 signal 的程序。否则 corpus 很快会被偶发、冗余、昂贵的程序污染。

### 为什么 `maxSignal` 会记录最终没有入库的元素？

它的责任是去重和抑制重复工作，而不是证明元素稳定。把偶发元素也记住能避免同一 flake 不断触发 triage；代价是它与 corpus 的稳定 signal 集合不完全相同。

### smash 是什么？

一份新种子通过 triage 后，对其克隆连续做多次结构化变异的后续任务。它利用“刚发现新区域时，附近可能仍有路径”的局部性；它不是验证任务，也不是所有普通 fuzz 都要经过的步骤。

### corpus 怎样真正影响未来？

它以 `len(Item.Signal)` 为权重供 `ChooseProgram` 抽取，变异器从抽到的种子出发；同时 corpus 规模增长会触发 `ChoiceTable` 的动态重建，改变调用组合的统计偏好。

---

## 十二、源码导航与下一阶段边界

| 主题 | 位置 |
| --- | --- |
| `Signal`、差集、合并、集合最小化 | `pkg/signal/signal.go:15`、`:42`、`:78`、`:98` |
| `maxSignal` / `newSignal` | `pkg/fuzzer/cover.go:16`、`:28` |
| 队列组合与普通生成/变异比例 | `pkg/fuzzer/fuzzer.go:84`、`:92`、`:285` |
| 结果处理与启动 triage | `pkg/fuzzer/fuzzer.go:146`、`:231` |
| 去抖、最小化、smash/hints/fault 和保存 | `pkg/fuzzer/job.go:148`、`:171`、`:228`、`:350`、`:449` |
| corpus 保存、加权选择、corpus 最小化 | `pkg/corpus/corpus.go:130`、`pkg/corpus/prio.go:20`、`pkg/corpus/minimize.go:12` |
| manager 持久化与定期 corpus 最小化 | `syz-manager/manager.go:991`、`:1044` |
| `maxSignal` 向其他实例同步 | `syz-manager/manager.go:1307` |
| 最小但完整的反馈闭环测试 | `pkg/fuzzer/fuzzer_test.go:31`、`pkg/fuzzer/job_test.go:19` |

至此，阶段 2 的主线可以补成一个真正的循环：

```text
Target / corpus
  → 生成或变异 Prog
  → executor 执行并返回 signal
  → maxSignal 去重
  → triage 验证和最小化
  → corpus 保存、加权抽样与 ChoiceTable 更新
  → 下一轮生成或变异
```

阶段 6 将向下追踪这个循环在 Target 上的那一段：executor 为什么用瞬态测试子进程、怎样执行并发 syscall、如何从 KCOV 缓冲区写出 `signal`、`cover` 和 comparisons。那时就能把本文的 `CallInfo.Signal` 从一个 Host 端输入，接回具体的 C++ 产生过程。
