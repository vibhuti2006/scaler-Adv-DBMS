// main.cpp — ADBMS Lab 8 demo / 24BCS10288 Vibhuti Bhatnagar
//
// Drives the transaction manager through six scenarios drawn from the
// classic bank-accounts setting, each verified by an explicit assertion:
//
//   1. MVCC snapshot isolation — a long-running reader keeps seeing the
//      pre-transfer balance even after a writer commits.
//   2. Tombstone visibility   — deleted keys disappear for new snapshots
//      while older snapshots still see them.
//   3. Strict 2PL              — a second writer on the same account blocks
//      with LOCK_WAIT until the holder releases.
//   4. Deadlock detection      — cross-account transfers (A→B and B→A) form
//      a waits-for cycle; the youngest txn is the victim.
//   5. First-updater-wins      — two transactions both read the same
//      balance, only the first commit lands; the second hits
//      SERIALIZATION_FAILURE.
//   6. gc()                    — once the long-running readers finish, dead
//      versions get reclaimed; latest live values survive.
//
// Every step also calls Manager::check_invariants() to confirm internal
// data structures remain consistent.

#include "txn_manager.hpp"

#include <cassert>
#include <iostream>
#include <string>

using adbms::txn::Manager;
using adbms::txn::Result;
using adbms::txn::State;
using adbms::txn::txn_id_t;

namespace {

void section(const std::string& s) { std::cout << "\n=== " << s << " ===\n"; }

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [pass] " : "  [FAIL] ") << what << "\n";
    if (!ok) std::exit(1);
}
void ok(const Manager& m, const std::string& after) {
    auto e = m.check_invariants();
    if (!e.empty()) {
        std::cerr << "INVARIANT FAIL after " << after << ": " << e << "\n";
        std::exit(1);
    }
}

// Read helper that returns "<none>" when the key is absent / deleted.
std::string rd(Manager& m, txn_id_t tx, const std::string& key) {
    std::string v;
    Result r = m.read(tx, key, v);
    return r == Result::Ok ? v : std::string("<none>");
}

}  // namespace

int main() {
    Manager bank;

    // --------------------------------------------------------------------
    section("0) Seed alice=1000, bob=500, carol=750");
    // --------------------------------------------------------------------
    {
        txn_id_t s = bank.begin();
        bank.write(s, "alice",  "1000");
        bank.write(s, "bob",     "500");
        bank.write(s, "carol",   "750");
        check(bank.commit(s) == Result::Ok, "seed committed");
        ok(bank, "seed commit");
    }

    // --------------------------------------------------------------------
    section("1) MVCC snapshot isolation — readers see their own snapshot");
    // --------------------------------------------------------------------
    {
        txn_id_t reader = bank.begin();                  // sees alice=1000
        check(rd(bank, reader, "alice") == "1000", "reader snapshot sees 1000");

        txn_id_t writer = bank.begin();
        check(bank.write(writer, "alice", "1500") == Result::Ok, "writer locks alice");
        check(bank.commit(writer) == Result::Ok, "writer commits alice=1500");

        // Reader still sees its snapshot — the new committed version is
        // invisible because the reader's snapshot timestamp is older.
        check(rd(bank, reader, "alice") == "1000", "reader still sees alice=1000");

        txn_id_t fresh = bank.begin();
        check(rd(bank, fresh, "alice") == "1500", "fresh reader sees alice=1500");
        bank.commit(reader);
        bank.commit(fresh);
        ok(bank, "mvcc scenario");
    }

    // --------------------------------------------------------------------
    section("2) Tombstones — delete is visible to new readers only");
    // --------------------------------------------------------------------
    {
        txn_id_t older  = bank.begin();                  // sees carol=750
        check(rd(bank, older, "carol") == "750", "older reader sees carol=750");

        txn_id_t closer = bank.begin();
        check(bank.remove(closer, "carol") == Result::Ok, "delete carol");
        check(bank.commit(closer) == Result::Ok, "delete committed");

        check(rd(bank, older, "carol") == "750", "old snapshot still sees carol");
        txn_id_t fresh = bank.begin();
        check(rd(bank, fresh, "carol") == "<none>", "new snapshot sees carol as gone");
        bank.commit(older);
        bank.commit(fresh);
        ok(bank, "tombstone scenario");
    }

    // --------------------------------------------------------------------
    section("3) Strict 2PL — second writer blocks until first finishes");
    // --------------------------------------------------------------------
    {
        txn_id_t t1 = bank.begin();
        txn_id_t t2 = bank.begin();
        check(bank.write(t1, "bob", "600") == Result::Ok, "T1 takes X-lock on bob");
        check(bank.write(t2, "bob", "700") == Result::LockWait, "T2 blocks -> LOCK_WAIT");

        bank.abort(t1);                                  // release lock
        check(bank.state_of(t1) == State::Aborted, "T1 aborted -> released lock");
        check(bank.write(t2, "bob", "700") == Result::Ok, "T2 retries -> Ok");
        check(bank.commit(t2) == Result::Ok, "T2 commits bob=700");

        txn_id_t v = bank.begin();
        check(rd(bank, v, "bob") == "700", "bob = 700");
        bank.commit(v);
        ok(bank, "2PL scenario");
    }

    // --------------------------------------------------------------------
    section("4) Deadlock — A->B and B->A transfers form a cycle");
    // --------------------------------------------------------------------
    {
        txn_id_t alice_to_bob = bank.begin();
        txn_id_t bob_to_alice = bank.begin();

        // First each transaction grabs the X-lock on the account it's
        // debiting; that part is fine because the locks don't overlap.
        check(bank.write(alice_to_bob, "alice", "1400") == Result::Ok, "T_AB locks alice");
        check(bank.write(bob_to_alice, "bob",   "650")  == Result::Ok, "T_BA locks bob");

        // Now they try to lock the *other* account → cycle.
        check(bank.write(alice_to_bob, "bob",   "750")  == Result::LockWait,
              "T_AB waits for bob");
        Result r = bank.write(bob_to_alice, "alice", "1600");
        std::cout << "  T_BA wants alice -> " << to_string(r)
                  << "; victim = T" << bank.last_victim() << "\n";
        check(r == Result::Aborted, "younger txn is the deadlock victim");
        check(bank.last_victim() == bob_to_alice, "victim is T_BA (higher id)");
        check(bank.state_of(bob_to_alice) == State::Aborted, "T_BA aborted");

        // T_AB can now claim bob's lock and commit.
        check(bank.write(alice_to_bob, "bob", "750") == Result::Ok, "T_AB grabs bob");
        check(bank.commit(alice_to_bob) == Result::Ok, "T_AB commits");
        ok(bank, "deadlock scenario");
    }

    // --------------------------------------------------------------------
    section("5) First-updater-wins — second commit hits SERIALIZATION_FAILURE");
    // --------------------------------------------------------------------
    {
        // Two transactions both snapshot alice = 1400 (after scenario 4).
        // Both want to apply different updates. T1 commits first; T2 then
        // tries to write the same key, blocks until T1 releases the lock,
        // and finally gets rejected at commit because alice's xmin moved
        // past its snapshot.
        txn_id_t t1 = bank.begin();
        txn_id_t t2 = bank.begin();
        check(rd(bank, t1, "alice") == "1400", "T1 reads alice=1400");
        check(rd(bank, t2, "alice") == "1400", "T2 reads alice=1400");

        check(bank.write(t1, "alice", "2000") == Result::Ok, "T1 writes alice=2000");
        check(bank.write(t2, "alice", "100")  == Result::LockWait,
              "T2 write blocked by T1's lock");

        check(bank.commit(t1) == Result::Ok, "T1 commits");
        check(bank.write(t2, "alice", "100") == Result::Ok, "T2 takes lock after T1 release");
        Result r = bank.commit(t2);
        std::cout << "  T2 commit -> " << to_string(r) << "\n";
        check(r == Result::SerializationFailure,
              "T2's commit rejected: alice changed under its snapshot");

        txn_id_t v = bank.begin();
        check(rd(bank, v, "alice") == "2000", "alice = 2000 (first updater won)");
        bank.commit(v);
        ok(bank, "first-updater-wins scenario");
    }

    // --------------------------------------------------------------------
    section("6) gc() — reclaim dead versions below the oldest snapshot");
    // --------------------------------------------------------------------
    {
        std::cout << "  alice chain before gc: " << bank.dump_chain("alice") << "\n";
        std::size_t before = bank.version_count();
        std::size_t pruned = bank.gc();
        std::size_t after  = bank.version_count();
        std::cout << "  versions: " << before << " -> " << after
                  << "  (pruned " << pruned << ")\n";
        std::cout << "  alice chain after  gc: " << bank.dump_chain("alice") << "\n";
        check(pruned > 0, "gc reclaimed at least one dead version");
        check(before - pruned == after, "version count is consistent after gc");

        // Live values must survive.
        txn_id_t v = bank.begin();
        check(rd(bank, v, "alice") == "2000", "alice survives gc");
        check(rd(bank, v, "bob")   == "750",  "bob survives gc");
        bank.commit(v);
        ok(bank, "gc scenario");
    }

    std::cout << "\nManager stats: "
              << bank.live_txn_count() << " live txns, "
              << bank.lock_count()     << " held locks, "
              << bank.version_count()  << " versions.\n";
    std::cout << "All transaction-manager checks passed.\n";
    return 0;
}
