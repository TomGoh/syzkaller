# klinux 哈希重写对照表（2026-08-07 rebase）

`pkvm-el2-kcov` 上的三个 per-CPU 提交压成了一个，`pkvm-tlb-flush-range-fix` 从 merge 改成 rebase 到它上面。**所有修复提交的哈希因此改变，内容一字未变**（`git diff` 旧顶端 vs 新顶端为空）。

改动前后的分支形状：

```
之前                                   之后
pkvm-tlb-flush-range-fix               pkvm-tlb-flush-range-fix
  Merge 'pkvm-el2-kcov'   ×3             12 个修复提交（线性）
  三个 per-CPU 提交（穿插在 merge 间）    ├─ pkvm-el2-kcov
  12 个修复提交                          │    一个 per-CPU 提交（压缩后）
                                        │    configs / EL2 coverage bridge
```

## 对照

| 旧 | 新 | 提交 |
| --- | --- | --- |
| `43672b00aa4d` + `1a737a500f60` + `e7ac1760705d` | `a72543fe21b8` | pkvm_cov: one coverage ring per CPU（三合一） |
| `8bdbd1873442` | `684f834c7ca5` | record host_vcpu before the checks that can fail（issue 006 的修复） |
| `a35f0a8c899e` | `d887c147402a` | correct the comment on why the split path carries the state |
| `22f8b90f89c7` | `e0a58de38a3c` | propagate the dirty-log granule probe's own error |
| `c6c3a2503e4b` | `6109975eadcb` | account hyp donations even when the topup fails |
| `cba248683e5c` | `5bd89c758506` | split then relax（issue 005 的修复） |
| `24714fe308bb` | `9a363ca8f0b0` | revert the dirty-log page-state fix, it livelocks |
| `889bd261c945` | `8d2b88dd98dc` | hand the dirty-log hypercall the pfn instead of a lookup |
| `f8ac14623978` | `4d561affbddb` | report the donation residual signed |
| `61173329e416` | `67908a178ef1` | account the hyp donations made on the fault paths |
| `cfaadd217ab6` | `382c9d9e29b4` | keep the page state when the dirty log remaps a page |
| `3608223e5012` | `65ac46446f0f` | route range TLB flush through the VM handle（issue 002 的修复） |
| `348c94763cc6` | `e60070abd355` | defer RLIMIT_MEMLOCK accounting out of mmap_lock（issue 001 的修复） |

## 没有改动任何既有文档，这是刻意的

`issues/` 和 `notes/` 里的旧哈希**全部保持原样**。理由：那些哈希绝大多数出现在**观测记录**里——“内核 #16 @cba248683e5c”“`Source Version: cba248683e5c`”——它们陈述的是当时那台机器上实际跑的、实际打印出来的东西。那是事实，不是索引，改掉就是篡改记录。

第一次尝试是用 `sed` 批量替换散文文件、只排除原始捕获件。那是错的：同一个 `.md` 里既有“修复是哪个提交”（指针，可以更新），也有引用的内核 WARN 原文（引文，绝不能动）。按文件排除挡不住这个区别，结果把引文里的 `Source Version` 也改了。已全部回退。

**所以规则是：文档不动，用这张表换算。** 拿到一个旧哈希 `git show` 失败时，回来查一下。

## 为什么值得单独记一笔

观测记录里的提交哈希是**双重身份**的：既是历史事实（当时跑的就是它），又是一个可点击的代码指针。rebase 只废掉了后者，却让两者看起来一样失效。

区分它们需要读上下文，不是读文件名——这也是为什么批量替换在这里必然出错。

以后要 rebase 已经被观测记录引用过的提交，先想清楚这一点；更省事的做法是不 rebase 已经写进记录的分支。

---

# 第二次重写（同日，为了 ring_dump）

`ring_dump` 属于覆盖率基础设施，不属于修复系列，所以它落在 `pkvm-el2-kcov` 上；`pkvm-tlb-flush-range-fix` 随之 rebase 到新顶端。**12 个修复提交的内容一字未变**——`git diff a89de9c463bc pkvm-tlb-flush-range-fix` 只有 `pkvm_cov.c` 的 66 行新增。

顺带修掉了一处遗漏：issue 006 的修复提交是 13 条里唯一缺 `Signed-off-by` 和 `K2CI-Arch` 的，已补齐（它本来就走不了 Gerrit）。

分支形状：

```
之前                                   之后
pkvm-el2-kcov                          pkvm-el2-kcov
  一个 per-CPU 提交                      per-CPU 提交
                                       └ ring_dump                    ← 新增
pkvm-tlb-flush-range-fix               pkvm-tlb-flush-range-fix
  per-CPU 提交（重复的一份）              12 个修复提交（线性，接在 ring_dump 之上）
  12 个修复提交
```

**那份重复的 per-CPU 提交被 rebase 自动丢弃了**（`b93d9fb02e7c`，与 `pkvm-el2-kcov` 上的 `1b629e14e9db` patch-id 相同 `70d403d6…`，只有 trailer 不同：后者带 `Signed-off-by` 与 `K2CI-Arch`，是正式版）。丢掉的是本地那份，留下的是正式那份——方向正确，但值得记一笔，因为 `git rebase` 是**静默**跳过的，只在 stderr 留了一行提示。

## 对照

| 旧（第一次重写后） | 新 | 提交 |
| --- | --- | --- |
| `a89de9c463bc` | `8a2140adc7f8` | record host_vcpu before the checks that can fail（issue 006 的修复） |
| `5e0f9501dc0f` | `07e38664d9b1` | correct the comment on why the split path carries the state |
| `56f0eb9b9cb8` | `0f514a31e95a` | propagate the dirty-log granule probe's own error |
| `a43a2ffdae6d` | `6a6ea2d9ab75` | account hyp donations even when the topup fails |
| `86244737f356` | `5ac898be14cd` | split then relax（issue 005 的修复） |
| `c98cb052d881` | `62970fa9f1a6` | revert the dirty-log page-state fix, it livelocks |
| `d4fc45388129` | `775e1ea71210` | hand the dirty-log hypercall the pfn instead of a lookup |
| `b6b4085db938` | `c7b6f8a0a588` | report the donation residual signed |
| `c364dbc89580` | `45c6e7f4907b` | account the hyp donations made on the fault paths |
| `f4633260b106` | `97ace9f944c7` | keep the page state when the dirty log remaps a page |
| `dd7f6c8e09f3` | `22a54a9fd32a` | route range TLB flush through the VM handle（issue 002 的修复） |
| `b8c268de7fc6` | `c8487de3cb0a` | defer RLIMIT_MEMLOCK accounting out of mmap_lock（issue 001 的修复） |
| `b93d9fb02e7c` | **已丢弃** | one coverage ring per CPU（重复，正式版是 `1b629e14e9db`） |
| — | `1f52c77ba624` | pkvm_cov: export the raw ring so a wedged EL2 can be read（新增） |

## issue 007 的 `@a89de9c463bc` 保持原样

`issues/007-.../ISSUE.md` 的 front-matter 记的是 `klinux 6.6.103+ #25 @a89de9c463bc`。**不改。** 那是当时板子上实际跑的东西，是观测事实而不是索引——理由与第一次重写那节完全相同。拿旧哈希 `git show` 失败时，回来查这两张表。

板子上现在跑的内核 = 旧 `a89de9c463bc` + ring_dump 补丁，与新的 `pkvm-tlb-flush-range-fix` 顶端 `8a2140adc7f8` **内容一致**，无需重编。
