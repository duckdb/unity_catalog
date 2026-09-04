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
#include "uc_logging.hpp"
#include "uc_multi_file_list.hpp"
#include "uc_irc_expression.hpp"

#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

// Table identity and the fallback context, fixed at GetScanFunction time. The only mutable field is
// filter_json, which the optimizer fills in if it runs; everything derived from the scan plan lives
// in UCScanPlanGlobalState instead, so it is rebuilt per execution rather than baked into a plan
// that may be cached and replayed against expired credentials.
struct UCScanPlanBindData : public FunctionData {
	string catalog_name;
	string schema_name;
	string table_name;
	string storage_location;
	UCCredentials credentials;
	string scan_plan_endpoint;
	// Handles to live catalog state, not to plan state: this struct being const says nothing about
	// what they point at, and optional_ptr (unlike a raw pointer) would propagate the const.
	mutable optional_ptr<UnityCatalog> uc_catalog;
	mutable optional_ptr<TableInformation> table; // for the Delta fallback when /plan fails
	unique_ptr<EntryLookupInfo> lookup_info;

	//! Serialized WHERE predicates. Empty is a valid plan request meaning "no server-side pruning",
	//! which is exactly what happens when the filter-pushdown optimizer doesn't run.
	string filter_json;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<UCScanPlanBindData>();
		result->catalog_name = catalog_name;
		result->schema_name = schema_name;
		result->table_name = table_name;
		result->storage_location = storage_location;
		result->credentials = credentials;
		result->scan_plan_endpoint = scan_plan_endpoint;
		result->uc_catalog = uc_catalog; // non-owning handles: copy the view, not the object
		result->table = table;
		if (lookup_info) {
			result->lookup_info = make_uniq<EntryLookupInfo>(*lookup_info);
		}
		result->filter_json = filter_json;
		return std::move(result);
	}
	// Conservative on purpose: reporting "never equal" only ever costs a missed plan reuse, whereas
	// a wrong "equal" would share a plan between two different tables.
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

// One scan plan and the inner scan it feeds: parquet over the planned files, or the Delta scan when
// /plan failed.
struct UCScanPlanGlobalState : public GlobalTableFunctionState {
	unique_ptr<FunctionData> inner_bind_data;
	unique_ptr<GlobalTableFunctionState> inner_global;
	table_function_init_local_t inner_init_local = nullptr;
	table_function_t inner_scan = nullptr;
	table_function_progress_t inner_progress = nullptr;

	idx_t MaxThreads() const override {
		return inner_global ? inner_global->MaxThreads() : 1;
	}
};

// Adopt `fn` (already bound into gstate.inner_bind_data) as the inner scan. Both the scan-plan and
// Delta-fallback paths go through here so neither can forget one of the pointers.
static void AdoptInnerScan(ClientContext &context, UCScanPlanGlobalState &gstate, const TableFunction &fn,
                           TableFunctionInitInput &input) {
	if (fn.get_virtual_columns) {
		fn.get_virtual_columns(context, gstate.inner_bind_data.get());
	}
	gstate.inner_init_local = fn.init_local;
	gstate.inner_scan = fn.function;
	gstate.inner_progress = fn.table_scan_progress;
	TableFunctionInitInput inner_input(gstate.inner_bind_data.get(), input.column_ids, input.projection_ids,
	                                   input.filters);
	gstate.inner_global = fn.init_global(context, inner_input);
}

// The Delta read path: both the non-scan-plan branch and the /plan-failure fallback land here.
static TableFunction BuildDeltaScan(ClientContext &context, TableInformation &table, const EntryLookupInfo &lookup_info,
                                    unique_ptr<FunctionData> &bind_data) {
	table.RefreshCredentials(context);
	table.InternalAttach(context);
	auto &delta_catalog = *table.GetInternalCatalog();
	auto &schema = delta_catalog.GetSchema(context, Identifier::DefaultSchema());
	auto transaction = schema.GetCatalogTransaction(context);
	auto table_entry = schema.LookupEntry(transaction, lookup_info);
	if (!table_entry) {
		throw CatalogException("Table '%s' is registered in Unity Catalog at '%s', but no Delta table was found there",
		                       table.table_data->name, table.table_data->storage_location);
	}
	auto &delta_table = table_entry->Cast<TableCatalogEntry>();
	return delta_table.GetScanFunction(context, bind_data, lookup_info);
}

// ---------------------------------------------------------------------------
// UCScanPlanTableFunction callbacks
// ---------------------------------------------------------------------------

// The optimizer runs this only to capture the predicate; the scan plan itself happens in
// init_global, so a query that never reaches filter pushdown still scans correctly (it just gets
// no server-side pruning).
static void UCScanPlanPushdownFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                     vector<unique_ptr<Expression>> &filters) {
	auto &bd = bind_data_p->Cast<UCScanPlanBindData>();
	if (filters.empty()) {
		// The optimizer calls this a second time once the predicates have become table filters, with
		// nothing left to serialize. That call carries no information, so it must not overwrite what
		// the first one captured.
		return;
	}
	try {
		bd.filter_json = SerializeFiltersToIRC(filters);
	} catch (const std::exception &e) {
		// Serialization is best-effort: the filter is a pruning hint, so losing it costs speed, not
		// correctness, and must never fail the query.
		bd.filter_json.clear();
		UC_LOG_WARNING(context, "api-irc.PlanTableScan %s.%s.%s filter serialization failed, planning unfiltered: %s",
		               bd.catalog_name, bd.schema_name, bd.table_name, e.what());
	}
	// Filters are deliberately NOT cleared: DuckDB's own Filter operator re-applies them, which is
	// what makes a lossy server-side filter safe (design doc §2.3-2.4).
}

// Plan the scan and bind whatever will actually read it. This runs per execution, so a re-executed
// prepared statement re-plans rather than replaying a stale file list on expired credentials.
static unique_ptr<GlobalTableFunctionState> UCScanPlanInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bd = input.bind_data->Cast<UCScanPlanBindData>();
	auto gstate = make_uniq<UCScanPlanGlobalState>();

	// Only the /plan call belongs in this try: it is what the UNAVAILABLE latch describes. Widening
	// it would let one bad table (an S3 403, a corrupt footer) disable scan planning for the whole
	// catalog for 15 minutes.
	UCScanPlanResult plan;
	try {
		plan = UCAPI::PlanTableScan(context, bd.catalog_name, bd.schema_name, bd.table_name, bd.credentials,
		                            bd.scan_plan_endpoint, bd.filter_json);
		if (plan.status != UCScanPlanStatus::COMPLETED) {
			// A terminal non-completed status arrives as a 200 -- a value, not an exception -- so
			// without this the scan would proceed with no files.
			throw IOException("scan-plan: planning for '%s.%s.%s' returned status '%s'%s%s%s%s", bd.catalog_name,
			                  bd.schema_name, bd.table_name, UCScanPlanStatusToString(plan.status),
			                  plan.error_type.empty() ? "" : " type=", plan.error_type,
			                  plan.error_message.empty() ? "" : ": ", plan.error_message);
		}
	} catch (const InterruptException &) {
		throw; // query cancel/timeout is not a scan-plan failure — must not be wrapped or trigger fallback
	} catch (std::exception &e) {
		// Mark this attach's scan-plan endpoint UNAVAILABLE so later scans skip it (re-probed after
		// the wall-clock window; scan-plan-gating.md). For now every failure is treated the same
		// (404/405/5xx/network/non-completed status -- "treat like 404").
		if (bd.uc_catalog) {
			bd.uc_catalog->MarkScanPlanUnavailable();
		}
		UC_LOG_WARNING(context, "api-irc.PlanTableScan %s.%s.%s failed, falling back to Delta: %s", bd.catalog_name,
		               bd.schema_name, bd.table_name, e.what());
		// Fall back to Delta for THIS query. Needs a DELTA-format table and the fallback context
		// captured at bind time; else re-raise.
		if (!bd.table || !bd.lookup_info || bd.table->table_data->data_source_format != "DELTA") {
			throw IOException("UC scan plan API call failed for table '%s' and no Delta fallback is "
			                  "available: %s",
			                  bd.table_name, e.what());
		}
		auto delta_fn = BuildDeltaScan(context, *bd.table, *bd.lookup_info, gstate->inner_bind_data);
		AdoptInnerScan(context, *gstate, delta_fn, input);
		return std::move(gstate);
	}
	if (bd.uc_catalog) {
		bd.uc_catalog->MarkScanPlanAvailable();
	}

	// httpfs authenticates off secrets, so these must exist before parquet binds.
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

	tl_uc_file_list = make_shared_ptr<UCMultiFileList>(context, plan, bd.credentials, bd.catalog_name, bd.schema_name,
	                                                   bd.table_name, bd.scan_plan_endpoint);

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
	vector<Identifier> return_names;
	TableFunctionBindInput bind_input(inputs, named_params, input_table_types, input_table_names, nullptr, nullptr,
	                                  parquet_fn, dummy_ref);
	try {
		gstate->inner_bind_data = parquet_fn.bind(context, bind_input, return_types, return_names);
	} catch (...) {
		// The factory only consumes the thread-local once parquet's bind reaches
		// get_multi_file_reader; if bind threw before that, clear it so the list can't be handed to
		// an unrelated later bind on this thread.
		tl_uc_file_list.reset();
		throw;
	}

	AdoptInnerScan(context, *gstate, parquet_fn, input);
	return std::move(gstate);
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

static unique_ptr<LocalTableFunctionState> UCScanPlanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<UCScanPlanGlobalState>();
	TableFunctionInitInput inner_input(gstate.inner_bind_data.get(), input.column_ids, input.projection_ids,
	                                   input.filters);
	return gstate.inner_init_local(context, inner_input, gstate.inner_global.get());
}

static void UCScanPlanScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<UCScanPlanGlobalState>();
	TableFunctionInput inner_input(gstate.inner_bind_data.get(), data.local_state, gstate.inner_global.get());
	gstate.inner_scan(context, inner_input, output);
}

// No cardinality callback: it is queried during optimization, before any plan exists, so it could
// only ever return "unknown".
static double UCScanPlanProgress(ClientContext &context, const FunctionData *bind_data,
                                 const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return -1;
	}
	auto &gstate = global_state->Cast<UCScanPlanGlobalState>();
	if (!gstate.inner_progress) {
		return -1;
	}
	return gstate.inner_progress(context, gstate.inner_bind_data.get(), gstate.inner_global.get());
}

static TableFunction MakeUCScanPlanTableFunction() {
	TableFunction func("uc_scan_plan", {}, nullptr);
	func.pushdown_complex_filter = UCScanPlanPushdownFilter;
	func.init_global = UCScanPlanInitGlobal;
	func.init_local = UCScanPlanInitLocal;
	func.function = UCScanPlanScan;
	func.get_virtual_columns = UCScanPlanGetVirtualColumns;
	func.table_scan_progress = UCScanPlanProgress;
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

	// Opt-in, and skipped while this endpoint is known-unavailable (docs/sp/scan-plan-gating.md).
	if (table.catalog.ShouldTryScanPlan()) {
		table.RefreshCredentials(context);
		auto bd = make_uniq<UCScanPlanBindData>();
		bd->catalog_name = table_data->catalog_name;
		bd->schema_name = table_data->schema_name;
		bd->table_name = table_data->name;
		bd->storage_location = table_data->storage_location;
		bd->credentials = table.catalog.credentials;
		bd->scan_plan_endpoint = table.catalog.GetIRCEndpoint();
		bd->uc_catalog = &table.catalog;
		// Capture the Delta-fallback context so the pushdown can fall back if /plan fails.
		bd->table = &table;
		bd->lookup_info = make_uniq<EntryLookupInfo>(lookup_info);
		bind_data = std::move(bd);
		return MakeUCScanPlanTableFunction();
	}

	// --- Delta path ---
	if (table_data->data_source_format != "DELTA") {
		throw NotImplementedException("Table '%s' is of unsupported format '%s', ", table_data->name,
		                              table_data->data_source_format);
	}
	return BuildDeltaScan(context, table, lookup_info, bind_data);
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
