# RocksDB Architecture

**Author:** Vibhuti Bhatnagar — `24BCS10288`
**Course:** Advanced DBMS — Scaler School of Technology

RocksDB is an embedded key-value store that uses a **log-structured merge tree** (LSM-tree) instead of a B-tree. It was forked from LevelDB by Facebook in 2012 and tuned for SSDs and write-heavy workloads. Today it backs MyRocks (a MySQL storage engine alternative to InnoDB), CockroachDB, TiDB, Kafka Streams' state stores, and countless other systems where write throughput dominates.

This document explains *why* an LSM-tree exists and what trade-offs it accepts in exchange for its write speed. It is conceptual (no live RocksDB benchmark on this machine) but every architectural claim is grounded in the RocksDB source tree and documentation.

---

## 1. Problem Background

A B-tree is wonderful for reads — `O(log_t n)` random page seeks — but **writes are random by definition**. Inserting a new key means seeking to its B-tree leaf, modifying it (with possible page splits), and `fsync`-ing the result. On a spinning disk, every random seek costs ~10 ms; on an SSD, every random 4 KB write costs an erase-block-rewrite cycle that wears the device down. Workloads dominated by writes (logging, time-series, message queues, write-heavy OLTP) hit the random-write wall hard.

LSM-trees flip the access pattern. Writes go into an in-memory structure (the **MemTable**) and a sequential **WAL**. The MemTable is periodically flushed to disk as an *immutable sorted file* — a **SSTable** — written as a single sequential stream. Reads now have to look in multiple files instead of one B-tree, but the **write path never seeks**.

This is the architectural pitch:

* **B-tree:** O(log n) read, random write, in-place update.
* **LSM:** O(log levels × log entries per file) read, fully sequential write, append-only — compaction does the cleanup asynchronously.

---

## 2. Architecture Overview

```
  Application code
        │
        ▼
  WriteBatch  ───────────►  WAL (sequential append on disk)
        │
        ▼
  MemTable (skiplist, in RAM)
        │
        │  reaches write_buffer_size (default 64 MB)
        ▼
  Immutable MemTable   ───►  background flush thread
                                    │
                                    ▼
                             L0 SSTable file
                                    │
                                    ▼
                             compaction thread merges → L1, L2, …, Ln
                                                          (10× larger each level)

Reads:
   ┌─►  MemTable
   ├─►  Immutable MemTable
   ├─►  L0 SSTables (newest first, may overlap)
   ├─►  L1 → Ln SSTables (non-overlapping in each level; binary search)
   │       └── bloom filter per file skips files that can't contain the key
   ▼
   value (or "not found")
```

Five datatypes that matter:

| Component         | Where it lives | What it holds | Notes |
|-------------------|---------------|---------------|-------|
| WAL               | Disk, sequential | Every write in order | Replayed on restart to rebuild the MemTable |
| MemTable          | RAM (skiplist)   | Recently written keys, sorted | One active per column family |
| Immutable MemTable| RAM              | Same as above but read-only awaiting flush | Lets writes continue while the previous MemTable flushes |
| L0 SSTable        | Disk             | Sorted run, but L0 files may overlap each other | Result of a MemTable flush |
| L1..Ln SSTables   | Disk             | Each level holds a single sorted run (non-overlapping in keys); each ~10× the size of the previous | Result of compaction |

---

## 3. Internal Design

### 3.1 Write path

```
write(key, value):
   1. append (key, value, seqno) to WAL  → fsync once per batch
   2. insert into MemTable skiplist        → O(log MemTable size)
   3. return — no flush, no compaction yet
```

A write commits as soon as step 1 returns. **No disk seeks, no rewrites.** When the MemTable fills (`write_buffer_size`), RocksDB seals it (becomes "immutable") and starts a fresh MemTable; a background thread serialises the immutable one to disk as a sorted L0 file.

### 3.2 Read path

A point lookup walks down the hierarchy:

```
get(key):
   for level in [MemTable, immutable MemTable, L0, L1, ..., Ln]:
       if bloom_filter(level, key) == "definitely not there": continue
       result = lookup(level, key)
       if result: return result
   return NOT_FOUND
```

Every SSTable file has a **bloom filter** built into its block layout. A bloom filter answers "could this file possibly contain key K?" with a small false-positive rate but no false negatives. That's what makes the LSM read path cheap in practice — you only open the files that *might* hold the key.

* L0 files may overlap, so all L0 files must be checked.
* L1..Ln each hold one sorted run with no overlap, so within a level we binary-search for the right file.

If a key has been **deleted**, RocksDB writes a *tombstone* (a special record). Reads stop at the tombstone — the older value is invisible until compaction physically removes it.

### 3.3 Compaction

Compaction is what keeps reads from getting worse over time. It merges files from level `Lk` into `Lk+1`, dropping superseded values (same key, newer wins) and physically removing tombstones whose covered values no longer exist anywhere else. After compaction, level Lk+1 has fewer files than the sum of inputs.

Two strategies ship with RocksDB:

* **Leveled compaction** (default): each level holds one sorted run, ~10× the previous level. Reads are cheap (binary search per level). Writes are more expensive (each key may be rewritten log_n times — high write amplification).
* **Universal compaction**: keep multiple sorted runs at the same level; merge several smaller runs into one larger run when the count exceeds a threshold. Lower write amplification, higher space amplification.

The compaction algorithm is the source of the LSM-tree's three famous amplification numbers.

### 3.4 The three amplifications

| Term                 | Definition |
|----------------------|------------|
| **Write amplification (WA)** | Bytes physically written to disk ÷ bytes the application wrote |
| **Read amplification (RA)**  | Number of disk reads to satisfy one logical read |
| **Space amplification (SA)** | Bytes on disk ÷ bytes of live data |

For leveled compaction with 10× growth factor:

* WA ≈ 1 + 1 + 10 + 10 + 10 + ... ≈ **~20-30**. Each byte the user writes is eventually moved through L0, L1, ..., Ln. The deeper the tree, the more times the same key is rewritten.
* RA ≈ **~10** worst case for a non-bloom-filtered miss across all levels.
* SA ≈ **~1.1** in steady state (most data is in the bottommost level, which is nearly full).

For universal compaction the numbers are roughly reversed: lower WA, higher SA.

A B-tree, for comparison, has WA ≈ 1 (one in-place page write) but does that write *randomly*. The LSM trades 10–30× more bytes written for *sequential* writes, which on SSDs is dramatically faster than random — and gentler on the device.

### 3.5 Bloom filters

A bloom filter is a fixed-size bitmap plus *k* hash functions. To add a key, hash it *k* times and set those *k* bits. To test, hash and check: if any bit is 0, the key is definitely absent. If all *k* are 1, it *might* be present.

RocksDB ships ~10 bits per key by default, yielding ~1 % false-positive rate. That converts most read attempts on the wrong file into a bloom-filter check that doesn't read the file at all — keeping read amplification close to 1 instead of ~10 on a point miss.

### 3.6 Recovery

```
on startup:
   1. open WAL files
   2. replay every WAL record into a fresh MemTable
   3. discover existing SSTable files from the manifest
   4. resume normal service
```

This is the same "WAL → MemTable" path the write code uses; restart is just "all writes since the last flush, applied in order." Faster than B-tree recovery (which has to replay updates that *may* still be in the cache vs definitely on disk) because the WAL boundary is unambiguous.

---

## 4. Design Trade-Offs

| Choice                                | What it buys                                              | What it costs                                            |
|---------------------------------------|-----------------------------------------------------------|----------------------------------------------------------|
| Append-only writes                    | Sequential I/O — massive throughput on SSDs               | Same key written multiple times across compactions       |
| Multiple sorted runs                  | Writes always cheap                                       | Every read may have to consult several files             |
| Tombstones for deletes                | Deletes are O(1) (just append a tombstone)                | Free-space reclamation deferred until compaction         |
| Background compaction                 | Steady-state reads stay efficient                          | Compaction CPU/I/O storms can spike latency              |
| Bloom filter per file                 | Most negative lookups never touch the file                 | Memory cost (10 bits/key ≈ 12 MB for 10M keys)           |
| Leveled vs universal compaction       | Tunable WA/SA balance                                      | One workload's sweet spot is another's worst case        |
| Embedded key-value (no SQL)           | Predictable, low-level, drop-in for storage engines       | Application has to implement schemas, indexes, joins     |

**The clearest comparison:** a B-tree commits a write *quickly* but works *hard* (random seek + possibly split). An LSM commits a write *trivially* but pays *later* (compaction). On a write-heavy workload, the LSM wins because compaction is amortized and runs on background threads; on a read-heavy point-query workload with a hot-key skew, a B-tree (with its single source of truth) can win on tail latency.

---

## 5. Experiments / Observations

(RocksDB is not installed locally; the structure below mirrors how I'd run a benchmark and what each metric would reveal. The reference PR #1120 has live numbers from `librocksdb 11.1.1` confirming this shape.)

### 5.1 Write throughput vs B-tree

A representative `db_bench` invocation:

```bash
db_bench --benchmarks=fillrandom \
         --num=10000000 \
         --value_size=100 \
         --compression_type=none \
         --statistics=1
```

Expected output (order-of-magnitude on a modern SSD):

```
fillrandom :  ~ 1.5  us/op  =  ~600 000 ops/sec     (LSM)
                  (vs.    ~10-30 us/op for a random-write B-tree on the same hardware)
```

The 10–20× gap is the headline LSM win on writes.

### 5.2 The three amplifications under leveled compaction

`statistics=1` exposes RocksDB's internal counters. The interesting ones:

| Counter                                | Meaning |
|---------------------------------------|---------|
| `rocksdb.bytes.written`                | Application bytes |
| `rocksdb.flush.write.bytes` + `rocksdb.compact.write.bytes` | Disk bytes — sum gives WA |
| `rocksdb.number.keys.read`             | Application gets |
| `rocksdb.l0.hit` / `l1.hit` / `l2.hit` | Where each get landed |
| `rocksdb.bloom.filter.useful`          | Bloom filter saved an open |
| `rocksdb.db.get.micros`                | Per-op latency histogram |

Read amplification = (disk reads) / (logical reads). Bloom usefulness is the lever that keeps this near 1 even at deep trees.

### 5.3 Compaction storms

If you watch `compaction_pending` while loading data, you'll see it rise during ingest (L0 fills faster than compactions can drain it), then ramp down as background threads catch up. **Compaction is the latency tax of the LSM** — if you ingest faster than compactions can keep up, RocksDB will throttle writes (write stalls), and you'll see `rocksdb.compaction.times.micros` blow up.

This is the dark side of "writes are cheap" — they're cheap in the short term, but you have to pay the compaction debt eventually.

---

## 6. Key Learnings

* **LSM-trees solve a different problem than B-trees.** Both index sorted data, but B-trees update in place (one random write per change) while LSMs append (cheap up front, compaction debt later). On SSDs the LSM's sequential pattern is the right answer for write-heavy workloads; on read-heavy random-access workloads, the B-tree is usually better.
* **Bloom filters are doing a lot of work.** Without them, a point lookup on a 5-level LSM would touch 5 files per query. With 10-bit bloom filters, ~99 % of "wrong-file" probes are settled by a 64-byte filter check — read amplification collapses to ~1.
* **Compaction is a tax, not a one-time cost.** Steady-state write amplification under leveled compaction is ~20–30×. The application writes 1 GB, the device sees 20–30 GB written. This is fine for SSDs rated at terabytes-of-writes endurance and exactly the wrong fit for spinning disks.
* **There is no free MVCC.** RocksDB uses sequence numbers per write so that reads can return the version at a snapshot — same idea as PostgreSQL's `xmin` or InnoDB's `DB_TRX_ID`. Compaction has to be careful not to drop a value that some open snapshot still needs.
* **The right benchmark depends on the workload.** `fillrandom` favours LSMs; `readrandom` on a small cache favours B-trees. RocksDB's `db_bench` exists so you can run *your* mix on *your* hardware, because there is no single answer.

---

## References

- RocksDB Wiki (the canonical source) — https://github.com/facebook/rocksdb/wiki
- P. O'Neil et al., "*The Log-Structured Merge-Tree (LSM-Tree)*", Acta Informatica 1996 — the original paper.
- Facebook engineering blog: "*RocksDB: a persistent key-value store for fast storage environments*"
- Mark Callaghan, "*Read, write & space amplification — pick 2*" (mysqlatfacebook.com) — the canonical framing of the LSM amplification triangle
- Siying Dong, "*Optimizing space amplification in RocksDB*" — VLDB 2017
