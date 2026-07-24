ftrace-off deploy + smoke + EFI-pstore + campaign -- RAW EVIDENCE (N90, 2026-07-24)
==================================================================================
Summary record: ../../2026-07-24-pkvm-ftrace-off-deploy-and-campaign.md
Provenance (build IDs / hashes / commits): provenance.txt

smoke/        4-page ftrace-off #23 smoke, 3 replicates:
  stats-r{1,2,3}-p4.txt   raw kvm/pkvm_cov/stats per run (el2_hits ~11.7k, LOST_IN_RING 0)
  el2set-r{1,2,3}-p4.txt, el2set-union-p4.txt   the 127 unique EL2 PCs
  el2-symbolized.txt      each EL2 PC -> function + rust/src/*.rs:line
  el2-by-file.txt         composition (mem_protect/* donation-map dominant; ftrace.rs gone)

campaign/     supervised manager campaign:
  pkvm-campaign.cfg       the config (pkvm_serial, pkvm_owner_cpu=0, pstore=false)
  manager.log             manager output (corpus/coverage stayed 0)
  kernel-counters.txt     peak kernel pkvm_cov/stats (drains 323, all loss/skip 0)
  rpc-eof-raw.txt         the raw "failed to recv rpc ... n=0" line
  rpc-eof-analysis.txt    corrected analysis: n=0 is peer-closed EOF, errno is stale

efi-pstore/   n90-efi-setvariable-fails.txt   proof N90 firmware cannot SetVariable
                                              (efi-pstore registers but records nothing)

3b-reach/     observed.txt   3B reachability observed (fw_ipa=base reaches #23; 0x7fd00000
                             fails clean) -- the full matrix is Step 2

The mechanism (up through kcov_add_pcs) is proven; the open items are the manager
feedback transport (RPC EOF) and the 3B input-surface reach, both scoped as
infrastructure/tuning, not the Stage-2 mechanism.
