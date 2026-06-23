# PostgreSQL Internal Architecture

**Author:** Vibhuti Bhatnagar — `24BCS10288`
**Course:** Advanced DBMS — Scaler School of Technology

PostgreSQL is a process-per-connection relational database. Every query in this document was run against a local **PostgreSQL 16.13** install with the schema in [`setup.sql`](setup.sql) — 50 000 students × 200 000 enrollments × 7 courses. Every plan, statistic, and `ctid` you'll see is captured in [`results.txt`](results.txt); none of it is invented.

Four subsystems do most of the interesting work, and this document walks each one with evidence:

1. **Buffer manager** — shared memory page cache (Clock-Sweep replacement)
2. **B-tree access method** (`nbtree`) — the default index
3. **MVCC** — concurrent reads/writes without read locks
4. **WAL** — durability + crash recovery + replication source-of-truth

---

## 1. Problem Background

POSTGRES grew out of Michael Stonebraker's 1986 Berkeley project to build a database with first-class extensibility — user-defined types, user-defined operators, abstract data types. The SQL front-end was bolted on later (Postgres95 / PostgreSQL, 1995). Today PostgreSQL is the canonical "no-compromise" open-source RDBMS: it gives up some throughput (relative to MySQL/InnoDB) and some embedded-friendliness (relative to SQLite) in exchange for ACID across many writers, MVCC for non-blocking reads, an extensible type system, and a planner that learns from real statistics.

This write-up is concerned with the *engine* — the storage and concurrency machinery the planner sits on top of.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  client                                                      │
│     │  TCP / unix-socket / libpq                              │
│     ▼                                                        │
│  postmaster (listener)  ─ fork ──►  backend  (one per conn)  │
│                                       │                      │
│   bgwriter ─┐                          ▼                     │
│   checkpointer ─┼──► Shared Buffers (8 KB pages, 128 MB pool)│
│   autovacuum ─┘       ▲          ▲                           │
│   walwriter ─────►   wal records │                           │
│                       │           ▼                          │
│                    pg_wal/    base/<dbid>/  (1 file per      │
│                                              relation,       │
│                                              heap + each idx)│
└─────────────────────────────────────────────────────────────┘
```

A query travels:

```
parser ─► analyzer ─► rewriter ─► planner ─► executor
                                       │
                                       ▼
                              access methods (heap, nbtree, …)
                                       │
                                       ▼
                                 buffer manager
                                       │
                                       ▼
                                disk (heap + idx files)
```

Each access method (heap, B-tree, hash, GIN, GiST, BRIN…) plugs into the executor through the same `IndexAmRoutine` callback table; replacing an index type is in principle a drop-in. The buffer manager sits underneath them all.

---

## 3. Internal Design

### 3.1 Buffer manager (`src/backend/storage/buffer/`)

The cache is a shared-memory array of fixed-size buffer frames (`shared_buffers = 128 MB` by default → 16 384 frames of 8 KB each). Each frame carries:

| Field          | Purpose |
|----------------|---------|
| `tag`          | `(relfilenode, fork, block_number)` — which page does this hold |
| `usage_count`  | 0..5 — bumped on each access, decremented by the clock sweep |
| `refcount`     | "Pin" count — number of callers currently using this frame |
| `flags`        | dirty, valid, IO-in-progress, etc. |

**Replacement policy: Clock Sweep.** When a backend needs a free frame, it advances a global "clock hand" through the array. For each candidate frame: if `refcount > 0` it's pinned — skip; else if `usage_count > 0`, decrement and continue; else evict. This is exactly the algorithm I implemented in [Lab 3](../../Lab3/24BCS10288%20Vibhuti%20Bhatnagar/README.md). The `BM_MAX_USAGE_COUNT = 5` cap ensures even very hot pages cannot dominate the pool forever.

**Live evidence from `results.txt`:**

```
        rel        | pages_in_buffers
-------------------+------------------
 enrollments       |             1671
 students          |              597
 idx_students_city |               10
 idx_enr_student   |                6
 courses           |                1
```

After running the join, the entire `enrollments` heap (1667 pages) and the whole `students` heap (593 pages) are resident — 18 MB of working set fits comfortably in the 128 MB pool. The EXPLAIN `Buffers: shared hit=2890 read=8` lines confirm only 8 pages had to be fetched from disk; everything else came from the cache.

### 3.2 B-tree (`src/backend/access/nbtree/`)

PG B-trees are **Lehman-Yao** (a non-blocking variant) with two-pass deletion. Each index page is 8 KB and holds either pointers down to children (interior) or pointers into the heap (leaf, as `ctid = (block, offset)`).

The `pageinspect` extension lets us look at a real page:

```sql
SELECT * FROM bt_page_stats('idx_students_city', 1);
 blkno | type | live_items | dead_items | avg_item_size | page_size | free_size | btpo_prev | btpo_next | btpo_level | btpo_flags
   1   |  l   |     10     |      0     |      730      |    8192   |    804    |     0     |     2     |      0     |      1
```

* `type = 'l'` → leaf
* `btpo_level = 0` → bottom of the tree
* 10 entries fitting in ~7 KB (each `(city_text, ctid)` averages 730 bytes here because the 7 distinct city values are repeated)
* `free_size = 804 B` → near full; next insert may trigger a split

The single-level B-tree above the heap is enough because city has only 7 distinct values; an index on a unique column would be 3 levels deep over 50 000 rows. Lab 6 ([`../../Lab6/`](../../Lab6/24BCS10288%20Vibhuti%20Bhatnagar/README.md)) implements this from scratch — same data structure, same minimum-degree invariants.

### 3.3 MVCC (`src/backend/access/heap/`)

This is the most distinctive thing about PostgreSQL. Every heap tuple stores two transaction IDs in its header:

| Field    | Meaning |
|----------|---------|
| `xmin`   | Txn id that **created** this tuple |
| `xmax`   | Txn id that **deleted/updated** it, 0 if still live |
| `ctid`   | Physical row location `(page, offset)`; changes on UPDATE because PG doesn't update in place |

Visibility rule for a snapshot S (simplified):
> A tuple is visible iff `xmin < S` *and* (`xmax = 0` *or* `xmax > S`).

I proved this is what really happens by `UPDATE`-ing a row and watching its `ctid` and `xmin` change:

```
BEGIN;
SELECT id, name, xmin, xmax, ctid FROM students WHERE id = 1;
 id |   name    | xmin | xmax | ctid
----+-----------+------+------+------
  1 | Student_1 |  755 |  757 | (0,1)

UPDATE students SET cgpa = cgpa + 0.01 WHERE id = 1;
SELECT id, name, xmin, xmax, ctid FROM students WHERE id = 1;
 id |   name    | xmin | xmax |   ctid
----+-----------+------+------+----------
  1 | Student_1 |  766 |    0 | (592,16)
ROLLBACK;
```

The old tuple is *still on disk* at `(0, 1)` — and its `xmax` was set to 757 (the rollback consumed an id). The new version was written at `(592, 16)` with `xmin = 766`. **Updates are append-only in PG's heap.** Any concurrent reader running on a snapshot ≤ 766 keeps seeing the old tuple at `(0, 1)`; readers on a snapshot > 766 see the new tuple. No reader ever takes a row lock to do this.

That's why **VACUUM** is necessary: dead tuples (the ones with `xmax < oldest active snapshot`) pile up forever otherwise. `autovacuum` is the daemon that reclaims them by rewriting the heap pages without those tuples and updating the free-space map. I implemented exactly this `gc()` step in [Lab 8](../../Lab8/24BCS10288%20Vibhuti%20Bhatnagar/README.md) — same `xmin`/`xmax` accounting, same "oldest snapshot" floor.

### 3.4 WAL (`src/backend/access/transam/`)

Every change goes through the write-ahead log before it touches the data files. The unit is a WAL record (a typed `xlog` entry: insert, update, commit, checkpoint, …). At commit:

1. The backend appends its WAL records to a shared in-memory ring buffer.
2. `pg_xact_commit` writes a commit record.
3. `XLogFlush` calls `fsync()` on `pg_wal/000000010000000000000016` (or wherever the LSN landed).
4. Only after that `fsync()` returns is the client told "COMMIT OK".

The heap pages can stay dirty in the buffer pool for *minutes* after the commit; the checkpointer flushes them asynchronously. If the machine crashes in between, recovery replays the WAL from the most recent checkpoint LSN forward. Durability is bound to the WAL `fsync`, not to the data file write.

Live evidence:

```
SELECT pg_current_wal_lsn();
 pg_current_wal_lsn
--------------------
 0/16A9DCA8
```

The LSN moves monotonically with every commit. The same byte stream feeds streaming replication, point-in-time recovery, and logical decoding.

### 3.5 Planner & statistics

`ANALYZE` populates `pg_statistic` (visible through the `pg_stats` view). For our `students` table:

```
 attname | n_distinct | null_frac | avg_width | n_mcv
---------+------------+-----------+-----------+-------
 id      |         -1 |         0 |         8 |
 name    |         -1 |         0 |        13 |
 email   |         -1 |         0 |        23 |
 city    |          7 |         0 |         7 |     7
 cgpa    |        400 |         0 |         6 |     4
 joined  |       1000 |         0 |         4 |     6
```

* `n_distinct = -1` on `id`, `name`, `email` → "every value distinct" (cardinality scales with row count). This drives PG to expect a unique-index lookup to return 1 row.
* `n_distinct = 7` on `city` → only 7 different cities; the planner kept all 7 in the **most-common-values** list (`n_mcv = 7`). That's why our query `WHERE city = 'Bangalore'` got a *Bitmap Index Scan* (good for 14% selectivity) instead of a *Seq Scan* (preferred at >25% selectivity) or *Index Scan* (preferred at <1%).
* The MCV plus a histogram on continuous columns is what lets the planner say things like "`cgpa BETWEEN 8.0 AND 9.0` selects ≈25% of rows" without ever touching the table.

---

## 4. Design Trade-Offs

| Choice                                    | What it buys                                            | What it costs                                                |
|-------------------------------------------|---------------------------------------------------------|--------------------------------------------------------------|
| Process-per-connection (fork model)       | Isolation: backend crash can't corrupt others           | ~10 MB per connection; PG slows above ~500 connections (use pgBouncer) |
| MVCC with version-on-write                | Readers never wait                                       | Every update is an insert; needs VACUUM forever              |
| Heap + separate indexes                   | Multiple secondary indexes on the same heap             | Indexes carry their own ctids; every UPDATE bloats indexes (mitigated by HOT) |
| WAL fsync on every commit                 | Crash-safety = LSN, not file flush                       | Synchronous commit serialises behind disk IOPS               |
| Cost-based planner with `pg_statistic`    | Picks bitmap / index / seq scan correctly per query     | Bad stats → bad plan; `ANALYZE` must keep up                 |
| 8 KB pages, no clustered index            | Heap and indexes are independent — no clustered-key dependency on read paths | Random heap access from secondary index can be slow (mitigated by BRIN, fillfactor) |

The single most distinctive design choice is the MVCC-via-version-on-write approach. It makes readers extremely cheap (the dominant workload) at the price of forever-needing-VACUUM. Other engines (InnoDB, Oracle) chose undo-log MVCC instead, which doesn't bloat the heap but does require an undo segment large enough to support the longest open transaction.

---

## 5. Experiments / Observations

### 5.1 `EXPLAIN (ANALYZE, BUFFERS)` on a 3-table join

The full plan is in `results.txt`. Tree-flattened:

```
Sort  (actual 19.272..20.951 ms, rows=4)
  Buffers: hit=2890 read=8
└── Finalize GroupAggregate
    └── Gather Merge   [Workers Planned: 1, Launched: 1]
        └── Sort (per worker)
            └── Partial HashAggregate
                └── Hash Join  (Hash Cond: e.course_id = c.id)
                    ├── Hash Join  (Hash Cond: e.student_id = s.id)
                    │   ├── Parallel Seq Scan on enrollments
                    │   │      (filter term='2025-FALL', rows=25 000 loops=2)
                    │   └── Hash → Bitmap Heap Scan on students
                    │              └── Bitmap Index Scan on idx_students_city
                    └── Hash → Seq Scan on courses (7 rows)

Planning: 1.618 ms   Execution: 21.087 ms
```

What this tells us about the planner:

1. **It picked a bitmap index scan for `city = 'Bangalore'`** — not a plain index scan. Bitmap is the sweet spot when ~14 % of rows match: random heap access would be slower than buffering rowids and doing a sorted heap walk.
2. **It parallelised the larger scan but not the smaller ones.** `Parallel Seq Scan` on `enrollments` (1 667 pages) split across 2 workers; `Seq Scan on courses` (1 page) stayed serial. The cost model has a startup cost for workers; below a threshold parallelism isn't worth it.
3. **Hash join was chosen over merge join.** Builds a hash on the smaller side (`students` filtered) and probes the bigger side (`enrollments`) — works because we have no indexes that match the join order.
4. **`Buffers: hit=2890 read=8` is the cache-utilization smoking gun.** The actual disk I/O was 8 pages = 64 KB. Without `pg_buffercache` you'd be guessing about cache effectiveness; with it, "did we hit the cache?" becomes a metric.

### 5.2 Tuple physical layout

`pg_relation_size('students') = 4 744 KB / 8 KB = 593 pages.`
50 000 tuples ÷ 593 pages = ~84 tuples per page = ~100 B per tuple (matches the schema: 8 B id + ~30 B varlen strings + ~10 B header).

### 5.3 What VACUUM actually does

After 1 000 UPDATE statements I checked the chain:

```
SELECT id, xmin, xmax, ctid FROM students WHERE id = 1;
```

The visible row's `ctid` had moved several pages forward; the *previous* versions still lived at the original page. `VACUUM students;` then rewrote those pages, freeing space and reducing the relation back to its original size. Without VACUUM, the heap grows monotonically.

---

## 6. Key Learnings

* **Visibility is a tuple property, not a lock.** That's the entire reason a long reporting query doesn't block writers.
* **Updates aren't updates.** They are inserts + a flag-flip on the old tuple. VACUUM (or autovacuum) is the cost of that decision.
* **Replication is just WAL shipping.** Streaming replicas, logical decoding, point-in-time recovery — all of them read the same `pg_wal/` records the primary writes for crash safety. Durability and replication are the same mechanism, reused.
* **The planner is data-driven.** Without `ANALYZE`, the cost model falls back to defaults and picks bad plans. With it, you can usually trust the bitmap-vs-index-vs-seq choice it makes — verify with `EXPLAIN ANALYZE` rather than guessing.
* **The buffer manager is shared, not per-backend.** That means hot pages benefit *every* connection, and tools like `pg_buffercache` reveal what the working set actually looks like.
* **Clock sweep + MVCC + WAL + cost-based planning are not independent features.** They're four sides of the same architectural decision: trade *space* (extra tuple versions, WAL log, statistics) for *time* (cheap reads, fast crash recovery, smart plans).

---

## References

- PostgreSQL source tree — `src/backend/storage/buffer/freelist.c`, `src/backend/access/nbtree/`, `src/backend/access/heap/heapam.c`, `src/backend/access/transam/xlog.c`
- "PostgreSQL 14 Internals" (Egor Rogov, 2022) — the most accessible internals walkthrough
- Documentation chapters 14 (planner), 17 (server configuration), 73 (physical storage)
- My own Lab 3 (Clock-Sweep), Lab 6 (B-Tree from scratch), and Lab 8 (MVCC + 2PL transaction manager) in this repo — each prototypes one of the subsystems described above.
