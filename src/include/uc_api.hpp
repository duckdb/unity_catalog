//===----------------------------------------------------------------------===//
//                         DuckDB
//
// src/include/uc_api.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {
struct UCCredentials;

struct UCAPIColumnDefinition {
	string name;
	string type_text;
	idx_t precision;
	idx_t scale;
	idx_t position;
};

struct UCAPITable {
	string table_id;

	string name;
	string catalog_name;
	string schema_name;
	string table_type;
	string data_source_format;

	string storage_location;
	string delta_last_commit_timestamp;
	string delta_last_update_version;

	vector<UCAPIColumnDefinition> columns;
};

struct UCAPISchema {
	string schema_name;
	string catalog_name;
};

struct UCAPITableCredentials {
	string key_id;
	string secret;
	string session_token;
};

class UCAPI {
public:
	//! WARNING: not thread-safe. To be called once on extension initialization
	static void InitializeCurl();

	static UCAPITableCredentials GetTableCredentials(const string &table_id, const UCCredentials &credentials);
	static string GetDefaultSchema(const UCCredentials &credentials);
	static vector<string> GetCatalogs(const string &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTables(const string &catalog, const string &schema, const UCCredentials &credentials);
	static vector<UCAPISchema> GetSchemas(const string &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTablesInSchema(const string &catalog, const string &schema,
	                                            const UCCredentials &credentials);
};
} // namespace duckdb
