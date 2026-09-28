use std::cell::RefCell;
use std::collections::HashMap;
use std::panic::AssertUnwindSafe;
use std::time::Instant;

use hypervisor_memory_ownership::{
    MemDb, MemExtentStore, MemoryEntry, MemoryOwner, MemoryRange, OwnershipError,
};

type Db = MemDb<32>;
type Store = MemExtentStore<32, 32, 32, 32>;
type HKey = (u64, u32);

const ROOT: HKey = (1, 0);
const CHILD: HKey = (2, 0);
const ROOT_START: u64 = 0x1000;
const ROOT_END: u64 = 0x9000;

#[repr(C)]
struct RawHandle {
    id: u64,
    generation: u32,
}

fn raw_owner(id: u64, generation: u32) -> MemoryOwner {
    MemoryOwner::Object(unsafe {
        std::mem::transmute::<RawHandle, _>(RawHandle { id, generation })
    })
}

macro_rules! mk_handle {
    ($id:expr, $gen:expr) => {
        match raw_owner($id, $gen) {
            MemoryOwner::Object(h) => h,
            MemoryOwner::Unowned => unreachable!(),
        }
    };
}

fn verify_handle_layout() -> Result<(), String> {
    let h = mk_handle!(0x1122_3344_5566_7788u64, 0x0A0B_0C0Du32);
    if h.id != 0x1122_3344_5566_7788 || h.generation != 0x0A0B_0C0D {
        return Err(format!(
            "ObjectHandle layout mismatch: wrote (0x1122334455667788, 0x0a0b0c0d), read back ({:#x}, {:#x})",
            h.id, h.generation
        ));
    }
    Ok(())
}

fn hstr(k: HKey) -> String {
    format!("h{}:g{}", k.0, k.1)
}

fn owner_key(owner: &MemoryOwner) -> Option<HKey> {
    match owner {
        MemoryOwner::Object(h) => Some((h.id, h.generation)),
        MemoryOwner::Unowned => None,
    }
}

fn err_name(e: &OwnershipError) -> &'static str {
    match e {
        OwnershipError::InvalidRange => "InvalidRange",
        OwnershipError::Unaligned => "Unaligned",
        OwnershipError::Uncovered => "Uncovered",
        OwnershipError::WrongOwner => "WrongOwner",
        OwnershipError::NoSpace => "NoSpace",
        OwnershipError::NotFound => "NotFound",
        OwnershipError::AlreadyExists => "AlreadyExists",
        OwnershipError::OutsideExtent => "OutsideExtent",
        OwnershipError::Busy => "Busy",
        OwnershipError::HasChildren => "HasChildren",
        OwnershipError::OutstandingDonation => "OutstandingDonation",
        OwnershipError::MappingMismatch => "MappingMismatch",
        OwnershipError::Denied => "Denied",
    }
}

fn err_name_opt(e: Option<&OwnershipError>) -> String {
    match e {
        Some(err) => err_name(err).to_string(),
        None => "Ok".to_string(),
    }
}

struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    fn new(seed: u64) -> Self {
        SplitMix64 { state: seed }
    }

    fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    fn below(&mut self, n: u64) -> u64 {
        if n == 0 {
            0
        } else {
            self.next_u64() % n
        }
    }
}

#[derive(Clone)]
enum Op {
    RegisterRoot(HKey, u64, u64, bool),
    RegisterSparseRoot(HKey, u64, u64, bool),
    Derive(HKey, HKey, u64, u64),
    Donate(HKey, HKey, u64, u64),
    PrepareCommit(HKey, HKey, u64, u64),
    PrepareCommitRollback(HKey, HKey, u64, u64),
    Reclaim(HKey, u64, u64),
    AddMapping(HKey, u64, u64),
    RemoveMapping(HKey, u64, u64),
    DestroyExtent(HKey),
    ReclaimOwnerMappings(HKey),
    ValidateDestroy(HKey, usize),
    OwnerMappingCleanup(Vec<HKey>),
}

impl Op {
    fn name(&self) -> &'static str {
        match self {
            Op::RegisterRoot(..) => "register_root",
            Op::RegisterSparseRoot(..) => "register_sparse_root",
            Op::Derive(..) => "derive",
            Op::Donate(..) => "donate",
            Op::PrepareCommit(..) => "prepare_donation+commit_donation",
            Op::PrepareCommitRollback(..) => "prepare_donation+commit_donation+rollback_donation",
            Op::Reclaim(..) => "reclaim",
            Op::AddMapping(..) => "add_mapping",
            Op::RemoveMapping(..) => "remove_mapping",
            Op::DestroyExtent(..) => "destroy_extent",
            Op::ReclaimOwnerMappings(..) => "reclaim_owner_mappings",
            Op::ValidateDestroy(..) => "validate_destroy_after_reclaim",
            Op::OwnerMappingCleanup(..) => "prepare_owner_mapping_cleanup+validate",
        }
    }

    fn describe(&self) -> String {
        match self {
            Op::RegisterRoot(h, s, sz, fss) => format!(
                "register_root(h={}, start={:#x}, size={:#x}, from_start_size={})",
                hstr(*h), s, sz, fss
            ),
            Op::RegisterSparseRoot(h, s, sz, fss) => format!(
                "register_sparse_root(h={}, start={:#x}, size={:#x}, from_start_size={})",
                hstr(*h), s, sz, fss
            ),
            Op::Derive(p, c, o, s) => format!(
                "derive(parent={}, child={}, offset={:#x}, size={:#x})",
                hstr(*p),
                hstr(*c),
                o,
                s
            ),
            Op::Donate(a, b, o, s) => format!(
                "donate(src={}, dst={}, offset={:#x}, size={:#x})",
                hstr(*a),
                hstr(*b),
                o,
                s
            ),
            Op::PrepareCommit(a, b, o, s) => format!(
                "prepare_donation+commit_donation(src={}, dst={}, offset={:#x}, size={:#x})",
                hstr(*a),
                hstr(*b),
                o,
                s
            ),
            Op::PrepareCommitRollback(a, b, o, s) => format!(
                "prepare_donation+commit_donation+rollback_donation(src={}, dst={}, offset={:#x}, size={:#x})",
                hstr(*a),
                hstr(*b),
                o,
                s
            ),
            Op::Reclaim(h, o, s) => format!(
                "reclaim(cur={}, offset={:#x}, size={:#x})",
                hstr(*h), o, s
            ),
            Op::AddMapping(h, o, s) => format!(
                "add_mapping(extent={}, offset={:#x}, size={:#x})",
                hstr(*h), o, s
            ),
            Op::RemoveMapping(h, o, s) => format!(
                "remove_mapping(extent={}, offset={:#x}, size={:#x})",
                hstr(*h), o, s
            ),
            Op::DestroyExtent(h) => format!("destroy_extent(h={})", hstr(*h)),
            Op::ReclaimOwnerMappings(h) => format!("reclaim_owner_mappings(owner={})", hstr(*h)),
            Op::ValidateDestroy(h, n) => format!(
                "validate_destroy_after_reclaim(h={}, transfer_count={})",
                hstr(*h), n
            ),
            Op::OwnerMappingCleanup(list) => format!(
                "prepare_owner_mapping_cleanup+validate(owners=[{}])",
                list.iter().map(|k| hstr(*k)).collect::<Vec<_>>().join(", ")
            ),
        }
    }
}

fn gen_handle(rng: &mut SplitMix64) -> HKey {
    match rng.below(16) {
        0..=10 => (1 + rng.below(8), 0),
        11 => (1 + rng.below(8), 1 + rng.below(3) as u32),
        12 => (0, 0),
        13 => (1 + rng.below(8), u32::MAX),
        14 => (90 + rng.below(20), 0),
        _ => (u64::MAX, u32::MAX),
    }
}

fn gen_offset(rng: &mut SplitMix64) -> u64 {
    match rng.below(20) {
        0 => 0,
        1 => 0x1000,
        2 => 0x2000,
        3 => 0x3000,
        4 => 0x1000 * rng.below(8),
        5 => 0x1000 * rng.below(4),
        6 => 0x1000 * rng.below(6),
        7 => 0x1000 * rng.below(3),
        8 => 0x4000,
        9 => 0x5000,
        10 => 0x6000,
        11 => 0x7000,
        12 => rng.below(0x8000) & !0xfff,
        13 => 0x1800,
        14 => 0xfff,
        15 => rng.below(0x9000),
        16 => u64::MAX,
        17 => u64::MAX - 0x1000,
        18 => 0x1_0000_0000,
        _ => 0x8000,
    }
}

fn gen_size(rng: &mut SplitMix64) -> u64 {
    match rng.below(20) {
        0 => 0x1000,
        1 => 0x2000,
        2 => 0x3000,
        3 => 0x1000 * (1 + rng.below(6)),
        4 => 0x1000 * (1 + rng.below(3)),
        5 => 0x1000 * (1 + rng.below(4)),
        6 => 0x4000,
        7 => 0x5000,
        8 => 0x1000,
        9 => 0x2000,
        10 => 0x3000,
        11 => 0x1000 * (1 + rng.below(2)),
        12 => ((0x1000 + rng.below(0x4000)) & !0xfff).max(0x1000),
        13 => 0,
        14 => 0,
        15 => 0x100,
        16 => 0xfff,
        17 => u64::MAX,
        18 => 0x1_0000_0000,
        _ => 0x8000,
    }
}

fn gen_start(rng: &mut SplitMix64) -> u64 {
    match rng.below(8) {
        0 => ROOT_START,
        1 => 0x2000,
        2 => 0x4000,
        3 => ROOT_START + 0x1000 * rng.below(8),
        4 => 0,
        5 => ROOT_END - 0x1000,
        6 => u64::MAX - 0x1000,
        _ => rng.below(0x9000),
    }
}

fn gen_op(rng: &mut SplitMix64) -> Op {
    let pick = rng.below(100);
    let a = gen_handle(rng);
    let b = gen_handle(rng);
    let off = gen_offset(rng);
    let sz = gen_size(rng);
    let fss = rng.below(2) == 0;
    if pick < 7 {
        Op::RegisterRoot(a, gen_start(rng), sz, fss)
    } else if pick < 15 {
        Op::RegisterSparseRoot(a, gen_start(rng), sz, fss)
    } else if pick < 31 {
        Op::Derive(a, b, off, sz)
    } else if pick < 41 {
        Op::Donate(a, b, off, sz)
    } else if pick < 48 {
        Op::PrepareCommit(a, b, off, sz)
    } else if pick < 55 {
        Op::PrepareCommitRollback(a, b, off, sz)
    } else if pick < 66 {
        Op::Reclaim(a, off, sz)
    } else if pick < 73 {
        Op::AddMapping(a, off, sz)
    } else if pick < 79 {
        Op::RemoveMapping(a, off, sz)
    } else if pick < 87 {
        Op::DestroyExtent(a)
    } else if pick < 92 {
        Op::ReclaimOwnerMappings(a)
    } else if pick < 96 {
        Op::ValidateDestroy(a, rng.below(4) as usize)
    } else {
        let n = 1 + rng.below(3);
        Op::OwnerMappingCleanup((0..n).map(|_| gen_handle(rng)).collect())
    }
}

fn gen_ops(rng: &mut SplitMix64) -> Vec<Op> {
    let n = 20 + rng.below(41);
    (0..n).map(|_| gen_op(rng)).collect()
}

#[derive(Clone)]
struct Finding {
    seed: u64,
    op_index: usize,
    kind: String,
    detail: String,
}

fn finding(seed: u64, op_index: usize, kind: &str, detail: String) -> Finding {
    Finding {
        seed,
        op_index,
        kind: kind.to_string(),
        detail,
    }
}

struct Ctx {
    seed: u64,
    findings: Vec<Finding>,
    inject: bool,
}

impl Ctx {
    fn new(seed: u64, inject: bool) -> Self {
        Ctx {
            seed,
            findings: Vec::new(),
            inject,
        }
    }

    fn push(&mut self, op_index: usize, kind: &str, detail: String) {
        if self.findings.len() < 64 {
            self.findings.push(finding(self.seed, op_index, kind, detail));
        }
    }
}

fn make_range(start: u64, size: u64, from_start_size: bool) -> Result<MemoryRange, OwnershipError> {
    if from_start_size {
        MemoryRange::from_start_size(start, size)
    } else {
        MemoryRange::new(start, start.wrapping_add(size))
    }
}

fn build_store() -> Option<Store> {
    let whole = MemoryRange::new(ROOT_START, ROOT_END).ok()?;
    let memdb = Db::from_range(whole, raw_owner(ROOT.0, ROOT.1)).ok()?;
    let mut store = Store::new(memdb);
    let _ = store.register_root(mk_handle!(ROOT.0, ROOT.1), whole);
    let _ = store.derive(
        mk_handle!(ROOT.0, ROOT.1),
        mk_handle!(CHILD.0, CHILD.1),
        0x2000,
        0x3000,
    );
    Some(store)
}

fn find_overlap(entries: &[MemoryEntry]) -> Option<String> {
    for i in 0..entries.len() {
        let a = &entries[i].range;
        if a.start > a.end {
            return Some(format!("entry[{}] inverted range {:#x}..{:#x}", i, a.start, a.end));
        }
        for j in (i + 1)..entries.len() {
            let b = &entries[j].range;
            if a.start < b.end && b.start < a.end {
                return Some(format!(
                    "entry[{}] {:#x}..{:#x} owner {:?} overlaps entry[{}] {:#x}..{:#x} owner {:?}",
                    i,
                    a.start,
                    a.end,
                    owner_key(&entries[i].owner),
                    j,
                    b.start,
                    b.end,
                    owner_key(&entries[j].owner)
                ));
            }
        }
    }
    None
}

fn entries_dump(entries: &[MemoryEntry]) -> String {
    entries
        .iter()
        .map(|e| format!(
            "{:#x}..{:#x}:owner={:?}",
            e.range.start,
            e.range.end,
            owner_key(&e.owner)
        ))
        .collect::<Vec<_>>()
        .join(", ")
}

fn find_uncovered_owner(entries: &[MemoryEntry], addr: u64, owner: HKey) -> Option<String> {
    let covered = entries.iter().any(|e| {
        e.range.start <= addr && addr < e.range.end && owner_key(&e.owner) == Some(owner)
    });
    if covered {
        None
    } else {
        Some(format!(
            "owner_of({:#x}) = Object({}) but no ranges() entry covers that address with that owner; entries = [{}]",
            addr,
            hstr(owner),
            entries_dump(entries)
        ))
    }
}

fn check_db(ctx: &mut Ctx, store: &Store, addrs: &[u64], op_index: usize) {
    let db = store.memdb();
    let entries = db.ranges();
    if let Some(detail) = find_overlap(entries) {
        ctx.push(op_index, "OVERLAP", detail);
    }
    for &addr in addrs {
        if let Some(MemoryOwner::Object(h)) = db.owner_of(addr) {
            let key = (h.id, h.generation);
            if let Some(detail) = find_uncovered_owner(entries, addr, key) {
                ctx.push(op_index, "OWNER_NOT_COVERED", detail);
            }
        }
    }
    if ctx.inject {
        let bogus_addr = 0xDEAD_0000u64;
        let actual = match db.owner_of(bogus_addr) {
            Some(MemoryOwner::Object(h)) => format!("Object({})", hstr((h.id, h.generation))),
            Some(MemoryOwner::Unowned) => "Unowned".to_string(),
            None => "None".to_string(),
        };
        let holds = match db.owner_of(bogus_addr) {
            Some(MemoryOwner::Object(h)) => h.id == 4242,
            _ => false,
        };
        let detail = if holds {
            format!(
                "injected false invariant unexpectedly HELD: owner_of({:#x}) == Object(h4242)",
                bogus_addr
            )
        } else {
            format!(
                "injected false invariant was falsified as designed: harness asserted owner_of({:#x}) == Object(h4242), actual = {}",
                bogus_addr, actual
            )
        };
        ctx.push(op_index, "SELFTEST-INJECTED", detail);
    }
}

fn probe(ctx: &mut Ctx, store: &mut Store, op_index: usize) {
    let before = store.counts();
    let _ = store.extent_range(mk_handle!(ROOT.0, ROOT.1));
    let _ = store.extent_range(mk_handle!(CHILD.0, CHILD.1));
    let _ = store.extent_range(mk_handle!(9999, 0));
    let _ = store.has_mapping_for(
        mk_handle!(ROOT.0, ROOT.1),
        mk_handle!(CHILD.0, CHILD.1),
        0x1000,
        0x1000,
    );
    let _ = store.has_mapping_for(mk_handle!(CHILD.0, CHILD.1), mk_handle!(ROOT.0, ROOT.1), 0, 0);
    let after = store.counts();
    if before != after {
        ctx.push(
            op_index,
            "COUNTS_CHANGED_BY_QUERY",
            format!(
                "read-only probes (extent_range/has_mapping_for) changed counts: {:?} -> {:?}",
                before, after
            ),
        );
    }
}

fn check_validate_destroy(ctx: &mut Ctx, store: &mut Store, key: HKey, transfers: usize, op_index: usize) {
    let before = store.counts();
    let first = store.validate_destroy_after_reclaim(mk_handle!(key.0, key.1), transfers);
    let second = store.validate_destroy_after_reclaim(mk_handle!(key.0, key.1), transfers);
    let after = store.counts();
    let a = err_name_opt(first.as_ref().err());
    let b = err_name_opt(second.as_ref().err());
    if a != b {
        ctx.push(
            op_index,
            "VALIDATE_DESTROY_NONDETERMINISTIC",
            format!(
                "validate_destroy_after_reclaim({}, {}) returned {} then {}",
                hstr(key), transfers, a, b
            ),
        );
    }
    if first.is_ok() && is_unknown(key) {
        ctx.push(
            op_index,
            "VALIDATE_DESTROY_OK_FOR_UNKNOWN_HANDLE",
            format!(
                "validate_destroy_after_reclaim({}, {}) returned Ok for a handle that was never registered",
                hstr(key), transfers
            ),
        );
    }
    if before != after {
        ctx.push(
            op_index,
            "COUNTS_CHANGED_BY_VALIDATE",
            format!(
                "validate_destroy_after_reclaim({}, {}) changed counts: {:?} -> {:?}",
                hstr(key), transfers, before, after
            ),
        );
    }
}

fn is_unknown(key: HKey) -> bool {
    key.0 == 0 || key.0 >= 90 || key.0 == u64::MAX || key.1 != 0
}

fn is_soft(kind: &str) -> bool {
    matches!(kind, "VALIDATE_DESTROY_OK_FOR_UNKNOWN_HANDLE")
}

fn apply(ctx: &mut Ctx, store: &mut Store, op: &Op, op_index: usize, addrs: &[u64]) -> bool {
    let before = store.counts();
    let result: Result<(), OwnershipError> = match op {
        Op::RegisterRoot(h, start, size, fss) => match make_range(*start, *size, *fss) {
            Ok(range) => store.register_root(mk_handle!(h.0, h.1), range),
            Err(e) => Err(e),
        },
        Op::RegisterSparseRoot(h, start, size, fss) => match make_range(*start, *size, *fss) {
            Ok(range) => store.register_sparse_root(mk_handle!(h.0, h.1), range),
            Err(e) => Err(e),
        },
        Op::Derive(p, c, off, sz) => store.derive(
            mk_handle!(p.0, p.1),
            mk_handle!(c.0, c.1),
            *off,
            *sz,
        ),
        Op::Donate(a, b, off, sz) => store.donate(
            mk_handle!(a.0, a.1),
            mk_handle!(b.0, b.1),
            *off,
            *sz,
        ),
        Op::PrepareCommit(a, b, off, sz) => {
            match store.prepare_donation(mk_handle!(a.0, a.1), mk_handle!(b.0, b.1), *off, *sz) {
                Ok(prepared) => store.commit_donation(prepared).map(|_committed| ()),
                Err(e) => Err(e),
            }
        }
        Op::PrepareCommitRollback(a, b, off, sz) => {
            match store.prepare_donation(mk_handle!(a.0, a.1), mk_handle!(b.0, b.1), *off, *sz) {
                Ok(prepared) => match store.commit_donation(prepared) {
                    Ok(committed) => store.rollback_donation(committed),
                    Err(e) => Err(e),
                },
                Err(e) => Err(e),
            }
        }
        Op::Reclaim(h, off, sz) => store.reclaim(mk_handle!(h.0, h.1), *off, *sz),
        Op::AddMapping(h, off, sz) => store.add_mapping(mk_handle!(h.0, h.1), *off, *sz),
        Op::RemoveMapping(h, off, sz) => store.remove_mapping(mk_handle!(h.0, h.1), *off, *sz),
        Op::DestroyExtent(h) => store.destroy_extent(mk_handle!(h.0, h.1)),
        Op::ReclaimOwnerMappings(h) => {
            store.reclaim_owner_mappings(mk_handle!(h.0, h.1));
            let c1 = store.counts();
            let c2 = store.counts();
            if c1 != c2 {
                ctx.push(
                    op_index,
                    "COUNTS_UNSTABLE",
                    format!("counts() differed between two consecutive reads: {:?} vs {:?}", c1, c2),
                );
            }
            check_db(ctx, store, addrs, op_index);
            probe(ctx, store, op_index);
            return true;
        }
        Op::ValidateDestroy(h, n) => {
            check_validate_destroy(ctx, store, *h, *n, op_index);
            check_db(ctx, store, addrs, op_index);
            probe(ctx, store, op_index);
            return true;
        }
        Op::OwnerMappingCleanup(list) => {
            let owners: Vec<_> = list.iter().map(|k| mk_handle!(k.0, k.1)).collect();
            let before_cleanup = store.counts();
            let _ = match store.prepare_owner_mapping_cleanup(&owners) {
                Ok(prepared) => {
                    let first = store.validate_prepared_owner_mapping_cleanup(&prepared);
                    let a = err_name_opt(first.as_ref().err());
                    let repeat = std::panic::catch_unwind(AssertUnwindSafe(|| {
                        store.validate_prepared_owner_mapping_cleanup(&prepared)
                    }));
                    match repeat {
                        Ok(second) => {
                            let b = err_name_opt(second.as_ref().err());
                            if a != b {
                                ctx.push(
                                    op_index,
                                    "CLEANUP_VALIDATE_NONDETERMINISTIC",
                                    format!(
                                        "validate_prepared_owner_mapping_cleanup(owners=[{}]) returned {} then {}",
                                        list.iter().map(|k| hstr(*k)).collect::<Vec<_>>().join(", "),
                                        a,
                                        b
                                    ),
                                );
                            }
                        }
                        Err(_) => ctx.push(
                            op_index,
                            "REPEAT_VALIDATE_PANICS",
                            format!(
                                "second validate_prepared_owner_mapping_cleanup(owners=[{}]) panicked: {}",
                                list.iter().map(|k| hstr(*k)).collect::<Vec<_>>().join(", "),
                                take_panic()
                            ),
                        ),
                    }
                    first
                }
                Err(e) => Err(e),
            };
            let after_cleanup = store.counts();
            if before_cleanup != after_cleanup {
                ctx.push(
                    op_index,
                    "COUNTS_CHANGED_BY_VALIDATE",
                    format!(
                        "prepare/validate owner mapping cleanup changed counts: {:?} -> {:?}",
                        before_cleanup, after_cleanup
                    ),
                );
            }
            check_db(ctx, store, addrs, op_index);
            probe(ctx, store, op_index);
            return true;
        }
    };

    let ok = result.is_ok();
    if !ok {
        let after = store.counts();
        if before != after {
            ctx.push(
                op_index,
                "COUNTS_CHANGED_ON_ERR",
                format!(
                    "{} returned Err({}) but counts changed: {:?} -> {:?}",
                    op.name(),
                    err_name_opt(result.as_ref().err()),
                    before,
                    after
                ),
            );
        }
    } else if let Op::DestroyExtent(h) = op {
        let resolved = store.extent_range(mk_handle!(h.0, h.1));
        if resolved.is_ok() {
            ctx.push(
                op_index,
                "DESTROYED_EXTENT_STILL_RESOLVES",
                format!(
                    "destroy_extent({}) returned Ok but extent_range({}) still resolves",
                    hstr(*h),
                    hstr(*h)
                ),
            );
        }
    }
    let c1 = store.counts();
    let c2 = store.counts();
    if c1 != c2 {
        ctx.push(
            op_index,
            "COUNTS_UNSTABLE",
            format!("counts() differed between two consecutive reads: {:?} vs {:?}", c1, c2),
        );
    }
    check_db(ctx, store, addrs, op_index);
    probe(ctx, store, op_index);
    ok
}

fn sample_addrs(seed: u64) -> Vec<u64> {
    let mut rng = SplitMix64::new(seed ^ 0x5DEE_CE66_DFFF_1234);
    let mut addrs = vec![
        0x0,
        ROOT_START,
        ROOT_START + 0x1000,
        ROOT_END - 1,
        ROOT_END,
        0xFFFF_FFFF_FFFF_F000,
    ];
    for _ in 0..3 {
        addrs.push(ROOT_START + 0x1000 * rng.below(8));
        addrs.push(rng.next_u64());
    }
    addrs
}

fn run_ops(seed: u64, ops: &[Op], inject: bool) -> (Vec<Finding>, u64, u64, u64) {
    let mut ctx = Ctx::new(seed, inject);
    let addrs = sample_addrs(seed);
    let mut ok_count = 0u64;
    let mut err_count = 0u64;
    let mut panic_count = 0u64;
    let mut store = match build_store() {
        Some(s) => s,
        None => {
            ctx.push(0, "SETUP_FAILED", "canonical setup pattern returned Err".to_string());
            return (ctx.findings, 0, 0, 0);
        }
    };
    if std::panic::catch_unwind(AssertUnwindSafe(|| check_db(&mut ctx, &store, &addrs, usize::MAX)))
        .is_err()
    {
        ctx.push(
            usize::MAX,
            "PANIC",
            format!("oracle check on the freshly built store panicked: {}", take_panic()),
        );
        return (ctx.findings, 0, 0, 1);
    }
    let mut pending_reclaim: Option<HKey> = None;
    for (i, op) in ops.iter().enumerate() {
        let after_reclaim = pending_reclaim;
        pending_reclaim = match op {
            Op::Reclaim(h, ..) => Some(*h),
            _ => None,
        };
        let outcome = std::panic::catch_unwind(AssertUnwindSafe(|| {
            apply(&mut ctx, &mut store, op, i, &addrs)
        }));
        let ok = match outcome {
            Ok(ok) => ok,
            Err(_) => {
                let message = take_panic();
                ctx.push(
                    i,
                    "PANIC",
                    format!("{} panicked: {}", op.describe(), message),
                );
                panic_count += 1;
                break;
            }
        };
        if ok {
            ok_count += 1;
        } else {
            err_count += 1;
        }
        if let Op::DestroyExtent(h) = op {
            if let Some(reclaimed) = after_reclaim {
                let validated = std::panic::catch_unwind(AssertUnwindSafe(|| {
                    for transfers in 0..3usize {
                        check_validate_destroy(&mut ctx, &mut store, reclaimed, transfers, i);
                    }
                    check_validate_destroy(&mut ctx, &mut store, *h, 0, i);
                }));
                if validated.is_err() {
                    ctx.push(
                        i,
                        "PANIC",
                        format!(
                            "validate_destroy_after_reclaim after reclaim+destroy panicked: {}",
                            take_panic()
                        ),
                    );
                    panic_count += 1;
                    break;
                }
            }
        }
    }
    (ctx.findings, ok_count, err_count, panic_count)
}

thread_local! {
    static LAST_PANIC: RefCell<Option<String>> = const { RefCell::new(None) };
}

fn install_panic_hook() {
    std::panic::set_hook(Box::new(|info| {
        let location = match info.location() {
            Some(l) => format!("{}:{}:{}", l.file(), l.line(), l.column()),
            None => "<unknown location>".to_string(),
        };
        let message = info
            .payload()
            .downcast_ref::<&str>()
            .map(|s| (*s).to_string())
            .or_else(|| info.payload().downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "<non-string payload>".to_string());
        LAST_PANIC.with(|slot| {
            *slot.borrow_mut() = Some(format!("{} at {}", message, location));
        });
    }));
}

fn take_panic() -> String {
    LAST_PANIC
        .with(|slot| slot.borrow_mut().take())
        .unwrap_or_else(|| "<panic message unavailable>".to_string())
}

fn fails_with(seed: u64, ops: &[Op]) -> Option<Finding> {
    let outcome = std::panic::catch_unwind(AssertUnwindSafe(|| run_ops(seed, ops, false)));
    match outcome {
        Ok((findings, _, _, _)) => findings.into_iter().find(|f| !is_soft(&f.kind)),
        Err(_) => Some(finding(seed, usize::MAX, "PANIC", take_panic())),
    }
}

fn minimize(seed: u64, ops: Vec<Op>) -> Vec<Op> {
    let mut current = ops;
    if fails_with(seed, &current).is_none() {
        return current;
    }
    let mut chunk = (current.len() / 2).max(1);
    while chunk >= 1 {
        let mut i = 0;
        let mut shrank = false;
        while i < current.len() {
            let end = (i + chunk).min(current.len());
            let mut candidate = current.clone();
            candidate.drain(i..end);
            if !candidate.is_empty() && fails_with(seed, &candidate).is_some() {
                current = candidate;
                shrank = true;
            } else {
                i = end;
            }
        }
        if !shrank {
            if chunk == 1 {
                break;
            }
            chunk /= 2;
        }
    }
    current
}

fn ops_for_seed(seed: u64) -> Vec<Op> {
    let mut rng = SplitMix64::new(
        seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0xA5A5_A5A5_A5A5_A5A5,
    );
    gen_ops(&mut rng)
}

fn print_failure(seed: u64, ops: &[Op], minimal: &[Op], findings: &[Finding]) {
    println!("=== FAILING SEED {} ===", seed);
    for f in findings {
        let index = if f.op_index == usize::MAX {
            "setup".to_string()
        } else {
            format!("#{}", f.op_index)
        };
        println!("  [{}] seed {} op {}: {}", f.kind, f.seed, index, f.detail);
    }
    println!(
        "  reproduce: cargo run --offline --release --bin ownership-fuzz -- 1 {}",
        seed
    );
    println!("  minimal reproducer ({} of {} ops):", minimal.len(), ops.len());
    for (i, op) in minimal.iter().enumerate() {
        println!("    {:>3}. {}", i, op.describe());
    }
    println!("  full op sequence ({} ops):", ops.len());
    for (i, op) in ops.iter().enumerate() {
        println!("    {:>3}. {}", i, op.describe());
    }
    println!();
}

fn run_minimize(seed: u64) -> i32 {
    let ops = ops_for_seed(seed);
    let first = match fails_with(seed, &ops) {
        Some(f) => f,
        None => {
            println!("seed {} does not reproduce a hard violation", seed);
            return 0;
        }
    };
    let minimal = minimize(seed, ops.clone());
    println!("seed {}: [{}] op #{}: {}", seed, first.kind, first.op_index, first.detail);
    print_failure(seed, &ops, &minimal, &[first]);
    1
}

fn run_selftest() -> i32 {
    let mut reported: Vec<Finding> = Vec::new();
    let mut note = |kind: &str, detail: String| {
        println!("FINDING [{}] {}", kind, detail);
        reported.push(finding(0, 0, kind, detail));
    };

    let a = MemoryRange::new(0x1000, 0x3000).expect("range a");
    let b = MemoryRange::new(0x2000, 0x4000).expect("range b");
    let fabricated = vec![
        MemoryEntry {
            range: a,
            owner: raw_owner(ROOT.0, ROOT.1),
        },
        MemoryEntry {
            range: b,
            owner: raw_owner(CHILD.0, CHILD.1),
        },
    ];
    match find_overlap(&fabricated) {
        Some(detail) => note("SELFTEST-OVERLAP-DETECTED", detail),
        None => note(
            "SELFTEST-BROKEN",
            "the overlap oracle did not flag a fabricated overlapping pair".to_string(),
        ),
    }

    match find_uncovered_owner(&fabricated, 0x5000, CHILD) {
        Some(detail) => note("SELFTEST-COVER-ORACLE-DETECTED", detail),
        None => note(
            "SELFTEST-BROKEN",
            "the covering-entry oracle did not flag an address that no entry covers".to_string(),
        ),
    }

    let mut rng = SplitMix64::new(0xC0FF_EE12_3456_789A);
    let ops = gen_ops(&mut rng);
    match std::panic::catch_unwind(AssertUnwindSafe(|| run_ops(0xBAD_5EED, &ops, true))) {
        Ok((findings, _, _, _)) => {
            let injected: Vec<Finding> = findings
                .iter()
                .filter(|f| f.kind == "SELFTEST-INJECTED")
                .cloned()
                .collect();
            if injected.is_empty() {
                note(
                    "SELFTEST-BROKEN",
                    "the injected false invariant was not reported by the real store path".to_string(),
                );
            } else {
                note(&injected[0].kind, injected[0].detail.clone());
                println!(
                    "  (the injected false invariant fired on {} of {} ops in that run)",
                    injected.len(),
                    ops.len()
                );
            }
            for f in findings.iter().filter(|f| f.kind != "SELFTEST-INJECTED") {
                note(&f.kind, f.detail.clone());
            }
        }
        Err(_) => note(
            "SELFTEST-PANIC",
            format!("injected run panicked: {}", take_panic()),
        ),
    }

    let panicked = std::panic::catch_unwind(AssertUnwindSafe(|| {
        let mut v: Vec<u64> = Vec::new();
        v.push(1);
        let _ = v[3];
    }));
    match panicked {
        Err(_) => note(
            "SELFTEST-PANIC-CAUGHT",
            format!("panic oracle caught and reported: {}", take_panic()),
        ),
        Ok(()) => note(
            "SELFTEST-BROKEN",
            "catch_unwind did not report a deliberate panic".to_string(),
        ),
    }

    let broken = reported.iter().any(|f| f.kind == "SELFTEST-BROKEN");
    if broken || reported.is_empty() {
        println!("SELFTEST: FAIL - the oracle machinery did not report the injected violations");
        1
    } else {
        let mut kinds: Vec<String> = reported.iter().map(|f| f.kind.clone()).collect();
        kinds.sort();
        kinds.dedup();
        println!(
            "SELFTEST: PASS - the harness reported {} failures across {} kinds; a normal run reports none of them",
            reported.len(),
            kinds.len()
        );
        0
    }
}

fn main() {
    install_panic_hook();
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--selftest-oracle") {
        if let Err(detail) = verify_handle_layout() {
            eprintln!("FATAL: {}", detail);
            std::process::exit(2);
        }
        std::process::exit(run_selftest());
    }
    if let Err(detail) = verify_handle_layout() {
        eprintln!("FATAL: {}", detail);
        std::process::exit(2);
    }
    let positional: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let count: u64 = positional
        .first()
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(200_000);
    let start_seed: u64 = positional
        .get(1)
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(1);
    if count == 0 {
        println!("seeds run: 0, nothing to do");
        return;
    }

    let started = Instant::now();
    let mut retained: Vec<(u64, Vec<Op>, Vec<Op>, Vec<Finding>)> = Vec::new();
    let mut kind_counts: HashMap<String, u64> = HashMap::new();
    let mut soft_examples: HashMap<String, (u64, Finding)> = HashMap::new();
    let mut failing_seeds = 0u64;
    let mut soft_seeds = 0u64;
    let mut panic_seeds = 0u64;
    let mut total_ok = 0u64;
    let mut total_err = 0u64;
    let mut seeds_done = 0u64;

    for seed in start_seed..start_seed.saturating_add(count) {
        let mut rng = SplitMix64::new(
            seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0xA5A5_A5A5_A5A5_A5A5,
        );
        let ops = gen_ops(&mut rng);
        match std::panic::catch_unwind(AssertUnwindSafe(|| run_ops(seed, &ops, false))) {
            Ok((findings, ok_count, err_count, panics)) => {
                total_ok += ok_count;
                total_err += err_count;
                panic_seeds += panics.min(1);
                let hard: Vec<Finding> = findings.iter().filter(|f| !is_soft(&f.kind)).cloned().collect();
                let soft: Vec<Finding> = findings.iter().filter(|f| is_soft(&f.kind)).cloned().collect();
                if !soft.is_empty() {
                    soft_seeds += 1;
                    for f in &soft {
                        *kind_counts.entry(f.kind.clone()).or_insert(0) += 1;
                        soft_examples
                            .entry(f.kind.clone())
                            .or_insert_with(|| (seed, f.clone()));
                    }
                }
                if !hard.is_empty() {
                    failing_seeds += 1;
                    for f in &hard {
                        if f.kind.contains("PANIC") {
                            eprintln!("PANIC on seed {} op #{}: {}", seed, f.op_index, f.detail);
                        }
                        *kind_counts.entry(f.kind.clone()).or_insert(0) += 1;
                    }
                    if retained.len() < 10 {
                        retained.push((seed, ops.clone(), minimize(seed, ops.clone()), hard));
                    }
                }
            }
            Err(_) => {
                let message = take_panic();
                panic_seeds += 1;
                failing_seeds += 1;
                *kind_counts.entry("PANIC".to_string()).or_insert(0) += 1;
                eprintln!("PANIC on seed {}: {}", seed, message);
                if retained.len() < 10 {
                    retained.push((
                        seed,
                        ops.clone(),
                        minimize(seed, ops.clone()),
                        vec![finding(seed, usize::MAX, "PANIC", message)],
                    ));
                }
            }
        }
        seeds_done += 1;
        if seeds_done % 25_000 == 0 {
            eprintln!(
                "progress: {}/{} seeds, {} failing, {:.1}s elapsed",
                seeds_done,
                count,
                failing_seeds,
                started.elapsed().as_secs_f64()
            );
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    println!(
        "seeds run: {} ({}..={}), wall time {:.2}s ({:.0} seeds/s)",
        seeds_done,
        start_seed,
        start_seed + seeds_done - 1,
        elapsed,
        seeds_done as f64 / elapsed.max(1e-9)
    );
    println!(
        "calls: {} returned Ok, {} returned Err (an Err is expected and is not a finding)",
        total_ok, total_err
    );
    println!(
        "seeds with a hard violation (panic/oracle): {} ({} of them panicked)",
        failing_seeds, panic_seeds
    );
    println!("seeds with a soft observation: {}", soft_seeds);
    let mut kinds: Vec<(String, u64)> = kind_counts.into_iter().collect();
    kinds.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    if !kinds.is_empty() {
        println!("finding kinds (hard unless marked soft):");
        for (kind, n) in &kinds {
            let tier = if is_soft(kind) { "soft" } else { "HARD" };
            println!("  [{}] {}: {}", tier, kind, n);
        }
    }
    if !soft_examples.is_empty() {
        println!();
        println!("soft observations (design questions, not treated as bugs):");
        let mut soft_list: Vec<(String, (u64, Finding))> = soft_examples.into_iter().collect();
        soft_list.sort_by(|a, b| a.0.cmp(&b.0));
        for (kind, (seed, f)) in &soft_list {
            println!("  {} (first seen on seed {}): {}", kind, seed, f.detail);
            println!(
                "    reproduce: cargo run --offline --release --bin ownership-fuzz -- 1 {}",
                seed
            );
        }
    }
    if failing_seeds == 0 && panic_seeds == 0 {
        println!();
        println!(
            "RESULT: no panics and no hard oracle violations over {} seeds ({} calls)",
            seeds_done,
            total_ok + total_err
        );
    } else {
        println!();
        for (seed, ops, minimal, findings) in &retained {
            print_failure(*seed, ops, minimal, findings);
        }
        println!(
            "RESULT: {} seeds with hard violations out of {} (first {} reproduced above)",
            failing_seeds,
            seeds_done,
            retained.len()
        );
        std::process::exit(1);
    }
}
