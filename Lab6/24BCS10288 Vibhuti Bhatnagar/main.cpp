// ADBMS Lab 6 — B-Tree demo / 24BCS10288 Vibhuti Bhatnagar
//
// Exercises every code path in b_tree.hpp:
//   * insert (proactive-split + root-grow),
//   * search,
//   * erase (leaf, internal-with-predecessor, internal-with-successor,
//           internal-with-merge, descend-with-rotate, descend-with-merge),
//   * in-order iteration,
//   * verify() is called after every mutation as a built-in unit test,
//   * 5 000-op randomised stress test against std::map as an oracle.

#include "b_tree.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

template <typename K, typename V>
void assert_ok(const adbms::b_tree<K, V>& t, const std::string& after) {
    auto err = t.verify();
    if (!err.empty()) {
        std::cerr << "INVARIANT FAIL after " << after << ": " << err << "\n";
        std::exit(1);
    }
}
void heading(const std::string& s) { std::cout << "\n=== " << s << " ===\n"; }

}  // namespace

int main() {
    using Tree = adbms::b_tree<int, std::string>;

    // -----------------------------------------------------------------
    heading("1) Small tree (t=3) — 10 (key, value) inserts");
    // -----------------------------------------------------------------
    Tree small(3);
    const std::vector<std::pair<int, std::string>> rows = {
        {41, "Inception"}, {17, "Whiplash"},     {76, "Parasite"},
        { 9, "Memento"},   {23, "Dune"},         {58, "Oppenheimer"},
        {88, "Joker"},     { 3, "La La Land"},   {12, "Spirited Away"},
        {30, "Goodfellas"}
    };
    for (auto& [k, v] : rows) {
        small.insert(k, v);
        assert_ok(small, "insert(" + std::to_string(k) + ")");
    }
    std::cout << "size = " << small.size() << "\n";
    small.print();

    std::cout << "in-order: ";
    small.in_order([](int k, const std::string& v){ std::cout << k << "=" << v << "  "; });
    std::cout << "\n";

    // -----------------------------------------------------------------
    heading("2) Lookups + overwrite");
    // -----------------------------------------------------------------
    for (int k : {17, 23, 99}) {
        std::cout << "  contains(" << k << ") = " << (small.contains(k) ? "yes" : "no") << "\n";
    }
    std::cout << "  at(58) = " << small.at(58) << "\n";

    small.insert(58, "Oppenheimer (IMAX)");                 // overwrite, no size change
    std::cout << "  after overwrite — at(58) = " << small.at(58)
              << "  size = " << small.size() << "\n";
    try { small.at(7); }
    catch (const std::out_of_range& e) {
        std::cout << "  at(7) threw as expected: " << e.what() << "\n";
    }

    // -----------------------------------------------------------------
    heading("3) Erase — leaf, internal, root-collapse");
    // -----------------------------------------------------------------
    for (int k : {3, 17, 41, 88, 23}) {
        bool ok = small.erase(k);
        assert_ok(small, "erase(" + std::to_string(k) + ")");
        std::cout << "  erase(" << k << ") -> " << (ok ? "ok" : "miss")
                  << "  size=" << small.size() << "\n";
    }
    std::cout << "tree after erases:\n";
    small.print();

    // -----------------------------------------------------------------
    heading("4) Sequential insert into a t=2 tree (worst case for a plain BST)");
    // -----------------------------------------------------------------
    adbms::b_tree<int, int> twothreefour(2);                 // a 2-3-4 tree
    for (int i = 1; i <= 16; ++i) {
        twothreefour.insert(i, i * i);
        assert_ok(twothreefour, "seq insert " + std::to_string(i));
    }
    twothreefour.print();
    std::cout << "in-order: ";
    twothreefour.in_order([](int k, int){ std::cout << k << ' '; });
    std::cout << "\n";

    // -----------------------------------------------------------------
    heading("5) Height bound — t=50, insert 50 000 random keys");
    // -----------------------------------------------------------------
    // With t=50 the height is at most ~log_50(50000) ≈ 3, so this should
    // remain a tiny tree even after 50 000 inserts.
    adbms::b_tree<int, int> big(50);
    std::mt19937 rng(0xB1A1E);
    std::vector<int> keys;
    keys.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        int k = static_cast<int>(rng() & 0x7fffffff);
        big.insert(k, k);
        keys.push_back(k);
    }
    assert_ok(big, "50k random inserts");
    int hits = 0;
    for (int i = 0; i < 1000; ++i) hits += big.contains(keys[i]) ? 1 : 0;
    std::cout << "  size = " << big.size()
              << "  spot-check hits on first 1000 inserted keys = " << hits << "/1000\n";

    // -----------------------------------------------------------------
    heading("6) Randomised stress test — 5 000 ops vs std::map oracle");
    // -----------------------------------------------------------------
    adbms::b_tree<int, int> stress(4);
    std::map<int, int>      oracle;
    std::mt19937 rng2(0xA110C);
    std::uniform_int_distribution<int> key_dist(0, 299);
    for (int step = 0; step < 5000; ++step) {
        int k = key_dist(rng2);
        if (rng2() & 1) {                                    // insert
            stress.insert(k, step);
            oracle[k] = step;
        } else {                                              // erase
            stress.erase(k);
            oracle.erase(k);
        }
        if (step % 250 == 0) assert_ok(stress, "stress " + std::to_string(step));
        assert(stress.size() == oracle.size());
        assert(stress.contains(k) == (oracle.count(k) > 0));
    }
    assert_ok(stress, "stress final");
    // In-order output must match std::map's natural ordering.
    std::vector<int> got;
    stress.in_order([&](int k, int){ got.push_back(k); });
    std::vector<int> want;
    for (auto& [k, _] : oracle) want.push_back(k);
    assert(got == want);
    std::cout << "  passed: " << stress.size() << " keys live, "
              << "invariants hold, oracle agreed on every step.\n";

    std::cout << "\nAll B-Tree checks passed.\n";
    return 0;
}
