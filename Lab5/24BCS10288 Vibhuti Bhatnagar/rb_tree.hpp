// Red-Black Tree — ADBMS Lab 5, 24BCS10288 Vibhuti Bhatnagar
//
// Header-only, generic ordered map: keys are templated, values are templated,
// comparison is templated. The algorithm is the standard CLRS chapter 13
// presentation (3 insert cases × 2 mirror sides, 4 delete cases × 2 mirror
// sides) with a single shared black sentinel for nil. The public surface
// mimics what an in-memory DBMS index might expose: insert / contains /
// at / erase / size / in-order traversal / a debug invariant checker.
//
// Why a sentinel rather than nullptr leaves? The CLRS fix-up routines need
// to read `nil->color` and `nil->parent` without branching, so we hand them
// one shared black Node instead. Sentinel mutation is local to delete-fixup
// and is undone before the call returns, so the tree as a whole remains
// well-defined.

#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace adbms {

enum class Colour : unsigned char { Red, Black };

template <typename Key, typename Value, typename Compare = std::less<Key>>
class rb_tree {
public:
    rb_tree() : nil_(new Node{}), root_(nil_), size_(0) {
        nil_->colour = Colour::Black;
        nil_->left = nil_->right = nil_->parent = nil_;
    }

    rb_tree(const rb_tree&)            = delete;   // tree owns raw nodes
    rb_tree& operator=(const rb_tree&) = delete;
    rb_tree(rb_tree&&)                 = delete;
    rb_tree& operator=(rb_tree&&)      = delete;

    ~rb_tree() {
        drop_subtree(root_);
        delete nil_;
    }

    // --- modifiers --------------------------------------------------------

    // Inserts or overwrites. Returns true if a new node was created.
    bool insert(const Key& k, const Value& v) {
        Node* parent = nil_;
        Node* cur    = root_;
        while (cur != nil_) {
            parent = cur;
            if      (cmp_(k, cur->key)) cur = cur->left;
            else if (cmp_(cur->key, k)) cur = cur->right;
            else { cur->value = v; return false; }
        }
        Node* z = new Node{k, v, Colour::Red, parent, nil_, nil_};
        if (parent == nil_)             root_ = z;
        else if (cmp_(k, parent->key))  parent->left  = z;
        else                            parent->right = z;
        ++size_;
        fix_after_insert(z);
        return true;
    }

    // Removes the node with the given key. Returns true if something was removed.
    bool erase(const Key& k) {
        Node* z = locate(k);
        if (z == nil_) return false;
        --size_;

        Node*  spliced     = z;
        Colour gone_colour = z->colour;
        Node*  hole;                                   // node that now sits where z was

        if (z->left == nil_) {
            hole = z->right;
            graft(z, z->right);
        } else if (z->right == nil_) {
            hole = z->left;
            graft(z, z->left);
        } else {
            spliced     = subtree_min(z->right);       // in-order successor
            gone_colour = spliced->colour;
            hole        = spliced->right;
            if (spliced->parent == z) {
                hole->parent = spliced;                // sentinel may receive parent here
            } else {
                graft(spliced, spliced->right);
                spliced->right        = z->right;
                spliced->right->parent = spliced;
            }
            graft(z, spliced);
            spliced->left         = z->left;
            spliced->left->parent = spliced;
            spliced->colour       = z->colour;
        }

        delete z;
        if (gone_colour == Colour::Black) fix_after_erase(hole);
        nil_->parent = nil_;                            // undo any sentinel mutation
        return true;
    }

    // --- queries ----------------------------------------------------------

    bool        contains(const Key& k) const { return locate(k) != nil_; }
    std::size_t size()                  const { return size_; }
    bool        empty()                 const { return size_ == 0; }

    // Returns the value or throws std::out_of_range if not present.
    Value& at(const Key& k) {
        Node* n = locate(k);
        if (n == nil_) throw std::out_of_range("rb_tree::at — key not found");
        return n->value;
    }
    const Value& at(const Key& k) const {
        const Node* n = locate(k);
        if (n == nil_) throw std::out_of_range("rb_tree::at — key not found");
        return n->value;
    }

    // In-order traversal — visits keys in sorted order without using recursion
    // on the call stack (matters for very deep trees).
    template <typename F>
    void in_order(F&& visit) const {
        std::stack<Node*> stk;
        Node* cur = root_;
        while (cur != nil_ || !stk.empty()) {
            while (cur != nil_) { stk.push(cur); cur = cur->left; }
            cur = stk.top(); stk.pop();
            visit(cur->key, cur->value);
            cur = cur->right;
        }
    }

    // Level-order pretty printer for debugging.
    void print_bfs(std::ostream& os = std::cout) const {
        if (root_ == nil_) { os << "(empty)\n"; return; }
        std::vector<Node*> cur = {root_}, nxt;
        std::size_t level = 0;
        while (!cur.empty()) {
            os << "L" << level << ": ";
            for (Node* n : cur) {
                os << n->key << "(" << (n->colour == Colour::Red ? 'R' : 'B') << ") ";
                if (n->left  != nil_) nxt.push_back(n->left);
                if (n->right != nil_) nxt.push_back(n->right);
            }
            os << "\n";
            cur.swap(nxt); nxt.clear(); ++level;
        }
    }

    // --- invariants -------------------------------------------------------
    // Verifies all five red-black properties + BST ordering. Returns an
    // empty string on success; the first violation otherwise. Designed for
    // unit-testing the tree from main().
    std::string check_invariants() const {
        if (root_ != nil_ && root_->colour != Colour::Black)
            return "property 2 violated: root is not black";
        std::string err;
        int dummy = 0;
        verify_node(root_, nil_, nullptr, nullptr, dummy, err);
        return err;
    }

private:
    struct Node {
        Key    key{};
        Value  value{};
        Colour colour = Colour::Red;
        Node*  parent = nullptr;
        Node*  left   = nullptr;
        Node*  right  = nullptr;
    };

    Node*       nil_;          // shared black sentinel
    Node*       root_;
    std::size_t size_;
    Compare     cmp_{};

    // --- traversal helpers -----------------------------------------------

    Node*       locate(const Key& k) {
        Node* cur = root_;
        while (cur != nil_) {
            if      (cmp_(k, cur->key)) cur = cur->left;
            else if (cmp_(cur->key, k)) cur = cur->right;
            else return cur;
        }
        return nil_;
    }
    const Node* locate(const Key& k) const {
        const Node* cur = root_;
        while (cur != nil_) {
            if      (cmp_(k, cur->key)) cur = cur->left;
            else if (cmp_(cur->key, k)) cur = cur->right;
            else return cur;
        }
        return nil_;
    }
    Node* subtree_min(Node* n) const {
        while (n->left != nil_) n = n->left;
        return n;
    }

    void drop_subtree(Node* n) {
        if (n == nil_) return;
        drop_subtree(n->left);
        drop_subtree(n->right);
        delete n;
    }

    // --- rotations --------------------------------------------------------
    // Names follow the "rotate around x" convention from CLRS. A left rotate
    // makes x's right child its parent; right rotate is the mirror.
    void rotate_left(Node* x) {
        Node* y = x->right;
        x->right = y->left;
        if (y->left != nil_) y->left->parent = x;
        y->parent = x->parent;
        if      (x->parent == nil_)        root_ = y;
        else if (x == x->parent->left)     x->parent->left  = y;
        else                               x->parent->right = y;
        y->left   = x;
        x->parent = y;
    }
    void rotate_right(Node* x) {
        Node* y = x->left;
        x->left = y->right;
        if (y->right != nil_) y->right->parent = x;
        y->parent = x->parent;
        if      (x->parent == nil_)        root_ = y;
        else if (x == x->parent->right)    x->parent->right = y;
        else                               x->parent->left  = y;
        y->right  = x;
        x->parent = y;
    }

    // Replace subtree rooted at u with subtree rooted at v.
    void graft(Node* u, Node* v) {
        if      (u->parent == nil_)      root_ = v;
        else if (u == u->parent->left)   u->parent->left  = v;
        else                             u->parent->right = v;
        v->parent = u->parent;
    }

    // --- fix-ups ----------------------------------------------------------

    void fix_after_insert(Node* z) {
        while (z->parent->colour == Colour::Red) {
            Node* p = z->parent;
            Node* g = p->parent;
            if (p == g->left) {
                Node* u = g->right;
                if (u->colour == Colour::Red) {            // case 1 — recolour
                    p->colour = u->colour = Colour::Black;
                    g->colour = Colour::Red;
                    z = g;
                } else {
                    if (z == p->right) {                    // case 2 — zig-zag
                        z = p;
                        rotate_left(z);
                        p = z->parent;
                    }
                    p->colour = Colour::Black;              // case 3 — straight
                    g->colour = Colour::Red;
                    rotate_right(g);
                }
            } else {                                         // mirror of above
                Node* u = g->left;
                if (u->colour == Colour::Red) {
                    p->colour = u->colour = Colour::Black;
                    g->colour = Colour::Red;
                    z = g;
                } else {
                    if (z == p->left) {
                        z = p;
                        rotate_right(z);
                        p = z->parent;
                    }
                    p->colour = Colour::Black;
                    g->colour = Colour::Red;
                    rotate_left(g);
                }
            }
        }
        root_->colour = Colour::Black;
    }

    void fix_after_erase(Node* x) {
        while (x != root_ && x->colour == Colour::Black) {
            if (x == x->parent->left) {
                Node* s = x->parent->right;
                if (s->colour == Colour::Red) {              // case 1
                    s->colour         = Colour::Black;
                    x->parent->colour = Colour::Red;
                    rotate_left(x->parent);
                    s = x->parent->right;
                }
                if (s->left->colour == Colour::Black &&
                    s->right->colour == Colour::Black) {     // case 2
                    s->colour = Colour::Red;
                    x = x->parent;
                } else {
                    if (s->right->colour == Colour::Black) { // case 3
                        s->left->colour = Colour::Black;
                        s->colour       = Colour::Red;
                        rotate_right(s);
                        s = x->parent->right;
                    }
                    s->colour          = x->parent->colour;  // case 4
                    x->parent->colour  = Colour::Black;
                    s->right->colour   = Colour::Black;
                    rotate_left(x->parent);
                    x = root_;
                }
            } else {                                          // mirror of above
                Node* s = x->parent->left;
                if (s->colour == Colour::Red) {
                    s->colour         = Colour::Black;
                    x->parent->colour = Colour::Red;
                    rotate_right(x->parent);
                    s = x->parent->left;
                }
                if (s->right->colour == Colour::Black &&
                    s->left->colour == Colour::Black) {
                    s->colour = Colour::Red;
                    x = x->parent;
                } else {
                    if (s->left->colour == Colour::Black) {
                        s->right->colour = Colour::Black;
                        s->colour        = Colour::Red;
                        rotate_left(s);
                        s = x->parent->left;
                    }
                    s->colour          = x->parent->colour;
                    x->parent->colour  = Colour::Black;
                    s->left->colour    = Colour::Black;
                    rotate_right(x->parent);
                    x = root_;
                }
            }
        }
        x->colour = Colour::Black;
    }

    // --- invariant verifier ----------------------------------------------
    // Returns the black-height of the subtree (counting nil as 1 black step)
    // and writes the first violation it sees into `err`. lo/hi are open
    // ranges enforcing the BST property.
    int verify_node(const Node* n, const Node* nil_marker,
                    const Key* lo, const Key* hi,
                    int& dummy_black_height, std::string& err) const {
        (void)dummy_black_height;
        if (n == nil_marker) return 1;
        if (lo && !cmp_(*lo, n->key)) { if (err.empty()) err = "BST order: key not > lo"; }
        if (hi && !cmp_(n->key, *hi)) { if (err.empty()) err = "BST order: key not < hi"; }
        if (n->colour == Colour::Red) {
            if (n->left->colour == Colour::Red || n->right->colour == Colour::Red)
                if (err.empty()) err = "property 4 violated: red node has red child";
        }
        int bhL = verify_node(n->left,  nil_marker, lo,        &n->key, dummy_black_height, err);
        int bhR = verify_node(n->right, nil_marker, &n->key,   hi,      dummy_black_height, err);
        if (bhL != bhR && err.empty()) err = "property 5 violated: black-heights differ";
        return bhL + (n->colour == Colour::Black ? 1 : 0);
    }
};

}  // namespace adbms
