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
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {
struct UCCredentials;

struct UCAPIColumnDefinition {
	string name;
	string type_text;
	string type_json; // optional, preferred over type_text if available
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
	idx_t version;
	int64_t timestamp;
	string file_name;
	int64_t file_size;
	int64_t file_modification_timestamp;
};

struct UCAPICommitsResult {
	vector<UCAPICommit> commits;
	// Newest version the catalog has assigned (may not yet be backfilled into _delta_log/).
	idx_t ratified_version = 0; // JSON: latest-table-version
	string etag;
};

class UCAPI {
public:
	// Vends temporary S3 credentials:
	// - CMTs go via protocol v1 endpoint -> (GET /delta/v1/.../credentials); (returns 400/5116 for EXTERNALs)
	// - EXTERNAL/plain tables -> POST /api/2.1/unity-catalog/temporary-table-credentials endpoint (keyed by table_id)
	// `catalog_managed` flag selects which to use.
	static UCAPITableCredentials GetTableCredentials(ClientContext &ctx, const string &catalog_name,
	                                                 const string &schema_name, const string &table_name,
	                                                 const string &table_id, bool catalog_managed, bool write,
	                                                 const UCCredentials &credentials);
	static string GetDefaultSchema(ClientContext &ctx, const UCCredentials &credentials);
	static vector<string> GetCatalogs(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	static vector<UCAPITable> GetTables(ClientContext &ctx, Catalog &catalog, const string &schema,
	                                    const UCCredentials &credentials);
	static vector<UCAPISchema> GetSchemas(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials);
	// delta.yaml v1: GET /delta/v1/catalogs/{catalog}/schemas/{schema}/tables/{table}
	// Returns commits (backfillable CCv2) + latest-table-version + etag.
	static UCAPICommitsResult LoadTable(ClientContext &ctx, const string &catalog_name, const string &schema_name,
	                                    const string &table_name, const UCCredentials &credentials);
	// delta.yaml v1: POST /delta/v1/catalogs/{catalog}/schemas/{schema}/tables/{table}
	// Sends add-commit update with optional assert-etag requirement.
	// Returns the new etag from the response (empty if absent).
	// backfill_version: when valid, appends a set-latest-backfilled-version update in the same POST
	// so readers can find the commit in _delta_log/ without hitting UC's staging path.
	static string UpdateTable(ClientContext &ctx, const string &catalog_name, const string &schema_name,
	                          const string &table_name, const string &table_id, const string &etag,
	                          const UCCredentials &credentials, idx_t version, idx_t timestamp, const string &file_name,
	                          idx_t file_size, idx_t file_modification_timestamp,
	                          optional_idx backfill_version = optional_idx());
};

} // namespace duckdb
