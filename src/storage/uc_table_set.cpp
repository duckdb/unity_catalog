#include <algorithm>

#include "uc_api.hpp"

#include "duckdb/common/exception/catalog_exception.hpp"
#include "uc_logging.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "uc_utils.hpp"

#include "storage/unity_catalog.hpp"
#include "storage/uc_table_set.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/parser/parser.hpp"
#include "yyjson.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

// Casts exactly like the Delta extension does, so a key and Delta's read agree on the instant, to the millisecond
// Delta commit timestamps carry. Delta holds a resolved timestamp for the rest of the transaction, which is
// what lets the timestamp itself key the entry.
static string AtClauseKeySuffix(const BoundAtClause &at_clause) {
	auto &unit = at_clause.Unit();
	if (unit == "version") {
		return "@" + to_string(at_clause.GetValue().DefaultCastAs(LogicalType::UBIGINT).GetValue<idx_t>());
	}
	if (unit == "timestamp") {
		auto timestamp = at_clause.GetValue().DefaultCastAs(LogicalType::TIMESTAMP_TZ).GetValue<timestamp_tz_t>();
		return "@ts:" + to_string(Timestamp::GetEpochMs(timestamp_t(timestamp)));
	}
	throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version' or 'timestamp'");
}

UCTableSet::UCTableSet(UCSchemaEntry &schema) : catalog(schema.ParentCatalog().Cast<UnityCatalog>()), schema(schema) {
}

// A type we cannot parse becomes UNKNOWN, as duckdb-iceberg does for a table it has not loaded: the
// listing keeps its row, and a read cannot bind the column to a guess. The caller records which
// columns these were, so the refusal can name them.
static ColumnDefinition CreateColumnDefinition(ClientContext &context, UCAPIColumnDefinition &coldef,
                                               vector<pair<string, string>> &unreadable) {
	try {
		return {Identifier(coldef.name), UCUtils::ColumnTypeFromDefinition(coldef)};
	} catch (const std::exception &e) {
		UC_LOG_WARNING(context, "schema.Describe column=%s type_text=%s unreadable: %s", coldef.name, coldef.type_text,
		               e.what());
		unreadable.emplace_back(coldef.name, coldef.type_text);
		return {Identifier(coldef.name), LogicalType::UNKNOWN};
	}
}

static string DescribeColumns(const TableCatalogEntry &entry) {
	string result;
	for (auto &column : entry.GetColumns().Logical()) {
		if (!result.empty()) {
			result += ", ";
		}
		result += column.GetName().GetIdentifierName() + " " + column.GetType().ToString();
	}
	return result;
}

static bool SameColumns(const CatalogEntry &left, const CatalogEntry &right) {
	auto &left_columns = left.Cast<TableCatalogEntry>().GetColumns();
	auto &right_columns = right.Cast<TableCatalogEntry>().GetColumns();
	if (left_columns.LogicalColumnCount() != right_columns.LogicalColumnCount()) {
		return false;
	}
	for (idx_t i = 0; i < left_columns.LogicalColumnCount(); i++) {
		auto &left_column = left_columns.GetColumn(LogicalIndex(i));
		auto &right_column = right_columns.GetColumn(LogicalIndex(i));
		if (left_column.GetName().GetIdentifierName() != right_column.GetName().GetIdentifierName() ||
		    left_column.GetType() != right_column.GetType()) {
			return false;
		}
	}
	return true;
}

static void WarnOnSchemaDivergence(ClientContext &context, const CatalogEntry &reported, const CatalogEntry &resolved) {
	if (SameColumns(reported, resolved)) {
		return;
	}
	auto &reported_table = reported.Cast<TableCatalogEntry>();
	auto &resolved_table = resolved.Cast<TableCatalogEntry>();
	UC_LOG_WARNING(context,
	               "schema.Resolve %s: the catalog reports (%s) but the Delta log holds (%s); reading the log. A "
	               "commit the catalog has not caught up with resolves itself; repeated, the registration is stale "
	               "or the table was written by an engine the catalog does not see",
	               resolved_table.name.GetIdentifierName(), DescribeColumns(reported_table),
	               DescribeColumns(resolved_table));
}

unique_ptr<CatalogEntry> TableInformation::EntryFromDeltaLog(ClientContext &context,
                                                             const EntryLookupInfo &lookup_info) {
	// RefreshCredentials first: InternalAttach may flush pending backfills, thus needing the credentials.
	RefreshCredentials(context);
	InternalAttach(context);
	auto &delta_catalog = *GetInternalCatalog();
	auto &delta_schema = delta_catalog.GetSchema(context, Identifier::DefaultSchema());
	auto transaction = delta_schema.GetCatalogTransaction(context);
	auto table_entry = delta_schema.LookupEntry(transaction, lookup_info);
	if (!table_entry) {
		return nullptr;
	}
	auto create_info = table_entry->GetInfo();
	auto &info = create_info->Cast<CreateTableInfo>();
	info.SetTableName(Identifier(table_data->name));
	return make_uniq<UCTableEntry>(catalog, schema, *this, info);
}

optional_ptr<CatalogEntry> TableInformation::GetVersion(ClientContext &context, const EntryLookupInfo &lookup_info) {
	// The kernel drops interval fields from the log's schema without an error, so the report is the only
	// place they show: refuse ahead of both branches and the per-transaction cache.
	ThrowIfIntervalColumns();
	auto at = lookup_info.GetAtClause();
	if (!at) {
		// Neither ScanPlan/non-delta has a log to resolve against, return reported a la UCTableEntry::GetScanFunction.
		if (reported &&
		    ((ScanPlanCanRead() && catalog.ShouldTryScanPlan()) || table_data->data_source_format != "DELTA")) {
			ThrowIfUnreadableColumns();
			return reported.get();
		}

		// Reads bind against the log, not the catalog's description of it -- including nested
		// children, which resolve by position. Per transaction, matching the child catalog, which
		// rebuilds its own entry per transaction from the commits after the snapshot it holds.
		auto &transaction = UCTransaction::Get(context, catalog);
		auto key = EntryKey();
		auto bound = transaction.GetTableEntry(key);
		if (bound) {
			return bound;
		}

		// Registered but never written to: nothing to resolve against yet. A credential or transport
		// failure is a different thing and must not quietly downgrade the schema, so it propagates.
		auto entry = EntryFromDeltaLog(context, lookup_info);
		if (!entry) {
			if (!reported) {
				ThrowNoDeltaTable();
			}
			ThrowIfUnreadableColumns();
			UC_LOG_WARNING(context, "schema.Resolve %s: no schema in the Delta log; using the catalog's report",
			               table_data->name);
			return reported.get();
		}
		if (reported) {
			WarnOnSchemaDivergence(context, *reported, *entry);
		}
		return transaction.SetTableEntry(key, std::move(entry));
	}

	auto &transaction = UCTransaction::Get(context, catalog);
	auto key = EntryKey(at);
	auto bound = transaction.GetTableEntry(key);
	if (bound) {
		return bound;
	}
	auto entry = EntryFromDeltaLog(context, lookup_info);
	if (!entry) {
		ThrowNoDeltaTable();
	}
	return transaction.SetTableEntry(key, std::move(entry));
};

//! Qualified name, plus the version or timestamp where the query named one; neither means latest.
string TableInformation::EntryKey(optional_ptr<BoundAtClause> at) const {
	auto key = schema.name.GetIdentifierName() + "." + table_data->name;
	return at ? key + AtClauseKeySuffix(*at) : key;
}

// `reported` describes a table, and tracks unrepresentables; this throws at bind with nice error
void TableInformation::ThrowIfUnreadableColumns() const {
	if (unreadable_columns.empty()) {
		return;
	}
	auto &column = unreadable_columns.front();
	throw NotImplementedException("Table '%s' has %llu column(s) with a type this build cannot read; the first is "
	                              "'%s', which Unity Catalog reports as '%s'",
	                              table_data->name, static_cast<uint64_t>(unreadable_columns.size()), column.first,
	                              column.second);
}

// Delta keeps year-month and day-time intervals apart, as a count of months or of microseconds; DuckDB's
// INTERVAL mixes months, days and micros and compares a month as 30 days, so neither maps without changing
// meaning. Searching the type text also catches a nested field, e.g. `struct<gap:interval day to second>`.
static bool TypeTextHasInterval(const string &type_text) {
	static const char *const UNITS[] = {"year", "month", "day", "hour", "minute", "second"};
	auto text = StringUtil::Lower(type_text);
	for (auto pos = text.find("interval "); pos != string::npos; pos = text.find("interval ", pos + 1)) {
		auto rest = text.substr(pos + 9);
		for (auto unit : UNITS) {
			if (StringUtil::StartsWith(rest, unit)) {
				return true;
			}
		}
	}
	return false;
}

void TableInformation::ThrowIfIntervalColumns() const {
	for (auto &column : table_data->columns) {
		if (TypeTextHasInterval(column.type_text)) {
			throw NotImplementedException("Table '%s' has interval column '%s', which Unity Catalog reports as "
			                              "'%s'; Delta interval semantics differ from DuckDB's INTERVAL",
			                              table_data->name, column.name, column.type_text);
		}
	}
}

void TableInformation::ThrowNoDeltaTable() const {
	throw CatalogException("Table '%s' is registered in Unity Catalog at '%s', but no Delta table was found there",
	                       table_data->name, table_data->storage_location);
}

optional_ptr<Catalog> TableInformation::GetInternalCatalog() {
	// TODO(race): unsynchronized read of internal_attached_database — a concurrent InternalDetach
	// (under attach_lock) can null it, risking a null/dangling deref. Needs locking or a guarantee
	// the caller holds attach_lock / has a live attach.
	return internal_attached_database->GetCatalog();
}

void TableInformation::RefreshCredentials(ClientContext &context) {
	D_ASSERT(table_data);
	if (table_data->storage_location.find("file://") == 0) {
		return;
	}
	auto &secret_manager = SecretManager::Get(context);
	// Get Credentials from UCAPI
	auto table_credentials = UCAPI::GetTableCredentials(
	    context, table_data->catalog_name, table_data->schema_name, table_data->name, table_data->table_id,
	    IsCatalogManaged(), !(catalog.access_mode == AccessMode::READ_ONLY), catalog.credentials);

	// Inject secret into secret manager scoped to this path
	CreateSecretInput input;
	input.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	input.persist_type = SecretPersistType::TEMPORARY;
	input.name = Identifier("__internal_uc_" + table_data->table_id);
	input.provider = "config";
	input.scope = {table_data->storage_location};
	ApplyVendedCredentials(context, input, table_data->storage_location, table_credentials, catalog.credentials.aws_region);

	secret_manager.CreateSecret(context, input);
}

string TableInformation::AttachedCatalogName() const {
	auto &schema_name = table_data->schema_name;
	auto &name = table_data->name;
	// Named after the attachment, not the Unity Catalog: one catalog can be attached twice, each with an internal
	// database of its own, so attach by identifier
	return "__unity_catalog_internal_" + catalog.GetName().GetIdentifierName() + "_" + schema_name + "_" + name;
}

void TableInformation::InternalDetach(ClientContext &context, const lock_guard<mutex> &_attach_lock) {
	if (!internal_attached_database) {
		return;
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto name = AttachedCatalogName();
	// This pointer is ours, but the attachment it names belongs to DuckDB, which undoes it when the
	// statement that attached it rolls back -- leaving the pointer set and nothing to detach. Tolerate
	// that: the name has to be released either way, or the next attach collides with it.
	db_manager.DetachDatabase(context, Identifier(name), OnEntryNotFound::RETURN_NULL);
	internal_attached_database = nullptr;
}

// Returns false if dst already exists (another session backfilled it); throws on real errors.
static bool CopyStagedCommitToDeltaLog(ClientContext &context, const string &src, const string &dst) {
	constexpr idx_t BUFFER_SIZE = 8ULL * 1024 * 1024;
	auto &fs = FileSystem::GetFileSystem(context);
	auto src_file = fs.OpenFile(src, FileOpenFlags::FILE_FLAGS_READ);
	// Exclusive create + null-if-exists: skip (return null) if another session already backfilled
	// this version, rather than clobbering it. NULL_IF_EXISTS is only valid with EXCLUSIVE_CREATE
	// (asserted in debug builds); EXCLUSIVE_CREATE in turn requires a base FILE_CREATE.
	auto dst_flags = FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE |
	                 FileOpenFlags::FILE_FLAGS_EXCLUSIVE_CREATE | FileOpenFlags::FILE_FLAGS_NULL_IF_EXISTS;
	auto dst_file = fs.OpenFile(dst, dst_flags);
	if (dst_file) {
		auto buf = make_unsafe_uniq_array<char>(BUFFER_SIZE);
		int64_t n;
		while ((n = src_file->Read(buf.get(), BUFFER_SIZE)) > 0) {
			dst_file->Write(buf.get(), n);
		}
		dst_file->Close();
		return true;
	} else {
		return false;
	}
}

optional_idx TableInformation::BackfillCommits(ClientContext &context, const vector<UCAPICommit> &commits,
                                               optional_idx backfilled_version) {
	// Operates on a local watermark and does file I/O; the caller writes the returned value back into
	// commit_state under its lock, so commit_state's lock is never held across these copies.
	vector<reference<const UCAPICommit>> ordered;
	ordered.reserve(commits.size());
	for (auto &c : commits) {
		ordered.push_back(c);
	}
	std::sort(ordered.begin(), ordered.end(),
	          [](const UCAPICommit &a, const UCAPICommit &b) { return a.version < b.version; });
	for (auto commit_ref : ordered) {
		auto const &commit = commit_ref.get();
		if (backfilled_version.IsValid() && commit.version <= backfilled_version.GetIndex()) {
			continue;
		}
		string src = table_data->storage_location + "/_delta_log/_staged_commits/" + commit.file_name;
		string dst =
		    StringUtil::Format("%s/_delta_log/%020llu.json", table_data->storage_location, (uint64_t)commit.version);
		try {
			// TODO: consider? prefetch _delta_log/ listing (name + size) and skip copies where both match,
			//       eliminating file/object open round trips if the list is >1 length.
			if (!CopyStagedCommitToDeltaLog(context, src, dst)) {
				// false = dst already exists; another session beat us — still update backfill version
			}
			backfilled_version = commit.version;
		} catch (std::exception &e) {
			// Real failure — stop to avoid advancing backfilled_version past a gap. Surface it: a
			// silently-swallowed backfill failure lets staged commits accumulate until the server
			// rejects further commits (HTTP 429, "too many un-backfilled commits").
			UC_LOG_WARNING(context, "api.BackfillCommits version=%lld src=%s dst=%s failed: %s",
			               (int64_t)commit.version, src.c_str(), dst.c_str(), e.what());
			break;
		}
	}
	return backfilled_version;
}

void TableInformation::MarkDirty(const lock_guard<mutex> &_attach_lock) {
	is_dirty = true;
}

bool TableInformation::HasDeltaFeature(const string &feature) const {
	auto it = table_data->properties.find("delta.feature." + feature);
	return it != table_data->properties.end() && it->second == "supported";
}

bool TableInformation::IsCatalogManaged() const {
	// Databricks preview property (pre-GA)
	if (HasDeltaFeature("catalogOwned-preview")) {
		return true;
	}
	// Databricks GA + OSS UC v0.5+: set for tables that use the Delta CMT protocol
	return HasDeltaFeature("catalogManaged");
}

// A planned scan reads the planned files with parquet alone, bound by position to the columns UC reports. Spark
// leaves partition columns out of the files, and a mapped table's files name their columns physically, so once a
// column is dropped or added nothing lines them up. Such a table reads through its Delta log instead. A table that
// is not Delta has no log to read, and only a planned scan can read it.
bool TableInformation::ScanPlanCanRead() const {
	if (table_data->data_source_format != "DELTA") {
		return true;
	}
	for (auto &column : table_data->columns) {
		if (column.is_partition_column) {
			return false;
		}
	}
	auto mode = table_data->properties.find("delta.columnMapping.mode");
	return mode == table_data->properties.end() || mode->second == "none";
}

static Value BuildLogTailFromCommits(const UCAPICommitsResult &commits, const string &storage_location) {
	vector<Value> commit_values;
	for (const auto &commit : commits.commits) {
		child_list_t<Value> commit_struct;
		commit_struct.push_back(make_pair("version", Value::BIGINT((int64_t)commit.version)));
		commit_struct.push_back(make_pair("timestamp", Value::BIGINT(commit.timestamp)));
		commit_struct.push_back(
		    make_pair("file_name", Value(storage_location + "/_delta_log/_staged_commits/" + commit.file_name)));
		commit_struct.push_back(make_pair("file_size", Value::BIGINT(commit.file_size)));
		commit_struct.push_back(
		    make_pair("file_modification_timestamp", Value::BIGINT(commit.file_modification_timestamp)));
		commit_values.push_back(Value::STRUCT(std::move(commit_struct)));
	}

	return Value::LIST(
	    LogicalType::STRUCT({make_pair("version", LogicalType::BIGINT), make_pair("timestamp", LogicalType::BIGINT),
	                         make_pair("file_name", LogicalType::VARCHAR), make_pair("file_size", LogicalType::BIGINT),
	                         make_pair("file_modification_timestamp", LogicalType::BIGINT)}),
	    commit_values);
}

void TableInformation::InternalAttach(ClientContext &context) {
	lock_guard<mutex> l(attach_lock);
	// A transaction that already read this table keeps the catalog it read: the meta transaction refuses a
	// second database under a name it holds, and the reads either side of the swap would straddle two
	// commits. The next transaction reattaches, since is_dirty stays set.
	bool read_by_this_transaction =
	    internal_attached_database && MetaTransaction::Get(context).TryGetTransaction(*internal_attached_database);
	if (is_dirty && !read_by_this_transaction) {
		InternalDetach(context, l);
		is_dirty = false;
	}

	if (internal_attached_database) {
		return;
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto &name = table_data->name;

	// Create the attach info for the table
	AttachInfo info;
	info.name = Identifier(AttachedCatalogName());
	info.options = {{"type", Value("Delta")},
	                {"child_catalog_mode", Value(true)},
	                {"internal_table_name", Value(name)},
	                {"unity_table_id", Value(table_data->table_id)}};
	info.path = table_data->storage_location;

	if (IsCatalogManaged()) {
		auto &uc_catalog = catalog.Cast<UnityCatalog>();
		auto commits = UCAPI::LoadTable(context, table_data->catalog_name, table_data->schema_name, table_data->name,
		                                uc_catalog.credentials);
		info.options["parent_catalog"] = Value(catalog.GetName());
		info.options["parent_catalog_schema"] = Value(schema.name.GetIdentifierName());
		info.options["parent_commit"] = Value(true);
		info.options["max_catalog_version"] = Value::BIGINT((int64_t)commits.ratified_version);
		// Backfill outside commit_state's lock (file I/O), then publish {etag, watermark} atomically.
		// Only InternalAttach writes backfilled_version and it is serialized by attach_lock, so the
		// start watermark read here is stable.
		//
		// Backfill is a writer's duty (publishing staged commits into _delta_log/). Skip it on a
		// read-only attach: a RO attach must not mutate table storage (it also holds only read-scoped
		// credentials). Reads still see staged commits via the log_tail + max_catalog_version above.
		bool read_only = uc_catalog.access_mode == AccessMode::READ_ONLY;
		optional_idx start_wm = commit_state.with_locked([](const UCCommitState &s) { return s.backfilled_version; });
		if (read_only) {
			UC_LOG_DEBUG(context,
			             "api.InternalAttach %s.%s.%s read-only: skipping backfill (%zu backfillable commit(s))",
			             table_data->catalog_name, table_data->schema_name, table_data->name, commits.commits.size());
		}
		optional_idx new_wm = read_only ? start_wm : BackfillCommits(context, commits.commits, start_wm);
		commit_state.with_locked([&](UCCommitState &s) {
			s.etag = commits.etag;
			s.backfilled_version = new_wm;
		});
		if (!commits.commits.empty()) {
			info.options["log_tail"] = BuildLogTailFromCommits(commits, table_data->storage_location);
		}
	}

	AttachOptions options(info.options, AccessMode::READ_WRITE);
	options.access_mode = AccessMode::READ_WRITE;
	options.db_type = "delta";

	auto &internal_db = internal_attached_database;
	internal_db = db_manager.AttachDatabase(context, info, options);
}

void UCTableSet::OnDetach(ClientContext &context) {
	for (auto &entry : tables) {
		auto &table = entry.second;
		lock_guard<mutex> l(table.attach_lock);
		table.InternalDetach(context, l);
	}
}

void TableInformation::InternalCheckpoint(ClientContext &context, bool force) {
	// attach and call down to Delta's table specific checkpoint
	D_ASSERT(table_data);
	RefreshCredentials(context);
	InternalAttach(context);
	// TODO(race): unsynchronized read of internal_attached_database (same hazard as
	// GetInternalCatalog). InternalAttach above released attach_lock, so another thread could detach
	// before this deref.
	internal_attached_database->GetTransactionManager().Checkpoint(context, force);
}

void UCTableSet::CheckpointTable(ClientContext &context, const string &table_name, bool force) {
	EnsureLoaded(context);
	auto it = tables.find(table_name);
	if (it == tables.end()) {
		throw InvalidInputException("Table '%s' not found", table_name);
	}
	it->second.InternalCheckpoint(context, force);
}

// TODO: remove or update syntax from DuckDB perspective; left here as ready-to-go
// code for possible additional conditional checkpoint scan/act logic
#if 0
void UCTableSet::Checkpoint(ClientContext &context, bool force) {
	if (!is_loaded) {
		LoadEntries(context);
		is_loaded = true;
	}
	for (auto &entry : tables) {
		entry.second.InternalCheckpoint(context, force);
	}
}
#endif

void UCTableSet::LoadEntries(ClientContext &context, const lock_guard<mutex> &_entry_lock) {
	auto &unity_catalog = catalog.Cast<UnityCatalog>();
	auto get_tables_result =
	    UCAPI::GetTables(context, catalog, schema.name.GetIdentifierName(), unity_catalog.credentials);

	for (auto &table : get_tables_result) {
		D_ASSERT(schema.name == table.schema_name);
		CreateTableInfo info;
		vector<pair<string, string>> unreadable;
		// `position` is the API's ordering contract; the response's array order says nothing. Stable,
		// so columns sharing a position (or carrying none) keep their listed order.
		std::stable_sort(table.columns.begin(), table.columns.end(),
		                 [](const UCAPIColumnDefinition &left, const UCAPIColumnDefinition &right) {
			                 return left.position < right.position;
		                 });
		for (auto &col : table.columns) {
			info.columns.AddColumn(CreateColumnDefinition(context, col, unreadable));
		}

		lock_guard<mutex> l(entry_lock);
		if (table.name.empty()) {
			throw InternalException("UCTableSet::CreateEntry called with empty name");
		}
		auto it = tables.find(table.name);
		if (it != tables.end()) {
			continue;
		}

		auto res = tables.emplace(std::piecewise_construct, std::forward_as_tuple(table.name),
		                          std::forward_as_tuple(catalog, schema));
		auto &table_info = res.first->second;

		info.SetTableName(Identifier(table.name));
		auto table_entry = make_uniq<UCTableEntry>(catalog, schema, table_info, info);

		table_info.table_data = make_uniq<UCAPITable>(table);
		table_info.reported = std::move(table_entry);
		table_info.unreadable_columns = std::move(unreadable);
	}
}

void UCTableSet::EnsureLoaded(ClientContext &context) {
	lock_guard<mutex> l(load_lock);
	if (!is_loaded) {
		LoadEntries(context, l); // acquires entry_lock internally; fine, load_lock != entry_lock
		is_loaded = true;        // set only after LoadEntries succeeds
	}
}

static vector<string> ReadJsonStringArray(duckdb_yyjson::yyjson_val *arr) {
	vector<string> result;
	if (!arr || !duckdb_yyjson::yyjson_is_arr(arr)) {
		return result;
	}
	size_t idx, max;
	duckdb_yyjson::yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		if (duckdb_yyjson::yyjson_is_str(item)) {
			result.emplace_back(duckdb_yyjson::yyjson_get_str(item));
		}
	}
	return result;
}

static vector<pair<string, string>> ReadJsonStringMap(duckdb_yyjson::yyjson_val *obj) {
	vector<pair<string, string>> result;
	if (!obj || !duckdb_yyjson::yyjson_is_obj(obj)) {
		return result;
	}
	size_t idx, max;
	duckdb_yyjson::yyjson_val *key, *val;
	yyjson_obj_foreach(obj, idx, max, key, val) {
		if (duckdb_yyjson::yyjson_is_str(val)) {
			result.emplace_back(duckdb_yyjson::yyjson_get_str(key), duckdb_yyjson::yyjson_get_str(val));
		}
	}
	return result;
}

//! Whether kernel takes this property when a table is created. Mirrors `ALLOWED_DELTA_PROPERTIES` in
//! kernel's create-table builder, which rejects every other `delta.*` key; a key kernel has since added
//! is merely not used here, which is the safe direction. Keys outside the `delta.` namespace are the
//! table's own and are passed through.
static bool CreatableDeltaProperty(const string &key) {
	static const unordered_set<string> CREATABLE = {"delta.appendOnly",
	                                                "delta.checkpointInterval",
	                                                "delta.checkpointPolicy",
	                                                "delta.checkpoint.writeStatsAsJson",
	                                                "delta.checkpoint.writeStatsAsStruct",
	                                                "delta.columnMapping.mode",
	                                                "delta.dataSkippingNumIndexedCols",
	                                                "delta.dataSkippingStatsColumns",
	                                                "delta.deletedFileRetentionDuration",
	                                                "delta.enableChangeDataFeed",
	                                                "delta.enableDeletionVectors",
	                                                "delta.enableExpiredLogCleanup",
	                                                "delta.enableIcebergCompatV3",
	                                                "delta.enableInCommitTimestamps",
	                                                "delta.enableRowTracking",
	                                                "delta.enableTypeWidening",
	                                                "delta.logRetentionDuration",
	                                                "delta.parquet.format.version",
	                                                "delta.setTransactionRetentionDuration"};
	if (!StringUtil::StartsWith(key, "delta.")) {
		return true;
	}
	return CREATABLE.count(key) > 0;
}

//! A secret scoped to the staging location, so writing version 0 there is authorised. Local storage, which
//! the OSS server hands out, needs none.
static void StageCredentials(ClientContext &context, UnityCatalog &uc_catalog, const UCAPIStagingTable &staging) {
	if (StringUtil::StartsWith(staging.location, "file://")) {
		return;
	}
	auto credentials = UCAPI::GetStagingTableCredentials(context, staging.table_id, uc_catalog.credentials);
	CreateSecretInput input;
	input.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	input.persist_type = SecretPersistType::TEMPORARY;
	input.name = Identifier("__internal_uc_" + staging.table_id);
	input.provider = "config";
	input.scope = {staging.location};
	ApplyVendedCredentials(context, input, staging.location, credentials, uc_catalog.credentials.aws_region);
	auto &secret_manager = SecretManager::Get(context);
	secret_manager.CreateSecret(context, input);
}

//! What the table's own version 0 says it is: the schema, partition columns, protocol and configuration.
//! UC is told this rather than what the statement asked for, so the catalog cannot describe a table that
//! differs from its log.
static UCAPICommittedTable ReadCommittedTable(ClientContext &context, const string &name, const string &location) {
	auto log_entry = Path::FromString(location).Join("_delta_log").Join("00000000000000000000.json").ToString();
	auto &fs = FileSystem::GetFileSystem(context);
	auto file = fs.OpenFile(log_entry, FileOpenFlags::FILE_FLAGS_READ);
	auto size = NumericCast<idx_t>(file->GetFileSize());
	string contents(size, '\0');
	file->Read((void *)contents.data(), size);

	UCAPICommittedTable result;
	result.name = name;
	result.location = location;
	// Version 0 holds one action per line; the protocol and metaData are the two that describe the table
	for (auto &line : StringUtil::Split(contents, '\n')) {
		if (line.find_first_not_of(" \t\r") == string::npos) {
			continue;
		}
		duckdb_yyjson::yyjson_doc *doc =
		    duckdb_yyjson::yyjson_read(line.c_str(), line.size(), duckdb_yyjson::YYJSON_READ_NOFLAG);
		if (!doc) {
			throw IOException("Could not read the committed Delta log at %s", log_entry);
		}
		auto *root = duckdb_yyjson::yyjson_doc_get_root(doc);
		auto *protocol = duckdb_yyjson::yyjson_obj_get(root, "protocol");
		if (protocol) {
			result.min_reader_version =
			    (int64_t)duckdb_yyjson::yyjson_get_sint(duckdb_yyjson::yyjson_obj_get(protocol, "minReaderVersion"));
			result.min_writer_version =
			    (int64_t)duckdb_yyjson::yyjson_get_sint(duckdb_yyjson::yyjson_obj_get(protocol, "minWriterVersion"));
			result.reader_features = ReadJsonStringArray(duckdb_yyjson::yyjson_obj_get(protocol, "readerFeatures"));
			result.writer_features = ReadJsonStringArray(duckdb_yyjson::yyjson_obj_get(protocol, "writerFeatures"));
		}
		auto *commit_info = duckdb_yyjson::yyjson_obj_get(root, "commitInfo");
		if (commit_info) {
			// A table with inCommitTimestamp carries the authoritative one there; `timestamp` is what a
			// writer without that feature records.
			auto *ict = duckdb_yyjson::yyjson_obj_get(commit_info, "inCommitTimestamp");
			auto *written = duckdb_yyjson::yyjson_obj_get(commit_info, "timestamp");
			result.last_commit_timestamp_ms = (int64_t)duckdb_yyjson::yyjson_get_sint(ict ? ict : written);
		}
		auto *metadata = duckdb_yyjson::yyjson_obj_get(root, "metaData");
		if (metadata) {
			auto *schema_string = duckdb_yyjson::yyjson_obj_get(metadata, "schemaString");
			if (schema_string && duckdb_yyjson::yyjson_is_str(schema_string)) {
				result.schema_json = duckdb_yyjson::yyjson_get_str(schema_string);
			}
			result.partition_columns = ReadJsonStringArray(duckdb_yyjson::yyjson_obj_get(metadata, "partitionColumns"));
			result.properties = ReadJsonStringMap(duckdb_yyjson::yyjson_obj_get(metadata, "configuration"));
		}
		duckdb_yyjson::yyjson_doc_free(doc);
	}
	if (result.schema_json.empty()) {
		throw IOException("The committed Delta log at %s holds no schema", log_entry);
	}
	if (result.last_commit_timestamp_ms == 0) {
		throw IOException("The committed Delta log at %s holds no commit timestamp", log_entry);
	}
	return result;
}

static bool IsEntryAlreadyExists(const CatalogException &e) {
	ErrorData error(e);
	auto &extra = error.ExtraInfo();
	auto subtype = extra.find("error_subtype");
	return subtype != extra.end() && subtype->second == "ENTRY_ALREADY_EXISTS";
}

//! Attaches a Delta catalog at `location`, writes the table's version 0 through it, and returns what that
//! version committed. The attachment is gone again when this returns, whichever way it returns.
static UCAPICommittedTable WriteVersionZero(ClientContext &context, BoundCreateTableInfo &info, AttachInfo &attach_info,
                                            const string &table_name, const string &location) {
	AttachOptions attach_options(attach_info.options, AccessMode::READ_WRITE);
	attach_options.access_mode = AccessMode::READ_WRITE;
	attach_options.db_type = "delta";

	auto &db_manager = DatabaseManager::Get(context);
	auto attached_name = attach_info.name;
	auto delta_db = db_manager.AttachDatabase(context, attach_info, attach_options);
	UCAPICommittedTable committed;
	try {
		auto &delta_catalog = delta_db->GetCatalog();
		auto &delta_schema = delta_catalog.GetSchema(context, Identifier::DefaultSchema());
		delta_schema.CreateTable(delta_schema.GetCatalogTransaction(context), info);
		committed = ReadCommittedTable(context, table_name, location);
	} catch (...) {
		try {
			db_manager.DetachDatabase(context, attached_name, OnEntryNotFound::RETURN_NULL);
		} catch (std::exception &detach_error) {
			UC_LOG_WARNING(context, "CreateTable %s could not detach the database it wrote through: %s", table_name,
			               detach_error.what());
		}
		throw;
	}
	db_manager.DetachDatabase(context, attached_name, OnEntryNotFound::RETURN_NULL);
	return committed;
}

//! Registers `committed` with UC. A failed call may hide a registration that went through -- a 5xx after the
//! write, a dropped connection -- so on failure the name is loaded, and `is_ours` decides whether the table
//! registered under it is the one just written. Anything else is the real error.
static void RegisterTable(ClientContext &context, UnityCatalog &uc_catalog, const string &catalog_name,
                          const string &schema_name, const UCAPICommittedTable &committed, const string &table_type,
                          const std::function<bool(const UCAPICommitsResult &)> &is_ours) {
	try {
		UCAPI::CreateTable(context, catalog_name, schema_name, committed, table_type, uc_catalog.credentials);
	} catch (const InterruptException &) {
		throw;
	} catch (const CatalogException &) {
		throw;
	} catch (std::exception &e) {
		bool registered = false;
		try {
			registered =
			    is_ours(UCAPI::LoadTable(context, catalog_name, schema_name, committed.name, uc_catalog.credentials));
		} catch (std::exception &) {
		}
		if (!registered) {
			throw;
		}
		UC_LOG_WARNING(context,
		               "api.CreateTable %s.%s.%s reported an error but the table is registered as written; "
		               "treating it as created: %s",
		               catalog_name, schema_name, committed.name, e.what());
	}
}

//! A location as a path that can be compared: no `file://` prefix, one leading slash, no trailing one.
static string ComparableLocation(string location) {
	if (StringUtil::StartsWith(location, "file:")) {
		location = location.substr(5);
		auto first = location.find_first_not_of('/');
		location = "/" + (first == string::npos ? string() : location.substr(first));
	}
	while (location.size() > 1 && location.back() == '/') {
		location.pop_back();
	}
	return location;
}

//! The `location` option, evaluated as delta evaluates its options. It must be absolute: UC resolves a
//! relative path against the server's own working directory, which no client can see.
static string ExternalLocation(ClientContext &context, const ParsedExpression &option) {
	auto binder = Binder::CreateBinder(context);
	TableFunctionBinder option_binder(*binder, context, "CREATE TABLE", "Table option");
	auto copy = option.Copy();
	auto bound = option_binder.Bind(copy);
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound, true);
	if (value.IsNull()) {
		throw BinderException("NULL is not a valid CREATE TABLE location");
	}
	auto location = value.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>();
	if (!StringUtil::StartsWith(location, "/") && !StringUtil::Contains(location, "://")) {
		throw BinderException("CREATE TABLE location must be an absolute path or a URI, found '%s'", location);
	}
	return location;
}

//! Removes the version 0 a failed external create wrote: kernel refuses to create where `_delta_log` holds
//! any file, so it would block every later create at `location`, and SQL has no way to remove it. Kept when
//! UC lists the table at `location` after all, or cannot say, since a registered table owns that log.
static void RemoveUnregisteredVersionZero(ClientContext &context, UnityCatalog &uc_catalog, const string &catalog_name,
                                          const string &schema_name, const string &table_name, const string &location) {
	auto log_dir = location;
	try {
		if (UCAPI::TableExists(context, catalog_name, schema_name, table_name, uc_catalog.credentials)) {
			auto loaded = UCAPI::LoadTable(context, catalog_name, schema_name, table_name, uc_catalog.credentials);
			if (ComparableLocation(loaded.location) == ComparableLocation(location)) {
				return;
			}
		}
		auto &fs = FileSystem::GetFileSystem(context);
		log_dir = fs.JoinPath(location, "_delta_log");
		// The filesystem committer writes version 0 only if absent, so every version 0 file here is ours
		for (auto &file : fs.Glob(fs.JoinPath(log_dir, "00000000000000000000.*"))) {
			fs.RemoveFile(file.path);
		}
		UC_LOG_DEBUG(context, "CreateTable %s.%s.%s removed the unregistered version 0 at %s", catalog_name,
		             schema_name, table_name, log_dir);
	} catch (std::exception &e) {
		UC_LOG_WARNING(context,
		               "CreateTable %s.%s.%s could not remove the version 0 it wrote at %s, which blocks any later "
		               "create there: %s",
		               catalog_name, schema_name, table_name, log_dir, e.what());
	}
}

optional_ptr<CatalogEntry> UCTableSet::CreateTable(ClientContext &context, BoundCreateTableInfo &info) {
	auto &uc_catalog = catalog.Cast<UnityCatalog>();
	if (uc_catalog.access_mode == AccessMode::READ_ONLY) {
		throw InvalidInputException("Can not create a table in a read only Unity Catalog");
	}
	auto &base = info.Base().Cast<CreateTableInfo>();
	auto table_name = base.GetTableName().GetIdentifierName();
	auto catalog_name = catalog.GetDBPath();
	auto schema_name = schema.name.GetIdentifierName();

	auto location_option = base.options.find("location");
	if (location_option != base.options.end()) {
		// The caller chose where the table lives, which in UC makes it EXTERNAL: no staging, no catalog committer,
		// nothing UC requires of the log. Version 0 is written before UC hears of the table, so a taken name is
		// checked first rather than found at registration, after a write.
		auto location = ExternalLocation(context, *location_option->second);
		// Delta evaluates the option again, and must see the same path even when the expression is volatile
		location_option->second = ConstantExpression::String(location);
		if (UCAPI::TableExists(context, catalog_name, schema_name, table_name, uc_catalog.credentials)) {
			if (base.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
				return nullptr;
			}
			throw CatalogException::EntryAlreadyExists(CatalogType::TABLE_ENTRY, Identifier(table_name));
		}
		AttachInfo attach_info;
		// Unique, as the staging id makes the managed one: concurrent creates of a name must not collide here
		attach_info.name = Identifier("__unity_catalog_internal_create_" + UUID::ToString(UUID::GenerateRandomUUID()));
		attach_info.path = location;
		attach_info.options = {
		    {"type", Value("Delta")}, {"child_catalog_mode", Value(true)}, {"internal_table_name", Value(table_name)}};
		auto committed = WriteVersionZero(context, info, attach_info, table_name, location);
		auto written = ComparableLocation(location);
		try {
			RegisterTable(
			    context, uc_catalog, catalog_name, schema_name, committed, "EXTERNAL",
			    [&](const UCAPICommitsResult &loaded) { return ComparableLocation(loaded.location) == written; });
		} catch (const CatalogException &e) {
			RemoveUnregisteredVersionZero(context, uc_catalog, catalog_name, schema_name, table_name, location);
			// Taken between the check and the registration
			if (base.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && IsEntryAlreadyExists(e)) {
				return nullptr;
			}
			throw;
		} catch (std::exception &) {
			RemoveUnregisteredVersionZero(context, uc_catalog, catalog_name, schema_name, table_name, location);
			throw;
		}
		MarkNeedsReload();
		return nullptr;
	}

	// UC allocates the location and the table id, and says what the table has to be. The catalog is
	// the only authority on whether the name is taken, so the name is not checked here first.
	UCAPIStagingTable staging;
	try {
		staging = UCAPI::CreateStagingTable(context, catalog_name, schema_name, table_name, uc_catalog.credentials);
	} catch (const CatalogException &e) {
		if (base.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && IsEntryAlreadyExists(e)) {
			return nullptr;
		}
		throw;
	}
	StageCredentials(context, uc_catalog, staging);

	// A suggestion is UC saying "this is how we would write it", so take the ones that can be taken: kernel
	// refuses at creation any `delta.*` key it does not accept.
	for (auto &property : staging.suggested_properties) {
		if (!CreatableDeltaProperty(property.first)) {
			UC_LOG_DEBUG(context, "CreateTable %s.%s.%s skipped suggested property %s", catalog_name, schema_name,
			             table_name, property.first);
			continue;
		}
		base.options[property.first] = ConstantExpression::String(property.second);
	}
	// What UC requires becomes part of the statement kernel sees: its properties as they are, and its
	// protocol features as `delta.feature.<name> = supported`, which is how a table asks for them at
	// creation. Without `catalogManaged` among them kernel refuses to commit through a catalog
	// committer at all. Written after the suggestions, so a key UC names in both is the way it
	// requires it, not the way it would prefer it.
	for (auto &property : staging.required_properties) {
		base.options[property.first] = ConstantExpression::String(property.second);
	}
	for (auto &features : {staging.reader_features, staging.writer_features}) {
		for (auto &feature : features) {
			base.options["delta.feature." + feature] = ConstantExpression::String("supported");
		}
	}

	// Version 0 is written through this catalog's committer, so the delta catalog is attached the way a
	// read attaches one, minus what only an existing table has (its commits and ratified version).
	AttachInfo attach_info;
	attach_info.name = Identifier("__unity_catalog_internal_staging_" + staging.table_id);
	attach_info.path = staging.location;
	attach_info.options = {{"type", Value("Delta")},
	                       {"child_catalog_mode", Value(true)},
	                       {"internal_table_name", Value(table_name)},
	                       {"unity_table_id", Value(staging.table_id)},
	                       {"parent_catalog", Value(catalog.GetName())},
	                       {"parent_catalog_schema", Value(schema_name)},
	                       {"parent_commit", Value(true)}};
	auto committed = WriteVersionZero(context, info, attach_info, table_name, staging.location);
	try {
		RegisterTable(context, uc_catalog, catalog_name, schema_name, committed, "MANAGED",
		              [&](const UCAPICommitsResult &loaded) { return loaded.table_uuid == staging.table_id; });
	} catch (const CatalogException &e) {
		// Taken between staging and registration
		if (base.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && IsEntryAlreadyExists(e)) {
			return nullptr;
		}
		throw;
	}

	// The catalog now owns a table this set has never seen; its next lookup loads it from UC.
	MarkNeedsReload();
	return nullptr;
}

void UCTableSet::AlterTable(ClientContext &context, RenameTableInfo &info) {
	throw NotImplementedException("UCTableSet::AlterTable");
}

void UCTableSet::AlterTable(ClientContext &context, RenameColumnInfo &info) {
	throw NotImplementedException("UCTableSet::AlterTable");
}

void UCTableSet::AlterTable(ClientContext &context, AddColumnInfo &info) {
	throw NotImplementedException("UCTableSet::AlterTable");
}

void UCTableSet::AlterTable(ClientContext &context, RemoveColumnInfo &info) {
	throw NotImplementedException("UCTableSet::AlterTable");
}

void UCTableSet::AlterTable(ClientContext &context, AlterTableInfo &alter) {
	throw NotImplementedException("UCTableSet::AlterTable");
}

optional_ptr<CatalogEntry> UCTableSet::GetEntry(ClientContext &context, const EntryLookupInfo &lookup) {
	EnsureLoaded(context);
	lock_guard<mutex> l(entry_lock);
	auto &name = lookup.GetEntryName();
	auto entry = tables.find(name);
	if (entry == tables.end()) {
		return nullptr;
	}
	auto &table_info = entry->second;
	return table_info.GetVersion(context, lookup);
}

void UCTableSet::MarkNeedsReload() {
	lock_guard<mutex> ll(load_lock);
	is_loaded = false;
}

void UCTableSet::ClearEntries() {
	lock_guard<mutex> ll(load_lock);
	lock_guard<mutex> le(entry_lock);
	tables.clear();
	is_loaded = false;
}

void UCTableSet::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("UCTableSet::DropEntry");
}

void UCTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	EnsureLoaded(context);
	lock_guard<mutex> l(entry_lock);
	for (auto &table : tables) {
		callback(*table.second.reported);
	}
}

} // namespace duckdb
