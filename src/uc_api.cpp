#include "duckdb/common/helper.hpp"
#include <chrono>
#include <cstddef>
#include <sys/stat.h>
#include <thread>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "duckdb/common/http_util.hpp"
#include "uc_logging.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "uc_api.hpp"
#include "uc_logging.hpp"
#include "storage/unity_catalog.hpp"
#include "yyjson.hpp"

namespace duckdb {

// RAII wrapper for yyjson_doc* to ensure yyjson_doc_free is called even when exceptions are thrown
struct YYJsonDoc {
	explicit YYJsonDoc(const string &json) : doc(duckdb_yyjson::yyjson_read(json.c_str(), json.size(), 0)) {
	}
	~YYJsonDoc() {
		if (doc) {
			duckdb_yyjson::yyjson_doc_free(doc);
		}
	}
	duckdb_yyjson::yyjson_val *Root() const {
		return duckdb_yyjson::yyjson_doc_get_root(doc);
	}
	// non-copyable
	YYJsonDoc(const YYJsonDoc &) = delete;
	YYJsonDoc &operator=(const YYJsonDoc &) = delete;

	duckdb_yyjson::yyjson_doc *doc;
};

static string YYJsonEncodeString(const string &raw) {
	duckdb_yyjson::yyjson_val val;
	duckdb_yyjson::yyjson_set_strn(&val, raw.data(), raw.size());
	size_t encoded_len = 0;
	char *encoded = duckdb_yyjson::yyjson_val_write(&val, duckdb_yyjson::YYJSON_WRITE_NOFLAG, &encoded_len);
	if (!encoded) {
		throw InvalidInputException("yyjson string encoding failed");
	}
	string result(encoded, encoded_len);
	free(encoded);
	return result;
}

static void AuthenticateViaBearerToken(HTTPHeaders &hdrs, const string &token) {
	if (!token.empty()) {
		hdrs.Insert("Authorization", "Bearer " + token);
		// curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
	}
}

static void EnsureHttpfsExtension(const shared_ptr<DatabaseInstance> &db) {
	// autoloading/requiring HTTPFS at this ext's Load time fails, iceberg does the same deferred load
	if (!db) {
		throw InvalidConfigurationException("Context does not have database instance");
	}
	ExtensionHelper::AutoLoadExtension(*db, "httpfs");
	if (!db->ExtensionIsLoaded("httpfs")) {
		throw MissingExtensionException("The iceberg extension requires the httpfs extension to be loaded!");
	}
}

// Core request: returns the full response (so callers can read headers/status), throwing on
// non-2xx. MakeRequest wraps it for callers that only need the body.
static unique_ptr<HTTPResponse> MakeRequestResp(ClientContext &ctx, const string &url, const string &token = "",
                                                const string &body = "", bool send_as_get = false) {
	auto db = ctx.db;
	EnsureHttpfsExtension(db);
	auto &http_util = HTTPUtil::Get(*db);
	auto params = http_util.InitializeParameters(*db, url);
	params->logger = ctx.logger;

	HTTPHeaders hdrs(*ctx.db);
	AuthenticateViaBearerToken(hdrs, token);
	// TODO: conditional set if not override?
	hdrs.Insert("Content-Type", "application/json");

	unique_ptr<HTTPResponse> resp;
	if (body.empty()) {
		GetRequestInfo req(url, hdrs, *params, nullptr, nullptr);
		resp = http_util.Request(req);
	} else {
		PostRequestInfo req(url, hdrs, *params, const_data_ptr_cast(body.data()), body.size());
		req.send_post_as_get_request = send_as_get;
		resp = http_util.Request(req);
	}

	if (!resp->Success()) {
		throw IOException("Request to '%s' failed (HTTP %d): %s\nResponse body: %s", url,
		                  static_cast<int>(resp->status), resp->GetError(),
		                  resp->body.empty() ? "(empty)" : resp->body);
	}
	return resp;
}

static string MakeRequest(ClientContext &ctx, const string &url, const string &token = "", const string &body = "",
                          bool send_as_get = false) {
	return std::move(MakeRequestResp(ctx, url, token, body, send_as_get)->body);
}

// Retry-After delay-seconds header -> milliseconds; -1 if absent or not a plain integer (the
// HTTP-date form isn't emitted by these planning endpoints, so we don't parse it).
static int64_t ParseRetryAfterMs(const HTTPResponse &resp) {
	if (!resp.HasHeader("Retry-After")) {
		return -1;
	}
	try {
		auto secs = std::stoll(resp.GetHeaderValue("Retry-After"));
		return secs >= 0 ? secs * 1000 : -1;
	} catch (...) {
		return -1;
	}
}

// Best-effort DELETE of a server-side plan we're abandoning (query cancelled, timed out, or
// errored mid-poll) so the server can release it. Never throws — a failed cleanup must not mask
// the original error.
static void CancelPlan(ClientContext &ctx, const string &scan_plan_endpoint, const string &catalog_name,
                       const string &schema_name, const string &table_name, const string &plan_id,
                       const string &token) {
	if (plan_id.empty()) {
		return;
	}
	UC_LOG_DEBUG(ctx, "api-irc.CancelPlan plan_id=%s", plan_id);
	string url = scan_plan_endpoint + "/v1/catalogs/" + catalog_name + "/namespaces/" + schema_name + "/tables/" +
	             table_name + "/plan/" + plan_id;
	try {
		auto db = ctx.db;
		auto &http_util = HTTPUtil::Get(*db);
		auto params = http_util.InitializeParameters(*db, url);
		params->logger = ctx.logger;
		HTTPHeaders hdrs(*ctx.db);
		AuthenticateViaBearerToken(hdrs, token);
		DeleteRequestInfo req(url, hdrs, *params);
		http_util.Request(req);
	} catch (const std::exception &e) {
		// Still best-effort (we don't rethrow), but a failed cleanup means a plan may leak
		// server-side until it ages out — worth a WARNING so an operator can spot it.
		UC_LOG_WARNING(ctx, "api-irc.CancelPlan plan_id=%s cleanup DELETE failed: %s", plan_id, e.what());
	} catch (...) {
		// best-effort cleanup — swallow
	}
}

template <class TYPE, uint8_t TYPE_NUM, TYPE (*get_function)(duckdb_yyjson::yyjson_val *obj)>
static TYPE TemplatedTryGetYYJson(duckdb_yyjson::yyjson_val *obj, const string &field, TYPE default_val,
                                  bool fail_on_missing = true) {
	auto val = yyjson_obj_get(obj, field.c_str());
	if (val && !yyjson_is_null(val)) {
		if (yyjson_get_type(val) == TYPE_NUM) {
			return get_function(val);
		}
		throw IOException("Invalid field found while parsing field: " + field);
	}
	// field absent or JSON null: both are "missing" for a required field. The spec declares no
	// response property nullable, so a null in a required field is a non-conforming response.
	if (fail_on_missing) {
		throw IOException("Invalid field found while parsing field: " + field);
	}
	return default_val;
}

static uint64_t TryGetNumFromObject(duckdb_yyjson::yyjson_val *obj, const string &field, bool fail_on_missing = true,
                                    uint64_t default_val = 0) {
	return TemplatedTryGetYYJson<uint64_t, YYJSON_TYPE_NUM, duckdb_yyjson::yyjson_get_uint>(obj, field, default_val,
	                                                                                        fail_on_missing);
}
static string TryGetStrFromObject(duckdb_yyjson::yyjson_val *obj, const string &field, bool fail_on_missing = true,
                                  const char *default_val = "") {
	return TemplatedTryGetYYJson<const char *, YYJSON_TYPE_STR, duckdb_yyjson::yyjson_get_str>(obj, field, default_val,
	                                                                                           fail_on_missing);
}

namespace {

struct UCAPIError {
public:
	UCAPIError() {
	}
	UCAPIError(const string &error_code, const string &message) : error_code(error_code), message(message) {
	}

public:
	bool HasError() {
		return !error_code.empty();
	}

public:
	void ThrowError(const string &prefix) {
		D_ASSERT(HasError());
		throw IOException("%s. error_code: %s, message: %s", prefix, error_code, message);
	}

private:
	string error_code;
	string message;
};

} // namespace

static UCAPIError CheckError(duckdb_yyjson::yyjson_val *api_result) {
	// delta.yaml v1 format: {"error": {"message": "...", "type": "CommitVersionConflictException", "code": N}}
	auto *error_obj = yyjson_obj_get(api_result, "error");
	if (error_obj && yyjson_is_obj(error_obj)) {
		auto message = TryGetStrFromObject(error_obj, "message", false);
		if (!message.empty()) {
			auto type = TryGetStrFromObject(error_obj, "type", false);
			return UCAPIError(type.empty() ? "error" : type, message);
		}
	}
	// all.yaml legacy format: {"error_code": "...", "message": "..."}
	auto error_code = TryGetStrFromObject(api_result, "error_code", false);
	if (!error_code.empty()) {
		auto message = TryGetStrFromObject(api_result, "message", false);
		if (message.empty()) {
			message = "-";
		}
		return UCAPIError(error_code, message);
	}
	return UCAPIError();
}

// # list catalogs
//     echo "List of catalogs"
//     curl --request GET
//     "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/catalogs" \
//  	--header "Authorization: Bearer ${TOKEN}" | jq .
//
// # list short version of all tables
//     echo "Table Summaries"
//     curl --request GET
//     "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/table-summaries?catalog_name=workspace"
//     \
//  	--header "Authorization: Bearer ${TOKEN}" | jq .
//
// # list tables in `default` schema
//     echo "Tables in default schema"
//     curl --request GET
//     "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/tables?catalog_name=workspace&schema_name=default"
//     \
//  	--header "Authorization: Bearer ${TOKEN}" | jq .

string UCAPI::GetDefaultSchema(ClientContext &ctx, const UCCredentials &credentials) {
	UC_LOG_DEBUG(ctx, "api.GetDefaultSchema endpoint=%s", credentials.endpoint);
	auto url = credentials.endpoint + "/api/2.0/settings/types/default_namespace_ws/names/default";
	auto resp = MakeRequest(ctx, url, credentials.token);

	YYJsonDoc doc(resp);
	auto *root = doc.Root();

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError("Failed to get default schema of the catalog");
	}

	auto setting_name = TryGetStrFromObject(root, "setting_name", false);
	if (setting_name.empty()) {
		throw InvalidInputException("Failed to get default schema of the catalog, "
		                            "API response is invalid!");
	}

	return setting_name;
}

UCAPICommitsResult UCAPI::LoadTable(ClientContext &ctx, const string &catalog_name, const string &schema_name,
                                    const string &table_name, const UCCredentials &credentials) {
	UCAPICommitsResult result;
	string url = StringUtil::Format("%s/api/2.1/unity-catalog/delta/v1/catalogs/%s/schemas/%s/tables/%s",
	                                credentials.endpoint, catalog_name, schema_name, table_name);
	UC_LOG_DEBUG(ctx, "api.LoadTable %s.%s.%s", catalog_name, schema_name, table_name);
	auto api_result = MakeRequest(ctx, url, credentials.token);

	YYJsonDoc doc(api_result);
	auto *root = doc.Root();

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError(StringUtil::Format("Failed to load table %s.%s.%s", catalog_name, schema_name, table_name));
	}

	auto *metadata = yyjson_obj_get(root, "metadata");
	if (metadata && yyjson_is_obj(metadata)) {
		result.etag = TryGetStrFromObject(metadata, "etag", false);
	}

	// `latest-table-version` is optional per spec but gates which staged commits are visible
	// (max_catalog_version), so a default of 0 would hide them all. Fall back to the highest
	// commit version: when present it equals that anyway (it's the latest ratified version).
	auto *ltv = yyjson_obj_get(root, "latest-table-version");
	bool ltv_present = ltv && !yyjson_is_null(ltv);
	idx_t server_ltv = TryGetNumFromObject(root, "latest-table-version", false, 0);

	idx_t max_commit_version = 0;
	auto *commits = yyjson_obj_get(root, "commits");
	if (commits && yyjson_is_arr(commits)) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *commit;
		yyjson_arr_foreach(commits, idx, max, commit) {
			UCAPICommit c;
			c.version = TryGetNumFromObject(commit, "version", true);
			c.timestamp = (int64_t)TryGetNumFromObject(commit, "timestamp", true);
			c.file_name = TryGetStrFromObject(commit, "file-name", true);
			c.file_size = (int64_t)TryGetNumFromObject(commit, "file-size", true);
			c.file_modification_timestamp = (int64_t)TryGetNumFromObject(commit, "file-modification-timestamp", true);
			if (c.version > max_commit_version) {
				max_commit_version = c.version;
			}
			result.commits.push_back(c);
		}
	}

	// In case server ltv exceeds max commit, take server's value. Shouldn't occur, check below!
	result.ratified_version = server_ltv > max_commit_version ? server_ltv : max_commit_version;

	// Invariant: when the server reports a latest version alongside backfillable commits, it must
	// equal the highest commit; the check is exempt if ltv is absent or there are no commits.
	// DEBUG asserts here; (consider always failing?). Release recovers via max() above; only
	// the absent-with-commits case is logged below — a present-but-unequal ltv is not.
	D_ASSERT(server_ltv == max_commit_version || !ltv_present || result.commits.empty());
	if (!ltv_present && !result.commits.empty()) {
		UC_LOG_WARNING(ctx,
		               "api.LoadTable %s.%s.%s -> incoherent response: %zu commit(s) but no "
		               "latest-table-version; using max commit version %lld as max_catalog_version",
		               catalog_name, schema_name, table_name, result.commits.size(), (int64_t)result.ratified_version);
	}

	UC_LOG_DEBUG(ctx, "api.LoadTable %s.%s.%s -> etag=%s commits=%zu ratified_version=%lld", catalog_name, schema_name,
	             table_name, result.etag.empty() ? "(none)" : result.etag, result.commits.size(),
	             (int64_t)result.ratified_version);
	return result;
}

string UCAPI::UpdateTable(ClientContext &ctx, const string &catalog_name, const string &schema_name,
                          const string &table_name, const string &table_id, const string &etag,
                          const UCCredentials &credentials, idx_t version, idx_t timestamp, const string &file_name,
                          idx_t file_size, idx_t file_modification_timestamp, optional_idx backfill_version) {
	string uuid_req = StringUtil::Format(R"({"type": "assert-table-uuid", "uuid": "%s"})", table_id);
	// YYJsonEncodeString returns a JSON string token w/ quotes, so no quotes in format below
	string etag_req =
	    etag.empty() ? "" : StringUtil::Format(R"(, {"type": "assert-etag", "etag": %s})", YYJsonEncodeString(etag));
	string backfill_update;
	if (backfill_version.IsValid()) {
		backfill_update =
		    StringUtil::Format(R"(, {"action": "set-latest-backfilled-version", "latest-published-version": %lld})",
		                       (int64_t)backfill_version.GetIndex());
	}
	string body = StringUtil::Format(
	    R"({"requirements": [%s%s], "updates": [{"action": "add-commit", "commit": {"version": %lld, "timestamp": %lld, "file-name": "%s", "file-size": %lld, "file-modification-timestamp": %lld}}%s]})",
	    uuid_req, etag_req, (int64_t)version, (int64_t)timestamp, file_name, (int64_t)file_size,
	    (int64_t)file_modification_timestamp, backfill_update);
	// XXX: hard-coded delta v1 protocol; protocol negotiation via GET /delta/v1/config not yet implemented
	string url = StringUtil::Format("%s/api/2.1/unity-catalog/delta/v1/catalogs/%s/schemas/%s/tables/%s",
	                                credentials.endpoint, catalog_name, schema_name, table_name);
	string backfill_log =
	    backfill_version.IsValid() ? to_string((int64_t)backfill_version.GetIndex()) : string("(none)");
	UC_LOG_DEBUG(ctx, "api.UpdateTable %s.%s.%s version=%lld table_id=%s etag=%s backfill_version=%s", catalog_name,
	             schema_name, table_name, (int64_t)version, table_id.empty() ? "(none)" : table_id,
	             etag.empty() ? "(none)" : etag, backfill_log);
	auto api_result = MakeRequest(ctx, url, credentials.token, body);

	YYJsonDoc doc(api_result);
	auto *root = doc.Root();

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError(StringUtil::Format("Failed to commit to %s.%s.%s", catalog_name, schema_name, table_name));
	}

	string new_etag;
	auto *metadata = yyjson_obj_get(root, "metadata");
	if (metadata && yyjson_is_obj(metadata)) {
		new_etag = TryGetStrFromObject(metadata, "etag", false);
	}
	UC_LOG_DEBUG(ctx, "api.UpdateTable %s.%s.%s -> new_etag=%s", catalog_name, schema_name, table_name,
	             new_etag.empty() ? "(none)" : new_etag);
	return new_etag;
}

UCAPITableCredentials UCAPI::GetTableCredentials(ClientContext &ctx, const string &catalog_name,
                                                 const string &schema_name, const string &table_name,
                                                 const string &table_id, bool catalog_managed, bool write,
                                                 const UCCredentials &credentials) {
	UCAPITableCredentials result;
	const char *operation = write ? "READ_WRITE" : "READ";

	if (!catalog_managed) {
		// EXTERNAL / plain Delta: the /delta/v1 credentials endpoint rejects these (HTTP 400, 5116).
		// Use the temporary-table-credentials endpoint, keyed by table_id.
		string url = credentials.endpoint + "/api/2.1/unity-catalog/temporary-table-credentials";
		string body = StringUtil::Format(R"({"table_id": "%s", "operation": "%s"})", table_id, operation);
		UC_LOG_DEBUG(ctx, "api.GetTableCredentials %s.%s.%s op=%s managed=0 table_id=%s", catalog_name, schema_name,
		             table_name, operation, table_id);
		auto api_result = MakeRequest(ctx, url, credentials.token, body);

		YYJsonDoc doc(api_result);
		auto *root = doc.Root();
		auto error = CheckError(root);
		if (error.HasError()) {
			error.ThrowError(StringUtil::Format("Failed to get table credentials for %s.%s.%s", catalog_name,
			                                    schema_name, table_name));
		}
		auto *aws = yyjson_obj_get(root, "aws_temp_credentials");
		if (aws && yyjson_is_obj(aws)) {
			result.key_id = TryGetStrFromObject(aws, "access_key_id", false);
			result.secret = TryGetStrFromObject(aws, "secret_access_key", false);
			result.session_token = TryGetStrFromObject(aws, "session_token", false);
		}
		return result;
	}

	// Managed (catalog-managed) Delta: delta.yaml v1 credentials endpoint.
	string url = StringUtil::Format(
	    "%s/api/2.1/unity-catalog/delta/v1/catalogs/%s/schemas/%s/tables/%s/credentials?operation=%s",
	    credentials.endpoint, catalog_name, schema_name, table_name, operation);
	UC_LOG_DEBUG(ctx, "api.GetTableCredentials %s.%s.%s op=%s managed=1", catalog_name, schema_name, table_name,
	             operation);
	auto api_result = MakeRequest(ctx, url, credentials.token);

	YYJsonDoc doc(api_result);
	auto *root = doc.Root();

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError(
		    StringUtil::Format("Failed to get table credentials for %s.%s.%s", catalog_name, schema_name, table_name));
	}

	// Parse storage-credentials array; use first entry (longest-prefix matching is a TODO)
	auto *creds_arr = yyjson_obj_get(root, "storage-credentials");
	if (creds_arr && yyjson_is_arr(creds_arr) && yyjson_arr_size(creds_arr) > 0) {
		auto *cred = yyjson_arr_get_first(creds_arr);
		auto *cfg = yyjson_obj_get(cred, "config");
		if (cfg && yyjson_is_obj(cfg)) {
			result.key_id = TryGetStrFromObject(cfg, "s3.access-key-id", false);
			result.secret = TryGetStrFromObject(cfg, "s3.secret-access-key", false);
			result.session_token = TryGetStrFromObject(cfg, "s3.session-token", false);
		}
	}

	return result;
}

vector<string> UCAPI::GetCatalogs(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials) {
	throw NotImplementedException("UCAPI::GetCatalogs");
}

static UCAPIColumnDefinition ParseColumnDefinition(duckdb_yyjson::yyjson_val *column_def) {
	UCAPIColumnDefinition result;

	result.name = TryGetStrFromObject(column_def, "name");
	result.type_text = TryGetStrFromObject(column_def, "type_text");
	// type_precision/type_scale are OPTIONAL in the UC /tables ColumnInfo: Delta-Spark-created
	// tables report them as JSON null (only DECIMAL columns carry a real value), and a null/absent
	// value must coerce to 0, not throw. Without fail_on_missing=false, ANY parse of the /tables
	// response (SHOW ALL TABLES / DESCRIBE / SELECT) raises "Invalid field ... type_precision" as
	// soon as such a column is present. (Matches the sibling optional fields below.)
	result.precision = TryGetNumFromObject(column_def, "type_precision", false);
	result.scale = TryGetNumFromObject(column_def, "type_scale", false);
	result.position = TryGetNumFromObject(column_def, "position");

	return result;
}

vector<UCAPITable> UCAPI::GetTables(ClientContext &ctx, Catalog &catalog, const string &schema,
                                    const UCCredentials &credentials) {
	vector<UCAPITable> result;
	UC_LOG_DEBUG(ctx, "api.GetTables catalog=%s schema=%s", catalog.GetDBPath(), schema);
	auto url = credentials.endpoint + "/api/2.1/unity-catalog/tables?catalog_name=" + catalog.GetDBPath() +
	           "&schema_name=" + schema;
	auto api_result = MakeRequest(ctx, url, credentials.token);

	YYJsonDoc doc(api_result);
	auto *root = doc.Root();

	// Get root["hits"], iterate over the array
	auto *tables = yyjson_obj_get(root, "tables");
	size_t idx, max;
	duckdb_yyjson::yyjson_val *table;
	yyjson_arr_foreach(tables, idx, max, table) {
		UCAPITable table_result;
		table_result.catalog_name = catalog.GetDBPath();
		table_result.schema_name = schema;

		table_result.name = TryGetStrFromObject(table, "name");
		table_result.table_type = TryGetStrFromObject(table, "table_type");
		table_result.data_source_format = TryGetStrFromObject(table, "data_source_format", false);
		table_result.storage_location = TryGetStrFromObject(table, "storage_location", false);
		table_result.table_id = TryGetStrFromObject(table, "table_id");

		auto *columns = yyjson_obj_get(table, "columns");
		duckdb_yyjson::yyjson_val *col;
		size_t col_idx, col_max;
		yyjson_arr_foreach(columns, col_idx, col_max, col) {
			auto column_definition = ParseColumnDefinition(col);
			table_result.columns.push_back(column_definition);
		}

		auto *properties = yyjson_obj_get(table, "properties");
		duckdb_yyjson::yyjson_val *key, *val;
		size_t prop_idx, prop_max;
		yyjson_obj_foreach(properties, prop_idx, prop_max, key, val) {
			auto val_cstr = duckdb_yyjson::yyjson_get_str(val);
			if (val_cstr) {
				table_result.properties[duckdb_yyjson::yyjson_get_str(key)] = val_cstr;
			}
		}

		result.push_back(table_result);
	}

	UC_LOG_DEBUG(ctx, "api.GetTables catalog=%s schema=%s -> tables=%zu", catalog.GetDBPath(), schema, result.size());
	return result;
}

vector<UCAPISchema> UCAPI::GetSchemas(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials) {
	vector<UCAPISchema> result;
	UC_LOG_DEBUG(ctx, "api.GetSchemas catalog=%s", catalog.GetDBPath());
	auto url = credentials.endpoint + "/api/2.1/unity-catalog/schemas?catalog_name=" + catalog.GetDBPath();
	auto api_result = MakeRequest(ctx, url, credentials.token);

	YYJsonDoc doc(api_result);
	auto *root = doc.Root();

	// Get root["hits"], iterate over the array
	auto *schemas = yyjson_obj_get(root, "schemas");
	size_t idx, max;
	duckdb_yyjson::yyjson_val *schema;
	yyjson_arr_foreach(schemas, idx, max, schema) {
		UCAPISchema schema_result;

		auto *name = yyjson_obj_get(schema, "name");
		if (name) {
			schema_result.schema_name = yyjson_get_str(name);
		}
		schema_result.catalog_name = catalog.GetDBPath();

		result.push_back(schema_result);
	}

	UC_LOG_DEBUG(ctx, "api.GetSchemas catalog=%s -> schemas=%zu", catalog.GetDBPath(), result.size());
	return result;
}

// ---------------------------------------------------------------------------
// Scan plan API helpers
// ---------------------------------------------------------------------------

static void ParseCountMap(duckdb_yyjson::yyjson_val *map_val, unordered_map<uint32_t, int64_t> &out) {
	if (!map_val) {
		return;
	}
	auto *keys = yyjson_obj_get(map_val, "keys");
	auto *vals = yyjson_obj_get(map_val, "values");
	if (!keys || !vals) {
		return;
	}
	vector<uint32_t> key_vec;
	size_t k_idx, k_max;
	duckdb_yyjson::yyjson_val *k_val;
	yyjson_arr_foreach(keys, k_idx, k_max, k_val) {
		key_vec.push_back((uint32_t)duckdb_yyjson::yyjson_get_uint(k_val));
	}
	size_t v_idx, v_max;
	duckdb_yyjson::yyjson_val *v_val;
	yyjson_arr_foreach(vals, v_idx, v_max, v_val) {
		if (v_idx < key_vec.size()) {
			out[key_vec[v_idx]] = duckdb_yyjson::yyjson_get_sint(v_val);
		}
	}
}

static UCScanPlanDataFile ParseDataFile(duckdb_yyjson::yyjson_val *df_val) {
	UCScanPlanDataFile df;
	df.content = TryGetStrFromObject(df_val, "content", false);
	df.file_path = TryGetStrFromObject(df_val, "file-path");
	df.file_format = TryGetStrFromObject(df_val, "file-format", false);
	df.spec_id = (int64_t)TryGetNumFromObject(df_val, "spec-id", false);
	df.file_size_in_bytes = (int64_t)TryGetNumFromObject(df_val, "file-size-in-bytes", false);
	df.record_count = (int64_t)TryGetNumFromObject(df_val, "record-count", false);
	auto *frid = yyjson_obj_get(df_val, "first-row-id");
	if (frid) {
		df.first_row_id = duckdb_yyjson::yyjson_get_sint(frid);
	}
	ParseCountMap(yyjson_obj_get(df_val, "column-sizes"), df.column_sizes);
	ParseCountMap(yyjson_obj_get(df_val, "value-counts"), df.value_counts);
	ParseCountMap(yyjson_obj_get(df_val, "null-value-counts"), df.null_value_counts);
	ParseCountMap(yyjson_obj_get(df_val, "nan-value-counts"), df.nan_value_counts);
	// lower/upper bounds stored as-is; not yet acted on
	return df;
}

static UCScanDeleteFile ParseDeleteFile(duckdb_yyjson::yyjson_val *del_val) {
	UCScanDeleteFile df;
	string content = TryGetStrFromObject(del_val, "content");
	df.content = (content == "position-deletes") ? UCScanDeleteFileType::POSITION_DELETES
	                                             : UCScanDeleteFileType::EQUALITY_DELETES;
	df.file_path = TryGetStrFromObject(del_val, "file-path");
	df.file_format = TryGetStrFromObject(del_val, "file-format", false);
	df.file_size_in_bytes = (int64_t)TryGetNumFromObject(del_val, "file-size-in-bytes", false);
	df.record_count = (int64_t)TryGetNumFromObject(del_val, "record-count", false);
	auto *eq_ids = yyjson_obj_get(del_val, "equality-ids");
	if (eq_ids) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *id_val;
		yyjson_arr_foreach(eq_ids, idx, max, id_val) {
			df.equality_ids.push_back((uint32_t)duckdb_yyjson::yyjson_get_uint(id_val));
		}
	}
	auto *offset = yyjson_obj_get(del_val, "content-offset");
	if (offset) {
		df.content_offset = duckdb_yyjson::yyjson_get_sint(offset);
	}
	auto *csize = yyjson_obj_get(del_val, "content-size-in-bytes");
	if (csize) {
		df.content_size_in_bytes = duckdb_yyjson::yyjson_get_sint(csize);
	}
	return df;
}

// Parses the ScanTasks payload fields (delete-files, file-scan-tasks, plan-tasks,
// storage-credentials) from a JSON root into an existing result struct.
// Used by both ParseScanPlanResponse (completed path) and FetchScanTasks.
static void ParseScanTasksPayload(duckdb_yyjson::yyjson_val *root, UCScanPlanResult &result) {
	auto *del_files = yyjson_obj_get(root, "delete-files");
	if (del_files) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *del_val;
		yyjson_arr_foreach(del_files, idx, max, del_val) {
			result.delete_files.push_back(ParseDeleteFile(del_val));
		}
	}

	auto *tasks = yyjson_obj_get(root, "file-scan-tasks");
	if (tasks) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *task_val;
		yyjson_arr_foreach(tasks, idx, max, task_val) {
			UCScanPlanFileScanTask task;
			auto *df_val = yyjson_obj_get(task_val, "data-file");
			if (df_val) {
				task.data_file = ParseDataFile(df_val);
			}
			auto *refs = yyjson_obj_get(task_val, "delete-file-references");
			if (refs) {
				size_t r_idx, r_max;
				duckdb_yyjson::yyjson_val *ref_val;
				yyjson_arr_foreach(refs, r_idx, r_max, ref_val) {
					task.delete_file_references.push_back((idx_t)duckdb_yyjson::yyjson_get_uint(ref_val));
				}
			}
			// residual-filter decoded but not yet re-applied
			result.file_scan_tasks.push_back(std::move(task));
		}
	}

	auto *plan_tasks = yyjson_obj_get(root, "plan-tasks");
	if (plan_tasks) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *pt_val;
		yyjson_arr_foreach(plan_tasks, idx, max, pt_val) {
			const char *pt_str = duckdb_yyjson::yyjson_get_str(pt_val);
			if (pt_str) {
				result.plan_tasks.emplace_back(pt_str);
			}
		}
	}

	auto *storage_creds = yyjson_obj_get(root, "storage-credentials");
	if (storage_creds) {
		size_t idx, max;
		duckdb_yyjson::yyjson_val *cred_val;
		yyjson_arr_foreach(storage_creds, idx, max, cred_val) {
			string prefix = TryGetStrFromObject(cred_val, "prefix", false);
			unordered_map<string, string> config;
			auto *cfg = yyjson_obj_get(cred_val, "config");
			if (cfg) {
				size_t cidx, cmax;
				duckdb_yyjson::yyjson_val *key, *val;
				yyjson_obj_foreach(cfg, cidx, cmax, key, val) {
					const char *k = duckdb_yyjson::yyjson_get_str(key);
					const char *v = duckdb_yyjson::yyjson_get_str(val);
					if (k && v) {
						config.emplace(k, v);
					}
				}
			}
			result.storage_credentials.emplace_back(std::move(prefix), std::move(config));
		}
	}
}

static UCScanPlanResult ParseScanPlanResponse(const string &json_str) {
	YYJsonDoc doc(json_str);
	auto *root = doc.Root();
	if (!root) {
		throw IOException("Failed to parse scan plan response");
	}

	UCScanPlanResult result;
	// NOTE: checkin w/ DB -- should be uppers according to spec
	string status_str = StringUtil::Lower(TryGetStrFromObject(root, "status"));
	if (status_str == "completed") {
		result.status = UCScanPlanStatus::COMPLETED;
	} else if (status_str == "submitted") {
		result.status = UCScanPlanStatus::SUBMITTED;
	} else if (status_str == "failed") {
		result.status = UCScanPlanStatus::FAILED;
	} else if (status_str == "cancelled") {
		result.status = UCScanPlanStatus::CANCELLED;
	}
	result.plan_id = TryGetStrFromObject(root, "plan-id", false);

	if (result.status == UCScanPlanStatus::FAILED) {
		auto *error_obj = yyjson_obj_get(root, "error");
		if (error_obj) {
			result.error_message = TryGetStrFromObject(error_obj, "message", false);
			result.error_type = TryGetStrFromObject(error_obj, "type", false);
		}
		return result;
	}

	if (result.status != UCScanPlanStatus::COMPLETED) {
		return result;
	}

	ParseScanTasksPayload(root, result);
	return result;
}

// ---------------------------------------------------------------------------
// Scan plan API methods
// ---------------------------------------------------------------------------

static const char *UCScanPlanStatusToString(UCScanPlanStatus s) {
	switch (s) {
	case UCScanPlanStatus::COMPLETED:
		return "completed";
	case UCScanPlanStatus::SUBMITTED:
		return "submitted";
	case UCScanPlanStatus::FAILED:
		return "failed";
	case UCScanPlanStatus::CANCELLED:
		return "cancelled";
	default:
		return "unknown";
	}
}

UCScanPlanResult UCAPI::FetchPlanningResult(ClientContext &ctx, const string &catalog_name, const string &schema_name,
                                            const string &table_name, const string &plan_id,
                                            const UCCredentials &credentials, const string &scan_plan_endpoint,
                                            optional_ptr<int64_t> retry_after_ms_out) {
	string url = scan_plan_endpoint + "/v1/catalogs/" + catalog_name + "/namespaces/" + schema_name + "/tables/" +
	             table_name + "/plan/" + plan_id;
	UC_LOG_DEBUG(ctx, "api-irc.FetchPlanningResult plan_id=%s", plan_id);
	auto resp = MakeRequestResp(ctx, url, credentials.token);
	if (retry_after_ms_out) {
		*retry_after_ms_out = ParseRetryAfterMs(*resp);
	}
	auto result = ParseScanPlanResponse(resp->body);
	UC_LOG_DEBUG(ctx, "api-irc.FetchPlanningResult plan_id=%s -> status=%s inline=%zu plan_tasks=%zu delete=%zu%s%s",
	             plan_id, UCScanPlanStatusToString(result.status), result.file_scan_tasks.size(),
	             result.plan_tasks.size(), result.delete_files.size(),
	             result.status == UCScanPlanStatus::FAILED ? " error=" : "",
	             result.status == UCScanPlanStatus::FAILED ? result.error_message.c_str() : "");
	return result;
}

UCScanPlanResult UCAPI::PlanTableScan(ClientContext &ctx, const string &catalog_name, const string &schema_name,
                                      const string &table_name, const UCCredentials &credentials,
                                      const string &scan_plan_endpoint, const string &filter_json) {
	string url = scan_plan_endpoint + "/v1/catalogs/" + catalog_name + "/namespaces/" + schema_name + "/tables/" +
	             table_name + "/plan";
	// TODO: remove hard coded case-sensitive?
	string body = filter_json.empty() ? "{\"case-sensitive\":false}"
	                                  : "{\"case-sensitive\":false,\"filter\":" + filter_json + "}";
	UC_LOG_DEBUG(ctx, "api-irc.PlanTableScan %s.%s.%s filter=%s", catalog_name, schema_name, table_name,
	             filter_json.empty() ? "(none)" : filter_json);
	auto resp = MakeRequestResp(ctx, url, credentials.token, body);
	int64_t retry_after_ms = ParseRetryAfterMs(*resp);
	auto result = ParseScanPlanResponse(resp->body);
	UC_LOG_DEBUG(ctx, "api-irc.PlanTableScan %s.%s.%s -> status=%s plan_id=%s inline=%zu plan_tasks=%zu delete=%zu%s%s",
	             catalog_name, schema_name, table_name, UCScanPlanStatusToString(result.status), result.plan_id.c_str(),
	             result.file_scan_tasks.size(), result.plan_tasks.size(), result.delete_files.size(),
	             result.status == UCScanPlanStatus::FAILED ? " error=" : "",
	             result.status == UCScanPlanStatus::FAILED ? result.error_message.c_str() : "");

	// Poll fetchPlanningResult until a terminal status (completed/failed/cancelled). No fixed
	// iteration cap — a legitimately slow server-side plan must not be cut off (the old
	// 20x500ms=10s cap spuriously failed large tables); a total wall-clock budget is only a
	// backstop against a wedged server. Each sleep honors the server's Retry-After when present,
	// else backs off 100ms->1s, and is chunked so a query cancel lands within ~100ms. On abort
	// (cancel / budget exhaustion / mid-poll error) the plan is best-effort DELETEd.
	constexpr int64_t POLL_BUDGET_MS = 5 * 60 * 1000;
	constexpr int64_t POLL_MIN_SLEEP_MS = 100;
	constexpr int64_t POLL_MAX_SLEEP_MS = 1000;
	constexpr int64_t POLL_MAX_RETRY_AFTER_MS = 60 * 1000; // clamp a hostile Retry-After
	int64_t backoff_ms = POLL_MIN_SLEEP_MS;
	int64_t elapsed_ms = 0;
	try {
		while (result.status == UCScanPlanStatus::SUBMITTED) {
			if (elapsed_ms >= POLL_BUDGET_MS) {
				throw IOException("scan-plan: planning for '%s.%s.%s' (plan_id=%s) did not reach a terminal "
				                  "status within %lld s",
				                  catalog_name, schema_name, table_name, result.plan_id,
				                  (long long)(POLL_BUDGET_MS / 1000));
			}
			int64_t sleep_ms = retry_after_ms >= 0 ? retry_after_ms : backoff_ms;
			sleep_ms = MinValue<int64_t>(MaxValue<int64_t>(sleep_ms, POLL_MIN_SLEEP_MS), POLL_MAX_RETRY_AFTER_MS);
			for (int64_t slept = 0; slept < sleep_ms; slept += POLL_MIN_SLEEP_MS) {
				ctx.InterruptCheck(); // land a cancel within ~100ms rather than blocking the whole sleep
				std::this_thread::sleep_for(
				    std::chrono::milliseconds(MinValue<int64_t>(POLL_MIN_SLEEP_MS, sleep_ms - slept)));
			}
			elapsed_ms += sleep_ms;
			retry_after_ms = -1;
			result = FetchPlanningResult(ctx, catalog_name, schema_name, table_name, result.plan_id, credentials,
			                             scan_plan_endpoint, &retry_after_ms);
			backoff_ms = MinValue<int64_t>(backoff_ms * 2, POLL_MAX_SLEEP_MS);
		}
	} catch (...) {
		CancelPlan(ctx, scan_plan_endpoint, catalog_name, schema_name, table_name, result.plan_id, credentials.token);
		throw;
	}

	return result;
}

UCScanPlanResult UCAPI::FetchScanTasks(ClientContext &ctx, const string &catalog_name, const string &schema_name,
                                       const string &table_name, const string &plan_task,
                                       const UCCredentials &credentials, const string &scan_plan_endpoint) {
	string url = scan_plan_endpoint + "/v1/catalogs/" + catalog_name + "/namespaces/" + schema_name + "/tables/" +
	             table_name + "/tasks";
	// Body is {"plan-task":"<token>"}; token is opaque so we escape it as a JSON string manually.
	string escaped;
	for (char c : plan_task) {
		if (c == '"' || c == '\\') {
			escaped += '\\';
		}
		escaped += c;
	}
	string body = "{\"plan-task\":\"" + escaped + "\"}";
	UC_LOG_DEBUG(ctx, "api-irc.FetchScanTasks %s.%s.%s token=%s", catalog_name, schema_name, table_name, plan_task);
	auto resp = MakeRequest(ctx, url, credentials.token, body);

	// FetchScanTasksResult is a bare ScanTasks object — no status field.
	YYJsonDoc doc(resp);
	auto *root = doc.Root();
	if (!root) {
		throw IOException("Failed to parse fetchScanTasks response from '%s'", url);
	}
	UCScanPlanResult result;
	result.status = UCScanPlanStatus::COMPLETED;
	ParseScanTasksPayload(root, result);
	UC_LOG_DEBUG(ctx, "api-irc.FetchScanTasks %s.%s.%s token=%s -> inline=%zu plan_tasks=%zu delete=%zu", catalog_name,
	             schema_name, table_name, plan_task, result.file_scan_tasks.size(), result.plan_tasks.size(),
	             result.delete_files.size());
	return result;
}

} // namespace duckdb
