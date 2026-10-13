#include "storage/uc_schema_set.hpp"
#include "storage/unity_catalog.hpp"
#include "uc_api.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

UCSchemaSet::UCSchemaSet(UnityCatalog &catalog) : catalog(catalog) {
}

static bool IsInternalTable(const string &catalog, const string &schema) {
	if (schema == "information_schema") {
		return true;
	}
	return false;
}

void UCSchemaSet::LoadEntries(ClientContext &context) {
	auto tables = UCAPI::GetSchemas(context, catalog, catalog.credentials);

	for (const auto &schema : tables) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName(info.GetQualifiedName().Catalog(), Identifier(schema.schema_name),
		                                    info.GetQualifiedName().Name()));
		info.internal = IsInternalTable(schema.catalog_name, schema.schema_name);
		auto schema_entry = make_uniq<UCSchemaEntry>(catalog, info);
		schema_entry->schema_data = make_uniq<UCAPISchema>(schema);
		CreateEntry(std::move(schema_entry));
	}
}

optional_ptr<CatalogEntry> UCSchemaSet::GetEntry(ClientContext &context, const EntryLookupInfo &lookup) {
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
	lock_guard<mutex> l(entry_lock);
	auto &name = lookup.GetEntryName();
	auto schema = schemas.find(name);
	if (schema == schemas.end()) {
		return nullptr;
	}
	return schema->second.get();
}

optional_ptr<CatalogEntry> UCSchemaSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	lock_guard<mutex> l(entry_lock);
	auto result = entry.get();
	if (result->name.empty()) {
		throw InternalException("UCSchemaSet::CreateEntry called with empty name");
	}
	schemas.emplace(result->name, unique_ptr_cast<CatalogEntry, SchemaCatalogEntry>(std::move(entry)));
	return result;
}

void UCSchemaSet::ClearEntries() {
	schemas.clear();
	is_loaded = false;
}

void UCSchemaSet::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("UCSchemaSet::DropEntry");
}

void UCSchemaSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
	lock_guard<mutex> l(entry_lock);
	for (auto &schema : schemas) {
		callback(*schema.second);
	}
}

optional_ptr<CatalogEntry> UCSchemaSet::CreateSchema(ClientContext &context, CreateSchemaInfo &info) {
	throw NotImplementedException("Schema creation");
}

} // namespace duckdb
