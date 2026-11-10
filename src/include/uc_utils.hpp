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
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
class UCSchemaEntry;
class UCTransaction;

enum class UCTypeAnnotation { STANDARD, CAST_TO_VARCHAR, NUMERIC_AS_DOUBLE, CTID, JSONB, FIXED_LENGTH_CHAR };

struct UCType {
	idx_t oid = 0;
	UCTypeAnnotation info = UCTypeAnnotation::STANDARD;
	vector<UCType> children;
};

//! Fill in a secret's type and options from UC-vended credentials, dispatching on the scheme of
//! `storage_location`: a gcs secret with the bearer token for gs:// and gcs://, else an s3 secret.
//! `aws_region` is the s3 secret's region: the catalog secret's for UC's credential endpoints, the
//! `client.region` /plan vends for a planned scan. A gcs secret takes none.
void ApplyVendedCredentials(ClientContext &context, CreateSecretInput &input, const string &storage_location,
                            const UCAPITableCredentials &credentials, const string &aws_region);

class UCUtils {
public:
	static LogicalType ToUCType(const LogicalType &input);
	static LogicalType ColumnTypeFromDefinition(const UCAPIColumnDefinition &column);
	static LogicalType TypeFromJson(const string &type_json);
	static LogicalType TypeToLogicalType(const string &columnDefinition);
	static string TypeToString(const LogicalType &input);
};

} // namespace duckdb
