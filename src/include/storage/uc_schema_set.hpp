//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_schema_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/uc_schema_entry.hpp"
#include "uc_mutex_protected.hpp"

namespace duckdb {
struct CreateSchemaInfo;
class UnityCatalog;

class UCSchemaSet {
public:
	explicit UCSchemaSet(UnityCatalog &catalog);

public:
	optional_ptr<CatalogEntry> CreateSchema(ClientContext &context, CreateSchemaInfo &info);
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const EntryLookupInfo &lookup);
	virtual void DropEntry(ClientContext &context, DropInfo &info);
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	virtual optional_ptr<CatalogEntry> CreateEntry(unique_ptr<CatalogEntry> entry);
	void ClearEntries();

private:
	//! The lazily-loaded schema list plus its load-once flag: both must move together, or a reader can
	//! see is_loaded == true over a map the loader has not finished populating.
	struct SchemaSetState {
		case_insensitive_map_t<unique_ptr<SchemaCatalogEntry>> schemas;
		bool is_loaded = false;
	};

	//! Load-once. The flag check, the fetch and the flag set are one critical section, so concurrent
	//! readers block until the list is complete rather than observing a partial one.
	void EnsureLoaded(ClientContext &context, SchemaSetState &state);
	void LoadEntries(ClientContext &context, SchemaSetState &state);
	optional_ptr<CatalogEntry> AddEntry(SchemaSetState &state, unique_ptr<CatalogEntry> entry);

	UnityCatalog &catalog;
	MutexProtected<SchemaSetState> state;
};

} // namespace duckdb
