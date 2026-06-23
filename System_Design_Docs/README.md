# System Design Docs — Advanced DBMS

**Author:** Vibhuti Bhatnagar — `24BCS10288`
**Course:** Advanced DBMS — Scaler School of Technology

Architecture studies of four real database engines. Each subfolder has a self-contained `README.md` covering the six required sections (problem background, architecture overview, internal design, design trade-offs, experiments, key learnings), plus the SQL scripts and captured output used to write it.

| Topic | Engine(s) | Focus |
|---|---|---|
| [PostgreSQL_vs_SQLite](./PostgreSQL_vs_SQLite/) | PostgreSQL 16.13 + SQLite 3.51.0 | Client-server vs embedded — how one architectural choice cascades through file layout, concurrency, durability |
| [PostgreSQL_Internals](./PostgreSQL_Internals/) | PostgreSQL 16.13 | Buffer manager, B-tree, MVCC (`xmin`/`xmax`/`ctid`), WAL, planner + statistics |
| [MySQL_InnoDB](./MySQL_InnoDB/) | InnoDB (conceptual) | Clustered index = table; secondary-key indirection; undo-log MVCC; gap + next-key locks |
| [RocksDB](./RocksDB/) | RocksDB (conceptual) | LSM-tree; compaction; bloom filters; write/read/space amplification |

## Method

All PostgreSQL-side numbers are **measured locally** — every `EXPLAIN ANALYZE` plan, every `pg_stats` row, every `ctid` shift on UPDATE was captured from a live 16.13 install with the schema in [`PostgreSQL_Internals/setup.sql`](./PostgreSQL_Internals/setup.sql) (50 000 students × 200 000 enrollments). The raw query output is in [`PostgreSQL_Internals/results.txt`](./PostgreSQL_Internals/results.txt). SQLite numbers are taken from my own [Lab 1](../README.md) and [Lab 4](../Lab4/24BCS10288%20Vibhuti%20Bhatnagar/README.md) hands-on runs.

MySQL/InnoDB and RocksDB are not installed on this machine; their write-ups are conceptual but every claim is grounded in a specific source-tree component or a documented behavior, with comparative analysis against PostgreSQL throughout.

## Cross-cutting theme

Reading the four engines side-by-side, one pattern keeps recurring:

> **Every interesting database design is a trade choice in the same triangle.**
>
> *Cheap reads*, *cheap writes*, *cheap reclamation* — you can have any two, never all three.

* PostgreSQL: cheap reads (MVCC, no read locks), cheap writes (append a new tuple) — but expensive reclamation (VACUUM).
* InnoDB: cheap reads (one B-tree walk), cheap reclamation (no heap bloat) — but more expensive writes (in-place update + undo log).
* RocksDB / LSM: cheap writes (sequential append), cheap reclamation (compaction merges everything) — but read paths must consult many files, mitigated by bloom filters.
* SQLite: cheap reads (single file, no IPC), cheap writes (per-file lock) — but no multi-writer concurrency at all.

Each engine picks a different corner of that triangle. The discipline in this study was to figure out *which corner* each engine occupies and *why* they made that choice.

## Connection to my lab work

This write-up reuses data from earlier labs in this repository:

| Lab | What it built | Used where |
|-----|---------------|-----------|
| [Lab 1](../README.md) | Hands-on PG + SQLite install with page-size/timing/mmap experiments | Cross-checked in `PostgreSQL_vs_SQLite` |
| [Lab 3](../Lab3/24BCS10288%20Vibhuti%20Bhatnagar/README.md) | Clock-sweep buffer pool from scratch | Cited in `PostgreSQL_Internals` §3.1 |
| [Lab 4](../Lab4/24BCS10288%20Vibhuti%20Bhatnagar/README.md) | Byte-level walk of a SQLite file | Cited in `PostgreSQL_vs_SQLite` §3.2 |
| [Lab 6](../Lab6/24BCS10288%20Vibhuti%20Bhatnagar/README.md) | CLRS B-tree with insert + erase | Cited in `PostgreSQL_Internals` §3.2 |
| [Lab 8](../Lab8/24BCS10288%20Vibhuti%20Bhatnagar/README.md) | MVCC + 2PL transaction manager | Cited in `PostgreSQL_Internals` §3.3 and `MySQL_InnoDB` §5.2 |

The labs and the design docs are two halves of the same coursework — the labs build a tiny version of each subsystem, the docs explain how PostgreSQL/InnoDB/RocksDB do it for real.
