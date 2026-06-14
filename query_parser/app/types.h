
#pragma once
#include <string>
enum class TokenType {
    SELECT, FROM, WHERE, OR, IDENTIFIER, NUMBER,
    GT, LT, LPAREN, RPAREN, END
};
struct Token {
    TokenType type;
    std::string text;
};