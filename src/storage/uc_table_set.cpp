#include "uc_api.hpp"
#include "uc_utils.hpp"

#include "storage/unity_catalog.hpp"
#include "storage/uc_table_set.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "storage/uc_schema_entry.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

static idx_t ParseDeltaVersionFromAtClause(const BoundAtClause &at_clause) {
	if (StringUtil::Lower(at_clause.Unit()) != "version") {
		throw InvalidConfigurationException("Delta tables only support at_clause with unit 'version'");
	}
	Value version_value = at_clause.GetValue();
	if (!version_value.DefaultTryCastAs(LogicalType::UBIGINT, false)) {
		throw InvalidInputException("Failed to parse version number '%s' into a valid version",
		                            at_clause.GetValue().ToString().c_str());
	}
	return version_value.GetValue<idx_t>();
}

UCTableSet::UCTableSet(UCSchemaEntry &schema) : catalog(schema.ParentCatalog().Cast<UnityCatalog>()), schema(schema) {
}

static ColumnDefinition CreateColumnDefinition(ClientContext &context, UCAPIColumnDefinition &coldef) {
	return {coldef.name, UCUtils::TypeToLogicalType(context, coldef.type_text)};
}

optional_ptr<CatalogEntry> TableInformation::GetVersion(ClientContext &context, const EntryLookupInfo &lookup_info) {
	lock_guard<mutex> l(entry_lock);
	auto at = lookup_info.GetAtClause();
	if (!at) {
		//! No version provided, just return the dummy entry (should represent latest version)
		return dummy.get();
	}

	auto version = ParseDeltaVersionFromAtClause(*at);
	auto it = schema_versions.find(version);
	if (it == schema_versions.end()) {
		InternalAttach(context);
		auto &delta_catalog = *GetInternalCatalog();
		RefreshCredentials(context);
		auto &schema = delta_catalog.GetSchema(context, table_data->schema_name);
		auto transaction = schema.GetCatalogTransaction(context);
		auto table_entry = schema.LookupEntry(transaction, lookup_info);
		auto create_info = table_entry->GetInfo();
		auto res = schema_versions.emplace(
		    version, make_uniq<UCTableEntry>(catalog, schema, *this, create_info->Cast<CreateTableInfo>()));
		return res.first->second.get();
	}
	auto &entry = it->second;
	return entry.get();
};

optional_ptr<Catalog> TableInformation::GetInternalCatalog() {
	return internal_attached_database->GetCatalog();
}

void TableInformation::RefreshCredentials(ClientContext &context) {
	D_ASSERT(table_data);
	if (table_data->storage_location.find("file://") == 0) {
		return;
	}
	auto &secret_manager = SecretManager::Get(context);
	// Get Credentials from UCAPI
	auto table_credentials = UCAPI::GetTableCredentials(context, table_data->table_id, catalog.credentials);

	// Inject secret into secret manager scoped to this path
	CreateSecretInput input;
	input.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	input.persist_type = SecretPersistType::TEMPORARY;
	input.name = "__internal_uc_" + table_data->table_id;
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

void TableInformation::InternalDetach(ClientContext &context) {
	if (!internal_attached_database) {
		return;
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto name = AttachedCatalogName();
	db_manager.DetachDatabase(context, name, OnEntryNotFound::THROW_EXCEPTION);
}

void TableInformation::InternalAttach(ClientContext &context) {
	if (internal_attached_database) {
		return;
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto &name = table_data->name;

	// Create the attach info for the table
	AttachInfo info;
	info.name = AttachedCatalogName();
	info.options = {
	    {"type", Value("Delta")}, {"child_catalog_mode", Value(true)}, {"internal_table_name", Value(name)}};
	info.path = table_data->storage_location;
	AttachOptions options(context.db->config.options);
	options.access_mode = AccessMode::READ_WRITE;
	options.db_type = "delta";

	auto &internal_db = internal_attached_database;
	internal_db = db_manager.AttachDatabase(context, info, options);
}

void UCTableSet::OnDetach(ClientContext &context) {
	for (auto &entry : tables) {
		auto &table = entry.second;
		table.InternalDetach(context);
	}
}

void TableInformation::InternalCheckpoint(ClientContext &context, bool force) {
	// attach and call down to Delta's table specific checkpoint
	D_ASSERT(table_data);
	RefreshCredentials(context);
	InternalAttach(context);
	internal_attached_database->GetTransactionManager().Checkpoint(context, force);
}

void UCTableSet::CheckpointTable(ClientContext &context, const string &table_name, bool force) {
	if (!is_loaded) {
		LoadEntries(context);
		is_loaded = true;
	}
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

void UCTableSet::LoadEntries(ClientContext &context) {
	auto &unity_catalog = catalog.Cast<UnityCatalog>();
	auto get_tables_result = UCAPI::GetTables(context, catalog, schema.name, unity_catalog.credentials);

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

		info.table = table.name;
		auto table_entry = make_uniq<UCTableEntry>(catalog, schema, table_info, info);

		table_info.table_data = make_uniq<UCAPITable>(table);
		table_info.dummy = std::move(table_entry);
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
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
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
	tables.clear();
	is_loaded = false;
}

void UCTableSet::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("UCTableSet::DropEntry");
}

void UCTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
	lock_guard<mutex> l(entry_lock);
	for (auto &table : tables) {
		callback(*table.second.dummy);
	}
}

} // namespace duckdb
