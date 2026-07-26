#include "storage/unity_catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_table_entry.hpp"
#include "storage/uc_table_set.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/main/database.hpp"

#include "uc_api.hpp"
#include "uc_multi_file_list.hpp"
#include "uc_irc_expression.hpp"

#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

// ---------------------------------------------------------------------------
// UCScanPlanBindData
//
// Holds pre-pushdown metadata (set at GetScanFunction time) and the
// post-pushdown parquet delegate state (filled by pushdown_complex_filter).
// ---------------------------------------------------------------------------

struct UCScanPlanBindData : public FunctionData {
	// Pre-pushdown: table identity and credentials (set at GetScanFunction time)
	string catalog_name;
	string schema_name;
	string table_name;
	string storage_location;
	UCCredentials credentials;
	string scan_plan_endpoint;

	// Post-pushdown: parquet delegate (filled by pushdown_complex_filter)
	bool scan_plan_done = false;
	unique_ptr<FunctionData> parquet_bind_data;
	table_function_init_global_t parquet_init_global = nullptr;
	table_function_init_local_t parquet_init_local = nullptr;
	table_function_t parquet_scan_fn = nullptr;

	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("UCScanPlanBindData::Copy");
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

// ---------------------------------------------------------------------------
// UCScanPlanTableFunction callbacks
// ---------------------------------------------------------------------------

// pushdown_complex_filter: called by DuckDB's FilterPushdown optimizer (even with an empty
// filter list, so it also handles full-table scans).  Serializes filters to IRC JSON, calls
// PlanTableScan, builds a UCMultiFileList from the result, and binds parquet_scan via
// MultiFileBindInternal — a single path for both greedy (inline files) and lazy (plan-tasks).
static void UCScanPlanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                     vector<unique_ptr<Expression>> &filters) {
	auto &bd = reinterpret_cast<UCScanPlanBindData &>(*bind_data_p);
	if (bd.scan_plan_done) {
		return;
	}
	try {
		string filter_json = SerializeFiltersToIRC(filters);
		auto plan = UCAPI::PlanTableScan(context, bd.catalog_name, bd.schema_name, bd.table_name, bd.credentials,
		                                 bd.scan_plan_endpoint, filter_json);

		if (plan.status == UCScanPlanStatus::COMPLETED) {
			// Wire in scan-plan credentials before binding parquet so that httpfs can authenticate.
			// Maps Iceberg/S3 config -> DuckDB Secret
			if (!plan.storage_credentials.empty()) {
				auto &secret_manager = SecretManager::Get(context);
				for (idx_t i = 0; i < plan.storage_credentials.size(); i++) {
					const string &prefix = plan.storage_credentials[i].first;
					const unordered_map<string, string> &cfg = plan.storage_credentials[i].second;
					auto get_cfg = [&cfg](const string &k) {
						auto it = cfg.find(k);
						return it != cfg.end() ? it->second : string();
					};
					CreateSecretInput sec;
					sec.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
					sec.persist_type = SecretPersistType::TEMPORARY;
					sec.name = Identifier("__internal_uc_scanplan__" + bd.catalog_name + "__" + bd.schema_name + "__" +
					                      bd.table_name + "__" + to_string(i));
					sec.type = "s3";
					sec.provider = "config";
					sec.options = {
					    {"key_id", get_cfg("s3.access-key-id")},
					    {"secret", get_cfg("s3.secret-access-key")},
					    {"session_token", get_cfg("s3.session-token")},
					    {"region", get_cfg("client.region")},
					};
					sec.scope = {prefix};
					secret_manager.CreateSecret(context, sec);
				}
			}

			// Build UCMultiFileList and stage it in the thread-local so UCMultiFileReaderFactory
			// can pick it up when parquet_scan's bind calls get_multi_file_reader.
			tl_uc_file_list = make_shared_ptr<UCMultiFileList>(context, plan, bd.credentials, bd.catalog_name,
			                                                   bd.schema_name, bd.table_name, bd.scan_plan_endpoint);

			auto &sys_cat = Catalog::GetSystemCatalog(context);
			auto &parquet_entry = sys_cat.GetEntry<TableFunctionCatalogEntry>(
			    context, QualifiedName({Identifier(DEFAULT_SCHEMA)}, Identifier("parquet_scan")));
			auto parquet_fn = parquet_entry.functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
			parquet_fn.get_multi_file_reader = UCMultiFileReaderFactory;

			string scan_path = bd.storage_location.empty() ? "uc://scan_plan" : bd.storage_location;
			vector<Value> inputs = {Value(scan_path)};
			named_parameter_map_t named_params;
			vector<LogicalType> input_table_types;
			vector<Identifier> input_table_names;
			TableFunctionRef dummy_ref;
			vector<LogicalType> return_types;
			vector<string> return_names;
			TableFunctionBindInput bind_input(inputs, named_params, input_table_types, input_table_names, nullptr,
			                                  nullptr, parquet_fn, dummy_ref);
			bd.parquet_bind_data = parquet_fn.bind(context, bind_input, return_types, return_names);
			// tl_uc_file_list was moved by UCMultiFileReaderFactory during bind; now null.

			if (parquet_fn.get_virtual_columns) {
				parquet_fn.get_virtual_columns(context, bd.parquet_bind_data.get());
			}
			bd.parquet_init_global = parquet_fn.init_global;
			bd.parquet_init_local = parquet_fn.init_local;
			bd.parquet_scan_fn = parquet_fn.function;
			bd.scan_plan_done = true;
			// Filters intentionally NOT cleared: DuckDB will add a Filter operator that applies
			// the original predicates, which subsumes any per-file residual the server returned.
			// Alternative: parse UCScanPlanFileScanTask::residual_filter_json back into DuckDB
			// expressions and apply them per-file; avoids redundant row-level work but requires
			// bidirectional IRC ↔ DuckDB expression translation.
			return;
		}
	} catch (const InterruptException &) {
		throw; // query cancel/timeout is not a scan-plan failure — must not be wrapped or trigger fallback
	} catch (std::exception &e) {
		// TODO: distinguish "feature not available for this caller" from transient errors.
		// HTTP 405 confirmed as "not enabled" status from live endpoint.
		// On a feature-unavailable response, set a per-UnityCatalog atomic flag
		// (AVAILABLE/UNAVAILABLE, checked in GetScanPlanEndpoint) so all subsequent queries on
		// this attach silently fall back to the Delta path without retrying.  Transient errors
		// (5xx, network) must NOT set UNAVAILABLE — propagate per-query and allow retry.
		// Granularity: per-ATTACH (per UnityCatalog instance) — availability is per-caller.
		throw IOException("UC scan plan API call failed for table '%s': %s", bd.table_name, e.what());
	}
}

// Advertise virtual columns so DuckDB uses COLUMN_IDENTIFIER_EMPTY (not ROW_ID) for
// count(*).  Parquet and delta both understand EMPTY; neither handles ROW_ID.
static virtual_column_map_t UCScanPlanGetVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	virtual_column_map_t result;
	result.insert(
	    make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILENAME, TableColumn("filename", LogicalType::VARCHAR)));
	result.insert(
	    make_pair(MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX, TableColumn("file_index", LogicalType::UBIGINT)));
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	return result;
}

static unique_ptr<GlobalTableFunctionState> UCScanPlanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bd = reinterpret_cast<const UCScanPlanBindData &>(*input.bind_data);
	D_ASSERT(bd.scan_plan_done);
	TableFunctionInitInput parquet_input(bd.parquet_bind_data.get(), input.column_ids, input.projection_ids,
	                                     input.filters);
	return bd.parquet_init_global(context, parquet_input);
}

static unique_ptr<LocalTableFunctionState> UCScanPlanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
	auto &bd = reinterpret_cast<const UCScanPlanBindData &>(*input.bind_data);
	D_ASSERT(bd.scan_plan_done);
	TableFunctionInitInput parquet_input(bd.parquet_bind_data.get(), input.column_ids, input.projection_ids,
	                                     input.filters);
	return bd.parquet_init_local(context, parquet_input, global_state);
}

static void UCScanPlanScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bd = reinterpret_cast<const UCScanPlanBindData &>(*data.bind_data);
	D_ASSERT(bd.scan_plan_done);
	TableFunctionInput parquet_input(bd.parquet_bind_data.get(), data.local_state, data.global_state);
	bd.parquet_scan_fn(context, parquet_input, output);
}

static TableFunction MakeUCScanPlanTableFunction() {
	TableFunction func("uc_scan_plan", {}, nullptr);
	func.pushdown_complex_filter = UCScanPlanPushdownFilter;
	func.init_global = UCScanPlanInitGlobal;
	func.init_local = UCScanPlanInitLocal;
	func.function = UCScanPlanScan;
	func.get_virtual_columns = UCScanPlanGetVirtualColumns;
	func.filter_pushdown = true;
	func.projection_pushdown = true;
	return func;
}

// ---------------------------------------------------------------------------
// UCTableEntry
// ---------------------------------------------------------------------------

UCTableEntry::UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, TableInformation &table, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info), table(table) {
	this->internal = false;
}

unique_ptr<BaseStatistics> UCTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

void UCTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &, LogicalProjection &, LogicalUpdate &,
                                         ClientContext &) {
	throw NotImplementedException("BindUpdateConstraints");
}

TableFunction UCTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	throw InternalException("UCTableEntry::GetScanFunction called without entry lookup info");
}

TableFunction UCTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                            const EntryLookupInfo &lookup_info) {
	auto &table_data = table.table_data;
	D_ASSERT(table_data);

	// --- Scan plan path (try first when a scan plan endpoint is configured) ---
	auto scan_ep = table.catalog.GetScanPlanEndpoint();
	if (!scan_ep.empty()) {
		table.RefreshCredentials(context);
		auto bd = make_uniq<UCScanPlanBindData>();
		bd->catalog_name = table_data->catalog_name;
		bd->schema_name = table_data->schema_name;
		bd->table_name = table_data->name;
		bd->storage_location = table_data->storage_location;
		bd->credentials = table.catalog.credentials;
		bd->scan_plan_endpoint = scan_ep;
		bind_data = std::move(bd);
		return MakeUCScanPlanTableFunction();
	}

	// --- Delta path (unchanged fallback) ---
	if (table_data->data_source_format != "DELTA") {
		throw NotImplementedException("Table '%s' is of unsupported format '%s', ", table_data->name,
		                              table_data->data_source_format);
	}

	table.RefreshCredentials(context);
	table.InternalAttach(context);

	auto &delta_catalog = *table.GetInternalCatalog();
	auto &schema = delta_catalog.GetSchema(context, Identifier::DefaultSchema());
	auto transaction = schema.GetCatalogTransaction(context);
	auto table_entry = schema.LookupEntry(transaction, lookup_info);
	D_ASSERT(table_entry);

	auto &delta_table = table_entry->Cast<TableCatalogEntry>();
	return delta_table.GetScanFunction(context, bind_data, lookup_info);
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
