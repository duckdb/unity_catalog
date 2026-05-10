//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_table_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/uc_table_entry.hpp"

namespace duckdb {
struct CreateTableInfo;
class UCResult;
class UnityCatalog;
class UCSchemaEntry;

class TableInformation {
public:
	TableInformation(UnityCatalog &catalog, UCSchemaEntry &schema) : catalog(catalog), schema(schema) {
	}

public:
	optional_ptr<CatalogEntry> GetVersion(ClientContext &context, const EntryLookupInfo &lookup);
	optional_ptr<Catalog> GetInternalCatalog();
	void RefreshCredentials(ClientContext &context, bool write = false);
	void InternalAttach(ClientContext &context);
	void InternalDetach(ClientContext &context, const lock_guard<mutex> &_attach_lock);
	void InternalCheckpoint(ClientContext &context, bool force);
	bool IsCCV2() const;
	Value BuildLogTail(ClientContext &context);
	void MarkDirty();

private:
	string AttachedCatalogName() const;
	bool is_dirty = false;

public:
	UnityCatalog &catalog;
	UCSchemaEntry &schema;
	unique_ptr<UCAPITable> table_data;
	shared_ptr<AttachedDatabase> internal_attached_database;
	optional_ptr<Transaction> active_transaction;

	//! Guards schema_versions and dummy
	mutex entry_lock;
	//! Guards is_dirty and internal_attached_database
	mutex attach_lock;
	//! Map of delta version to TableCatalogEntry for the table
	unordered_map<idx_t, unique_ptr<CatalogEntry>> schema_versions;
	//! Dummy entry created from the "List tables" API result, presumably the latest schema version
	//! Only used for things like SHOW TABLES
	unique_ptr<CatalogEntry> dummy;
};

class UCTableSet {
public:
	explicit UCTableSet(UCSchemaEntry &schema);

public:
	optional_ptr<CatalogEntry> CreateTable(ClientContext &context, BoundCreateTableInfo &info);
	void AlterTable(ClientContext &context, AlterTableInfo &info);

	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const EntryLookupInfo &lookup);
	void DropEntry(ClientContext &context, DropInfo &info);
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	void ClearEntries();
	void OnDetach(ClientContext &context);
	// void Checkpoint(ClientContext &context, bool force); TODO: remove/update (see definition)
	void CheckpointTable(ClientContext &context, const string &table_name, bool force = false);

protected:
	void LoadEntries(ClientContext &context, const lock_guard<mutex> &_entry_lock);

	void AlterTable(ClientContext &context, RenameTableInfo &info);
	void AlterTable(ClientContext &context, RenameColumnInfo &info);
	void AlterTable(ClientContext &context, AddColumnInfo &info);
	void AlterTable(ClientContext &context, RemoveColumnInfo &info);

private:
	// Ensure tables are loaded exactly once, must be done before entry_lock.
	void EnsureLoaded(ClientContext &context);

	UnityCatalog &catalog;
	UCSchemaEntry &schema;
	mutex load_lock; // Guard is_loaded
	mutex entry_lock;
	case_insensitive_map_t<TableInformation> tables;
	bool is_loaded = false;
};

} // namespace duckdb
