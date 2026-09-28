# Phase 1 fuzz harness — hypervisor_memory_ownership

Randomized, offline, zero-external-dependency fuzz harness for the `hypervisor_memory_ownership`
crate (the no_std memory-ownership model). It drives random operation sequences and checks
invariants (no panic with overflow/debug-assertions on; counts stability; owner_of/ranges
non-overlap; the crate's own `validate_destroy_after_reclaim` / `validate_prepared_owner_mapping_cleanup`).
Findings are split into HARD (panic/oracle) and SOFT (surprising-but-maybe-intended). Built by
opencode+qwen3.8-max, gated by Claude (independent build+run; `--selftest-oracle` proves the oracle
catches injected bugs; crate sources verified unmodified).

## Build & run
Add `ownership-fuzz.rs` as a workspace member binary depending (path) on `hypervisor_memory_ownership`
(see `harness-Cargo.toml.sample`; `API.md` is the API reference used to write it). Then:

    RUSTFLAGS="-C overflow-checks=on -C debug-assertions=on" cargo run --release --bin ownership-fuzz -- 200000
    RUSTFLAGS="-C overflow-checks=on -C debug-assertions=on" cargo run --release --bin ownership-fuzz -- --selftest-oracle

## Findings (2026-09-26)
- **HARD (low severity): `reclaim_owner_mappings` panics on bad input.**
  `MemExtentStore::reclaim_owner_mappings(owner)` returns `()` but internally does
  `self.prepare_owner_mapping_cleanup(&[owner]).expect("owner mapping cleanup must prepare")`
  (memextent.rs:1191). When `prepare_owner_mapping_cleanup` returns `Err(Denied)` the public
  function panics. In an EL2 hypervisor a panic is a shutdown/hang. **Severity is low today**: the
  only callers are the crate's own tests — no runtime/manager path calls it (grep-verified). It is a
  latent robustness footgun: a public fn hiding a fallible op behind `.expect()`. Repro seeds e.g.
  7243 (op #1), 1079 (op #16). Per project rules (fix-only-if-we-rewrite; FIXME otherwise), recorded
  not fixed.
- **SOFT: `validate_destroy_after_reclaim(unknown_handle, 0)` returns `Ok`** for handles never
  registered (~3% of seeds). Possibly-intended leniency; noted for the invariant review, not a bug.
