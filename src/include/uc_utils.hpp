//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mysql_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "uc_api.hpp"
#include "duckdb/common/mutex.hpp"
#include "storage/uc_catalog.hpp"
#include <unordered_map>
#include <chrono>

namespace duckdb {
class UCSchemaEntry;
class UCTransaction;

enum class UCTypeAnnotation { STANDARD, CAST_TO_VARCHAR, NUMERIC_AS_DOUBLE, CTID, JSONB, FIXED_LENGTH_CHAR };

struct UCType {
	idx_t oid = 0;
	UCTypeAnnotation info = UCTypeAnnotation::STANDARD;
	vector<UCType> children;
};

/**
 * Manages AWS temporary credentials caching for Unity Catalog tables.
 * Provides thread-safe credential caching with expiration checking.
 */
class UCTableCredentialManager {
public:
	/**
	 * Get the singleton instance of the credential manager.
	 */
	static UCTableCredentialManager &GetInstance();

	/**
	 * Ensure that valid AWS credentials are cached for the given table.
	 * This method handles mutex locking, expiration checking, and credential refresh.
	 *
	 * @param context The client context
	 * @param table_id The Unity Catalog table ID
	 * @param storage_location The storage location path
	 * @param credentials The Unity Catalog credentials
	 */
	void EnsureTableCredentials(ClientContext &context, const string &table_id, const string &storage_location,
	                            const UCCredentials &credentials);

private:
	UCTableCredentialManager() = default;
	~UCTableCredentialManager() = default;
	UCTableCredentialManager(const UCTableCredentialManager &) = delete;
	UCTableCredentialManager &operator=(const UCTableCredentialManager &) = delete;

	// Mutex map to synchronize secret creation per table_id
	unordered_map<string, unique_ptr<mutex>> table_secret_mutexes;
	mutex mutex_map_mutex;
	// Track secret expiration times per table_id (Unix epoch timestamp in milliseconds)
	unordered_map<string, int64_t> secret_expiration_times;

	// Safety margin for credential refresh (5 minutes in milliseconds)
	static constexpr int64_t REFRESH_SAFETY_MARGIN_MS = 300000;
};

class UCUtils {
public:
	static LogicalType ToUCType(const LogicalType &input);
	static LogicalType TypeToLogicalType(ClientContext &context, const string &columnDefinition);
	static string TypeToString(const LogicalType &input);
};

} // namespace duckdb
