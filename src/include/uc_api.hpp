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
	unordered_map<string, string> properties;
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

struct UCAPICommit {
	int64_t version;
	int64_t timestamp;
	string file_name;
	int64_t file_size;
	int64_t file_modification_timestamp;
};

struct UCAPICommitsResult {
	vector<UCAPICommit> commits;
	int64_t latest_table_version;
};

class UCAPI {
public:
	static UCAPITableCredentials GetTableCredentials(ClientContext &ctx, const string &table_id, bool write,
	                                                 const UCCredentials &credentials);
	static string GetDefaultSchema(ClientContext &ctx, const UCCredentials &credentials);
	static vector<string> GetCatalogs(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTables(ClientContext &ctx, Catalog &catalog, const string &schema,
	                                    const UCCredentials &credentials);
	static vector<UCAPISchema> GetSchemas(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	static UCAPICommitsResult GetCommits(ClientContext &ctx, const string &table_id, const string &table_uri,
	                                     const UCCredentials &credentials);
	static bool PostCommit(ClientContext &ctx, const string &table_id, const string &table_uri,
	                       const UCCredentials &credentials, idx_t version, idx_t timestamp, const string &file_name,
	                       idx_t file_size, idx_t file_modification_timestamp);
};

} // namespace duckdb
