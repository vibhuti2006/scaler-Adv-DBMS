// B-Tree — ADBMS Lab 6, 24BCS10288 Vibhuti Bhatnagar
//
// Header-only templated B-tree of minimum degree t, used as an ordered
// key→value map. Implements:
//   * insert  — CLRS proactive-split top-down pass
//   * search / contains / at
//   * erase   — CLRS chapter 18 deletion (three cases for hit-in-node,
//               three cases for descend-with-fix)
//   * in_order  — sorted (key, value) traversal
//   * verify  — runtime invariant checker for all five B-tree properties
//
// Each node stores 2t-1 (key,value) pairs and up to 2t children. We keep
// keys and values in *parallel* vectors so cache behaviour is the same
// as the reference int-only tree; the value side never participates in
// the comparison decisions.

#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace adbms {

template <typename Key, typename Value, typename Compare = std::less<Key>>
class b_tree {
public:
    explicit b_tree(int t = 3) : t_(t < 2 ? 2 : t) {}      // t < 2 is degenerate

    b_tree(const b_tree&)            = delete;
    b_tree& operator=(const b_tree&) = delete;
    b_tree(b_tree&&)                 = delete;
    b_tree& operator=(b_tree&&)      = delete;

    ~b_tree() { delete root_; }

    // --- modifiers --------------------------------------------------------

    // Returns true on first insert, false on overwrite (in which case the
    // existing value is replaced).
    bool insert(const Key& k, const Value& v) {
        if (!root_) {
            root_ = new Node(true);
            root_->keys.push_back(k);
            root_->values.push_back(v);
            ++size_;
            return true;
        }
        if (root_->full(t_)) {
            // Grow the tree's height by 1 — only way the height ever increases.
            Node* fresh = new Node(false);
            fresh->children.push_back(root_);
            split_child(fresh, 0);
            root_ = fresh;
        }
        return insert_nonfull(root_, k, v);
    }

    bool erase(const Key& k) {
        if (!root_) return false;
        bool removed = erase_from(root_, k);
        // If the root collapsed to a single child (this can happen when
        // its only key was merged into a child), drop a level.
        if (root_->keys.empty() && !root_->leaf) {
            Node* old = root_;
            root_     = root_->children[0];
            old->children.clear();   // prevent destructor cascade
            delete old;
        } else if (root_->keys.empty() && root_->leaf) {
            delete root_;
            root_ = nullptr;
        }
        if (removed) --size_;
        return removed;
    }

    // --- queries ----------------------------------------------------------

    bool contains(const Key& k) const { return find_node(k).first != nullptr; }

    Value& at(const Key& k) {
        auto [n, i] = find_node(k);
        if (!n) throw std::out_of_range("b_tree::at — key not found");
        return n->values[i];
    }
    const Value& at(const Key& k) const {
        auto [n, i] = find_node(k);
        if (!n) throw std::out_of_range("b_tree::at — key not found");
        return n->values[i];
    }

    std::size_t size()  const { return size_; }
    bool        empty() const { return size_ == 0; }
    int         degree() const { return t_; }

    // Sorted (key, value) traversal — calls visit(k, v) on every entry.
    template <typename F>
    void in_order(F&& visit) const {
        if (root_) in_order_rec(root_, visit);
    }

    void print(std::ostream& os = std::cout) const {
        if (!root_) { os << "<empty>\n"; return; }
        print_rec(root_, 0, os);
    }

    // Returns "" when all invariants hold; otherwise the first violation.
    std::string verify() const {
        if (!root_) return "";
        int leaf_depth = -1;
        return verify_rec(root_, /*is_root=*/true, /*depth=*/0, leaf_depth);
    }

private:
    struct Node {
        std::vector<Key>    keys;
        std::vector<Value>  values;
        std::vector<Node*>  children;
        bool                leaf;
        explicit Node(bool is_leaf) : leaf(is_leaf) {}
        ~Node() { for (Node* c : children) delete c; }
        bool full(int t) const { return static_cast<int>(keys.size()) == 2*t - 1; }
        int  nkeys()      const { return static_cast<int>(keys.size()); }
    };

    Node*       root_  = nullptr;
    int         t_;
    std::size_t size_  = 0;
    Compare     cmp_{};

    // Returns the slot `i` such that keys[i-1] < k <= keys[i].
    int lower_bound_in(const Node* n, const Key& k) const {
        int i = 0;
        const int K = n->nkeys();
        while (i < K && cmp_(n->keys[i], k)) ++i;
        return i;
    }

    // --- search ----------------------------------------------------------

    std::pair<Node*, int> find_node(const Key& k) {
        Node* cur = root_;
        while (cur) {
            int i = lower_bound_in(cur, k);
            if (i < cur->nkeys() && !cmp_(k, cur->keys[i])) return {cur, i};
            if (cur->leaf) return {nullptr, 0};
            cur = cur->children[i];
        }
        return {nullptr, 0};
    }
    std::pair<const Node*, int> find_node(const Key& k) const {
        const Node* cur = root_;
        while (cur) {
            int i = lower_bound_in(cur, k);
            if (i < cur->nkeys() && !cmp_(k, cur->keys[i])) return {cur, i};
            if (cur->leaf) return {nullptr, 0};
            cur = cur->children[i];
        }
        return {nullptr, 0};
    }

    // --- split + insert --------------------------------------------------

    // Splits parent->children[i] which must be full (2t-1 keys).
    void split_child(Node* parent, int i) {
        Node* y = parent->children[i];
        Node* z = new Node(y->leaf);

        // Upper t-1 keys/values move to z.
        z->keys.assign(  y->keys.begin()   + t_, y->keys.end());
        z->values.assign(y->values.begin() + t_, y->values.end());
        if (!y->leaf) {
            z->children.assign(y->children.begin() + t_, y->children.end());
            y->children.erase(y->children.begin() + t_, y->children.end());
        }
        // Promote the median into the parent.
        Key   median_k = std::move(y->keys[t_-1]);
        Value median_v = std::move(y->values[t_-1]);
        y->keys.erase(  y->keys.begin()   + (t_-1), y->keys.end());
        y->values.erase(y->values.begin() + (t_-1), y->values.end());

        parent->children.insert(parent->children.begin() + i + 1, z);
        parent->keys.insert(  parent->keys.begin()   + i, std::move(median_k));
        parent->values.insert(parent->values.begin() + i, std::move(median_v));
    }

    // Insert (k, v) into a non-full node. Returns true if size grew.
    bool insert_nonfull(Node* n, const Key& k, const Value& v) {
        int i = n->nkeys() - 1;

        if (n->leaf) {
            // Check for an overwrite hit first.
            int slot = lower_bound_in(n, k);
            if (slot < n->nkeys() && !cmp_(k, n->keys[slot])) {
                n->values[slot] = v;
                return false;
            }
            n->keys.insert(  n->keys.begin()   + slot, k);
            n->values.insert(n->values.begin() + slot, v);
            ++size_;
            return true;
        }
        // Internal node — find the correct child.
        while (i >= 0 && cmp_(k, n->keys[i])) --i;
        // If keys[i] == k, overwrite here.
        if (i >= 0 && !cmp_(n->keys[i], k)) { n->values[i] = v; return false; }
        ++i;
        if (n->children[i]->full(t_)) {
            split_child(n, i);
            if (cmp_(n->keys[i], k))      ++i;
            else if (!cmp_(k, n->keys[i])) { n->values[i] = v; return false; }
        }
        return insert_nonfull(n->children[i], k, v);
    }

    // --- erase (CLRS chapter 18) -----------------------------------------

    // Remove `k` from the subtree rooted at `n`. Caller guarantees `n` has
    // ≥ t keys *unless* `n` is the root.
    bool erase_from(Node* n, const Key& k) {
        int i = lower_bound_in(n, k);
        bool here = (i < n->nkeys()) && !cmp_(k, n->keys[i]);

        if (here && n->leaf) {                         // case 1: leaf hit
            n->keys.erase(n->keys.begin() + i);
            n->values.erase(n->values.begin() + i);
            return true;
        }
        if (here) {                                     // case 2: internal hit
            return erase_internal(n, i);
        }
        if (n->leaf) return false;                      // not in tree

        // case 3: must descend; fix child first so it has ≥ t keys.
        bool last_child = (i == n->nkeys());
        if (n->children[i]->nkeys() < t_) {
            ensure_min_t(n, i);
        }
        // After fix, the child index might have shifted (merge collapses
        // children[i] and children[i+1] into children[i]).
        if (last_child && i > n->nkeys()) --i;
        return erase_from(n->children[i], k);
    }

    // Internal hit: replace with predecessor or successor, or merge.
    bool erase_internal(Node* n, int i) {
        Node* left  = n->children[i];
        Node* right = n->children[i+1];
        if (left->nkeys() >= t_) {
            // case 2a: take predecessor
            auto [pk, pv] = pop_max(left);             // recursively, may rebalance
            n->keys[i]   = std::move(pk);
            n->values[i] = std::move(pv);
            return true;
        }
        if (right->nkeys() >= t_) {
            // case 2b: take successor
            auto [sk, sv] = pop_min(right);
            n->keys[i]   = std::move(sk);
            n->values[i] = std::move(sv);
            return true;
        }
        // case 2c: both children minimal — merge k down into them, then
        // recurse into the merged child to actually remove it. After the
        // merge, the target sits at left->keys[t_-1] (the median slot).
        // The merged node has 2t-1 keys, so the next step is safe.
        merge_children(n, i);
        Key target = left->keys[t_-1];
        return erase_from(left, target);
    }

    // Pop the smallest entry in the subtree rooted at n. Caller guarantees
    // n has ≥ t keys at entry; we maintain that as we descend.
    std::pair<Key, Value> pop_min(Node* n) {
        if (n->leaf) {
            Key   k = std::move(n->keys.front());
            Value v = std::move(n->values.front());
            n->keys.erase(n->keys.begin());
            n->values.erase(n->values.begin());
            return {std::move(k), std::move(v)};
        }
        if (n->children[0]->nkeys() < t_) ensure_min_t(n, 0);
        return pop_min(n->children[0]);
    }
    std::pair<Key, Value> pop_max(Node* n) {
        if (n->leaf) {
            Key   k = std::move(n->keys.back());
            Value v = std::move(n->values.back());
            n->keys.pop_back(); n->values.pop_back();
            return {std::move(k), std::move(v)};
        }
        int last = n->nkeys();
        if (n->children[last]->nkeys() < t_) ensure_min_t(n, last);
        return pop_max(n->children[n->nkeys()]);
    }

    // Make sure children[i] has ≥ t keys by borrowing or merging.
    void ensure_min_t(Node* parent, int i) {
        Node* child = parent->children[i];
        if (child->nkeys() >= t_) return;

        Node* left_sib  = (i > 0)                        ? parent->children[i-1] : nullptr;
        Node* right_sib = (i < parent->nkeys())          ? parent->children[i+1] : nullptr;

        if (left_sib && left_sib->nkeys() >= t_) {
            // 3a-left: rotate one key from left sibling through parent.
            child->keys.insert(  child->keys.begin(),   std::move(parent->keys[i-1]));
            child->values.insert(child->values.begin(), std::move(parent->values[i-1]));
            parent->keys[i-1]   = std::move(left_sib->keys.back());
            parent->values[i-1] = std::move(left_sib->values.back());
            left_sib->keys.pop_back();
            left_sib->values.pop_back();
            if (!child->leaf) {
                child->children.insert(child->children.begin(), left_sib->children.back());
                left_sib->children.pop_back();
            }
            return;
        }
        if (right_sib && right_sib->nkeys() >= t_) {
            // 3a-right: rotate one key from right sibling through parent.
            child->keys.push_back(  std::move(parent->keys[i]));
            child->values.push_back(std::move(parent->values[i]));
            parent->keys[i]   = std::move(right_sib->keys.front());
            parent->values[i] = std::move(right_sib->values.front());
            right_sib->keys.erase(right_sib->keys.begin());
            right_sib->values.erase(right_sib->values.begin());
            if (!child->leaf) {
                child->children.push_back(right_sib->children.front());
                right_sib->children.erase(right_sib->children.begin());
            }
            return;
        }
        // 3b: merge with a sibling.
        if (right_sib) merge_children(parent, i);          // merges i and i+1 into i
        else           merge_children(parent, i - 1);      // merges i-1 and i into i-1
    }

    // Merge parent->children[i] and parent->children[i+1] into a single
    // node containing 2t-1 keys with parent->keys[i] dropped down between.
    void merge_children(Node* parent, int i) {
        Node* left  = parent->children[i];
        Node* right = parent->children[i+1];

        left->keys.push_back(  std::move(parent->keys[i]));
        left->values.push_back(std::move(parent->values[i]));
        for (auto& k : right->keys)   left->keys.push_back(std::move(k));
        for (auto& v : right->values) left->values.push_back(std::move(v));
        if (!left->leaf) {
            for (Node* c : right->children) left->children.push_back(c);
            right->children.clear();
        }
        parent->keys.erase(  parent->keys.begin()   + i);
        parent->values.erase(parent->values.begin() + i);
        parent->children.erase(parent->children.begin() + i + 1);
        delete right;
    }

    // --- helpers ---------------------------------------------------------

    template <typename F>
    void in_order_rec(const Node* n, F& visit) const {
        const int k = n->nkeys();
        for (int i = 0; i < k; ++i) {
            if (!n->leaf) in_order_rec(n->children[i], visit);
            visit(n->keys[i], n->values[i]);
        }
        if (!n->leaf) in_order_rec(n->children[k], visit);
    }

    static void print_rec(const Node* n, int depth, std::ostream& os) {
        os << std::string(static_cast<std::size_t>(depth) * 2, ' ') << "[";
        for (int i = 0; i < n->nkeys(); ++i) {
            if (i) os << ' ';
            os << n->keys[i];
        }
        os << "]" << (n->leaf ? "  (leaf)\n" : "\n");
        for (Node* c : n->children) print_rec(c, depth + 1, os);
    }

    std::string verify_rec(const Node* n, bool is_root, int depth, int& leaf_depth) const {
        const int k = n->nkeys();
        if (k > 2*t_ - 1)           return "node holds more than 2t-1 keys";
        if (!is_root && k < t_ - 1) return "non-root holds fewer than t-1 keys";
        if (is_root && k < 1 && !n->leaf) return "non-leaf root has no keys";

        for (int i = 1; i < k; ++i)
            if (!cmp_(n->keys[i-1], n->keys[i]))
                return "keys not strictly increasing within node";

        if (n->leaf) {
            if (leaf_depth == -1) leaf_depth = depth;
            else if (leaf_depth != depth) return "leaves at different depths";
            if (!n->children.empty()) return "leaf has children attached";
            return "";
        }
        if (static_cast<int>(n->children.size()) != k + 1)
            return "internal node has k != children-1";

        for (int i = 0; i <= k; ++i) {
            const Node* c = n->children[i];
            for (const Key& x : c->keys) {
                if (i < k && !cmp_(x, n->keys[i]))
                    return "child key not strictly less than parent separator";
                if (i > 0 && !cmp_(n->keys[i-1], x))
                    return "child key not strictly greater than parent separator";
            }
            std::string e = verify_rec(c, /*is_root=*/false, depth + 1, leaf_depth);
            if (!e.empty()) return e;
        }
        return "";
    }
};

}  // namespace adbms
