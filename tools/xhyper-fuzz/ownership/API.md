# hypervisor_memory_ownership — complete API for the harness (READ ONLY THIS FILE)

Do NOT open any .rs file, Cargo.lock, or anything under target/. Everything you need is here.

## Types (all fields below are `pub`)
```rust
use hypervisor_object_model::ObjectHandle; // pub struct ObjectHandle { pub id: u64, pub generation: u32 }
//   construct directly: ObjectHandle { id, generation }  OR  ObjectHandle::from_raw(u64)  (both pub)
use hypervisor_memory_ownership::{MemoryRange, MemoryOwner, MemoryEntry, MemDb, MemExtentStore, OwnershipError, OwnershipCounts};

// pub struct MemoryRange { pub start: u64, pub end: u64 }
MemoryRange::new(start: u64, end: u64) -> Result<MemoryRange, OwnershipError>
MemoryRange::from_start_size(start: u64, size: u64) -> Result<MemoryRange, OwnershipError>

// pub enum MemoryOwner { Unowned, Object(ObjectHandle) }
// pub struct MemoryEntry { pub range: MemoryRange, pub owner: MemoryOwner }

// pub enum OwnershipError { InvalidRange, Unaligned, Uncovered, WrongOwner, NoSpace,
//   NotFound, AlreadyExists, OutsideExtent, Busy, HasChildren, OutstandingDonation,
//   MappingMismatch, Denied }   // every fallible call returns Result<_, OwnershipError>. An Err is NORMAL. A panic is a BUG.
```

## MemDb<const N: usize>
```rust
MemDb::<N>::from_range(range: MemoryRange, owner: MemoryOwner) -> Result<MemDb<N>, OwnershipError>
memdb.owner_of(address: u64) -> Option<MemoryOwner>
memdb.ranges() -> &[MemoryEntry]
```

## MemExtentStore<const N, const E, const D, const M>
```rust
MemExtentStore::<N,E,D,M>::new(memdb: MemDb<N>) -> MemExtentStore<N,E,D,M>
store.memdb() -> &MemDb<N>
store.counts() -> OwnershipCounts            // OwnershipCounts: derive(PartialEq,Eq,Clone,Copy,Debug)
store.extent_range(handle: ObjectHandle) -> Result<MemoryRange, OwnershipError>
store.register_root(handle: ObjectHandle, range: MemoryRange) -> Result<(), OwnershipError>
store.register_sparse_root(handle: ObjectHandle, range: MemoryRange) -> Result<(), OwnershipError>
store.derive(parent: ObjectHandle, child: ObjectHandle, offset: u64, size: u64) -> Result<(), OwnershipError>
store.donate(source: ObjectHandle, destination: ObjectHandle, offset: u64, size: u64) -> Result<(), OwnershipError>
store.prepare_donation(source, destination, offset, size) -> Result<PreparedDonation<N>, OwnershipError>
store.commit_donation(prepared: PreparedDonation<N>) -> Result<CommittedDonation, OwnershipError>
store.rollback_donation(prepared: PreparedDonation<N>) -> Result<(), OwnershipError>
store.reclaim(current: ObjectHandle, offset: u64, size: u64) -> Result<(), OwnershipError>
store.add_mapping(extent: ObjectHandle, offset: u64, size: u64) -> Result<(), OwnershipError>
store.remove_mapping(extent: ObjectHandle, offset: u64, size: u64) -> Result<(), OwnershipError>
store.has_mapping_for(owner: ObjectHandle, extent: ObjectHandle, offset: u64, size: u64) -> Result<bool, OwnershipError>
store.destroy_extent(handle: ObjectHandle) -> Result<(), OwnershipError>
store.reclaim_owner_mappings(owner: ObjectHandle)      // returns ()
store.validate_destroy_after_reclaim(handle: ObjectHandle, transfer_count: usize) -> Result<(), OwnershipError>
store.prepare_owner_mapping_cleanup(owners: &[ObjectHandle]) -> Result<PreparedOwnerMappingCleanup, OwnershipError>
store.validate_prepared_owner_mapping_cleanup(prepared: &PreparedOwnerMappingCleanup) -> Result<(), OwnershipError>
// PreparedDonation<N>, CommittedDonation, PreparedOwnerMappingCleanup are opaque values you just pass along.
```

## Canonical setup pattern (copy this)
```rust
const ROOT:  ObjectHandle = ObjectHandle { id: 1, generation: 0 };
const CHILD: ObjectHandle = ObjectHandle { id: 2, generation: 0 };
let whole = MemoryRange::new(0x1000, 0x9000).unwrap();
let memdb = MemDb::<8>::from_range(whole, MemoryOwner::Object(ROOT)).unwrap();
let mut store = MemExtentStore::<8,4,4,4>::new(memdb);
store.register_root(ROOT, whole).unwrap();
store.derive(ROOT, CHILD, 0x2000, 0x3000).unwrap();
// make more handles as ObjectHandle { id: n, generation: 0 } for n in 1..=N
```
Pick const sizes big enough for your sequences, e.g. MemExtentStore::<32,32,32,32>.
