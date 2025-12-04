#include "storage/uc_catalog.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_table_entry.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/common/mutex.hpp"
#include "uc_api.hpp"
#include <unordered_map>
#include <chrono>

namespace duckdb {

// Mutex map to synchronize secret creation per table_id
unordered_map<string, unique_ptr<mutex>> table_secret_mutexes;
mutex mutex_map_mutex;
// Track secret expiration times per table_id (Unix epoch timestamp in milliseconds)
unordered_map<string, int64_t> secret_expiration_times;

UCTableEntry::UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info) {
	this->internal = false;
}

UCTableEntry::UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, UCTableInfo &info)
    : TableCatalogEntry(catalog, schema, *info.create_info) {
	this->internal = false;
}

unique_ptr<BaseStatistics> UCTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

optional_ptr<Catalog> UCTableEntry::GetInternalCatalog() {
	return internal_attached_database->GetCatalog();
}

void UCTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &, LogicalProjection &, LogicalUpdate &,
                                         ClientContext &) {
	throw NotImplementedException("BindUpdateConstraints");
}

TableFunction UCTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto &db = DatabaseInstance::GetDatabase(context);

	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, DEFAULT_SCHEMA);
	auto catalog_entry = schema.GetEntry(data, CatalogType::TABLE_FUNCTION_ENTRY, "delta_scan");
	if (!catalog_entry) {
		throw InvalidInputException("Function with name \"%s\" not found in "
		                            "ExtensionLoader::GetTableFunction",
		                            name);
	}
	auto &delta_function_set = catalog_entry->Cast<TableFunctionCatalogEntry>();

	auto delta_scan_function = delta_function_set.functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
	auto &uc_catalog = catalog.Cast<UCCatalog>();

	D_ASSERT(table_data);

	if (table_data->data_source_format != "DELTA") {
		throw NotImplementedException("Table '%s' is of unsupported format '%s', ", table_data->name,
		                              table_data->data_source_format);
	}

	// Set the S3 path as input to table function
	vector<Value> inputs = {table_data->storage_location};

	if (table_data->storage_location.find("file://") != 0) {
		auto &secret_manager = SecretManager::Get(context);
		string secret_name = "__internal_uc_" + table_data->table_id;

		// Get or create mutex for this specific table_id to prevent concurrent secret creation
		mutex *table_mutex;
		{
			lock_guard<mutex> map_lock(mutex_map_mutex);
			auto it = table_secret_mutexes.find(table_data->table_id);
			if (it == table_secret_mutexes.end()) {
				table_secret_mutexes.emplace(table_data->table_id, make_uniq<mutex>());
				it = table_secret_mutexes.find(table_data->table_id);
			}
			table_mutex = it->second.get();
		}

		// Lock this specific table's secret creation to prevent concurrent writes
		lock_guard<mutex> secret_lock(*table_mutex);

		// Check if secret exists and is still valid (not expired)
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto existing_secret = secret_manager.GetSecretByName(transaction, secret_name, "memory");

		bool needs_refresh = true;
		if (existing_secret) {
			// Check expiration time if we have it cached
			auto it = secret_expiration_times.find(table_data->table_id);
			if (it != secret_expiration_times.end() && it->second > 0) {
				// Get current time in milliseconds (Unix epoch timestamp)
				auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()
				).count();

				// Calculate time remaining until expiration (in milliseconds)
				int64_t time_remaining_ms = it->second - now_ms;

				// Refresh if expired or within 5 minutes of expiration (safety margin)
				// 5 minutes = 300000 milliseconds
				if (time_remaining_ms > 300000) {
					needs_refresh = false;
				}
			}
		}

		if (needs_refresh) {
			// Get fresh credentials from UCAPI (includes expiration_time)
			auto table_credentials = UCAPI::GetTableCredentials(context, table_data->table_id, uc_catalog.credentials);

			// Cache expiration time for future checks
			if (table_credentials.expiration_time > 0) {
				secret_expiration_times[table_data->table_id] = table_credentials.expiration_time;
			}

			// Inject secret into secret manager scoped to this path
			CreateSecretInput input;
			input.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
			input.persist_type = SecretPersistType::TEMPORARY;
			input.name = secret_name;
			input.type = "s3";
			input.provider = "config";
			input.options = {
			    {"key_id", table_credentials.key_id},
			    {"secret", table_credentials.secret},
			    {"session_token", table_credentials.session_token},
			    {"region", uc_catalog.credentials.aws_region},
			};
			input.scope = {table_data->storage_location};

			secret_manager.CreateSecret(context, input);
		}
		// If secret exists and not expired, use cached secret
	}
	named_parameter_map_t param_map;
	vector<LogicalType> return_types;
	vector<string> names;
	TableFunctionRef empty_ref;

	TableFunctionBindInput bind_input(inputs, param_map, return_types, names, nullptr, nullptr, delta_scan_function,
	                                  empty_ref);

	auto result = delta_scan_function.bind(context, bind_input, return_types, names);
	bind_data = std::move(result);

	return delta_scan_function;
}

virtual_column_map_t UCTableEntry::GetVirtualColumns() const {
	//! FIXME: requires changes in core to be able to delegate this
	return TableCatalogEntry::GetVirtualColumns();
}

vector<column_t> UCTableEntry::GetRowIdColumns() const {
	//! FIXME: requires changes in core to be able to delegate this
	return TableCatalogEntry::GetRowIdColumns();
}

TableStorageInfo UCTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	// TODO fill info
	return result;
}

} // namespace duckdb
