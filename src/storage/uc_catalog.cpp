#include "storage/uc_catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

UCCatalog::UCCatalog(AttachedDatabase &db_p, const string &internal_name, AttachOptions &attach_options,
                     UCCredentials credentials, const string &default_schema)
    : Catalog(db_p), internal_name(internal_name), access_mode(attach_options.access_mode),
      credentials(std::move(credentials)), schemas(*this), default_schema(default_schema) {
}

UCCatalog::~UCCatalog() = default;

void UCCatalog::Initialize(bool load_builtin) {
}

optional_ptr<CatalogEntry> UCCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		DropInfo try_drop;
		try_drop.type = CatalogType::SCHEMA_ENTRY;
		try_drop.name = info.schema;
		try_drop.if_not_found = OnEntryNotFound::RETURN_NULL;
		try_drop.cascade = false;
		schemas.DropEntry(transaction.GetContext(), try_drop);
	}
	return schemas.CreateSchema(transaction.GetContext(), info);
}

void UCCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	return schemas.DropEntry(context, info);
}

void UCCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas.Scan(context, [&](CatalogEntry &schema) { callback(schema.Cast<UCSchemaEntry>()); });
}

optional_ptr<SchemaCatalogEntry> UCCatalog::LookupSchema(CatalogTransaction transaction,
                                                         const EntryLookupInfo &schema_lookup,
                                                         OnEntryNotFound if_not_found) {
	if (schema_lookup.GetEntryName() == DEFAULT_SCHEMA && default_schema != DEFAULT_SCHEMA) {
		if (default_schema.empty()) {
			throw InvalidInputException("Attempting to fetch the default schema - but no database was "
			                            "provided by the catalog");
		}
		return GetSchema(transaction, default_schema, if_not_found);
	}
	auto entry = schemas.GetEntry(transaction.GetContext(), schema_lookup.GetEntryName());
	if (!entry && if_not_found != OnEntryNotFound::RETURN_NULL) {
		throw BinderException("Schema with name \"%s\" not found", schema_lookup.GetEntryName());
	}
	return reinterpret_cast<SchemaCatalogEntry *>(entry.get());
}

bool UCCatalog::InMemory() {
	return false;
}

string UCCatalog::GetDBPath() {
	return internal_name;
}

string UCCatalog::GetDefaultSchema() const {
	return default_schema;
}

DatabaseSize UCCatalog::GetDatabaseSize(ClientContext &context) {
	if (default_schema.empty()) {
		throw InvalidInputException("Attempting to fetch the database size - but no database was provided "
		                            "in the connection string");
	}
	DatabaseSize size;
	return size;
}

void UCCatalog::ClearCache() {
	schemas.ClearEntries();
}

PhysicalOperator &UCCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                               LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("UCCatalog PlanCreateTableAs");
}

PhysicalOperator &UCCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                        optional_ptr<PhysicalOperator> plan) {
	auto &table = op.table.Cast<UCTableEntry>();

	// LAZY CREATE ATTACHED DB
	// TODO: move to transaction?
	if (!table.internal_attached_database) {
		auto &db_manager = DatabaseManager::Get(context);

		// Create the attach info for the table
		AttachInfo info;
		info.name = "__uc_catalog_internal_" + internal_name + "_" + table.schema.name + "_" + table.name; // TODO:
		info.options = {
		    {"type", Value("Delta")}, {"child_catalog_mode", Value(true)}, {"internal_table_name", Value(table.name)}};
		info.path = table.table_data->storage_location;
		AttachOptions options(context.db->config.options);
		options.access_mode = AccessMode::READ_WRITE;
		options.db_type = "delta";
		auto &internal_db = table.internal_attached_database;

		internal_db = db_manager.AttachDatabase(context, info, options);

		//! Initialize the database.
		internal_db->Initialize(context);
		internal_db->FinalizeLoad(context);
		db_manager.FinalizeAttach(context, info, internal_db);
	}

	// LOAD THE INTERNAL TABLE ENTRY
	auto internal_catalog = table.GetInternalCatalog();

	// CREATE TMP CREDENTIALS TODO: dedup with getScanFunction
	auto &table_data = table.table_data;
	if (table_data->storage_location.find("file://") != 0) {
		auto &secret_manager = SecretManager::Get(context);
		// Get Credentials from UCAPI
		auto table_credentials = UCAPI::GetTableCredentials(table_data->table_id, credentials);

		// Inject secret into secret manager sc oped to this path TODO:
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
		    {"region", credentials.aws_region},
		};
		input.scope = {table_data->storage_location};

		secret_manager.CreateSecret(context, input);
	}

	return internal_catalog->PlanInsert(context, planner, op, plan);
}

PhysicalOperator &UCCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                        PhysicalOperator &plan) {
	throw NotImplementedException("UCCatalog PlanDelete");
}

PhysicalOperator &UCCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	throw NotImplementedException("UCCatalog PlanDelete");
}

PhysicalOperator &UCCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                        PhysicalOperator &plan) {
	throw NotImplementedException("UCCatalog PlanUpdate");
}

unique_ptr<LogicalOperator> UCCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
                                                       unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("UCCatalog BindCreateIndex");
}

} // namespace duckdb
