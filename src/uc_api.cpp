#include <cstddef>
#include <sys/stat.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "uc_api.hpp"
#include "storage/uc_catalog.hpp"
#include "yyjson.hpp"

namespace duckdb {

static void AuthenticateViaBearerToken(HTTPHeaders &hdrs, const string &token) {
	if (!token.empty()) {
		hdrs.Insert("Authorization", "Bearer " + token);
		// curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
	}
}

static void EnsureHttpfsExtension(shared_ptr<DatabaseInstance> db) {
	// autoloading/requiring HTTPFS at this ext's Load time fails, iceberg does the same deferred load
	if (!db) {
		throw InvalidConfigurationException("Context does not have database instance");
	}
	ExtensionHelper::AutoLoadExtension(*db, "httpfs");
	if (!db->ExtensionIsLoaded("httpfs")) {
		throw MissingExtensionException("The iceberg extension requires the httpfs extension to be loaded!");
	}
}

static string GetRequest(ClientContext &ctx, const string &url, const string &token = "") {
	auto db = ctx.db;
	EnsureHttpfsExtension(db);
	auto &http_util = HTTPUtil::Get(*db);
	auto params = http_util.InitializeParameters(*db, url);
	params->logger = ctx.logger;
	HTTPHeaders hdrs(*ctx.db);
	AuthenticateViaBearerToken(hdrs, token);
	GetRequestInfo req(url, hdrs, *params, nullptr, nullptr);
	auto resp = http_util.Request(req);

	if (!resp->Success()) {
		throw IOException("GET Request to '%s' failed: '%s'", url, resp->GetError());
	}
	return std::move(resp->body);
}

template <class TYPE, uint8_t TYPE_NUM, TYPE (*get_function)(duckdb_yyjson::yyjson_val *obj)>
static TYPE TemplatedTryGetYYJson(duckdb_yyjson::yyjson_val *obj, const string &field, TYPE default_val,
                                  bool fail_on_missing = true) {
	auto val = yyjson_obj_get(obj, field.c_str());
	if (val && yyjson_get_type(val) == TYPE_NUM) {
		return get_function(val);
	} else if (!fail_on_missing) {
		return default_val;
	}
	throw IOException("Invalid field found while parsing field: " + field);
}

static uint64_t TryGetNumFromObject(duckdb_yyjson::yyjson_val *obj, const string &field, bool fail_on_missing = true,
                                    uint64_t default_val = 0) {
	return TemplatedTryGetYYJson<uint64_t, YYJSON_TYPE_NUM, duckdb_yyjson::yyjson_get_uint>(obj, field, default_val,
	                                                                                        fail_on_missing);
}
static bool TryGetBoolFromObject(duckdb_yyjson::yyjson_val *obj, const string &field, bool fail_on_missing = false,
                                 bool default_val = false) {
	return TemplatedTryGetYYJson<bool, YYJSON_TYPE_BOOL, duckdb_yyjson::yyjson_get_bool>(obj, field, default_val,
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
		throw InvalidInputException("%s. error_code: %s, message: %s", prefix, error_code, message);
	}

private:
	string error_code;
	string message;
};

} // namespace

static UCAPIError CheckError(duckdb_yyjson::yyjson_val *api_result) {
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

static string GetCredentialsRequest(ClientContext &ctx, const string &url, const string &table_id,
                                    const string &token = "") {
	auto db = ctx.db;
	auto &http_util = HTTPUtil::Get(*db);
	auto params = http_util.InitializeParameters(*db, url);

	string body = StringUtil::Format(R"({"table_id" : "%s", "operation" : "READ_WRITE"})", table_id);
	HTTPHeaders hdrs(*db);
	hdrs.Insert("Content-Type", "application/json");
	AuthenticateViaBearerToken(hdrs, token);

	params->logger = ctx.logger;
	PostRequestInfo req(url, hdrs, *params, reinterpret_cast<const_data_ptr_t>(body.c_str()), body.length());
	auto resp = http_util.Request(req);

	if (!resp->Success()) {
		throw IOException("POST Request to '%s' failed: '%s'", url, resp->GetError());
	}
	// Ugh. actual response body not in resp->body, but in req.buffer_out
	return std::move(req.buffer_out);
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
	auto url = credentials.endpoint + "/api/2.0/settings/types/default_namespace_ws/names/default";
	auto resp = GetRequest(ctx, url, credentials.token);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(resp.c_str(), resp.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

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

UCAPITableCredentials UCAPI::GetTableCredentials(ClientContext &ctx, const string &table_id,
                                                 const UCCredentials &credentials) {
	UCAPITableCredentials result;

	auto url = credentials.endpoint + "/api/2.1/unity-catalog/temporary-table-credentials";
	auto api_result = GetCredentialsRequest(ctx, url, table_id, credentials.token);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(api_result.c_str(), api_result.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError(StringUtil::Format("Failed to get table credentials for table_id: %s", table_id));
	}

	auto *aws_temp_credentials = yyjson_obj_get(root, "aws_temp_credentials");
	if (aws_temp_credentials) {
		result.key_id = TryGetStrFromObject(aws_temp_credentials, "access_key_id");
		result.secret = TryGetStrFromObject(aws_temp_credentials, "secret_access_key");
		result.session_token = TryGetStrFromObject(aws_temp_credentials, "session_token");
	}
	
	// Parse expiration_time if available (works for AWS credentials)
	// API returns expiration_time as Unix epoch timestamp in milliseconds
	uint64_t expiration_ms = TryGetNumFromObject(root, "expiration_time", false, 0);
	result.expiration_time = expiration_ms; // Store in milliseconds for direct comparison

	return result;
}

vector<string> UCAPI::GetCatalogs(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials) {
	throw NotImplementedException("UCAPI::GetCatalogs");
}

static UCAPIColumnDefinition ParseColumnDefinition(duckdb_yyjson::yyjson_val *column_def) {
	UCAPIColumnDefinition result;

	result.name = TryGetStrFromObject(column_def, "name");
	result.type_text = TryGetStrFromObject(column_def, "type_text");
	result.precision = TryGetNumFromObject(column_def, "type_precision");
	result.scale = TryGetNumFromObject(column_def, "type_scale");
	result.position = TryGetNumFromObject(column_def, "position");

	return result;
}

vector<UCAPITable> UCAPI::GetTables(ClientContext &ctx, Catalog &catalog, const string &schema,
                                    const UCCredentials &credentials) {
	vector<UCAPITable> result;
	auto url = credentials.endpoint + "/api/2.1/unity-catalog/tables?catalog_name=" + catalog.GetDBPath() +
	           "&schema_name=" + schema;
	auto api_result = GetRequest(ctx, url, credentials.token);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(api_result.c_str(), api_result.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

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

		result.push_back(table_result);
	}

	return result;
}

vector<UCAPISchema> UCAPI::GetSchemas(ClientContext &ctx, Catalog &catalog, const UCCredentials &credentials) {
	vector<UCAPISchema> result;
	auto url = credentials.endpoint + "/api/2.1/unity-catalog/schemas?catalog_name=" + catalog.GetDBPath();
	auto api_result = GetRequest(ctx, url, credentials.token);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(api_result.c_str(), api_result.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

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

	return result;
}

} // namespace duckdb
