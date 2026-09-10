#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

// __internal_uc_plan_table_scan(endpoint, catalog, schema, table, token [, filter => <json>])
//   -> table(status VARCHAR, plan_id VARCHAR, n_files BIGINT, n_delete_files BIGINT, n_plan_tasks BIGINT)
//
// Drives UCAPI::PlanTableScan directly, bypassing catalog attach, so the request/poll/cancel path
// can be exercised against a mock server (test/irc/).
class UCInternalPlanTableScanFunction : public TableFunction {
public:
	UCInternalPlanTableScanFunction();
};

} // namespace duckdb
