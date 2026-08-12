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

void UCSchemaSet::LoadEntries(ClientContext &context, SchemaSetState &state) {
	auto tables = UCAPI::GetSchemas(context, catalog, catalog.credentials);

	for (const auto &schema : tables) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName(info.GetQualifiedName().Catalog(), Identifier(schema.schema_name),
		                                    info.GetQualifiedName().Name()));
		info.internal = IsInternalTable(schema.catalog_name, schema.schema_name);
		auto schema_entry = make_uniq<UCSchemaEntry>(catalog, info);
		schema_entry->schema_data = make_uniq<UCAPISchema>(schema);
		AddEntry(state, std::move(schema_entry));
	}
}

void UCSchemaSet::EnsureLoaded(ClientContext &context, SchemaSetState &state) {
	if (state.is_loaded) {
		return;
	}
	// The list-schemas call is remote, so this holds the lock across I/O -- that exclusivity IS the
	// load-once guarantee; the alternative is readers racing an in-flight load.
	LoadEntries(context, state);
	state.is_loaded = true; // only after the fetch succeeded: a throw leaves the load retryable
}

optional_ptr<CatalogEntry> UCSchemaSet::GetEntry(ClientContext &context, const EntryLookupInfo &lookup) {
	return state.with_locked([&](SchemaSetState &s) -> optional_ptr<CatalogEntry> {
		EnsureLoaded(context, s);
		auto &name = lookup.GetEntryName();
		auto schema = s.schemas.find(name);
		if (schema == s.schemas.end()) {
			return nullptr;
		}
		return schema->second.get();
	});
}

optional_ptr<CatalogEntry> UCSchemaSet::AddEntry(SchemaSetState &state, unique_ptr<CatalogEntry> entry) {
	auto result = entry.get();
	if (result->name.empty()) {
		throw InternalException("UCSchemaSet::CreateEntry called with empty name");
	}
	state.schemas.emplace(result->name, unique_ptr_cast<CatalogEntry, SchemaCatalogEntry>(std::move(entry)));
	return result;
}

optional_ptr<CatalogEntry> UCSchemaSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	return state.with_locked(
	    [&](SchemaSetState &s) -> optional_ptr<CatalogEntry> { return AddEntry(s, std::move(entry)); });
}

void UCSchemaSet::ClearEntries() {
	state.with_locked([](SchemaSetState &s) {
		s.schemas.clear();
		s.is_loaded = false;
	});
}

void UCSchemaSet::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("UCSchemaSet::DropEntry");
}

void UCSchemaSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	state.with_locked([&](SchemaSetState &s) {
		EnsureLoaded(context, s);
		for (auto &schema : s.schemas) {
			callback(*schema.second);
		}
	});
}

optional_ptr<CatalogEntry> UCSchemaSet::CreateSchema(ClientContext &context, CreateSchemaInfo &info) {
	throw NotImplementedException("Schema creation");
}

} // namespace duckdb
