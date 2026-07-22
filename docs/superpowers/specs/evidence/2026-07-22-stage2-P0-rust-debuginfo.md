# Stage-2 P0 — Rust hyp DWARF prerequisite VERIFIED (isolated experiment, 2026-07-22)

Scope note: this proves **one prerequisite** — that an EL2 **link address** can be mapped to a Rust
**source file:line**. It is *not* "Stage 2 solved": the real-EL2-PC → link-address conversion and the
EL2-PC → ring → EL1 KCOV → syzkaller feedback path are still unbuilt. And it is a **feasibility** result in
a throwaway tree — the config is **not landed** in `/home/jose/common`, and N90 was not touched.

The audit's one hard blocker (§3 of `2026-07-22-stage2-el2-boundary-audit.md`) was that the Rust nvhe crate
ships with no DWARF, so EL2 Rust PCs symbolize to `nvhe_rust.<hash>-cgu.0:?`. Verified fixed **end-to-end
against a freshly linked vmlinux**, in a throwaway copy (`/home/jose/common-dbgtest`).

## The change (two flags together — combination proven, not each in isolation)
```diff
- export RUSTFLAGS="… -C target-feature=-neon"
+ export RUSTFLAGS="… -C target-feature=-neon -C debuginfo=2"
+ export CARGO_PROFILE_RELEASE_DEBUG=2
```
Both were set, so this proves the **combination** works; it does **not** yet establish which alone suffices.
Then a clean Rust-hyp rebuild (removed `rust/target` + `nvhe_rust.o*`) and `make vmlinux`. Result:
`nvhe_rust.o` goes from **0 → 14 `.debug_*` sections**; vmlinux grows 480,752,440 → 483,911,328 B (debug
sections only). **Not committed as a kernel change** — when Stage 2 is built for real, make the debuginfo
setting a **config-gated fuzz-kernel option** (not an unconditional change to all Rust hyp builds), landed
together with the SanCov change, so N90's kernel + the symbolization vmlinux + the EL2 coverage code all
come from one build.

## Acceptance — FULL chain to the final vmlinux (addr2line on the linked image)
Every previously-`??:?` Rust EL2 symbol now resolves to its exact definition line:
```
__kvm_nvhe__ZN…handle___pkvm_host_map_guest…  @ ffff800081d9c8cc -> hyp_main.rs:1069
__kvm_nvhe___pkvm_init_vm                      -> pkvm.rs:4014
__kvm_nvhe___pkvm_host_share_hyp               -> mem_protect/permissions.rs:251
__kvm_nvhe_host_stage2_idmap                   -> mem_protect/host.rs:1841
__kvm_nvhe_handle_trap                         -> hyp_main.rs:2975
(a mangled internal method)                    -> hyp_main.rs:803
control: __kvm_nvhe___get_fault_info (C)       -> hyp/fault.h:48   (still resolves)
```
The lines match the real source (e.g. `pub extern "C" fn __pkvm_init_vm` is at `pkvm.rs:4014`). So the
producer-side symbolization path is **unblocked**: with `-C debuginfo=2`, an EL2 Rust PC → `rust/src/*.rs:line`
works from vmlinux, no per-file-object workaround needed for Rust.

## Notes / caveats for the real Stage-2 build
- The debuginfo build is a **different binary** (a create-VM handler moved `ffff800081d9c8a0 → …c8cc`). So
  Stage 2 must use **one** build for both the N90 fuzzing kernel and the symbolization vmlinux — rebuild
  once with debuginfo and deploy that.
- `opt-level=3` is retained; inlining still collapses some frames (addr2line `-i` recovers the inline
  chain). Acceptable — line resolution works.
- Remaining open item (unchanged): Q1 hyp-VA → link-addr conversion is still only *nm-address* verified,
  not a real collected EL2 PC. That belongs to the producer/delivery prototype, not this P0.

Throwaway tree removed after capture.
