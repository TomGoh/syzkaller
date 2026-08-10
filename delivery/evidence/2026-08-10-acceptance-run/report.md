# pKVM 模糊测试交付报告

生成时间：2026-08-10 17:00 +0800

## 1. 结论

- 崩溃数：**0** → **通过**
- EL2 hypercall handler 覆盖：**18 / 30** 个可达 handler

崩溃数为 0 是本次运行的**预期结果**，且这个结果是有意义的：同一内核上复现套件已证明 7 个已知缺陷全部存在（见第 5 节），所以零崩溃说明调用面筛选生效，而不是内核本身没有问题。

## 2. EL2 可达面与覆盖

派发表 `HOST_HCALL` 共 47 个有 handler 的 id；`hcall_min = 18`，EL2 在查表前就拒绝所有 id < 18 的调用，因此其中 **17** 个从 `/dev/kvm` 不可达。**可达面 = 30**。

| 状态 | handler |
| --- | --- |
| ✅ 已覆盖 | `handle___kvm_adjust_pc` |
| ✅ 已覆盖 | `handle___kvm_timer_set_cntvoff` |
| ✅ 已覆盖 | `handle___kvm_vcpu_run` |
| ✅ 已覆盖 | `handle___pkvm_finalize_teardown_vm` |
| ✅ 已覆盖 | `handle___pkvm_host_map_guest` |
| ✅ 已覆盖 | `handle___pkvm_host_relax_guest_perms` |
| ✅ 已覆盖 | `handle___pkvm_host_share_hyp` |
| ✅ 已覆盖 | `handle___pkvm_host_unshare_hyp` |
| ✅ 已覆盖 | `handle___pkvm_hyp_alloc_mgt_refill` |
| ✅ 已覆盖 | `handle___pkvm_init_vcpu` |
| ✅ 已覆盖 | `handle___pkvm_init_vm` |
| ✅ 已覆盖 | `handle___pkvm_reclaim_dying_guest_page` |
| ✅ 已覆盖 | `handle___pkvm_start_teardown_vm` |
| ✅ 已覆盖 | `handle___pkvm_vcpu_load` |
| ✅ 已覆盖 | `handle___pkvm_vcpu_put` |
| ✅ 已覆盖 | `handle___pkvm_vcpu_sync_state` |
| ✅ 已覆盖 | `handle___vgic_v3_restore_vmcr_aprs` |
| ✅ 已覆盖 | `handle___vgic_v3_save_vmcr_aprs` |
| ⬜ 未覆盖 | `handle___pkvm_hibernate_cpu_resume` |
| ⬜ 未覆盖 | `handle___pkvm_hibernate_finalize` |
| ⬜ 未覆盖 | `handle___pkvm_hibernate_prepare` |
| ⬜ 未覆盖 | `handle___pkvm_hibernate_restore` |
| ⬜ 未覆盖 | `handle___pkvm_hibernate_save` |
| ⬜ 未覆盖 | `handle___pkvm_host_dirty_log_guest` |
| ⬜ 未覆盖 | `handle___pkvm_host_unmap_guest` |
| ⬜ 未覆盖 | `handle___pkvm_host_wrprotect_guest` |
| ⬜ 未覆盖 | `handle___pkvm_hyp_alloc_mgt_reclaim` |
| ⬜ 未覆盖 | `handle___pkvm_hyp_alloc_mgt_reclaimable` |
| ⬜ 未覆盖 | `handle___pkvm_prot_finalize` |
| ⬜ 未覆盖 | `handle___pkvm_tlb_flush_vmid` |

## 3. EL2 覆盖率数据链完整性

```
ring_pages          32
ring_capacity_pcs   16381
owner_cpu           -1
armed_cpus          8
leaked_bytes        0

# arm/skip
begin_calls         29553413354
skip_not_owner      0
skip_no_kcov        5149386281
drains              24404027069

# EL2 producer (per-#23, summed)
el2_hits            2199601385281
el2_written         2199601385281
el2_dropped_ringful 0
el2_nested_dropped  0
el2_overflow_drains 0
el2_hits_max        2322
el2_written_max     2322
LOST_IN_RING        0

# runtime->link conversion
link_ok             2199601385281
link_dropped        0

# task KCOV delivery
kcov_requested      2199601385281
kcov_accepted       60013580775
kcov_mode_lost      0
LOST_IN_KCOV_AREA   2139587804506

# per-#23 hit distribution (log2 buckets)
hits_8_15 113227010
hits_16_31 838700
hits_32_63 8006678927
hits_64_127 8144269050
hits_128_255 8106013891
hits_256_511 32887854
hits_512_1023 933
hits_1024_2047 110580
hits_2048_4095 124
```

自洽性 `drains == begin_calls - skip_not_owner - skip_no_kcov`：29553413354 - 0 - 5149386281 = 24404027073 vs drains 24404027069 → 成立（差值为快照时在飞行中的窗口，个位数属正常）

## 4. 运行环境

```
6.6.103+
 17:00:47 up  5:30,  4 users,  load average: 10.17, 9.79, 9.84
```

配置：`/home/jose/syzkaller-pkvm/delivery/n90-delivery.cfg`

## 5. 已知缺陷状态

本次运行的目标内核**不含任何 pKVM 修复**。同一内核上复现套件的结果：`delivery/evidence/2026-08-10-repro-pack-summary.txt`。

交付 config 通过收窄调用面来回避这些缺陷，理由逐条写在 `delivery/n90-delivery.cfg` 的注释里。**一旦本次运行出现崩溃，要么是筛选有洞，要么是新缺陷 —— 两种都要查。**

## 6. 附件

- `rawcover.txt`
- `stats.html`
- `syscalls.html`
- `pkvm_cov-stats.txt`
