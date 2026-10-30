# MiniDB — A tiny relational database written in C++20

**A functioning SQL-like storage engine built to understand how databases work under the hood.** MiniDB takes a statement through a hand-written tokenizer and parser, validates a typed schema, uses an in-memory **B+ tree** to locate primary-key records, and persists changes in a checksummed append-only binary log.

No SQLite, database SDK, Boost, or third-party runtime dependencies are used.

> **Educational scope:** MiniDB is a single-process prototype, **not** an ACID-compliant replacement for SQLite or PostgreSQL. Its B+ tree is **in memory** (rebuilt after restart), and its log format is a deliberately simple append-only format rather than a page-oriented storage engine.


**[▶ Try the C++ engine in your browser](https://ghassen258.github.io/minidb/)** — WebAssembly SQL workbench, no installation required.

## Browser edition (WebAssembly)

The browser edition compiles the **same C++20 SQL parser, B+ tree and storage engine** using Emscripten, with a thin C++ bridge (`src/web_bridge.cpp`) and a zero-framework interface (`web/`). It supports SQL editing, table browsing, query results, `EXPLAIN` plans, and a ready-to-use demo dataset.

Data is stored in Emscripten's in-memory filesystem during execution and serialized to browser `localStorage` where available. The database can be **exported as a native `.mdb` file** and imported back into the workbench. No queries or data are uploaded to a server. The browser edition limits snapshots to 2 MiB; use the native CLI for larger datasets. Browser local storage can be cleared by browser settings.

Use **Ctrl/⌘ + Enter** to run one or several semicolon-separated queries. SQL features and limitations are the same as in the native engine.

### Build the browser demo locally

Install [Emscripten](https://emscripten.org/docs/getting_started/downloads.html), then run with its environment activated:

```bash
mkdir -p dist
em++ -std=c++20 -O2 -Isrc src/bptree.cpp src/minidb.cpp src/sql.cpp src/web_bridge.cpp \
  --bind --no-entry -s MODULARIZE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web \
  -s FORCE_FILESYSTEM=1 -s ALLOW_MEMORY_GROWTH=1 \
  -o dist/minidb.mjs
cp web/index.html web/style.css web/app.js dist/
python -m http.server 8000 --directory dist
```

Open `http://localhost:8000`. GitHub Actions builds and deploys the same WebAssembly edition to Pages on each source update.


## Try it in 30 seconds

**Prerequisites:** CMake ≥3.18 and a C++20 compiler (GCC ≥10, Clang ≥12, or recent MSVC).

```bash
cmake -S . -B build
cmake --build build --config Release
```

Run the terminal REPL on Linux/macOS:

```bash
./build/minidb --db workspace.mdb
```

On Windows PowerShell, with a Visual Studio multi-configuration generator:

```powershell
.\build\Release\minidb.exe --db workspace.mdb
```

For a single-configuration generator on Windows, the executable may instead be `build\minidb.exe`.

In the MiniDB terminal:

```sql
CREATE TABLE engineers (id INT PRIMARY KEY, name TEXT, team TEXT, tickets INT);
INSERT INTO engineers VALUES (101, 'Ada Lovelace', 'Platform', 8);
INSERT INTO engineers VALUES (102, 'Grace Hopper', 'Systems', 12);
SELECT * FROM engineers;
SELECT name, team FROM engineers WHERE id = 102;
EXPLAIN SELECT * FROM engineers WHERE id = 102;
UPDATE engineers SET tickets = 13 WHERE id = 102;
DELETE FROM engineers WHERE id = 101;
STATS;
COMPACT;
```

Example query result:

```text
minidb> SELECT name, team FROM engineers WHERE id = 102;
+--------------+---------+
| name         | team    |
+--------------+---------+
| Grace Hopper | Systems |
+--------------+---------+
1 row(s) matched.

minidb> EXPLAIN SELECT * FROM engineers WHERE id = 102;
PLAN: B+ TREE PRIMARY KEY LOOKUP on engineers.id = 102
```

**Try the bundled demo** (uses a fresh database path):

```bash
./build/minidb --db demo.mdb --file examples/demo.sql
```

The `--file` runner accepts one statement per line (and lines starting with `--` or `#` for comments). A REPL command is likewise one statement per line. Quotes may contain doubled apostrophes, such as `'O''Brien'`.

## Feature overview

| Layer | Current implementation |
|---|---|
| SQL lexer/parser | Handwritten token scanner, quoted strings, typed integer literals, strict statement validation |
| Data model | Multiple named tables, `INT` (signed 64-bit), `TEXT`, exactly one `INT PRIMARY KEY` per table |
| CRUD | `CREATE TABLE`, `INSERT`, `SELECT`, `UPDATE`, `DELETE` |
| Queries | `SELECT *`, column projection, `COUNT(*)`, optional primary-key equality `WHERE` |
| Query planner | `EXPLAIN SELECT ...` distinguishes primary-key index lookup vs. full scan |
| Index | Real B+ tree with internal split propagation, ordered linked leaves, and upsert; used for keyed queries |
| Persistence | Checksummed length-prefixed binary append log, schema and row replay on restart |
| Crash detection | Torn/incomplete final record is discarded with a warning; checksum mismatch is a hard error |
| Maintenance | `STATS` and `COMPACT` (rewrite live rows; reclaim obsolete versions and index tombstones) |
| Tooling | CMake, CTest, 3-OS GitHub Actions, example script, reproducible microbenchmark |

### Supported SQL

```sql
CREATE TABLE books (id INT PRIMARY KEY, title TEXT, pages INT);
INSERT INTO books VALUES (1, 'The Hobbit', 310);
SELECT * FROM books;
SELECT title, pages FROM books WHERE id = 1;
SELECT COUNT(*) FROM books;
SELECT COUNT(*) FROM books WHERE id = 1;
UPDATE books SET pages = 320 WHERE id = 1;
DELETE FROM books WHERE id = 1;
EXPLAIN SELECT * FROM books WHERE id = 1;
SHOW TABLES;
DESCRIBE books;
STATS;
COMPACT;
```

Identifiers are case-insensitive. `WHERE` currently supports **only equality against the table's integer primary key**. MiniDB rejects unsupported syntax instead of silently falling back to another interpretation. `DELETE` and `UPDATE` require a primary-key `WHERE` clause. It does not implement joins, ordering, indexes on non-key columns, transactions, or arbitrary SQL expressions.

Other terminal commands: `.help`, `.tables`, `.stats`, `.quit`.

## How MiniDB works

```text
                          SQL statement
                               |
                    Tokenizer + SQL parser
                               |
                   Schema/type validation
                               |
                 +-------------+-------------+
                 |                           |
             SELECT                       Mutation
                 |                           |
       +---------+---------+           Encode event
       |                   |                |
  WHERE id = ?         No WHERE       Append checksum +
       |                   |           payload to .mdb
    B+ tree             Row scan            |
  key -> slot         live rows only    Apply to memory
       |                                   |
       +-------------> Result <------------+
```

**`src/bptree.cpp`** implements the in-memory ordered primary-key index. Leaves hold `(int64 key, row-slot)` pairs and are linked for ordered iteration. Internal nodes store separator keys. A leaf overflow splits its keys and promotes a separator to its parent; splits propagate to a new root if needed. Deleted row versions are marked inactive; `COMPACT` removes tombstones and rebuilds the index.

**`src/minidb.cpp`** owns the schema, rows and persistence. An inserted row is appended to the log before its in-memory state is changed. An update creates a new row version; a delete marks a row inactive. Existing data loads by replaying validated log frames. An incomplete trailing frame is truncated and reported; a checksum mismatch fails startup instead of pretending data is valid.

**`src/sql.cpp`** tokenizes and parses the explicitly supported SQL grammar; the parser rejects extra statements or unknown tokens. **`src/main.cpp`** supplies the REPL, formatted tables, one-shot `--execute` mode, and file-driven demos.

### On-disk record shape

```text
[8-byte file magic: "MINIDB1\n"]
[record #1]
  uint32 little-endian payload length
  uint32 little-endian FNV-1a checksum
  payload: operation + length-prefixed table/columns/values
[record #2]
  ...
```

The log stores `CREATE`, `INSERT`, `UPDATE` and `DELETE` events. Compaction writes a new log to `*.tmp`, moves the old file to `*.bak`, installs the new file, and rebuilds indexes. If interrupted when only `*.bak` remains, MiniDB restores the original at startup. Checkpoint/compaction operations are **not guaranteed power-loss atomic** across filesystems: no `fsync` calls or concurrency locking are implemented.

## Tests and benchmark

```bash
ctest --test-dir build -C Release --output-on-failure
```

The native test suite exercises thousands of B+ tree lookups across several split levels, parser errors, typed schemas, multiple tables, CRUD, persistence across process-like reopen cycles, checksum rejection, incomplete-write recovery, and compaction. The CLI smoke test is repeatable.

Run a microbenchmark with 5,000 rows (or specify a different row count, 10–100,000):

```bash
./build/minidb_bench 5000
```

The benchmark reports insert + append time, 300 indexed key lookups, and 30 full scans. **These timings measure different query workloads**—a keyed lookup returns one row and a full scan materializes all rows—so they are intentionally **not** an apples-to-apples comparison of query algorithms. They are best used for repeatable profiling of MiniDB itself, not comparisons with production databases. The benchmark refuses to overwrite an existing `minidb_benchmark.mdb` in the system temporary directory.

## Architecture / repository

```text
src/minidb.hpp              Database API and B+ tree declarations
src/bptree.cpp              B+ tree insert, split, lookup and leaf walk
src/minidb.cpp              Table engine, file format, recovery, compact
src/sql.cpp                 SQL tokenizer and recursive-descent parser
src/main.cpp                Native CLI / REPL
benchmarks/bench.cpp        Repeatable insert, lookup and scan microbenchmark
tests/test_minidb.cpp       Engine and persistence tests
examples/demo.sql           Complete executable sample session
.github/workflows/tests.yml Linux, Windows and macOS builds + tests
CMakeLists.txt              C++20 build targets
```

## Tradeoffs and next milestones

This first version keeps the moving parts visible: the B+ tree is **RAM-resident**, row versions live in a vector, and the entire log is read into memory on startup. There is no file locking; use **one writer process only**. Persistence is flush-to-stream, not an OS-level durability guarantee. Record checksums catch corruption, not malicious file modifications. Names must be simple identifiers, and text literals use standard single quotes.

Planned extensions are intentionally separate milestones rather than implied existing features: **(1)** page-based storage and an on-disk B+ tree with a buffer pool, **(2)** transactional write-ahead logging and crash-safe commits, **(3)** secondary indexes / a richer query planner, **(4)** SQL expressions, joins and range predicates, **(5)** systematic performance profiles and fuzzing.

## License

MIT © 2026 Ghassen258.
