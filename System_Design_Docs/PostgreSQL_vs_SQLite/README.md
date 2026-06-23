# PostgreSQL vs SQLite — Architecture Comparison

**Author:** Vibhuti Bhatnagar — `24BCS10288`
**Course:** Advanced DBMS — Scaler School of Technology

Two SQL databases, two completely different architectures. PostgreSQL is a multi-process server you connect to over a socket; SQLite is a C library you link into your application and which opens a database file directly. Almost every other technical difference between the two — file layout, locking model, concurrency, durability — falls out of that single design choice.

All numbers in this document were measured locally on **PostgreSQL 16.13** and **SQLite 3.51.0** on macOS arm64. The PostgreSQL setup script is `../PostgreSQL_Internals/setup.sql`; SQLite hands-on data is reused from my Lab 1 submission (`../../README.md`) and Lab 4 (`../../Lab4/24BCS10288 Vibhuti Bhatnagar/`).

---

## 1. Problem Background

**SQLite (D. Richard Hipp, 2000).** SQLite was written for software running on a US Navy destroyer, where you couldn't assume any database server would be alive — the application had to keep working even if it lived alone on a damaged machine. The design tagline became "*a replacement for `fopen()`*", not "a replacement for Oracle." The whole database is a single file, and the engine is a C library that runs inside the host process. There is no separate database process.

**PostgreSQL (UC Berkeley POSTGRES, 1986 → SQL frontend, 1994).** PostgreSQL descends from research into extensible, object-relational databases. Its target audience was always the *shared* database: hundreds of clients, long-lived data, durability across crashes and restarts, ACID isolation between concurrent users. None of that is possible without a long-running process that owns the data files and arbitrates access between clients.

The two systems answer different questions:

* **SQLite:** *"How do I give one application local, transactional, structured storage with zero configuration?"*
* **PostgreSQL:** *"How do I let many clients safely share one consistent dataset?"*

---

## 2. Architecture Overview

### SQLite — embedded / in-process

```
┌───────────────────────────────────────────────┐
│            Application process                 │
│                                                │
│   app code  ──►  SQLite library (libsqlite3)   │
│                     │  SQL compiler            │
│                     │  VDBE bytecode engine    │
│                     │  B-tree layer            │
│                     │  Pager + page cache      │
│                     ▼                          │
│                  OS read() / write()           │
└──────────────────────┬─────────────────────────┘
                       ▼
                 one file: app.db
                 (+ app.db-wal  /  app.db-journal)
```

A `SELECT` is a direct function call inside the application's address space — no IPC, no TCP, no fork. Concurrency between different processes that open the same `app.db` is coordinated entirely through **OS-level file locks** on that one file.

### PostgreSQL — client-server, process-per-connection

```
client(psql/app) ──TCP/socket──►  postmaster (listener)
                                      │ fork()
                                      ▼
                                 backend process (one per connection)
                                      │
                                      ▼
                          ╔═══════════════════════╗
                          ║   Shared Buffers      ║◄──── bgwriter / checkpointer
                          ║   (128 MB pool)       ║
                          ╚════════════╤══════════╝
                                       │
                                       ▼
                               heap + index files
                          (one file per relation)
                                       ▲
                                       │ WAL records
                                  WAL writer → pg_wal/
```

When a client connects, the **postmaster** forks a dedicated **backend process** for it. Backends share a single region of shared memory (the buffer pool) and coordinate through it. Background processes (WAL writer, checkpointer, autovacuum, bgwriter) handle durability and cleanup.

```bash
$ ps aux | grep postgres
postgres: logical replication launcher
postgres: autovacuum launcher
postgres: walwriter
postgres: background writer
postgres: checkpointer
/opt/homebrew/opt/postgresql@16/bin/postgres -D /opt/homebrew/var/postgresql@16
```

Six background processes running before any client even connects. SQLite has zero — its "background" work is performed by the calling application's own thread.

---

## 3. Internal Design

### 3.1 File layout

| Aspect              | SQLite                                            | PostgreSQL                                       |
|---------------------|---------------------------------------------------|--------------------------------------------------|
| Database = ?        | **One** file (`app.db`)                            | A **directory** under `$PGDATA/base/<dbid>/`     |
| Tables              | All in the single file                            | One file per relation (heap + each index)        |
| Page size           | **4096 B** (settable 512..65536)                  | **8192 B** (fixed at compile time)               |
| Page count          | `PRAGMA page_count`                                | `pg_relation_size(rel) / 8192`                   |
| File-size formula   | `page_size × page_count`  *(verified in Lab 1)*    | Sum of every relation's file                     |

Lab-1 example, same 100 000-row `users` table loaded into both engines:

| Engine     | Heap size | Pages | Total (incl. indexes) |
|------------|-----------|-------|-----------------------|
| SQLite     | 9.31 MB   | 2 383 | 9.31 MB (one file)    |
| PostgreSQL | 9.38 MB   | 1 173 | 13 MB (heap + 3 idx)  |

Same logical data, half the page count in PG because each page is twice as big. **Indexes** live in the same `.db` file in SQLite; in PG every index has its own file under `base/`.

### 3.2 The B-tree shape

I read both engines' on-disk B-trees during Lab 4:

* SQLite `movies.db` (25 rows × 1100-byte synopsis) → 12 pages including a single **interior page** routing to 9 leaves. Interior cells are `<4-byte child page><varint max-rowid>`. See [`../../Lab4/24BCS10288 Vibhuti Bhatnagar/README.md`](../../Lab4/24BCS10288%20Vibhuti%20Bhatnagar/README.md) for the full byte-level walk.
* PostgreSQL `students` (50 000 rows) → `pg_relation_size('idx_students_city')/8192 ≈ 17` pages, with `bt_page_stats('idx_students_city', 1)` showing the leaf page held 10 entries and 804 bytes of free space.

Same data structure, different framing: SQLite stores **(rowid, payload)** in leaves; PG B-trees store **(key, ctid)** pointing back at a row in the *separate* heap file.

### 3.3 Concurrency

| Question                          | SQLite                                                     | PostgreSQL                                                       |
|-----------------------------------|------------------------------------------------------------|------------------------------------------------------------------|
| Multiple readers                  | ✓ (shared lock on the file)                                | ✓ (each on its own MVCC snapshot)                                |
| Reader + writer simultaneously    | ✓ only in WAL mode; not in default rollback-journal mode   | ✓ always — readers don't block writers, writers don't block readers |
| Multiple writers                  | **No.** One writer at a time, full database lock           | ✓ row-level locks; concurrent writers on different rows fine     |
| Mechanism                         | OS file locks (`flock` / `fcntl`)                          | Per-tuple MVCC (`xmin`/`xmax`) + lightweight + heavy locks       |

The single-writer constraint is the headline limitation of SQLite. It's not a bug — it falls directly out of the embedded design. If two unrelated OS processes were allowed to write the same file concurrently, you'd need shared memory or a lock daemon to coordinate them, and the moment you have either you have a *server* and you're no longer SQLite.

### 3.4 Durability

* **SQLite** has two modes:
  * `journal_mode=delete` (default) → "rollback journal" file holds the original pages before they were overwritten; on commit, the journal is `fsync()`ed then deleted.
  * `journal_mode=wal`  → append-only `app.db-wal` next to the DB. Writers append to the WAL; readers can keep reading the original DB file at the same time. The WAL is periodically *checkpointed* back into the main DB.
* **PostgreSQL** uses WAL exclusively (`pg_wal/` directory). Every change is written to a redo log first; only after the WAL record is `fsync()`ed is the transaction considered committed. The shared buffer pool can lag the WAL — that's fine, because crash recovery replays the WAL.

The mechanism is the same in spirit: write the change to a log, fsync the log, then lazily apply to the data. PG does it system-wide; SQLite does it per database file.

---

## 4. Design Trade-Offs

| Trade-off                  | SQLite picks…                                    | PostgreSQL picks…                              |
|----------------------------|--------------------------------------------------|------------------------------------------------|
| Setup cost                 | **Zero** — no daemon, no users, no config        | A running server, roles, `pg_hba.conf`, ports  |
| Memory footprint           | The library is ~600 KB; no daemon                | 128 MB+ resident for shared buffers + workers  |
| Crash-safety domain        | One file at a time                               | Whole cluster                                  |
| Writes/sec ceiling         | Single-writer; very high read fanout             | Many concurrent writers; lower per-connection ceiling but excellent total throughput |
| Schema extensibility       | Strict SQL with type affinities                  | Rich types, generators, MVCC-aware index types, extensions |
| Query optimisation         | Cost-based but small (no parallel)               | Cost-based + parallel workers + JIT (16+)      |
| Data sharing across procs  | Impossible without IPC outside SQLite            | Free — that's the whole architecture           |

The takeaway is *not* that one is "better" — it's that they're optimising for different cost functions. A phone app for tracking workouts must not require a daemon. A bank ledger must not corrupt under concurrent updates. Each architecture serves one of those constraints cleanly and the other one badly.

---

## 5. Experiments / Observations

### 5.1 Same row count, different engines

From Lab 1 (verified locally — see [`../../README.md`](../../README.md)):

| Metric                | SQLite                   | PostgreSQL                  |
|-----------------------|--------------------------|-----------------------------|
| Page size             | 4096 B                   | 8192 B                       |
| Pages for 100 k users | 2 383                     | 1 173 heap                  |
| File size             | 9.31 MB (one file)       | 9.38 MB heap + 3.6 MB idx   |
| `time SELECT *` (full scan, 1M rows, 166 MB) | ≈ 0.20 s | ≈ 0.10 s (parallel) |
| Process model         | sqlite3 client (~2.4 MB RSS) | 6 daemon procs + per-client backend |

### 5.2 Live multi-table join on PG (`EXPLAIN (ANALYZE, BUFFERS)`)

Captured from `results.txt`:

```
Sort  (cost=5121.92..5121.94 rows=7 width=49) (actual time=19.272..20.951 rows=4 loops=1)
  Buffers: shared hit=2890 read=8
  →  Finalize GroupAggregate
        →  Gather Merge   [Workers Planned: 1, Launched: 1]
              →  Hash Join  (Hash Cond: e.course_id = c.id)
                    →  Hash Join  (Hash Cond: e.student_id = s.id)
                          →  Parallel Seq Scan on enrollments (rows=25000 loops=2)
                          →  Bitmap Index Scan on idx_students_city (city='Bangalore')
                                 Heap Blocks: exact=593
Planning Time: 1.618 ms
Execution Time: 21.087 ms
```

Three observations only PostgreSQL can give you:

1. **Parallel workers.** SQLite cannot split a scan across CPUs; PG's planner does it automatically when the table is big enough.
2. **Buffer hit ratio.** `shared hit=2890 read=8` says 99.7% of the pages were already cached in the buffer pool — exactly what the planner's `effective_cache_size` is supposed to anticipate.
3. **Bitmap index scan.** A small predicate (`city='Bangalore'`, 14% selectivity) gets a *bitmap* scan, not a direct index scan — PG built a bitmap of matching rowids and then did sorted heap fetches. Lower I/O than scanning the heap, less random than chasing one rowid at a time.

### 5.3 SQLite's equivalent

SQLite has no equivalent — there are no workers, no shared buffer telemetry, no bitmap index plans. `EXPLAIN QUERY PLAN` returns a single linear text plan; there is nothing to parallelise across.

---

## 6. Key Learnings

* **One decision cascades into everything.** Choosing "library vs server" determines whether you can have parallel workers, multi-writer ACID, shared buffer pools, autovacuum, hot backups — every one of those is unavailable to an embedded engine because it has no long-running process to host them.
* **SQLite's single-writer constraint is not a limitation, it's the point.** Removing it would require a daemon, which would break the "no setup" promise. The right alternative if you outgrow it is *to switch engines*, not to keep stretching SQLite.
* **PostgreSQL's process overhead is real.** Six daemons plus one process per connection means ~10 MB resident per client. For a 100k-user app this is fine; for a phone app it would be ridiculous. The fork-per-connection model is the cost PG accepts in exchange for isolation between clients.
* **Same algorithm, different framing.** Both engines use B-trees on disk, both serialise commits through a write-ahead log, both keep an in-memory page cache. The interesting difference is *who owns the file and how clients reach it* — that's where the architecture splits.
* **Use SQLite when no two writers will ever fight.** Mobile apps, browser cache, single-machine analytics. Use PostgreSQL when the data outlives any one process — most real applications.

---

## References

- SQLite documentation — *Database File Format* and *Atomic Commit* (https://www.sqlite.org/fileformat.html, https://www.sqlite.org/atomiccommit.html)
- PostgreSQL documentation, ch. 18 *Server Setup and Operation* and ch. 73 *Storage* (https://www.postgresql.org/docs/16/)
- Bruce Momjian, *PostgreSQL Internals through Pictures*, PGCon talks
- My own Lab 1 (PG vs SQLite hands-on) and Lab 4 (SQLite hex dump byte-by-byte walk) in this repo.
