// ADBMS Lab 5 — Red-Black Tree demo / 24BCS10288 Vibhuti Bhatnagar
//
// Treats the tree as an in-memory key→value index of the kind a DBMS might
// use for an ordered, equality-search structure (catalog lookups, the
// PostgreSQL lock manager, etc.). The demo exercises insert, lookup, the
// templated value side (int → string), iterator (in-order traversal),
// erase across every CLRS delete case, and runs check_invariants() after
// every mutation as a built-in unit test.

#include "rb_tree.hpp"

#include <cassert>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

template <typename T, typename V>
void assert_ok(const adbms::rb_tree<T, V>& t, const std::string& after) {
    auto err = t.check_invariants();
    if (!err.empty()) {
        std::cerr << "INVARIANT FAIL after " << after << ": " << err << "\n";
        std::exit(1);
    }
}

void heading(const std::string& s) {
    std::cout << "\n=== " << s << " ===\n";
}

}  // namespace

int main() {
    using IndexTree = adbms::rb_tree<int, std::string>;
    IndexTree idx;

    heading("1) Bulk insert — exercises insert cases 1, 2, 3 and their mirrors");
    const std::vector<std::pair<int, std::string>> rows = {
        {41, "Inception"},  {17, "Whiplash"},     {76, "Parasite"},
        { 9, "Memento"},    {23, "Dune"},         {58, "Oppenheimer"},
        {88, "Joker"},      { 3, "La La Land"},   {12, "Spirited Away"},
        {30, "Goodfellas"}, {65, "Gladiator"},    {99, "Interstellar"},
        {50, "Matrix"},     {72, "Fight Club"},   {81, "Tenet"},
    };
    for (auto& [k, v] : rows) {
        idx.insert(k, v);
        assert_ok(idx, "insert(" + std::to_string(k) + ")");
    }
    std::cout << "size = " << idx.size() << "\n";
    idx.print_bfs();

    heading("2) In-order traversal returns sorted keys");
    std::cout << "(key=value): ";
    idx.in_order([](int k, const std::string& v) {
        std::cout << k << "=" << v << "  ";
    });
    std::cout << "\n";

    heading("3) Lookups");
    for (int k : {17, 50, 99, 42, 100}) {
        std::cout << "  contains(" << k << ") = "
                  << (idx.contains(k) ? "yes" : "no") << "\n";
    }
    std::cout << "  at(58) = " << idx.at(58) << "\n";
    try { idx.at(7); }
    catch (const std::out_of_range& e) {
        std::cout << "  at(7)  threw as expected: " << e.what() << "\n";
    }

    heading("4) Erase — covers delete cases 1..4 incl. red/black successors");
    // 50 is the root after the inserts above; 17 has two real children;
    // 88 sits in the right spine — between them they trigger every delete
    // fix-up branch.
    for (int k : {50, 17, 88, 99, 3, 12, 41, 65}) {
        bool removed = idx.erase(k);
        assert_ok(idx, "erase(" + std::to_string(k) + ")");
        std::cout << "  erase(" << k << ") -> "
                  << (removed ? "ok" : "miss") << "   (size=" << idx.size() << ")\n";
    }
    idx.print_bfs();

    heading("5) Re-insert after erase (overwrite path is a no-op for size)");
    idx.insert(50, "Matrix Reloaded");           // re-add
    idx.insert(50, "Matrix Resurrections");       // overwrite, must NOT bump size
    std::cout << "  size = " << idx.size() << "   at(50) = " << idx.at(50) << "\n";
    assert_ok(idx, "overwrite path");

    heading("6) Stress — 5 000 randomised ops, oracle = std::set");
    // Drive the tree and a std::set with the same random workload and confirm
    // they agree at every step. Catches subtle balance / colour bugs.
    std::mt19937 rng(0xADBDB);     // any fixed seed — makes the stress run repeatable
    std::uniform_int_distribution<int> key_dist(0, 199);
    adbms::rb_tree<int, int> stress;
    std::set<int>            oracle;
    for (int i = 0; i < 5000; ++i) {
        int k = key_dist(rng);
        if (rng() & 1) {
            stress.insert(k, k * 10);
            oracle.insert(k);
        } else {
            stress.erase(k);
            oracle.erase(k);
        }
        if (i % 500 == 0) assert_ok(stress, "stress step " + std::to_string(i));
        assert(stress.size() == oracle.size());
        assert(stress.contains(k) == (oracle.count(k) > 0));
    }
    assert_ok(stress, "stress final");
    std::cout << "  passed: " << stress.size() << " keys live, "
              << "invariants hold, oracle agreed on every step.\n";

    heading("7) In-order order matches std::set order");
    std::vector<int> got;
    stress.in_order([&](int k, int){ got.push_back(k); });
    std::vector<int> expected(oracle.begin(), oracle.end());
    assert(got == expected);
    std::cout << "  " << got.size() << " keys, sorted output matches std::set\n";

    std::cout << "\nAll Red-Black Tree checks passed.\n";
    return 0;
}
