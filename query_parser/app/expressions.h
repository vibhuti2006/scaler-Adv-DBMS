#pragma once

#include <string>

struct Expression {
    virtual ~Expression() = default;
};

struct Literal : Expression {
    int value;

    explicit Literal(int v) : value(v) {
    }
};

struct ColumnRef : Expression {
    std::string name;

    explicit ColumnRef(std::string n) : name(std::move(n)) {
    }
};

struct BinaryExpression : Expression {
    std::string op;
    Expression *left;
    Expression *right;

    BinaryExpression(std::string o, Expression *l, Expression *r)
        : op(std::move(o)), left(l), right(r) {
    }
};
