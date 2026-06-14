#include <string>
#include "expressions.h"

struct SelectStatement {
    std::string column;
    std::string tableName;
    Expression *whereFilter;
};
