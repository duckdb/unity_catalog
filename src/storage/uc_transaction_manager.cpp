#include "storage/uc_transaction_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "storage/uc_schema_entry.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

UCTransactionManager::UCTransactionManager(AttachedDatabase &db_p, UnityCatalog &unity_catalog)
    : TransactionManager(db_p), unity_catalog(unity_catalog) {
}

Transaction &UCTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<UCTransaction>(unity_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData UCTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &uc_transaction = transaction.Cast<UCTransaction>();
	uc_transaction.Commit();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void UCTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &uc_transaction = transaction.Cast<UCTransaction>();
	uc_transaction.Rollback();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void UCTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// TODO: remove or update syntax from DuckDB perspective
#if 0
	unity_catalog.ScanSchemas(context, [&](SchemaCatalogEntry &entry) {
		auto &schema = entry.Cast<UCSchemaEntry>();
		schema.tables.Checkpoint(context, force);
	});
#else
	throw NotImplementedException("Unsupported: CHECKPOINT <catalog> unsupported in unity_catalog; please use "
	                              "function unity_catalog_checkpoint_table() for per-table checkpoints.");
#endif
}

} // namespace duckdb
