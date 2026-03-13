#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class UCCheckpointTableFunction : public TableFunction {
public:
	UCCheckpointTableFunction();
};

} // namespace duckdb
