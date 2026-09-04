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

namespace duckdb {
class UCSchemaEntry;
class UCTransaction;

enum class UCTypeAnnotation { STANDARD, CAST_TO_VARCHAR, NUMERIC_AS_DOUBLE, CTID, JSONB, FIXED_LENGTH_CHAR };

struct UCType {
	idx_t oid = 0;
	UCTypeAnnotation info = UCTypeAnnotation::STANDARD;
	vector<UCType> children;
};

class UCUtils {
public:
	static LogicalType ToUCType(const LogicalType &input);
	static LogicalType ColumnTypeFromDefinition(ClientContext &context, const UCAPIColumnDefinition &column);
	static LogicalType TypeFromJson(ClientContext &context, const string &type_json);
	static LogicalType TypeToLogicalType(ClientContext &context, const string &columnDefinition);
	static string TypeToString(const LogicalType &input);
};

} // namespace duckdb
