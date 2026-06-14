#include "types.h"
#include "lexer.h"

Lexer::Lexer(std::string sql) : input(std::move(sql)) {
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    size_t pos = 0;

    while (pos < input.size()) {
        if (isspace(input[pos])) {
            ++pos;
            continue;
        }

        if (isalpha(input[pos])) {
            std::string word;
            while (pos < input.size() &&
                   (isalnum(input[pos]) || input[pos] == '_')) {
                word += input[pos++];
            }
            if (word == "SELECT") tokens.push_back({TokenType::SELECT, word});
            else if (word == "FROM") tokens.push_back({TokenType::FROM, word});
            else if (word == "WHERE") tokens.push_back({TokenType::WHERE, word});
            else if (word == "OR") tokens.push_back({TokenType::OR, word});
            else tokens.push_back({TokenType::IDENTIFIER, word});
        } else if (isdigit(input[pos])) {
            std::string num;
            while (pos < input.size() && isdigit(input[pos])) num += input[pos++];
            tokens.push_back({TokenType::NUMBER, num});
        } else if (input[pos] == '>') {
            tokens.push_back({TokenType::GT, ">"});
            ++pos;
        } else if (input[pos] == '<') {
            tokens.push_back({TokenType::LT, "<"});
            ++pos;
        } else if (input[pos] == '(') {
            tokens.push_back({TokenType::LPAREN, "("});
            ++pos;
        } else if (input[pos] == ')') {
            tokens.push_back({TokenType::RPAREN, ")"});
            ++pos;
        } else { ++pos; }
    }
    tokens.push_back({TokenType::END, ""});
    return tokens;
}
