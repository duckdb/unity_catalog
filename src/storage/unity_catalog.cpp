#include "storage/unity_catalog.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

UnityCatalog::UnityCatalog(AttachedDatabase &db_p, const string &internal_name, AttachOptions &attach_options,
                           UCCredentials credentials, const string &default_schema, string catalog_name_p)
    : Catalog(db_p), internal_name(internal_name), access_mode(attach_options.access_mode),
      credentials(std::move(credentials)), catalog_name(std::move(catalog_name_p)), schemas(*this),
      default_schema(default_schema) {
}

UnityCatalog::~UnityCatalog() = default;

void UnityCatalog::Initialize(bool load_builtin) {
}

optional_ptr<CatalogEntry> UnityCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		DropInfo try_drop;
		try_drop.type = CatalogType::SCHEMA_ENTRY;
		try_drop.SetName(info.GetQualifiedName().Schema());
		try_drop.if_not_found = OnEntryNotFound::RETURN_NULL;
		try_drop.cascade = false;
		schemas.DropEntry(transaction.GetContext(), try_drop);
	}
	return schemas.CreateSchema(transaction.GetContext(), info);
}

void UnityCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	return schemas.DropEntry(context, info);
}

void UnityCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas.Scan(context, [&](CatalogEntry &schema) { callback(schema.Cast<UCSchemaEntry>()); });
}

optional_ptr<SchemaCatalogEntry> UnityCatalog::LookupSchema(CatalogTransaction transaction,
                                                            const EntryLookupInfo &schema_lookup,
                                                            OnEntryNotFound if_not_found) {
	if (schema_lookup.GetEntryName() == DEFAULT_SCHEMA && default_schema != DEFAULT_SCHEMA) {
		if (default_schema.empty()) {
			throw InvalidInputException(
			    "Default schema for catalog '%s' not found. This means auto-detection of default schema failed. Please "
			    "specify a DEFAULT_SCHEMA on ATTACH: `ATTACH '..' (TYPE unity_catalog, DEFAULT_SCHEMA 'my_schema')`",
			    GetName());
		}
		return GetSchema(transaction, Identifier(default_schema), if_not_found);
	}
	auto entry = schemas.GetEntry(transaction.GetContext(), schema_lookup);
	if (!entry && if_not_found != OnEntryNotFound::RETURN_NULL) {
		throw BinderException("Schema with name \"%s\" not found", schema_lookup.GetEntryName());
	}
	return reinterpret_cast<SchemaCatalogEntry *>(entry.get());
}

bool UnityCatalog::InMemory() {
	return false;
}

string UnityCatalog::GetDBPath() {
	return internal_name;
}

string UnityCatalog::GetDefaultSchema() const {
	return default_schema;
}

void UnityCatalog::OnDetach(ClientContext &context) {
	schemas.Scan(context, [&](CatalogEntry &entry) {
		auto &schema = entry.Cast<UCSchemaEntry>();
		auto &tables = schema.tables;
		tables.OnDetach(context);
	});
}

DatabaseSize UnityCatalog::GetDatabaseSize(ClientContext &context) {
	if (default_schema.empty()) {
		throw InvalidInputException("Attempting to fetch the database size - but no database was provided "
		                            "in the connection string");
	}
	DatabaseSize size;
	return size;
}

void UnityCatalog::ClearCache() {
	schemas.ClearEntries();
}

PhysicalOperator &UnityCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                  LogicalCreateTable &op, PhysicalOperator &plan) {
	throw NotImplementedException("UnityCatalog PlanCreateTableAs");
}

PhysicalOperator &UnityCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                           optional_ptr<PhysicalOperator> plan) {
	auto &table_entry = op.table.Cast<UCTableEntry>();
	auto &table = table_entry.table;

	// Credentials before Attach, since attach may backfill and need them.
	table.RefreshCredentials(context);
	table.InternalAttach(context);

	auto internal_catalog = table.GetInternalCatalog();
	return internal_catalog->PlanInsert(context, planner, op, plan);
}

PhysicalOperator &UnityCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("UnityCatalog PlanDelete");
}

PhysicalOperator &UnityCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) {
	throw NotImplementedException("UnityCatalog PlanDelete");
}

PhysicalOperator &UnityCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                           PhysicalOperator &plan) {
	throw NotImplementedException("UnityCatalog PlanUpdate");
}

unique_ptr<LogicalOperator> UnityCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                          TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("UnityCatalog BindCreateIndex");
}

} // namespace duckdb
