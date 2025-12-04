//===----------------------------------------------------------------------===//
//                         DuckDB
//
// src/include/uc_api.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"

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
	int64_t expiration_time = 0; // Unix epoch timestamp in milliseconds, 0 if not provided
};

class UCAPI {
public:
	static UCAPITableCredentials GetTableCredentials(ClientContext &ctx, const string &table_id,
	                                                 const UCCredentials &credentials);
	static string GetDefaultSchema(ClientContext &ctx, const UCCredentials &credentials);
	static vector<string> GetCatalogs(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTables(ClientContext &ctx, Catalog &catalog, const string &schema,
	                                    const UCCredentials &credentials);
	static vector<UCAPISchema> GetSchemas(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTablesInSchema(ClientContext &ctx, Catalog &catalog, const string &schema,
	                                            const UCCredentials &credentials);
};

} // namespace duckdb
