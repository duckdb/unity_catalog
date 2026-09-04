//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {
class UnityCatalog;
class UCSchemaEntry;
class UCTableEntry;

enum class UCTransactionState { TRANSACTION_NOT_YET_STARTED, TRANSACTION_STARTED, TRANSACTION_FINISHED };

class UCTransaction : public Transaction {
public:
	UCTransaction(UnityCatalog &unity_catalog, TransactionManager &manager, ClientContext &context);
	~UCTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	//	UCConnection &GetConnection();
	//	unique_ptr<UCResult> Query(const string &query);
	static UCTransaction &Get(ClientContext &context, Catalog &catalog);
	AccessMode GetAccessMode() const {
		return access_mode;
	}

	//! Schemas resolved from the Delta log, keyed by qualified name: a statement binds against one of
	//! these, so they outlive every statement that could still be reading them.
	optional_ptr<CatalogEntry> GetTableEntry(const string &qualified_name);
	CatalogEntry &SetTableEntry(const string &qualified_name, unique_ptr<CatalogEntry> entry);

private:
	unordered_map<string, unique_ptr<CatalogEntry>> table_entries;
	//	UCConnection connection;
	UCTransactionState transaction_state;
	AccessMode access_mode;
};

} // namespace duckdb
