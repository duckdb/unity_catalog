#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

// Serialize a vector of DuckDB filter expressions to a single IRC Expression
// JSON string.  Multiple filters are ANDed together.  Returns "" when filters
// is empty (caller should omit the filter field from the request body).
// Unsupported expressions are dropped; the local reader still applies the full predicate.
string SerializeFiltersToIRC(const vector<unique_ptr<Expression>> &filters);

} // namespace duckdb
