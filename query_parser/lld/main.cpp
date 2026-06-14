struct SQLQuery{
	string tableName;
	vector<string> columns;
	Expression* filters;
};
struct QueryResult{
	vector<Row> rows;
	Pointer* toNextPage;
};

class DbEngine{
	executeQuery(SQLQuery query){
		QueryResult resultPage = fetchTable(query.tableName);
		do{
			for(row in resultPage.rows){
				if(filter(row,query.expression)){
					for(colum in query.colums)
						print(row[column])
				}
			}
		}while(next(resultPage));
	}
	StorageBuffer buffer;
};

class DBAPP{
	SQLQury parseQuery(string query){}
	DbEngine engine;
};
bhatka@Kartiks-MacBook-Pro ~ % cat sql.cpp 
struct SQLQuery{
	string tableName;
	vector<string> columns;
	Expression* filters;
};
struct QueryResult{
	vector<Row> rows;
	Pointer* toNextPage;
};

class DbEngine{
	executeQuery(SQLQuery query){
		QueryResult resultPage = fetchTable(query.tableName);
		do{
			for(row in resultPage.rows){
				if(filter(row,query.expression)){
					for(colum in query.colums)
						print(row[column])
				}
			}
		}while(next(resultPage));
	}
	StorageBuffer buffer;
};

class DBAPP{
	SQLQury parseQuery(string query){}
	DbEngine engine;
};
