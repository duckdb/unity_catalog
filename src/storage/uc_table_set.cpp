#include <algorithm>

#include "uc_api.hpp"
#include "uc_logging.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
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
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

static idx_t ParseDeltaVersionFromAtClause(const BoundAtClause &at_clause) {
	if (at_clause.Unit() != "version") {
		throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version'");
	}
	auto version_value = at_clause.GetValue().DefaultTryCastAs(LogicalType::UBIGINT);
	if (!version_value) {
		throw InvalidInputException("Failed to parse version number '%s' into a valid version",
		                            at_clause.GetValue().ToString().c_str());
	}
	return version_value->GetValue<idx_t>();
}

UCTableSet::UCTableSet(UCSchemaEntry &schema) : catalog(schema.ParentCatalog().Cast<UnityCatalog>()), schema(schema) {
}

static ColumnDefinition CreateColumnDefinition(ClientContext &context, UCAPIColumnDefinition &coldef) {
	return {Identifier(coldef.name), UCUtils::TypeToLogicalType(context, coldef.type_text)};
}

optional_ptr<CatalogEntry> TableInformation::GetVersion(ClientContext &context, const EntryLookupInfo &lookup_info) {
	auto at = lookup_info.GetAtClause();
	if (!at) {
		//! No version provided, just return the dummy entry (should represent latest version)
		lock_guard<mutex> l(entry_lock);
		return dummy.get();
	}

	auto version = ParseDeltaVersionFromAtClause(*at);

	// Fast path: already cached
	{
		lock_guard<mutex> l(entry_lock);
		auto it = schema_versions.find(version);
		if (it != schema_versions.end()) {
			return it->second.get();
		}
	}

	// Not cached: attach and fetch schema — done outside entry_lock since it may block on I/O
	// RefreshCredentials first: InternalAttach may flush pending backfills, thus needing the credentials.
	RefreshCredentials(context);
	InternalAttach(context);
	auto &delta_catalog = *GetInternalCatalog();
	auto &schema = delta_catalog.GetSchema(context, Identifier(table_data->schema_name));
	auto transaction = schema.GetCatalogTransaction(context);
	auto table_entry = schema.LookupEntry(transaction, lookup_info);
	auto create_info = table_entry->GetInfo();

	lock_guard<mutex> l(entry_lock);
	// Re-check under lock in case another thread raced us here
	auto it = schema_versions.find(version);
	if (it != schema_versions.end()) {
		return it->second.get();
	}
	auto res = schema_versions.emplace(
	    version, make_uniq<UCTableEntry>(catalog, schema, *this, create_info->Cast<CreateTableInfo>()));
	return res.first->second.get();
};

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
	input.type = "s3";
	input.provider = "config";
	input.options = {
	    {"key_id", table_credentials.key_id},
	    {"secret", table_credentials.secret},
	    {"session_token", table_credentials.session_token},
	    {"region", catalog.credentials.aws_region},
	};
	input.scope = {table_data->storage_location};

	secret_manager.CreateSecret(context, input);
}

string TableInformation::AttachedCatalogName() const {
	auto &schema_name = table_data->schema_name;
	auto &catalog_name = table_data->catalog_name;
	auto &name = table_data->name;
	return "__unity_catalog_internal_" + catalog_name + "_" + schema_name + "_" + name;
}

void TableInformation::InternalDetach(ClientContext &context, const lock_guard<mutex> &_attach_lock) {
	if (!internal_attached_database) {
		return;
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto name = AttachedCatalogName();
	db_manager.DetachDatabase(context, Identifier(name), OnEntryNotFound::THROW_EXCEPTION);
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

bool TableInformation::IsCatalogManaged() const {
	// Databricks preview property (pre-GA)
	auto it = table_data->properties.find("delta.feature.catalogOwned-preview");
	if (it != table_data->properties.end() && it->second == "supported") {
		return true;
	}
	// Databricks GA + OSS UC v0.5+: set for tables that use the Delta CMT protocol
	it = table_data->properties.find("delta.feature.catalogManaged");
	return it != table_data->properties.end() && it->second == "supported";
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
	if (is_dirty) {
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

	AttachOptions options(context.db->config.options);
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
		for (auto &col : table.columns) {
			info.columns.AddColumn(CreateColumnDefinition(context, col));
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
		table_info.dummy = std::move(table_entry);
	}
}

void UCTableSet::EnsureLoaded(ClientContext &context) {
	lock_guard<mutex> l(load_lock);
	if (!is_loaded) {
		LoadEntries(context, l); // acquires entry_lock internally; fine, load_lock != entry_lock
		is_loaded = true;        // set only after LoadEntries succeeds
	}
}

optional_ptr<CatalogEntry> UCTableSet::CreateTable(ClientContext &context, BoundCreateTableInfo &info) {
	throw NotImplementedException("UCTableSet::CreateTable");
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
		callback(*table.second.dummy);
	}
}

} // namespace duckdb
