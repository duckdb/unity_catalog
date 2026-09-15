//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_table_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/uc_table_entry.hpp"
#include "uc_mutex_protected.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {
struct CreateTableInfo;
class UCResult;
class UnityCatalog;
class UCSchemaEntry;

// Mutable catalog-managed commit state for one table, read together when building an
// UpdateTable (add-commit) request. etag is the optimistic-concurrency token (assert-etag);
// backfilled_version is the backfill watermark (sent via set-latest-backfilled-version). They
// must be read as a consistent snapshot: a torn read could pair a stale etag with a newer
// watermark. Wrapped in MutexProtected (TableInformation::commit_state) so the pair is only ever
// accessed under its lock — copy a snapshot out via with_locked() to use past the critical section.
struct UCCommitState {
	string etag; // from LoadTable response; sent as assert-etag in UpdateTable (add-commit) calls
	// Delta CMT backfill: on InternalAttach, LoadTable returns outstanding staged commits;
	// BackfillCommits copies them into _delta_log/ and advances this watermark.
	// backfilled_version = newest version copied into _delta_log/ (a plain Delta reader sees it
	// without the catalog gate). Versions <= it are skipped to avoid redundant S3 round-trips.
	optional_idx backfilled_version; // highest version copied; invalid until the first backfill
};

class TableInformation {
public:
	TableInformation(UnityCatalog &catalog, UCSchemaEntry &schema) : catalog(catalog), schema(schema) {
	}

public:
	optional_ptr<CatalogEntry> GetVersion(ClientContext &context, const EntryLookupInfo &lookup);
	optional_ptr<Catalog> GetInternalCatalog();
	void RefreshCredentials(ClientContext &context);
	void InternalAttach(ClientContext &context);
	void InternalDetach(ClientContext &context, const lock_guard<mutex> &_attach_lock);
	void InternalCheckpoint(ClientContext &context, bool force);
	bool IsCatalogManaged() const;
	// is_dirty still lives under attach_lock; the guard-by-ref proves the caller holds it (idiom
	// from InternalDetach). commit_state moved to a MutexProtected member below.
	void MarkDirty(const lock_guard<mutex> &_attach_lock);

private:
	string AttachedCatalogName() const;
	// Copies outstanding staged commits to _delta_log/ (file I/O, NOT under commit_state's lock).
	// Takes the current watermark, returns the highest version successfully copied.
	optional_idx BackfillCommits(ClientContext &context, const vector<UCAPICommit> &commits,
	                             optional_idx backfilled_version);
	bool is_dirty = false;

public:
	// commit_state {etag, backfilled_version}: accessed only via with_locked(), so the pair is never
	// torn and there is no unlocked path. Public is safe — MutexProtected gates every access.
	MutexProtected<UCCommitState> commit_state;
	UnityCatalog &catalog;
	UCSchemaEntry &schema;
	unique_ptr<UCAPITable> table_data;
	shared_ptr<AttachedDatabase> internal_attached_database;
	optional_ptr<Transaction> active_transaction;

	//! Guards schema_versions and dummy
	mutex entry_lock;
	//! Guards is_dirty and internal_attached_database (commit_state is now self-guarding above)
	//
	// TODO(locks): widen the MutexProtected pattern used by commit_state to also cover is_dirty and
	// internal_attached_database — fold all three into a single MutexProtected<AttachState> and drop
	// this bare mutex. That makes unlocked access unrepresentable (subsuming the TODO(race) reads of
	// internal_attached_database in GetInternalCatalog/InternalCheckpoint). It MUST stay ONE protected
	// struct to preserve InternalAttach's single critical section across all three.
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
