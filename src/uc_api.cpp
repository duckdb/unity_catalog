#include "uc_api.hpp"
#include "storage/uc_catalog.hpp"
#include "yyjson.hpp"
#include <curl/curl.h>
#include <sys/stat.h>

namespace duckdb {

//! We use a global here to store the path that is selected on the
//! UCAPI::InitializeCurl call
static string SELECTED_CURL_CERT_PATH = "";

static size_t GetRequestWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
	((std::string *)userp)->append((char *)contents, size * nmemb);
	return size * nmemb;
}

// we statically compile in libcurl, which means the cert file location of the
// build machine is the place curl will look. But not every distro has this file
// in the same location, so we search a number of common locations and use the
// first one we find.
static string certFileLocations[] = {
    // Arch, Debian-based, Gentoo
    "/etc/ssl/certs/ca-certificates.crt",
    // RedHat 7 based
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    // Redhat 6 based
    "/etc/pki/tls/certs/ca-bundle.crt",
    // OpenSUSE
    "/etc/ssl/ca-bundle.pem",
    // Alpine
    "/etc/ssl/cert.pem"};

// Look through the the above locations and if one of the files exists, set that
// as the location curl should use.
static bool SelectCurlCertPath() {
	for (string &caFile : certFileLocations) {
		struct stat buf;
		if (stat(caFile.c_str(), &buf) == 0) {
			SELECTED_CURL_CERT_PATH = caFile;
		}
	}
	return false;
}

static bool SetCurlCAFileInfo(CURL *curl) {
	if (!SELECTED_CURL_CERT_PATH.empty()) {
		curl_easy_setopt(curl, CURLOPT_CAINFO, SELECTED_CURL_CERT_PATH.c_str());
		return true;
	}
	return false;
}

// Note: every curl object we use should set this, because without it some linux
// distro's may not find the CA certificate.
static void InitializeCurlObject(CURL *curl, const string &token) {
	if (!token.empty()) {
		curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, token.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
	}
	SetCurlCAFileInfo(curl);
}

static string GetRequest(const string &url, const string &token = "") {
	CURL *curl;
	CURLcode res;
	string readBuffer;

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, GetRequestWriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
		InitializeCurlObject(curl, token);
		res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);

		if (res != CURLcode::CURLE_OK) {
			string error = curl_easy_strerror(res);
			throw IOException("Curl Request to '%s' failed with error: '%s'", url, error);
		}
		return readBuffer;
	}
	throw InternalException("Failed to initialize curl");
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

static string GetCredentialsRequest(const string &url, const string &table_id, const string &token = "") {
	CURL *curl;
	CURLcode res;
	string readBuffer;

	string body = StringUtil::Format(R"({"table_id" : "%s", "operation" : "READ_WRITE"})", table_id);

	curl = curl_easy_init();
	if (!curl) {
		throw InternalException("Failed to initialize curl");
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, GetRequestWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

	// Set headers
	struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	// Set request body
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());

	InitializeCurlObject(curl, token);

	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLcode::CURLE_OK) {
		string error = curl_easy_strerror(res);
		throw IOException("Curl Request to '%s' failed with error: '%s'\n'%s'", url, error, readBuffer);
	}
	return readBuffer;
}

void UCAPI::InitializeCurl() {
	SelectCurlCertPath();
}

//# list catalogs
//    echo "List of catalogs"
//    curl --request GET
//    "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/catalogs" \
// 	--header "Authorization: Bearer ${TOKEN}" | jq .
//
//# list short version of all tables
//    echo "Table Summaries"
//    curl --request GET
//    "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/table-summaries?catalog_name=workspace"
//    \
// 	--header "Authorization: Bearer ${TOKEN}" | jq .
//
//# list tables in `default` schema
//    echo "Tables in default schema"
//    curl --request GET
//    "https://${DATABRICKS_HOST}/api/2.1/unity-catalog/tables?catalog_name=workspace&schema_name=default"
//    \
// 	--header "Authorization: Bearer ${TOKEN}" | jq .

static string GetDefaultSchemaRequest(const UCCredentials &credentials) {
	CURL *curl;
	CURLcode res;
	string readBuffer;

	curl = curl_easy_init();
	if (!curl) {
		throw InternalException("Failed to initialize curl");
	}
	auto url = credentials.endpoint + "/api/2.0/settings/types/default_namespace_ws/names/default";
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, GetRequestWriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

	// Set headers
	struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	InitializeCurlObject(curl, credentials.token);

	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLcode::CURLE_OK) {
		string error = curl_easy_strerror(res);
		throw IOException("Curl Request to '%s' failed with error: '%s'\n'%s'", url, error, readBuffer);
	}
	return readBuffer;
}

string UCAPI::GetDefaultSchema(const UCCredentials &credentials) {
	auto api_result = GetDefaultSchemaRequest(credentials);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(api_result.c_str(), api_result.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

	auto error = CheckError(root);
	if (error.HasError()) {
		error.ThrowError("Failed to get default schema of the catalog");
	}

	auto setting_name = TryGetStrFromObject(root, "setting_name");
	if (setting_name.empty()) {
		throw InvalidInputException("Failed to get default schema of the catalog, "
		                            "API response is invalid!");
	}

	return setting_name;
}

UCAPITableCredentials UCAPI::GetTableCredentials(const string &table_id, const UCCredentials &credentials) {
	UCAPITableCredentials result;

	auto api_result = GetCredentialsRequest(credentials.endpoint + "/api/2.1/unity-catalog/temporary-table-credentials",
	                                        table_id, credentials.token);

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

	return result;
}

vector<string> UCAPI::GetCatalogs(const string &catalog, const UCCredentials &credentials) {
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

vector<UCAPITable> UCAPI::GetTables(const string &catalog, const string &schema, const UCCredentials &credentials) {
	vector<UCAPITable> result;
	auto api_result = GetRequest(credentials.endpoint + "/api/2.1/unity-catalog/tables?catalog_name=" + catalog +
	                                 "&schema_name=" + schema,
	                             credentials.token);

	// Read JSON and get root
	duckdb_yyjson::yyjson_doc *doc = duckdb_yyjson::yyjson_read(api_result.c_str(), api_result.size(), 0);
	duckdb_yyjson::yyjson_val *root = yyjson_doc_get_root(doc);

	// Get root["hits"], iterate over the array
	auto *tables = yyjson_obj_get(root, "tables");
	size_t idx, max;
	duckdb_yyjson::yyjson_val *table;
	yyjson_arr_foreach(tables, idx, max, table) {
		UCAPITable table_result;
		table_result.catalog_name = catalog;
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

vector<UCAPISchema> UCAPI::GetSchemas(const string &catalog, const UCCredentials &credentials) {
	vector<UCAPISchema> result;

	auto api_result =
	    GetRequest(credentials.endpoint + "/api/2.1/unity-catalog/schemas?catalog_name=" + catalog, credentials.token);

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
		schema_result.catalog_name = catalog;

		result.push_back(schema_result);
	}

	return result;
}

} // namespace duckdb
