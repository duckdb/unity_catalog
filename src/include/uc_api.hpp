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

// IRC spec: DataFile (a ContentFile with content == "data").
// Carries the path and statistics for one Parquet/Avro/ORC data file.
// Column stats (sizes, counts, bounds) are fully decoded but not yet acted on in the POC.
struct UCScanPlanDataFile {
	string content; // always "data"
	string file_path;
	string file_format; // "parquet" | "avro" | "orc"
	int64_t spec_id = 0;
	int64_t file_size_in_bytes = 0;
	int64_t record_count = 0;
	int64_t first_row_id = -1; // first row ID assigned to the first row; -1 = absent
	// CountMap fields: column-id → count
	unordered_map<uint32_t, int64_t> column_sizes;
	unordered_map<uint32_t, int64_t> value_counts;
	unordered_map<uint32_t, int64_t> null_value_counts;
	unordered_map<uint32_t, int64_t> nan_value_counts;
	// ValueMap fields stored as raw JSON; not yet used
	string lower_bounds_json;
	string upper_bounds_json;
};

// IRC spec: PlanStatus — status of a server-side planning operation.
enum class UCScanPlanStatus { UNKNOWN = 0, COMPLETED, SUBMITTED, FAILED, CANCELLED };

// The IRC wire spelling ("completed", "submitted", ...); "unknown" for a status we didn't recognize.
const char *UCScanPlanStatusToString(UCScanPlanStatus s);

// IRC spec: discriminator on ContentFile.content for delete files.
enum class UCScanDeleteFileType { UNKNOWN = 0, POSITION_DELETES, EQUALITY_DELETES };

// IRC spec: DeleteFile — either PositionDeleteFile or EqualityDeleteFile. Applied by
// BuildUCDeleteFilter (uc_multi_file_list.cpp): position-deletes (classic parquet files and
// deletion-vector-v1 puffin blobs) are supported; equality-deletes raise NotImplementedException
// (see BuildUCDeleteFilter's doc comment for why).
struct UCScanDeleteFile {
	UCScanDeleteFileType content = UCScanDeleteFileType::POSITION_DELETES;
	string file_path;
	string file_format;
	int64_t file_size_in_bytes = 0;
	int64_t record_count = 0;
	vector<uint32_t> equality_ids;      // EqualityDeleteFile only
	int64_t content_offset = -1;        // PositionDeleteFile only; -1 = absent
	int64_t content_size_in_bytes = -1; // PositionDeleteFile only; -1 = absent
};

// IRC spec: FileScanTask — one data file to scan plus references into the delete-files array.
// residual_filter_json is the server-computed residual predicate after partition pruning;
// decoded but not yet re-applied (parquet_scan handles row-level filtering).
struct UCScanPlanFileScanTask {
	UCScanPlanDataFile data_file;
	vector<idx_t> delete_file_references; // 0-based indices into UCScanPlanResult::delete_files
	string residual_filter_json;          // IRC Expression JSON; not yet re-applied
};

// IRC spec: PlanTask — opaque server token exchanged via fetchScanTasks (see
// UCMultiFileList::ExpandNextPath, which drains these lazily as DuckDB requests more files).
using UCScanPlanTask = string;

// Combined result of planTableScan / fetchPlanningResult.
// Maps to PlanTableScanResult / FetchPlanningResult / CompletedPlanningResult in the IRC spec.
// When status == "completed", file_scan_tasks and delete_files are populated.
// When status == "failed", error_message and error_type carry the IRC ErrorModel fields.
struct UCScanPlanResult {
	UCScanPlanStatus status = UCScanPlanStatus::UNKNOWN;
	string plan_id; // present on submitted and completed-with-id responses
	// IRC spec: ScanTasks (present when status == "completed")
	vector<UCScanDeleteFile> delete_files;
	vector<UCScanPlanFileScanTask> file_scan_tasks;
	vector<UCScanPlanTask> plan_tasks;
	// StorageCredential: prefix → {s3.access-key-id, s3.secret-access-key, s3.session-token, client.region, ...}
	vector<pair<string, unordered_map<string, string>>> storage_credentials;
	// IRC spec: ErrorModel (present when status == "failed")
	string error_message;
	string error_type;
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

	// IRC spec: planTableScan — POST .../plan. Polls fetchPlanningResult automatically if
	// the server returns status "submitted". filter_json is an IRC Expression JSON string;
	// empty string sends no filter (full-table scan).
	static UCScanPlanResult PlanTableScan(ClientContext &ctx, const string &catalog_name, const string &schema_name,
	                                      const string &table_name, const UCCredentials &credentials,
	                                      const string &scan_plan_endpoint, const string &filter_json = "");
	// IRC spec: fetchPlanningResult — GET .../plan/{plan-id}. retry_after_ms_out (optional) receives
	// the response's Retry-After delay in ms (-1 if absent), so the poll loop can pace itself.
	static UCScanPlanResult FetchPlanningResult(ClientContext &ctx, const string &catalog_name,
	                                            const string &schema_name, const string &table_name,
	                                            const string &plan_id, const UCCredentials &credentials,
	                                            const string &scan_plan_endpoint,
	                                            optional_ptr<int64_t> retry_after_ms_out = nullptr);
	// IRC spec: fetchScanTasks — POST .../tasks. Exchanges one plan-task token for file-scan-tasks.
	// The response is a ScanTasks payload (no status field); result.status is always COMPLETED on success.
	static UCScanPlanResult FetchScanTasks(ClientContext &ctx, const string &catalog_name, const string &schema_name,
	                                       const string &table_name, const string &plan_task,
	                                       const UCCredentials &credentials, const string &scan_plan_endpoint);
};

} // namespace duckdb
