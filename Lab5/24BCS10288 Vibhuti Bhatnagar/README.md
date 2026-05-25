# Lab 5 — Red-Black Tree (C++17)

**Name:** Vibhuti Bhatnagar
**Role Number:** 24BCS10288
**Course:** Advanced DBMS — Scaler School of Technology

A header-only, templated red-black tree (`adbms::rb_tree<Key, Value, Compare>`) plus a demo driver that exercises every CLRS insert and delete case, an in-order iterator, and a 5 000-operation stress test that compares the tree against `std::set` as an oracle.

The algorithm is standard CLRS chapter 13. The non-standard parts are the **template surface** (it's an ordered *map*, not a set), the **iterative in-order traversal**, and a built-in **`check_invariants()`** that any test can call after a mutation to prove the tree is still well-formed.

---

## Files

```
Lab5/24BCS10288 Vibhuti Bhatnagar/
├── rb_tree.hpp       # header-only templated rb_tree<Key, Value, Compare>
├── main.cpp          # 7-scenario demo + 5 000-op randomised stress test
├── CMakeLists.txt    # C++17 build, -Wall -Wextra -Wpedantic -Wshadow -Wconversion
└── README.md         # this file
```

## Build & run

### CMake

```bash
cmake -S . -B build
cmake --build build
./build/rb_tree_demo
```

### One-liner

```bash
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o rb_tree_demo
./rb_tree_demo
```

Tested with Apple clang 21.0 on macOS arm64. Zero warnings; demo exits with status 0 and prints `All Red-Black Tree checks passed.`

---

## 1. What a red-black tree is — five invariants

A red-black tree is a binary search tree that stays balanced by giving every node a *colour* (RED or BLACK) and enforcing five properties:

1. Every node is RED or BLACK.
2. The root is BLACK.
3. Every leaf (the shared NIL sentinel) is BLACK.
4. **No RED node has a RED child.** (Two reds in a row would shorten one path relative to its neighbours.)
5. **Every path from a given node to any descendant NIL contains the same number of black nodes.** (This number is the *black-height* of that subtree.)

From (4) and (5) it follows that the longest root-to-leaf path is at most twice the shortest. That gives us the guarantee that matters:

> All operations — `insert`, `contains`, `erase` — are **O(log n)** worst-case.

Compare with a plain BST, which can degrade to `O(n)` on sorted input.

### Why a DBMS cares

A DBMS uses red-black or AVL trees in many places where a B-tree would be overkill: PostgreSQL's lock manager uses RB-trees to track waiting lockers (`src/backend/storage/lmgr/`), the planner uses them inside path-cost containers, and most C++ in-memory indexes (`std::map`, `std::set`) are red-black trees under the hood. They're the workhorse balanced-BST when persistence and disk-page layout don't matter.

---

## 2. Insert — three cases (mirrored)

A new key is BST-inserted as a RED leaf. RED is chosen because it preserves property 5 (black-height) immediately; the only thing that can break is property 4 (no two reds adjacent).

`fix_after_insert(z)` walks up from `z`, looking at the **uncle** (the sibling of `z`'s parent):

| Case | Trigger                          | Action |
|------|----------------------------------|--------|
| 1    | Parent and uncle are both RED    | Recolour both BLACK, grandparent RED, recurse on grandparent. |
| 2    | Uncle BLACK, `z` is the *zigzag* grandchild | Rotate around parent to convert into Case 3. |
| 3    | Uncle BLACK, `z` is the *outer*  grandchild | Recolour parent BLACK + grandparent RED, then rotate around the grandparent. |

After the loop, the root is forced BLACK (property 2). Each iteration moves `z` two levels up, so the fix-up is `O(log n)`.

The implementation appears in `rb_tree.hpp` (`fix_after_insert`) — left- and right-leaning branches are written out symmetrically rather than abstracted, because the symmetry argument is the easiest way to convince yourself the mirror works.

---

## 3. Erase — four cases (mirrored)

Erase is the harder operation. We do the standard BST splice first:

* If the doomed node `z` has at most one real child, *graft* that child in its place.
* If `z` has two real children, swap it with its in-order successor `y` (the leftmost node of its right subtree), then splice `y` out.

The variable `gone_colour` records the colour of whichever node was actually removed. If a RED node was removed, no black-height was changed and we're done. If a BLACK node was removed, a "double-black" deficit appears at the slot it used to occupy (`hole`). `fix_after_erase(hole)` removes that deficit by examining `hole`'s **sibling** `s`:

| Case | Trigger                          | Action |
|------|----------------------------------|--------|
| 1    | Sibling RED                       | Rotate to make sibling BLACK; reduces to one of cases 2–4. |
| 2    | Sibling BLACK with two BLACK kids | Push the deficit up by recolouring sibling RED; move `hole` to parent. |
| 3    | Sibling BLACK, sibling's *outer* nephew BLACK, *inner* nephew RED | Rotate sibling to convert into Case 4. |
| 4    | Sibling BLACK, *outer* nephew RED  | Final fix-up: recolour and rotate around parent. Terminates the loop. |

Sentinel detail worth noting: in the two-real-children branch we set `hole->parent = spliced` even when `hole` is the NIL sentinel, because the fix-up needs to read `hole->parent->{left,right}`. After the fix-up returns, the implementation resets `nil_->parent = nil_` so the sentinel is once again a true no-op leaf — important if anyone else then traverses the tree.

---

## 4. Rotations

Rotations are the only mechanism for restructuring the tree. They are local: only six pointers move, and the BST in-order property is preserved.

```
rotate_left(x):              rotate_right(y):
       x              y             y              x
      / \   becomes  / \           / \   becomes  / \
     a   y          x   c         x   c          a   y
        / \        / \           / \                / \
       b   c      a   b         a   b              b   c
```

`graft(u, v)` is a separate helper that just replaces `u` in its parent's pointer slot with `v` — used by `erase` to splice out nodes without thinking about rotations.

---

## 5. Public API

```cpp
adbms::rb_tree<int, std::string> idx;

idx.insert(42, "movie");        // returns true on first insert, false on overwrite
idx.contains(42);               // O(log n)
idx.at(42);                     // throws std::out_of_range if absent
idx.erase(42);                  // returns true if removed
idx.size();
idx.empty();

idx.in_order([](int k, const std::string& v) { ... });  // ascending key order
idx.print_bfs(std::cout);                                // level-order debug dump

auto err = idx.check_invariants();                       // "" when healthy
```

`in_order()` is implemented with an explicit stack, so it works on very deep trees that would blow a recursion-based traversal.

---

## 6. The demo (scenarios in `main.cpp`)

1. **Bulk insert** of 15 movie titles — exercises all three insert cases plus mirrors.
2. **In-order traversal** — confirms `(3, 9, 12, 17, 23, 30, 41, …)` is emitted sorted.
3. **Lookups** — positive, negative, exception path from `at()`.
4. **Erase** of 8 keys including the root, a two-real-children node, and a near-leaf — covers every delete case.
5. **Overwrite** — re-inserting an existing key updates the value and does not change `size()`.
6. **Random stress test (5 000 ops)** — driven by a fixed-seed `std::mt19937`. The tree and a `std::set` see identical operations; the demo asserts that `size()` and `contains()` agree at every step and that the invariant checker stays clean.
7. **In-order vs std::set** — verifies the iterator emits the same ordering as the oracle.

Sample tail of the run:

```
=== 6) Stress — 5 000 randomised ops, oracle = std::set ===
  passed: 98 keys live, invariants hold, oracle agreed on every step.

=== 7) In-order order matches std::set order ===
  98 keys, sorted output matches std::set

All Red-Black Tree checks passed.
```

---

## 7. `check_invariants()` — the cheap-but-thorough test harness

Rather than write external tests, the tree ships with an O(n) verifier that checks all five RB properties plus the BST ordering and returns a description of the first violation it sees:

* Property 2 — root must be BLACK.
* Property 4 — no RED node has a RED child.
* Property 5 — left and right subtrees of every node have equal black-height.
* BST ordering — `lo < key < hi` carried down through the recursion.

The demo calls it after **every** mutation. If a bug ever sneaks into a future edit of `fix_after_insert` or `fix_after_erase`, the next stress iteration will print `INVARIANT FAIL after …` and exit with status 1.

---

## 8. Complexity

| Operation              | Time         | Space (extra)             |
|------------------------|--------------|---------------------------|
| `insert`               | O(log n)     | O(1) — one new node       |
| `contains` / `at`      | O(log n)     | O(1)                      |
| `erase`                | O(log n)     | O(1)                      |
| `in_order` traversal   | O(n)         | O(log n) explicit stack   |
| `check_invariants`     | O(n)         | O(log n)                  |
| construction           | O(1)         | O(1) — sentinel only      |
| destruction            | O(n)         | O(log n) recursion        |

All `log n` factors are with constant ≤ 2× the height of a perfectly balanced tree (property 4 ⇒ no path is more than twice the length of any other), so in practice insert/erase touch fewer than `2 log₂ n` nodes plus a constant number of rotations.

---

## 9. What's different from a textbook (and from the reference PR)

* **Generic key/value/comparator.** The tree is `rb_tree<Key, Value, Compare>` rather than `int → bool`, so it can serve as a real in-memory index. The reference is a key-only `int` set.
* **Sentinel-mutation safety net.** `erase` resets `nil_->parent = nil_` after fix-up so the sentinel never carries leftover state from the previous call. Subtle, but the reason a careless rewrite of `erase` can corrupt the next traversal.
* **Iterative in-order.** The traversal uses `std::stack<Node*>` so it does not recurse `O(n)` deep on a worst-case tree.
* **Invariant verifier inside the class.** Available to the test harness via a clean public method instead of being an external function.
* **CMake build** with `-Wshadow -Wconversion` rather than a plain `Makefile` — catches a wider class of mistakes (variable shadowing, narrowing conversions) at compile time.

The CLRS algorithm itself, however, is the algorithm. There aren't ten "different" red-black trees — there's one, and either you implement it correctly or you don't. The originality here is in the surface, the test scaffolding, and the verification approach.

---

## 10. Quick command reference

```bash
# Build + run (CMake)
cmake -S . -B build && cmake --build build && ./build/rb_tree_demo

# Build + run (plain compiler)
clang++ -std=c++17 -Wall -Wextra -Wpedantic -O2 main.cpp -o rb_tree_demo && ./rb_tree_demo

# Expected: ends with
#   All Red-Black Tree checks passed.
```
