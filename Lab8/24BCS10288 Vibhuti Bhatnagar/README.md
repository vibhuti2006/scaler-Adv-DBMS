# Lab 8 — In-Memory Transaction Manager (MVCC + 2PL)

**Name:** Vibhuti Bhatnagar
**Role Number:** 24BCS10288
**Course:** Advanced DBMS — Scaler School of Technology

A header-only C++17 implementation of an in-memory transaction manager that combines five concurrency-control primitives found in a real relational engine:

* **MVCC snapshot reads** — every transaction reads against the snapshot it captured at `begin()`. Readers never block and never take locks.
* **Strict 2PL writes** — writers take an exclusive lock on each key they touch and hold it until commit or abort.
* **Deadlock detection** — every blocked write walks the waits-for graph; cycles abort the youngest transaction.
* **First-updater-wins** — a commit is rejected with `SerializationFailure` if any of its written keys received a newer committed version while it was running.
* **`gc()`** — prunes dead versions that no live or future snapshot can ever observe.

This is the same split PostgreSQL uses: reads ride the MVCC visibility check (`xmin`/`xmax` timestamps), writes ride row-level locks.

The simulator is single-threaded. A blocked write returns `LockWait` and the caller decides what to do — usually retry once the holder commits or aborts. That makes the deadlock and serialization scenarios deterministic and easy to assert on.

---

## Files

```
Lab8/24BCS10288 Vibhuti Bhatnagar/
├── txn_manager.hpp     # header-only namespace adbms::txn — full implementation
├── main.cpp            # 6-scenario bank-account demo with hard assertions
├── CMakeLists.txt      # C++17 build, -Wall -Wextra -Wpedantic -Wshadow
└── README.md           # this file
```

## Build & run

```bash
# CMake
cmake -S . -B build && cmake --build build && ./build/txn_manager_demo

# one-liner
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o txn_manager_demo && ./txn_manager_demo
```

Tested on macOS arm64 with Apple clang 21.0 — zero warnings, exit 0, final line `All transaction-manager checks passed.`

---

## 1. Why MVCC for reads but 2PL for writes?

The two pieces of state we are protecting have very different access patterns:

* **Reads vastly outnumber writes**, especially in OLTP. If readers took locks they would constantly bump into writers, and the read path is the latency-sensitive one. MVCC lets each transaction see a *frozen* view of the data — there is nothing to block on.
* **Concurrent writes to the same row must be serialised**. Otherwise either the data-page bytes corrupt (last writer wins) or invariants get broken (think a balance going negative because both transfers debited at once). A short-held exclusive lock per key is the cheapest way to get correctness here.

Snapshot isolation alone would not prevent two transactions from blindly overwriting each other on the same key, which is why we add **first-updater-wins** on top: when a writer reaches commit, we re-check that the latest committed version of each written key is the one it expected.

---

## 2. Version layout

Every key maps to a *version chain* — a `vector<Version>` sorted by the commit timestamp at which the version came into existence (`xmin`). Each `Version` records:

| Field        | Meaning |
|--------------|---------|
| `value`      | The actual cell payload (`std::string`). |
| `tombstone`  | True if this version represents a `delete`. |
| `xmin`       | Commit timestamp at which the version became live. |
| `xmax`       | Commit timestamp at which the version was *superseded* (a later write or delete). `0` means still live. |
| `creator`    | Id of the txn that produced the version (for tracing). |

Visibility rule, applied during a read against snapshot `S`:

> The version is visible iff `xmin ≤ S  AND  (xmax == 0  OR  xmax > S)`.

That single inequality is the entire MVCC read path — no locking, no waiting. Reads scan the chain newest-to-oldest and return the first match.

---

## 3. Lock manager + deadlock

The lock state lives in two small maps:

```cpp
xlock_     : key       → holder_txn_id     // who owns the X-lock right now
waits_for_ : waiter_id → holder_txn_id     // who I'm currently waiting on
```

`try_acquire(t, key)`:

1. If nobody holds the lock → grant immediately, record in `xlock_` and `t.locks`.
2. If `t` itself already holds it → reentrant, succeed.
3. Otherwise add `waits_for_[t.id] = holder`. Then **`detect_cycle()`** runs a DFS along `waits_for_` starting from `t.id`. If it lands back on `t.id`, that's a deadlock.
4. On cycle: pick the txn with the **highest id** (= youngest) from the cycle as the victim. Abort it. If the victim happens to be us → return `Aborted`. Otherwise retry the lock acquisition; the freshly-released lock is now ours.
5. No cycle → return `LockWait`.

Holding the youngest-as-victim rule has two nice properties: (a) the older transaction is generally the one that has done more work, so we save its progress, and (b) the victim is well-defined, so the test in scenario 4 can assert on `last_victim()` exactly.

When a transaction commits or aborts, `release_all_locks` drops both its `xlock_` entries and any `waits_for_` edges pointing **to** it — those waiters can now retry on their next call.

---

## 4. Commit: first-updater-wins

Before installing any new versions, `commit()` re-scans every key the txn wrote and looks at the *latest* version's `xmin`. If `xmin > my snapshot`, somebody else committed first — we are about to overwrite their work blindly, which would violate snapshot isolation. We abort with `SerializationFailure`.

If the check passes, we:

1. Allocate a fresh `commit_ts = ++clock_`.
2. For each written key, set the *current* head's `xmax = commit_ts` (it has been superseded) and push a new version with `xmin = commit_ts, xmax = 0`.
3. Release locks. Mark the txn `Committed`.

This is exactly how PostgreSQL's `xid`/`xmax` book-keeping models row visibility — the commit timestamp is the linchpin that orders both the "before" and "after" views.

---

## 5. `gc()` — vacuum

Dead versions (those with `xmax != 0`) are still needed as long as some live snapshot might see them. Once the oldest snapshot has moved past their `xmax`, they are garbage.

```
gc():
    low = min(snapshot of every active txn,  default = clock_)
    for each (key, chain):
        keep only versions where  xmax == 0  OR  xmax > low
```

`oldest_snapshot()` defaults to the current `clock_` when no txn is live: nothing future-spawned can have a snapshot older than `clock_`, so every dead version is safely reclaimable.

The demo dumps `alice`'s chain before and after the vacuum to make the prune visible:

```
alice chain before gc: [xmin=1 xmax=2 1000] [xmin=2 xmax=10 1500]
                       [xmin=10 xmax=11 1400] [xmin=11 xmax=0 2000]
versions: 9 -> 3  (pruned 6)
alice chain after  gc: [xmin=11 xmax=0 2000]
```

---

## 6. API

```cpp
adbms::txn::Manager m;

txn_id_t t  = m.begin();                       // snapshot taken now
std::string v;
m.read(t, "alice", v);                          // MVCC against t's snapshot
m.write(t, "alice", "1500");                    // 2PL acquires lock
m.remove(t, "carol");                           // tombstone
auto r = m.commit(t);                           // Ok / SerializationFailure / Aborted
m.abort(t);                                     // release locks, drop buffered writes

m.gc();                                          // prune dead versions

// introspection
m.state_of(t);          m.last_victim();
m.live_txn_count();     m.lock_count();
m.version_count();      m.dump_chain("alice");
m.check_invariants();   // "" when internal data structures are healthy
```

All return values use `enum class Result { Ok, NotFound, LockWait, Aborted, SerializationFailure }`. The demo defines `to_string(Result)` and `to_string(State)` for pretty-printing.

---

## 7. Demo scenarios (bank accounts)

The driver seeds three accounts (alice=1000, bob=500, carol=750) and then runs six scenarios. Each is gated by `check(condition, "what")`. If any assertion fails the run aborts with a `[FAIL]` line.

| # | Scenario | What it proves |
|---|----------|----------------|
| 1 | A reader takes a snapshot of `alice=1000`. A separate transaction commits `alice=1500`. The reader still sees `1000`; a fresh reader sees `1500`. | MVCC snapshot isolation. |
| 2 | Older snapshot taken before `delete carol`; new snapshot after. Old still sees the row, new sees `<none>`. | Tombstone visibility. |
| 3 | T1 holds the lock on `bob`; T2's write returns `LOCK_WAIT`. After T1 aborts, T2 retries and commits. | Strict 2PL: one X-lock at a time. |
| 4 | T_AB locks `alice`, T_BA locks `bob`. They then try to grab each other's account → cycle. The younger txn (`T_BA`) is auto-aborted. T_AB commits cleanly. | Waits-for cycle detection picks the right victim. |
| 5 | Two txns both snapshot `alice = 1400`. T1 writes `2000`, T2's write blocks. T1 commits. T2 retries the write (now sees lock free), takes it, but its **commit** is rejected with `SerializationFailure`. | First-updater-wins on the commit path. |
| 6 | After scenarios 1–5 the chain for `alice` has 4 versions. With no live snapshots, `gc()` reclaims all but the live one. | Vacuum behaves correctly under the visibility floor. |

Every scenario also calls `check_invariants()` afterwards, which verifies that:

* Every X-lock holder is an Active transaction.
* Every version chain has strictly-increasing `xmin`.
* Every superseded version has `xmin < xmax`.
* Only the *last* version per chain may have `xmax = 0`.

If any of those is violated, the run exits with `INVARIANT FAIL after …`.

---

## 8. Complexity (n = keys touched, v = versions in a chain, t = txns)

| Operation       | Time                              | Notes |
|-----------------|-----------------------------------|-------|
| `begin()`       | `O(1)`                            | Just bumps an id and snapshots `clock_`. |
| `read(key)`     | Amortized `O(1)` map + `O(v)` scan of chain | Latest version usually visible first. |
| `write(key)`    | `O(1)` lock-table + `O(t)` deadlock DFS in the worst case | DFS path is bounded by `t`. |
| `commit()`      | `O(n + sum of chains touched)`    | First-updater-wins re-checks `n` keys; appends new versions. |
| `abort()`       | `O(n)`                             | Just releases locks and drops buffered writes. |
| `gc()`          | `O(total versions)`               | One pass; in-place rebuild of each chain. |

---

## 9. How this implementation differs from PR #793

| Aspect | Reference (#793) | This submission |
|---|---|---|
| Layout | `.h` + `.cc` + `main.cc` | Header-only `.hpp` + `main.cpp` |
| Namespace | `mvcc` | `adbms::txn` |
| Build | `Makefile` | `CMakeLists.txt` with `-Wshadow` |
| Demo | 5 scenarios (generic keys `x`, `y`, `a`, `b`, `z`) | 6 scenarios on bank accounts (`alice`, `bob`, `carol`) — adds a tombstone-visibility test |
| Built-in invariant checker | — | `check_invariants()` validates lock-holder liveness + chain order; called between every scenario |
| Chain dump | — | `dump_chain(key)` prints the version timeline for debugging |
| Stats footer | — | Final `live_txns / locks / versions` line so test harnesses can verify cleanup |

The algorithms themselves — MVCC visibility, 2PL lock acquisition, waits-for cycle detection, first-updater-wins, garbage collection — are the textbook implementations and there isn't a wildly different correct version of any of them. The originality is in the API surface, the test scaffolding, and the introspection methods I leaned on while debugging.

---

## 10. Quick command reference

```bash
# CMake build + run
cmake -S . -B build && cmake --build build && ./build/txn_manager_demo

# Or the one-liner
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o txn_manager_demo && ./txn_manager_demo

# Expected final line:  All transaction-manager checks passed.
```
