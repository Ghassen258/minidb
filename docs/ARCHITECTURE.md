# MiniDB engineering notes

These notes describe the actual v0.1 implementation, not proposed future features.

## Invariants

**Table storage**

- Each table has exactly one 64-bit integer primary-key column.
- `Table::rows` holds immutable historical row values plus a mutable `alive` flag.
- `Table::index.find(key)` points to the newest row version for that key (including tombstoned keys).
- `Table::live` is the number of visible rows, not the number of stored versions.
- A duplicate insert is rejected when the indexed row is live; reinsertion after deletion is allowed.
- `UPDATE` records a complete new row version and marks the preceding version inactive.
- `COMPACT` writes only live rows and reconstructs the index, removing old versions and tombstoned keys.

**B+ tree**

- Leaf nodes store sorted integer keys and parallel row-slot values.
- Internal nodes store sorted separators and `keys.size()+1` child pointers.
- A separator is the minimum key of the subtree to its right at insertion time.
- `upper_bound` routes a lookup for a separator into the right subtree; leaf `lower_bound` finds an exact match.
- Non-root nodes may have fewer than the usual B+ tree occupancy guarantee after initialization, but ordinary insert splits distribute keys approximately equally. Deletions are tombstones, so the tree doesn't need merge/redistribution.
- Leaves link forward (`Node::next`) for an ordered walk; nodes are owned by `std::unique_ptr` with stable pointee addresses.
- MAX_KEYS is 8; a ninth key triggers a split. This is a small educational fanout, not a storage-page size.

## Complexity

Let `N` be the number of unique primary keys including tombstones, `R` the number of live and obsolete row versions, `E` the number of log events, and `B` the tree's branching factor.

| Operation | Approximate work |
|---|---|
| B+ tree lookup | `O(log_B N)` tree traversal and small in-node binary searches |
| B+ tree insert | `O(log_B N)` search + possible propagated splits |
| `SELECT ... WHERE primary_key = integer` | `O(log_B N)` plus cost of materializing the returned row |
| Unfiltered `SELECT` | `O(R)` scan; inactive versions are skipped |
| `INSERT` | index work plus append of a length-prefixed binary log event |
| `UPDATE`, `DELETE` | primary-key search plus append; old versions remain until compacted |
| Startup | sequential replay of `E` events, rebuilding indexes |
| `COMPACT` | writes all live rows, then replays the compacted file |

A generic `SELECT *` and a key lookup have dramatically different output sizes. The included benchmark is intended to expose these workload differences, not claim a direct performance speedup independent of result size.

## Recovery sequence

1. The constructor verifies the file magic and processes length-prefixed frames.
2. For each complete frame, validate its FNV-1a payload checksum before applying it.
3. If the final frame is incomplete, truncate the trailing partial data and warn.
4. If a complete frame has a bad checksum or impossible operation, report an error and refuse to open.
5. For compaction, write `.tmp`; rename the current file to `.bak`; rename `.tmp` to the database path; remove `.bak`.
6. If startup sees no main file but finds `.bak`, restore it. An orphan `.tmp` or `.bak` is never silently overwritten during `COMPACT`.

**Power-failure caveat:** `std::ofstream::flush()` does not imply an OS-level durable storage barrier. There is no `fsync`, file lock, multi-statement transaction boundary, or atomic checkpoint guarantee. MiniDB is appropriate for learning and non-critical experiments only.

## Why separate the modules?

`bptree.cpp` knows nothing about SQL or files. `minidb.cpp` owns row and persistence invariants. `sql.cpp` translates explicit grammar into typed database calls and rejects anything outside that grammar. `main.cpp` only deals with user interface and process arguments. This separation makes the index and persistence individually testable without invoking a CLI.

## Future design choices

An on-disk tree would replace slot-number leaf values with `(page_id, offset)` references, require a fixed-size page layout, a free-page manager, eviction policies, and crash-consistent page updates. A real WAL would need a transaction/commit record protocol and durable write barriers. Those are out of scope for v0.1, so they are not presented as implemented features.
