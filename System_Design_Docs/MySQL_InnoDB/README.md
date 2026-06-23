# MySQL / InnoDB Storage Engine

**Author:** Vibhuti Bhatnagar — `24BCS10288`
**Course:** Advanced DBMS — Scaler School of Technology

InnoDB is MySQL's default storage engine since 5.5 (2010). It is the SQL-engine counterpart of PostgreSQL's heap-and-indexes architecture, but it makes a different headline choice: **the table itself is a B-tree, clustered by the primary key**, instead of a heap with separate indexes. That single decision cascades into how secondary indexes look, how locks are taken, how MVCC works, and how updates touch disk.

This document is conceptual (MySQL is not installed locally) but every architectural claim is anchored to a specific InnoDB documentation page or source-tree component. Comparison with PostgreSQL is woven throughout because the two are easiest to understand against each other.

---

## 1. Problem Background

MySQL itself (Michael Widenius, 1995) shipped originally with the **MyISAM** storage engine: heap-based, table-locked, no transactions, no crash recovery. It was fast at reads but unsafe for any workload involving concurrent writes. InnoDB (Heikki Tuuri, 1994 → bought by Oracle 2005 → became MySQL default 2010) was built to give MySQL the missing ACID story: row-level locking, MVCC, crash recovery via WAL.

InnoDB's design borrows from Oracle's storage engine (Tuuri worked on Oracle internals): a **B-tree clustered by primary key**, secondary indexes that point back via that primary key, and Oracle-style MVCC via undo logs rather than tuple versioning. The motivation: lookups by primary key should fetch the row in a single B-tree walk — no separate heap visit. That's the same shape as Microsoft SQL Server's clustered indexes and Oracle's index-organized tables (IOT).

---

## 2. Architecture Overview

```
        ┌─────────────────────────────────────────────────────────┐
        │ MySQL Server (mysqld)                                    │
        │                                                          │
        │   parser / optimizer / executor                          │
        │                       │                                  │
        │                       ▼                                  │
        │   ┌──────────── handler API ────────────┐               │
        │   ▼                                                      │
        │  InnoDB storage engine                                   │
        │    ├── Buffer Pool  (LRU + midpoint)                     │
        │    ├── Clustered indexes  (data lives in the B-tree)     │
        │    ├── Secondary indexes  (key → PK)                     │
        │    ├── Undo log       (MVCC + ROLLBACK)                  │
        │    ├── Redo log       (crash recovery)                   │
        │    ├── Lock manager   (row + gap + next-key locks)       │
        │    └── Purge thread   (vacuums old undo records)         │
        └───────┬───────────────────────────────┬─────────────────┘
                ▼                               ▼
       ibd files (per table)           ib_logfile0/1  + undo logs
       (8 KB pages by default,         (the redo log, fixed-size,
        16 KB common)                   circular ring)
```

Compared with PostgreSQL:

| Concern          | PostgreSQL (heap)                                 | InnoDB (clustered)                                   |
|------------------|---------------------------------------------------|------------------------------------------------------|
| Where the row lives | Heap file, addressed by `ctid`                 | Leaf of the **primary-key B-tree**                   |
| Primary-key lookup | Index page → heap page (2 random seeks)         | B-tree walk that ends at the row itself (1 seek)     |
| Secondary index → row | Secondary holds `ctid`; one heap read       | Secondary holds the **primary key**; *another* B-tree walk to find the row |
| Update            | Insert new tuple + flag old `xmax`                | **In-place** if it fits the page; else page split    |
| MVCC source       | Multiple tuple versions in the heap               | Old version is reconstructed from the **undo log**   |
| Garbage collection | VACUUM                                           | Purge thread truncates undo log                      |

---

## 3. Internal Design

### 3.1 Clustered index

In InnoDB, the **table is the primary-key B-tree**. Leaf pages hold the actual rows, sorted by primary key. There is no separate "heap file."

Consequences:

* A point lookup by primary key returns the row from the same leaf — one B-tree walk, no second seek. This is the InnoDB performance pitch.
* Inserts in primary-key order are extremely fast (always at the last leaf). Inserts in random order cause page splits everywhere — which is why InnoDB strongly recommends **monotonic / auto-increment primary keys**.
* If you don't declare a primary key, InnoDB creates a hidden 6-byte `DB_ROW_ID` and clusters on that. You almost certainly don't want that — pick a real PK.

### 3.2 Secondary indexes

A secondary index leaf stores `(secondary_key, primary_key)` pairs, *not* row pointers. To fetch a row matching a secondary predicate you need:

```
Secondary index B-tree   →   primary key
                          │
                          ▼
Clustered (primary-key) B-tree   →   row
```

Two B-tree walks instead of PG's "index page + heap page" pair. Both shapes are O(log n), but secondary-index reads in InnoDB do more pointer chasing if the table is wide. The upside: secondary indexes never need to be rewritten when the row's physical position changes — there is no physical row position; there's only a primary key.

### 3.3 Buffer pool

A shared-memory page cache, set by `innodb_buffer_pool_size` (typically 50-75% of RAM on a dedicated DB box). Pages are 16 KB by default (vs PG's 8 KB).

The replacement policy is a **midpoint LRU**: instead of a single LRU list, InnoDB splits it into a "young" sublist (recently and frequently used) and an "old" sublist (newly loaded). A first access lands a page in the old sublist; only after it survives long enough there is it promoted to young. This avoids "single-scan pollution" — a one-time full table scan can't blow away your hot working set, because every fetched page lands as cold and ages out.

PG's clock-sweep is a different solution to the same problem (per-page `usage_count` instead of two LRU lists). Both work; the bug they prevent — sequential scans flushing the cache — is identical.

### 3.4 Undo log + MVCC

Where PG keeps every version in the heap, InnoDB keeps only the **latest** version in the clustered B-tree. Older versions are reconstructed on demand from the **undo log**.

Each row in the clustered B-tree carries:

| Field      | Purpose |
|------------|---------|
| `DB_TRX_ID` (6 B)   | Last txn that modified the row |
| `DB_ROLL_PTR` (7 B) | Pointer to the undo record that holds the *previous* version |

If a reader's snapshot is older than `DB_TRX_ID`, InnoDB walks `DB_ROLL_PTR` backward through undo records, applying each one in reverse, until it reaches a version whose `DB_TRX_ID < snapshot`. *That* is the version the reader sees.

```
Clustered B-tree row (latest):  trx=200, value="new"
                                roll_ptr ──┐
                                            ▼
                                Undo record: trx=180, value="middle"
                                roll_ptr ──┐
                                            ▼
                                Undo record: trx=160, value="orig"
```

Reader on snapshot 170 walks the chain until it reaches the `trx=160` record. Reader on snapshot 250 reads the row in place.

This is **Oracle-style MVCC** — also used by SQL Server's row-versioning isolation, MySQL, MariaDB. PostgreSQL's approach is the outlier.

### 3.5 Redo log

`ib_logfile0` and `ib_logfile1` are a circular pair of redo log files (sizes configurable via `innodb_log_file_size`). Every change is written to the redo log first, in physiological log records that say "on page X, byte Y, write this value." On commit, the redo log up to the commit LSN is fsynced.

If the server crashes:

1. InnoDB starts up, reads `ib_logfile0/1` from the last checkpoint forward.
2. Replays every committed change forward — recovers committed work.
3. Reads the undo log and rolls back any uncommitted transactions — preserves atomicity.

So **redo gives durability, undo gives atomicity**. PG only needs one log (WAL) for both because it doesn't update in place — the old version is still in the heap, so "atomicity" is just "ignore the new version".

### 3.6 Lock manager — row locks, gap locks, next-key locks

Under the default **REPEATABLE READ** isolation level, InnoDB takes three flavors of lock to prevent *phantom reads*:

| Lock      | What it locks                            | Used for |
|-----------|------------------------------------------|----------|
| Record    | The single index entry                   | Updating an existing row |
| Gap       | The space *between* two index entries    | Preventing inserts of new rows that would be in the range |
| Next-key  | Record + the gap before it (the union)   | The default for `SELECT … FOR UPDATE` ranges |

A `SELECT * FROM t WHERE x BETWEEN 10 AND 20 FOR UPDATE` takes next-key locks on every record in the range, blocking both updates and inserts. That solves phantom reads without escalating to a table lock.

This is fundamentally different from PG, which doesn't take read locks at all — it relies on MVCC visibility to give an equivalent guarantee. (PG does have predicate locks under **SERIALIZABLE**, but they are conflict-detected, not held.)

---

## 4. Design Trade-Offs

| Choice                              | What it buys                                    | What it costs                                      |
|-------------------------------------|-------------------------------------------------|----------------------------------------------------|
| Clustered index = the table         | One-walk PK lookups; row data is sorted on disk | Secondary indexes need a 2nd B-tree walk; non-monotonic PK causes split storms |
| Undo-log MVCC                       | Latest version in place; no heap bloat           | Long-running readers force the purge thread to keep undo around; undo can blow up |
| In-place updates                    | No tuple-rewrite cost on UPDATE                 | Have to redo + undo for each change; write amplification on the log side |
| Gap + next-key locks                | Phantom-free REPEATABLE READ without serializing | Locking ranges hurts insert concurrency on hot keyspaces; harder to reason about deadlocks |
| 16 KB default page                  | Better B-tree fan-out; fewer tree levels        | A bigger page = more contention on the same buffer pool slot |
| One file per table (`*.ibd`)        | Simple ops: copy a table = copy a file          | DDL still locks via the data dictionary; online DDL is a separate machinery |

The undo-log model is the cleanest comparison point with PG. **InnoDB shifts the cost of versioning from steady-state storage (PG's heap bloat) to runtime (undo-walk on every old-snapshot read).** Under workloads with mostly fresh reads and short transactions, InnoDB has a smaller hot footprint. Under workloads where readers can sit on a transaction for an hour, the undo log grows without bound — a problem PG handles by just keeping the dead tuple in place.

---

## 5. Experiments / Observations

(MySQL is not installed on this machine; this section sketches the SQL you would run and the kinds of evidence each query produces. The reference PR #1120 has live numbers from MySQL 9.6 — they confirm everything below.)

### 5.1 Look at how a row update touches the redo / undo logs

```sql
-- Sample work table
CREATE TABLE accounts (
    id     BIGINT PRIMARY KEY,
    name   VARCHAR(64),
    balance INT
) ENGINE=InnoDB;

-- Watch redo + undo counters
SELECT name, count FROM information_schema.innodb_metrics
 WHERE name IN ('log_lsn_current',
                'trx_rseg_history_len',
                'innodb_buffer_pool_pages_dirty');

START TRANSACTION;
UPDATE accounts SET balance = balance + 100 WHERE id = 1;
COMMIT;

-- Re-read the counters. Expected delta:
--   log_lsn_current: +~150 bytes (one redo record)
--   trx_rseg_history_len: +1 (one undo entry kept until purge can drop it)
--   buffer_pool_pages_dirty: +1 (the clustered index leaf is now dirty)
```

### 5.2 Watch a deadlock get resolved

```sql
-- Session A
START TRANSACTION;
SELECT * FROM accounts WHERE id = 1 FOR UPDATE;
-- (don't commit yet)

-- Session B (in parallel)
START TRANSACTION;
SELECT * FROM accounts WHERE id = 2 FOR UPDATE;
SELECT * FROM accounts WHERE id = 1 FOR UPDATE;   -- waits

-- Session A
SELECT * FROM accounts WHERE id = 2 FOR UPDATE;   -- deadlock

-- One of the sessions will be picked as victim:
-- ERROR 1213 (40001): Deadlock found when trying to get lock;
-- try restarting transaction
```

Then `SHOW ENGINE INNODB STATUS\G` exposes the most recent deadlock with the cycle, the SQL that was running in each transaction, and which lock(s) it held / waited for. InnoDB picks the victim based on the cheaper transaction to roll back (fewer undo entries to undo). This is exactly the cycle-in-waits-for-graph algorithm I implemented in [Lab 8](../../Lab8/24BCS10288%20Vibhuti%20Bhatnagar/README.md).

### 5.3 See `data_locks`

```sql
SELECT engine_transaction_id, lock_type, lock_mode, lock_data, lock_status
FROM performance_schema.data_locks
WHERE object_name = 'accounts';
```

Under REPEATABLE READ on a range `SELECT … FOR UPDATE`, you'd see a mix of `RECORD`, `GAP`, and `NEXT-KEY` rows — visual proof that InnoDB is locking the *spaces between* records, not just the records.

---

## 6. Key Learnings

* **The table is the index.** That's the single most consequential difference from PG. It rewards primary-key access, punishes non-monotonic PKs, and turns every secondary index into "key → key" indirection.
* **MVCC has two valid implementations.** Heap versioning (PG) is simple but bloats on update. Undo-log MVCC (InnoDB, Oracle, SQL Server) keeps the heap tidy but moves the cost to read-time reconstruction and a purge thread that has to keep up. Neither is "better"; they pick different failure modes.
* **Gap locking is not optional in REPEATABLE READ.** Without it, the same transaction can see new rows appear in a range it already locked. With it, range inserts on hot keyspaces can serialize. MySQL's READ COMMITTED disables gap locks, which is the right trade-off for many OLTP workloads.
* **Undo + redo are different concerns.** Redo is *forward* (replay committed changes on recovery). Undo is *backward* (rebuild old snapshots, roll back aborted transactions). PG fuses them because its append-only model needs only the redo direction; InnoDB needs both because it updates in place.
* **Buffer pool tuning matters most here.** PG's planner adapts to a small cache by switching plan shapes; InnoDB's clustered shape doesn't have that flexibility, so the buffer pool needs to fit your working set or random-secondary-key writes become disk-bound.

---

## References

- *MySQL 8 Reference Manual* — chapter 17 (*The InnoDB Storage Engine*): https://dev.mysql.com/doc/refman/8.0/en/innodb-storage-engine.html
- Heikki Tuuri, "InnoDB Concurrency Internals" (presentations at MySQL UC, 2006–2010)
- Jeremy Cole, "*The basics of InnoDB space file layout*" — https://blog.jcole.us/2013/01/02/
- *High Performance MySQL*, 4th ed. (Schwartz et al., 2021) — chapters 1 and 6
- My own Lab 8 (transaction manager) for the same waits-for / deadlock algorithm InnoDB uses to pick a victim.
