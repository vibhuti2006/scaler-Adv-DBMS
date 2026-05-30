# Lab 6 — B-Tree (C++17)

**Name:** Vibhuti Bhatnagar
**Role Number:** 24BCS10288
**Course:** Advanced DBMS — Scaler School of Technology

A header-only templated B-tree (`adbms::b_tree<Key, Value, Compare>`) parametrised by minimum degree `t`. The implementation follows CLRS — chapter 18 for insert (proactive split on the way down) and chapter 18.3 for delete (predecessor / successor / merge cases). A built-in `verify()` checks every B-tree invariant after every mutation, and a 5 000-operation randomised stress test confirms the tree agrees with `std::map` on the same workload.

The motivation for studying this tree in a DBMS course is direct: the table B-trees we read byte-by-byte in Lab 4 are exactly this shape, just stored on disk one node per page. SQLite, PostgreSQL, MySQL/InnoDB and almost every other relational engine use a B-tree (or its B+ variant) as the on-disk index structure for the same reason — short tree height keeps page reads bounded.

---

## Files

```
Lab6/24BCS10288 Vibhuti Bhatnagar/
├── b_tree.hpp        # header-only templated b_tree<Key, Value, Compare>
├── main.cpp          # 6-scenario demo + 5 000-op stress test
├── CMakeLists.txt    # C++17 build with -Wall -Wextra -Wpedantic -Wshadow
└── README.md         # this file
```

## Build & run

```bash
# CMake
cmake -S . -B build && cmake --build build && ./build/b_tree_demo

# one-liner
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o b_tree_demo && ./b_tree_demo
```

Tested with Apple clang 21.0 on macOS arm64. Zero warnings. Demo exits with status 0 and prints `All B-Tree checks passed.`

---

## 1. Why B-trees (and not the red-black tree from Lab 5)?

Both keep a balanced ordered map in `O(log n)`. The difference is the **branching factor**. A red-black tree is a *binary* search tree — every node holds exactly one key and has at most two children. A B-tree of minimum degree `t` lets every node hold up to `2t − 1` keys and `2t` children.

That has two consequences a database cares about:

1. **Shorter trees.** With `t = 50`, every node fans out into up to 100 children, so a tree over `n` keys has height at most `⌈log_t((n+1)/2)⌉`. For a million keys that's about **4 levels** of tree. The red-black tree from Lab 5 over the same million keys is ~40 levels deep.
2. **Page-aligned nodes.** Each B-tree node is sized to fit one disk page (4 KB in SQLite, 8 KB in PostgreSQL). Loading a single page from disk is the dominant cost of a database lookup; if one page covers 100+ keys, the lookup terminates in 4 page reads even on cold cache.

The trade-off is per-node work: scanning ≤ 99 keys inside a node is more code than comparing the one key in a binary node. But that scan runs over contiguous cache lines, so in practice it's free relative to the page fetch it saves.

---

## 2. The five invariants

For a B-tree of minimum degree `t ≥ 2`:

1. Every node holds between `t − 1` and `2t − 1` keys. (The **root** is the exception — it may hold just 1 key.)
2. Every internal node with `k` keys has exactly `k + 1` children.
3. Keys within a node are stored in strictly increasing order.
4. For an internal node with keys `K₁ < K₂ < … < K_k` and children `C₀, C₁, …, C_k`:
   * All keys in `C₀` are `< K₁`.
   * For `0 < i < k`, all keys in `C_i` lie in `(K_i, K_{i+1})`.
   * All keys in `C_k` are `> K_k`.
5. **All leaves are at the same depth.** This is the balance property — every root-to-leaf walk takes the same number of steps.

`b_tree::verify()` walks the tree once and returns the first violation it finds (empty string if healthy). The demo driver calls it after every mutation; if any code path ever breaks an invariant, the run aborts immediately with the violating message.

---

## 3. Insert — proactive split (CLRS 18.2)

Every new key ends up in a **leaf**. The interesting question is how we keep the path from root to that leaf free of overflowed nodes.

The trick is **splitting on the way down**: as we descend, if the next child we're about to enter has `2t − 1` keys (it's full), we split it *before* descending. That guarantees that whichever node finally absorbs the new key has at least one free slot — no upward rebalancing pass is ever needed.

```
split_child(parent, i):
  parent before:   [... K_{i-1}  K_i  K_{i+1} ...]
                            ↓
  child y (full):  [k_0 k_1 ... k_{t-1} ... k_{2t-2}]      (2t-1 keys)

  split:
    median = y->keys[t-1]                    // promoted up
    z := new Node carrying y->keys[t..2t-2]  // upper half
    y    keeps        y->keys[0..t-2]        // lower half
    (children are split the same way if y is internal)

  parent after:    [... K_{i-1}  median  K_i  K_{i+1} ...]
                          ↓                  ↓
                          y                  z
```

The **root** is the only node allowed to have fewer than `t − 1` keys. It's also the only place tree height ever grows: when an insert finds the root itself full, we create a fresh empty root, hand it the old root as its sole child, and split. That's how a B-tree gets taller — top-down, by exactly one level at a time.

---

## 4. Search

Within a single node, scan the keys left-to-right until you find one that is `≥ k`:

* equal → hit, return `(node, slot)`,
* strictly greater → recurse into the child to its left,

If we walk off the end of a leaf without a hit, the key isn't in the tree. Cost: one node touched per level (`O(log_t n)` of them), with at most `2t − 1` comparisons per node.

The implementation uses a small helper `lower_bound_in(n, k)` that returns the first slot `i` where `n->keys[i] ≥ k`. The same helper is reused by insert and erase.

---

## 5. Erase — the part the reference skipped (CLRS 18.3)

Erase is the harder operation. The hard part is keeping the *minimum* invariant alive: as we descend, we must guarantee every node we enter has ≥ `t` keys (so that even after we take one away, it still has ≥ `t − 1`). The CLRS algorithm splits into six cases.

### Cases when the target key is in the current node `n`

| Case | Trigger                                                | Action |
|------|--------------------------------------------------------|--------|
| 1    | `n` is a leaf                                           | Delete the key directly. |
| 2a   | `n` is internal; **left** child has ≥ `t` keys           | Pop its **predecessor**, replace the key, recurse to delete the popped one. |
| 2b   | `n` is internal; **right** child has ≥ `t` keys          | Pop its **successor**, replace the key, recurse to delete the popped one. |
| 2c   | Both children minimal (`t − 1` keys each)               | Merge them with `n`'s separator dropped between, then recurse into the merged node (which now has `2t − 1` keys). |

### Cases when the target key is below `n` (we must descend)

| Case | Trigger                                          | Action |
|------|--------------------------------------------------|--------|
| 3a-left  | Target child minimal, left sibling has ≥ `t` keys  | **Borrow** — rotate one key from the left sibling up through the parent and down into the child. |
| 3a-right | Target child minimal, right sibling has ≥ `t` keys | Mirror borrow from the right sibling. |
| 3b   | Target child and both adjacent siblings minimal   | **Merge** the child with a sibling, pulling the appropriate parent separator down. |

The function `ensure_min_t(parent, i)` packages cases 3a / 3b into one call. The recursive descent calls it before walking into a minimal child, so by the time we are *inside* a non-root node, that node is guaranteed to have ≥ `t` keys.

### One subtle landmine

In case 2c, after merging children `[i]` and `[i+1]` together with the separator, the original key we wanted to delete lives at slot `t − 1` of the merged node. The naive thing — `erase_from(left, n->keys[i])` — reads the *next* separator after the merge has already shifted things, so it deletes the wrong key. The implementation captures `left->keys[t_-1]` before recursing instead:

```cpp
merge_children(n, i);
Key target = left->keys[t_-1];
return erase_from(left, target);
```

I caught this exact bug on the first run of the stress test — the tree and `std::map` started disagreeing on `size()` around iteration 30. Worth keeping the test in.

### Root collapse

After erase, the root may end up with **zero** keys (its only key was merged into a child). If so, we drop a level: the root's sole child becomes the new root. This is the only way tree height ever **decreases**, mirroring the root-split that grows it.

---

## 6. Public API

```cpp
adbms::b_tree<int, std::string> idx(/*t=*/3);

idx.insert(42, "movie");          // true on first insert, false on overwrite
idx.contains(42);                  // O(log_t n) descent
idx.at(42);                        // throws std::out_of_range if missing
idx.erase(42);                     // true if removed

idx.size(); idx.empty(); idx.degree();
idx.in_order([](int k, const std::string& v){ ... });  // sorted (key, value) walk
idx.print(std::cout);               // indented debug picture
auto err = idx.verify();            // empty string = healthy
```

---

## 7. Demo (what `./b_tree_demo` runs)

1. **`t=3`, 10 movies** — small enough to print the tree shape. Demonstrates insert + in-order traversal returning sorted output.
2. **Lookups + overwrite** — `contains`, `at`, `out_of_range` on miss, and the overwrite path (which must not change `size()`).
3. **Erase covering all cases** — 5 deletions hitting leaf-delete, internal-with-predecessor, internal-with-merge, and an internal-key whose deletion collapses the root.
4. **`t=2` sequential insert** — inserts `1..16` into a 2-3-4 tree. The worst case for a plain BST (which would build a 16-deep right-leaning chain); this stays a 4-level bush.
5. **`t=50` × 50 000 random inserts** — confirms tree height stays small at high `t`. Spot-check on the first 1 000 inserted keys hits 1000/1000.
6. **Stress test** — 5 000 random insert/erase operations driven by a fixed-seed `std::mt19937` against `std::map<int,int>` as the oracle. After every step the demo asserts:
   * `size()` matches,
   * `contains(k)` matches `oracle.count(k) > 0`,
   * `verify()` returns empty.
   At the end, the in-order traversal must match `std::map`'s iteration order.

Tail of a clean run:

```
=== 6) Randomised stress test — 5 000 ops vs std::map oracle ===
  passed: 147 keys live, invariants hold, oracle agreed on every step.

All B-Tree checks passed.
```

---

## 8. Complexity

| Operation              | Time              | Worst-case nodes touched |
|------------------------|-------------------|--------------------------|
| `insert`               | `O(t · log_t n)`  | one per level + at most one split per level |
| `contains` / `at`      | `O(t · log_t n)`  | one per level             |
| `erase`                | `O(t · log_t n)`  | one per level (plus at most one borrow / merge per level) |
| `in_order` traversal   | `O(n)`             | every node                |
| `verify`               | `O(n)`             | every node                |

The `t` factor inside the log is the linear scan within a node. For the disk-resident use case (`t` around `100..200`), this scan happens on a single page held in cache after the read — it's effectively free. What matters is the `log_t n` factor: the **number of pages fetched**.

---

## 9. How this implementation differs from the reference (PR #551)

| Aspect | Reference (PR #551) | This submission |
|---|---|---|
| API | `int` set, key-only | Templated `b_tree<Key, Value, Compare>` — an ordered map |
| **Erase** | Not implemented | Implemented (CLRS chapter 18.3 — six cases) |
| Layout | Single `btree.cpp` | Header-only `b_tree.hpp` + separate `main.cpp` |
| Build | Plain `g++` one-liner | `CMakeLists.txt` with `-Wshadow` |
| Stress test | One spot-check on demo 3 | 5 000-op random workload + `std::map` oracle |
| Bug discovery | n/a | Stress test caught a case-2c off-by-one (documented in §5) |

The insert algorithm itself is the same CLRS algorithm in both files — there's no alternative B-tree insert worth implementing. The originality lies in the surface, the erase implementation, and the test scaffolding.

---

## 10. Quick command reference

```bash
# Build + run (CMake)
cmake -S . -B build && cmake --build build && ./build/b_tree_demo

# Build + run (plain compiler)
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o b_tree_demo && ./b_tree_demo

# Expected last line:  All B-Tree checks passed.
```
