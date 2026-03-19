#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class UCCheckpointTableFunction : public TableFunction {
public:
	UCCheckpointTableFunction();
};

class UCForceCheckpointTableFunction : public TableFunction {
public:
	UCForceCheckpointTableFunction();
};

} // namespace duckdb
