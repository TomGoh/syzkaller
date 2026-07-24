# Stage-2 第二步（前两项）：多页共享区生命周期 + 四页 ring 判决 smoke

日期：2026-07-24
目标机：N90（Kylin V10 SP1，Phytium gwd3000 级）
前置：`2026-07-24-pkvm-stage2-step1-feedback-measurement.md`（1A / 1A+ / 1B 实测）
原始数据：`evidence/step2-multipage-ring-2026-07-24/`

---

## 1. 结论

第一步留下的唯一未决问题是：**ring 那 5.02% 的 raw 截断，到底有没有吞掉 unique 覆盖率？** 只有让 ring 不再截断才能回答。现在答案是确定的：

> **没有。1 页 / 2 页 / 4 页三种容量下，unique EL2 PC 集合都是 132 个，而且是同一个集合——两两对称差为 0。被 ring 丢掉的全是重复 PC。**

同时，生命周期侧发现并修掉了一个真 bug：`kvm_unshare_hyp_checked()` 首个失败就返回，在多页下会把后续页留在 shared + EL2-mapped 且 host 记录未清的状态。单页时无害，多页时它不是可扩展的清理契约。

四页 ring 的判决 smoke 全项通过：`LOST_IN_RING == 0`、`hits_max(858–1032) < capacity(2045)`、`link_dropped == 0`、`kcov_requested == kcov_accepted`、`skip_not_owner == 0`、无 WARN/BUG、Rust `.rs:line` 符号化仍正确。

---

## 2. 生命周期先行：多页共享区

容量没有直接 bolt 到旧生命周期上，而是先把生命周期改成能扩展的形状。

### 2.1 被修掉的 bug

`kvm_unshare_hyp_checked()`（mmu.c）原本是：

```c
for (cur = start; cur < end; cur += PAGE_SIZE) {
        ret = unshare_pfn_hyp(__phys_to_pfn(cur));
        if (ret)
                return ret;          /* <-- 首个失败即返回 */
}
```

函数自己的注释也写明了它只适合单页。四页时第 i 页失败，i..3 页就留在 shared + EL2-mapped、host rb-tree 记录还在。「整块不释放」仍然安全（绝不 free 可能仍被映射的页），但那不是清理——调用方无法把这段范围恢复到已知状态，后续 enable() 会撞上残留记录。

改成**范围完整**契约：每一页都尝试，返回**第一个**错误但不因它跳过后面：

```c
for (cur = start; cur < end; cur += PAGE_SIZE) {
        err = unshare_pfn_hyp(__phys_to_pfn(cur));
        if (err && !ret)
                ret = err;           /* 记住第一个，继续 unwind */
}
```

注释里那句 "leaves earlier pages unshared" 方向也写反了（早退实际留下的是**后续**页），一并改正。

### 2.2 attempted 而非 succeeded

share 侧同样是首个失败即返回（`kvm_share_hyp`），而 `share_pfn_hyp()` 先写 host rb-tree 记录、后做 EL2 HVC、失败不回滚。所以**失败那一页的 host 记录已经存在**，清理范围必须按 `attempted`（0..i）而不是 `succeeded`（0..i-1）。

因此 enable() 逐页 share，并在调用**之前**就把该页计入 attempted：

```c
for (i = 0; i < nr_pages; i++) {
        char *p = (char *)ring + i * PAGE_SIZE;

        attempted = i + 1;      /* 先记，再调 —— 失败页也算已尝试 */
        ret = kvm_share_hyp(p, p + PAGE_SIZE);
        if (ret)
                goto rollback;
}
```

`pkvm_cov_release_pages(ring, nr_pages, attempted, what)` 统一了 enable 回滚和 teardown 两条路径：对 [0, attempted) 做范围完整的 checked unshare；**任一页未确认，就泄漏整个 order-N 块，绝不 free 其中任意一页**——这些页是物理连续、作为一个范围交给 hyp 的，把其中一页还给页分配器而邻页可能仍被 hyp 映射，正是要避免的所有权破坏。

### 2.3 其余改动

- **参数化 nr_pages**，默认 4，不把 order-2 写死。debugfs `kvm/pkvm_cov/nr_pages`（2 的幂，≤ `PKVM_COV_MAX_PAGES`=16），ring 存活期间写入返回 `-EBUSY`。容量扫描因此无需重编内核。
- **header 固定在第 0 页，PC 区跨页**：24 字节 header + `pcs[]` 柔性数组。容量 = `(nr_pages * PAGE_SIZE - 24) / 8` → 1/2/4 页 = 509/1021/2045。
- **HVC 改为 `__pkvm_cov_setup(pfn, nr_pages)`**（Rust dispatch 读 `cpu_reg(2)`，extern 声明与 `cov.c` 同步）。**hypercall id 未动**，仍在固定 `HOST_HCALL` 表之前显式分发。
- **EL2 侧校验 nr_pages**：host 在 pKVM 下是不可信的，`pkvm_cov_setup()` 拒绝 0、> `PKVM_COV_MAX_PAGES`、非 2 的幂，然后才用它决定 pin 范围。unpin 长度取自 setup 时自己记录的字节数，不取任何后来的 host 值。
- **producer 的上界来自 EL2 自己算出的 capacity**（per-CPU），不读共享 header 里的 `capacity` 字段——否则被篡改的 header 就能让 producer 写出 pin 范围之外。header 里的 `capacity` 只给 host 自己的 drain 用，经同一个指针访问，地址依赖即可定序。
- `pkvm_cov_begin()` 改为填充 `struct pkvm_cov_ctx {ring, capacity}` 并返回 `bool`，`end()` 接同一个 ctx——end() 不再需要重读可能正被并发 disable() 改动的全局状态。

### 2.4 失败分支：做了真机注入，不是只做静态审计

计划里说「若没有可控 fault injection，最多只能做静态审计」。这里加了一个 **test-only** 的一次性注入开关 `kvm/pkvm_cov/fault_inject`（bit0 EL2 teardown 失败、bit1 unshare 未确认、bit2 enable 期 EL2 setup 失败），泄漏路径**在 N90 上真跑过**，且每一步都有断言。

**三个注入不对称，这是刻意的（也是一次 review 修正）。** bit0/bit1 在真实操作**成功之后**伪造失败——安全，因为它们选中的分支只会泄漏，最坏 16 KiB。bit2 选中的分支**会 free**，所以它不能伪造：它给 EL2 一个必须被拒绝的 `nr_pages`（`PKVM_COV_MAX_PAGES + 1`），EL2 在 pin 任何东西、发布 per-CPU 指针**之前**就校验并拒绝，于是 hypercall 什么都没做就失败，回滚正是真正的「shared 但从未注册」路径——checked unshare、页确实该 free、`leaked_bytes` 必须保持 0。

早先的写法是在**成功的** setup 之后伪造失败，再发一个补偿 teardown IPI 并**丢弃它的返回值**：若那次 undo 失败，回滚的 unshare 仍可能被确认、页在 EL2 仍映射时被 free（UAF）；而且无论如何都报 `leaked_bytes == 0`，测试两头都像通过。这是同一 review 链一直在抓的那类 bug，出现在 test-only 代码里也必须修。

**FI 断言测试（`scripts/run-fi.sh`）在这个 build（Build ID `e81d2c18…`）上 25/25 通过：**

```
bit2 (setup 被拒)  -> enable rc=1, owner_cpu=-1, leaked_bytes 保持 0  (正确 FREE)
                      且拒绝后 buffer 立即可再 enable
bit0 (teardown 未确认) -> leaked_bytes 0 -> 16384, buffer 停用
bit1 (unshare 未确认)  -> leaked_bytes 16384 -> 32768, "over 4/4 pages"(范围完整)
恢复                   -> 正常 enable/disable 仍成功, leaked_bytes 不再增长
全程 0 WARN/BUG splat
```

每一项都是对 return value、`owner_cpu`、`leaked_bytes` 差值和事后可用性的显式断言——「没崩」不算通过。测试故意泄漏的 KiB 数由重启清除。

---

## 3. 四页 ring 判决 smoke

同一固定输入、CPU 0、`-procs=1 -threaded=0`、bridge on、kprobe off、hot path 无 printk。ring 容量扫 1 / 2 / 4 页，各 3 次。

| pages | capacity | drains | el2_hits | el2_written | **LOST_IN_RING** | overflow | hits_max | EL2 uniq |
|---|---|---|---|---|---|---|---|---|
| 1 | 509 | 78 | 27153–28453 | 26141–27147 | **994 / 1306 / 1470** | 4 | 914–991 | 132 |
| 2 | 1021 | 78 | 26992–28265 | = hits | **0 / 0 / 0** | 0 | 965–1020 | 132 |
| 4 | 2045 | 78 | 28038–28196 | = hits | **0 / 0 / 0** | 0 | 858–1032 | 132 |

每一次运行都满足：`link_dropped=0`、`kcov_requested == kcov_accepted`、`LOST_IN_KCOV=0`、`skip_not_owner=0`、`nested=0`、`leaked=0`、无 WARN/BUG。

### 3.1 判决输出：new_EL2_PC_set − old_132

这是本步骤最重要的一行输出：

```
p1 vs p2 对称差 = 0 PCs
p1 vs p4 对称差 = 0 PCs
p2 vs p4 对称差 = 0 PCs
comm -13 union_p1 union_p4  ->  0 行
```

**新增集合是空的。** 1 页时被丢掉的那 ~1,000–1,500 个 raw PC，没有一个是当时还没出现过的。所以第一步那 5.02% 的 raw 截断，在这个固定 workload 上**没有造成任何 unique 覆盖率损失**。

必须限定清楚：这是**对当前固定 #23 smoke** 的结论。pvmfw 启动决定了这 78 次 #23 的形状；换成可变异输入面（3B）或别的 boundary 之后，raw 高水位和 unique/raw 比都可能不同，要重测。它也不是「ring 容量无所谓」的许可证——它说明的是，在 append-only producer + 这个 workload 下，raw 截断先吃掉的是重复。

### 3.2 为什么仍然选 4 页而不是 2 页

2 页的三次运行确实零丢失，但那是因为它们的 `hits_max` 恰好落在 965–1020，全部 ≤ 1021。而：

- 同一轮 4 页运行里出现过 `hits_max = 1032`；
- 第一步 12 次运行里 `hits_max` 达到过 **1088**，12 次中有 6 次 > 1023。

也就是说**单次 #23 的需求分布正好骑在 2 页容量（1021）上**。2 页是运气，不是余量。4 页（2045）相对已观测最大值 1088 有约 1.9× 余量，且 16 KiB/CPU 的代价可以忽略。

---

## 4. 构建与板子状态

| 项 | 值 |
|---|---|
| kernel release | `6.6.30+` |
| Build ID | `e81d2c18452a871075163940eeaba2446354ec0d`（含 bit2 修正；容量判决数据来自其前身 `8371f4a0…`，二者 hyp .text 相同、EL2 PC 集合逐字节一致） |
| vmlinux sha256 | `45c1e553d03a1fae7b41c2cf8da3c9fe82c0f6da576c67208f864cf014785f11` |
| Image sha256 | `7b31222e9830b911c7f1785cfa1a8245d0a9f6c695191cbe668bbd0111373c8d` |
| System.map sha256 | `189b49a3418fcbcc020c2194f786b8ba2644eb50592b7683926dfe49f2af4f4d` |
| .config sha256 | `b1f010f5917a2eab6e7719380a777717443817d366ff34227645586aa9d256cf`（未变） |
| 规范 patch | `evidence/step2-multipage-ring-2026-07-24/stage2-el2-kcov.patch`（13 文件 / 1286 行，`--whitespace=error` 干净） |
| `__hyp_text_start` / `_end` | `0xffff800081d87ed4` / `0xffff800081de7000` |

**每次重链接地址都会整体平移，符号化只能用同一次链接产出的 vmlinux。** 消费侧 `TestPkvmCovSymbolizePipeline` 在这个 build 上仍然通过（EL2 PC → `.rs:line`）。

板上保留了两个回退镜像：`/boot/vmlinuz-6.6.30+.1abuild.bak`（1A 用的 build）与 `.1bbuild.bak`（1B 用的 build）；GRUB 默认项仍是已知可用的 `6.6.30-pkvm-fuzz`。

**操作提醒（本轮穷举确认的持久结论）：N90 的 Kylin GRUB 会忽略一切非交互式的默认项选择，只认菜单里的人工选择。** 本轮试过四种机制、约七次重启，全部启到默认的 `6.6.30-pkvm-fuzz`（index 0）：
- `next_entry` 按标题、按数字下标（grubenv，ESP 与 `/boot/grub` 两处都写）；
- `GRUB_DEFAULT` 按标题、按数字下标（改 `/etc/default/grub` + `update-grub`）。

关键证据：GRUB **确实消费了** `next_entry`（重启后它变空），头部也执行了 `set default="${next_entry}"`，但仍启 index 0——说明 Kylin GRUB 内核对 autoboot 直接忽略 `set default`。因此**没有任何 grubenv / default 手段能用**；`6.6.30+` 只能靠人在 5 秒菜单里手动选中（本轮 FI + 判决 smoke 即由用户手动选中该项后运行）。给板子部署非默认内核时，就按 deploy skill 的设计由人来选，不要浪费重启在自动化上。

---

## 5. 对后续的影响

- **第二步的容量部分到此完成**：多页生命周期是可扩展形状，4 页为默认值，且 `nr_pages` 可在运行时扫。
- **第一步「建议放弃 EL1 去重表」的结论进一步加强**：不仅 KCOV 侧没有压力，现在 ring 侧也零丢失，去重表两头都没有要解决的问题。
- **下一个仍然值得做的是 EL2 侧自插桩排除**（hyp 自己的 `ftrace.rs` / `trace.rs` 占已投递 EL2 PC 的 58.5%）。注意它的收益现在**不再是「减少 ring 截断」**（已经零截断），而是**提高每个 ring 槽位和每条 KCOV 条目的信息密度**，以及为将来更大的输入面留出余量。按第一步记录里写的，它的机制还没验证，要按可行性实验做。
- **仍然阻塞 campaign 的**：Stage-2 串行 executor 模式 + CPU pin、可生成的 Host 侧输入面、EFI pstore 与 isolated backend 的对接。本轮不涉及。
