#pragma once
#include "types.h"
#include <string>
#include <vector>
// age > 15
//      >
//   18    15

// ------------------- AST -------------------
class Lexer {
public:
    explicit Lexer(std::string sql);

    std::vector<Token> tokenize();

private:
    std::string input;
};
