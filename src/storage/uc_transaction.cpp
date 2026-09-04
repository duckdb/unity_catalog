#include "storage/uc_transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "storage/unity_catalog.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"

namespace duckdb {

UCTransaction::UCTransaction(UnityCatalog &unity_catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), access_mode(unity_catalog.access_mode) {
	//	connection = UCConnection::Open(unity_catalog.path);
}

UCTransaction::~UCTransaction() = default;

void UCTransaction::Start() {
	transaction_state = UCTransactionState::TRANSACTION_NOT_YET_STARTED;
}
void UCTransaction::Commit() {
	if (transaction_state == UCTransactionState::TRANSACTION_STARTED) {
		transaction_state = UCTransactionState::TRANSACTION_FINISHED;
		//		connection.Execute("COMMIT");
	}
}
void UCTransaction::Rollback() {
	if (transaction_state == UCTransactionState::TRANSACTION_STARTED) {
		transaction_state = UCTransactionState::TRANSACTION_FINISHED;
		//		connection.Execute("ROLLBACK");
	}
}

// UCConnection &UCTransaction::GetConnection() {
//	if (transaction_state == UCTransactionState::TRANSACTION_NOT_YET_STARTED) {
//		transaction_state = UCTransactionState::TRANSACTION_STARTED;
//		string query = "START TRANSACTION";
//		if (access_mode == AccessMode::READ_ONLY) {
//			query += " READ ONLY";
//		}
////		conne/**/ction.Execute(query);
//	}
//	return connection;
//}

// unique_ptr<UCResult> UCTransaction::Query(const string &query) {
//	if (transaction_state == UCTransactionState::TRANSACTION_NOT_YET_STARTED) {
//		transaction_state = UCTransactionState::TRANSACTION_STARTED;
//		string transaction_start = "START TRANSACTION";
//		if (access_mode == AccessMode::READ_ONLY) {
//			transaction_start += " READ ONLY";
//		}
//		connection.Query(transaction_start);
//		return connection.Query(query);
//	}
//	return connection.Query(query);
//}

optional_ptr<CatalogEntry> UCTransaction::GetTableEntry(const string &qualified_name) {
	auto entry = table_entries.find(qualified_name);
	if (entry == table_entries.end()) {
		return nullptr;
	}
	return *entry->second;
}

CatalogEntry &UCTransaction::SetTableEntry(const string &qualified_name, unique_ptr<CatalogEntry> entry) {
	auto &stored = table_entries[qualified_name];
	stored = std::move(entry);
	return *stored;
}

UCTransaction &UCTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<UCTransaction>();
}

} // namespace duckdb
