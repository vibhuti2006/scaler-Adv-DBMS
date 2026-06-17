// txn_manager.hpp — ADBMS Lab 8 / 24BCS10288 Vibhuti Bhatnagar
//
// An in-memory transaction manager that splits concurrency control the same
// way PostgreSQL does:
//
//   * READS  — MVCC against the snapshot taken at begin(). Readers never
//              block and never take locks. Each version records the commit
//              timestamp it appeared at (xmin) and the commit timestamp that
//              superseded it (xmax). A version is visible to snapshot S
//              when  xmin <= S  AND  (xmax == 0 OR xmax > S).
//
//   * WRITES — Strict two-phase locking. The writer takes an exclusive lock
//              on the key, holds it through every subsequent write to that
//              key, and only releases at commit/abort.
//
//   * DEADLOCK — Every blocked write checks for a cycle in the waits-for
//              graph rooted at the current waiter via DFS. If a cycle is
//              found, the highest-id (youngest) transaction in the cycle
//              is aborted to break it.
//
//   * COMMIT — First-updater-wins. A commit is rejected with
//              SerializationFailure if any of the keys this transaction
//              wrote received a *new* committed version while we were
//              running (i.e. the latest version's xmin > our snapshot).
//
//   * gc()  — Prunes versions whose end_ts is strictly older than the
//              oldest snapshot any live or future transaction can ever see.
//
// The simulator is single-threaded: a blocked write returns LockWait and
// the caller retries. This makes the deadlock and serialization scenarios
// deterministic and easy to assert on.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace adbms::txn {

using txn_id_t = std::uint64_t;
using ts_t     = std::uint64_t;        // commit timestamp / snapshot id

enum class Result {
    Ok,
    NotFound,
    LockWait,
    Aborted,
    SerializationFailure,
};

enum class State { Active, Committed, Aborted };

inline std::string to_string(Result r) {
    switch (r) {
        case Result::Ok:                   return "OK";
        case Result::NotFound:             return "NOT_FOUND";
        case Result::LockWait:             return "LOCK_WAIT";
        case Result::Aborted:              return "ABORTED";
        case Result::SerializationFailure: return "SERIALIZATION_FAILURE";
    }
    return "?";
}

inline std::string to_string(State s) {
    switch (s) {
        case State::Active:    return "Active";
        case State::Committed: return "Committed";
        case State::Aborted:   return "Aborted";
    }
    return "?";
}

class Manager {
public:
    // --- API --------------------------------------------------------------

    txn_id_t begin() {
        txn_id_t id = ++next_id_;
        Txn& t      = txns_[id];
        t.id        = id;
        t.snapshot  = clock_;            // see everything committed before now
        t.state     = State::Active;
        return id;
    }

    // MVCC read. Returns the caller's own uncommitted write if present;
    // otherwise the latest version visible to its snapshot.
    Result read(txn_id_t tx, const std::string& key, std::string& out) {
        Txn* t = lookup(tx);
        if (!t || t->state != State::Active) return Result::Aborted;

        if (auto it = t->buffer.find(key); it != t->buffer.end()) {
            if (it->second.tombstone) return Result::NotFound;
            out = it->second.value;
            return Result::Ok;
        }
        const Version* v = visible_to(key, t->snapshot);
        if (!v || v->tombstone) return Result::NotFound;
        out = v->value;
        return Result::Ok;
    }

    Result write(txn_id_t tx, const std::string& key, const std::string& value) {
        return write_impl(tx, key, value, /*tombstone=*/false);
    }

    Result remove(txn_id_t tx, const std::string& key) {
        return write_impl(tx, key, "", /*tombstone=*/true);
    }

    Result commit(txn_id_t tx) {
        Txn* t = lookup(tx);
        if (!t || t->state != State::Active) return Result::Aborted;

        // First-updater-wins: a concurrent transaction may have already
        // installed a newer version of one of the keys we wrote. If so we
        // must abort with a serialization failure instead of silently
        // overwriting their work.
        for (const auto& [key, _] : t->buffer) {
            const auto it = store_.find(key);
            if (it == store_.end()) continue;
            const auto& chain = it->second;
            if (chain.empty()) continue;
            const Version& head = chain.back();
            if (head.xmin > t->snapshot) {
                abort_internal(t->id);
                return Result::SerializationFailure;
            }
        }

        ts_t commit_ts = ++clock_;
        for (auto& [key, pending] : t->buffer) {
            auto& chain = store_[key];
            if (!chain.empty()) chain.back().xmax = commit_ts;     // supersede
            Version v;
            v.value     = pending.value;
            v.tombstone = pending.tombstone;
            v.xmin      = commit_ts;
            v.xmax      = 0;
            v.creator   = t->id;
            chain.push_back(std::move(v));
        }

        release_all_locks(*t);
        t->state = State::Committed;
        return Result::Ok;
    }

    void abort(txn_id_t tx) { abort_internal(tx); }

    // Prune versions that are dead (xmax != 0) and whose xmax is strictly
    // older than the oldest snapshot any live or future txn can see.
    std::size_t gc() {
        ts_t low = oldest_snapshot();
        std::size_t pruned = 0;
        for (auto& [key, chain] : store_) {
            std::vector<Version> kept;
            kept.reserve(chain.size());
            for (Version& v : chain) {
                if (v.xmax != 0 && v.xmax <= low) { ++pruned; continue; }
                kept.push_back(std::move(v));
            }
            chain.swap(kept);
        }
        return pruned;
    }

    // --- introspection ---------------------------------------------------

    State    state_of(txn_id_t tx) const {
        auto it = txns_.find(tx);
        return it == txns_.end() ? State::Aborted : it->second.state;
    }
    txn_id_t last_victim() const { return last_victim_; }

    std::size_t version_count() const {
        std::size_t n = 0;
        for (auto& [_, chain] : store_) n += chain.size();
        return n;
    }
    std::size_t live_txn_count() const {
        std::size_t n = 0;
        for (auto& [_, t] : txns_) if (t.state == State::Active) ++n;
        return n;
    }
    std::size_t lock_count() const { return xlock_.size(); }

    // Dump the version chain for one key — useful for debugging.
    std::string dump_chain(const std::string& key) const {
        auto it = store_.find(key);
        if (it == store_.end() || it->second.empty()) return "(no versions)";
        std::ostringstream os;
        for (const Version& v : it->second) {
            os << "[xmin=" << v.xmin << " xmax=" << v.xmax
               << " " << (v.tombstone ? "<del>" : v.value)
               << " by T" << v.creator << "] ";
        }
        return os.str();
    }

    // Internal consistency checker — returns "" when healthy. Designed to
    // be called between operations in the demo as a built-in unit test.
    std::string check_invariants() const {
        // Every X-lock holder must be an Active transaction.
        for (const auto& [_, holder] : xlock_) {
            auto it = txns_.find(holder);
            if (it == txns_.end() || it->second.state != State::Active)
                return "x-lock held by non-active txn";
        }
        // Version chains: xmin strictly increasing, last entry has xmax==0
        // unless explicitly deleted, no entry has xmax < xmin.
        for (const auto& [key, chain] : store_) {
            ts_t prev = 0;
            for (std::size_t i = 0; i < chain.size(); ++i) {
                const Version& v = chain[i];
                if (v.xmin <= prev)
                    return "version chain xmin not strictly increasing on '" + key + "'";
                if (v.xmax != 0 && v.xmax <= v.xmin)
                    return "version has xmax <= xmin on '" + key + "'";
                if (i + 1 < chain.size() && v.xmax == 0)
                    return "non-last version with xmax = 0 on '" + key + "'";
                prev = v.xmin;
            }
        }
        return "";
    }

private:
    struct Version {
        std::string value;
        bool        tombstone = false;
        ts_t        xmin      = 0;
        ts_t        xmax      = 0;       // 0 ⇒ still live
        txn_id_t    creator   = 0;
    };
    struct Pending {
        std::string value;
        bool        tombstone = false;
    };
    struct Txn {
        txn_id_t id        = 0;
        ts_t     snapshot  = 0;
        State    state     = State::Active;
        std::unordered_map<std::string, Pending> buffer;
        std::unordered_set<std::string>          locks;
    };

    txn_id_t next_id_     = 0;
    ts_t     clock_       = 0;
    txn_id_t last_victim_ = 0;

    std::unordered_map<txn_id_t, Txn>                       txns_;
    std::unordered_map<std::string, std::vector<Version>>   store_;
    std::unordered_map<std::string, txn_id_t>               xlock_;     // key → holder
    std::unordered_map<txn_id_t, txn_id_t>                  waits_for_; // waiter → holder

    Txn* lookup(txn_id_t tx) {
        auto it = txns_.find(tx);
        return it == txns_.end() ? nullptr : &it->second;
    }

    // Latest version visible to a snapshot, scanning newest → oldest.
    const Version* visible_to(const std::string& key, ts_t snapshot) const {
        auto it = store_.find(key);
        if (it == store_.end()) return nullptr;
        const auto& chain = it->second;
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            if (rit->xmin <= snapshot && (rit->xmax == 0 || rit->xmax > snapshot))
                return &*rit;
        }
        return nullptr;
    }

    // Try to grant `tx` an exclusive lock on `key`. On contention runs a
    // DFS over the waits-for graph and aborts the youngest txn in any cycle.
    Result try_acquire(Txn& t, const std::string& key) {
        if (auto it = xlock_.find(key); it != xlock_.end()) {
            txn_id_t holder = it->second;
            if (holder == t.id) return Result::Ok;            // reentrant

            waits_for_[t.id] = holder;
            std::vector<txn_id_t> cycle;
            if (detect_cycle(t.id, cycle)) {
                txn_id_t victim = *std::max_element(cycle.begin(), cycle.end());
                last_victim_ = victim;
                abort_internal(victim);
                if (victim == t.id) return Result::Aborted;
                // Victim wasn't us — retry the lock acquisition.
                return try_acquire(t, key);
            }
            return Result::LockWait;
        }
        xlock_[key] = t.id;
        t.locks.insert(key);
        waits_for_.erase(t.id);
        return Result::Ok;
    }

    // DFS from `start` in waits_for_ — returns true and fills `cycle` if a
    // back-edge to `start` is found. Cycle excludes the start node itself.
    bool detect_cycle(txn_id_t start, std::vector<txn_id_t>& cycle) const {
        std::vector<txn_id_t> path = {start};
        txn_id_t cur = start;
        while (true) {
            auto it = waits_for_.find(cur);
            if (it == waits_for_.end()) return false;
            txn_id_t next = it->second;
            if (next == start) {
                cycle = path;
                return true;
            }
            // Already visited (non-start) → terminates without closing on us.
            if (std::find(path.begin(), path.end(), next) != path.end())
                return false;
            path.push_back(next);
            cur = next;
        }
    }

    Result write_impl(txn_id_t tx, const std::string& key,
                      const std::string& value, bool tombstone) {
        Txn* t = lookup(tx);
        if (!t || t->state != State::Active) return Result::Aborted;

        Result lock = try_acquire(*t, key);
        if (lock != Result::Ok) return lock;

        Pending& p   = t->buffer[key];
        p.value      = value;
        p.tombstone  = tombstone;
        return Result::Ok;
    }

    void release_all_locks(Txn& t) {
        for (const std::string& k : t.locks) xlock_.erase(k);
        t.locks.clear();
        // Anyone waiting on this txn can retry now — we just drop the edges.
        for (auto it = waits_for_.begin(); it != waits_for_.end(); ) {
            if (it->second == t.id) it = waits_for_.erase(it);
            else                    ++it;
        }
        waits_for_.erase(t.id);
    }

    void abort_internal(txn_id_t tx) {
        Txn* t = lookup(tx);
        if (!t || t->state != State::Active) return;
        t->buffer.clear();
        release_all_locks(*t);
        t->state = State::Aborted;
    }

    // Oldest snapshot any active txn currently holds; if none, the global
    // clock is the floor (no future txn can see anything older).
    ts_t oldest_snapshot() const {
        ts_t low = clock_;
        bool any = false;
        for (const auto& [_, t] : txns_) {
            if (t.state == State::Active) {
                if (!any || t.snapshot < low) { low = t.snapshot; any = true; }
            }
        }
        return low;
    }
};

}  // namespace adbms::txn
