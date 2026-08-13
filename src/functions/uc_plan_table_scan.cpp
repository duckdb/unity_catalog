#include "functions/uc_plan_table_scan.hpp"

#include "uc_api.hpp"
#include "storage/unity_catalog.hpp"

namespace duckdb {

struct PlanScanBindData : public TableFunctionData {
	string endpoint;
	string catalog;
	string schema;
	string table;
	string token;
	string filter; // IRC Expression JSON; empty => no filter
};

struct PlanScanGlobalState : public GlobalTableFunctionState {
	UCScanPlanResult result;
	bool emitted = false;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PlanScanBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<PlanScanBindData>();
	result->endpoint = input.inputs[0].ToString();
	result->catalog = input.inputs[1].ToString();
	result->schema = input.inputs[2].ToString();
	result->table = input.inputs[3].ToString();
	result->token = input.inputs[4].ToString();
	auto it = input.named_parameters.find("filter");
	if (it != input.named_parameters.end()) {
		result->filter = it->second.ToString();
	}

	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT};
	names = {"status", "plan_id", "n_files", "n_delete_files", "n_plan_tasks"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> PlanScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<PlanScanBindData>();
	auto state = make_uniq<PlanScanGlobalState>();
	UCCredentials credentials;
	credentials.token = bind.token;
	// Runs the full request + submitted-status poll; an interrupt/timeout here triggers CancelPlan.
	state->result =
	    UCAPI::PlanTableScan(context, bind.catalog, bind.schema, bind.table, credentials, bind.endpoint, bind.filter);
	return std::move(state);
}

static void PlanScanFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<PlanScanGlobalState>();
	if (state.emitted) {
		output.SetChildCardinality(0);
		return;
	}
	state.emitted = true;
	auto &r = state.result;
	output.data[0].SetValue(0, Value(UCScanPlanStatusToString(r.status)));
	output.data[1].SetValue(0, r.plan_id.empty() ? Value(LogicalType::VARCHAR) : Value(r.plan_id));
	output.data[2].SetValue(0, Value::BIGINT(NumericCast<int64_t>(r.file_scan_tasks.size())));
	output.data[3].SetValue(0, Value::BIGINT(NumericCast<int64_t>(r.delete_files.size())));
	output.data[4].SetValue(0, Value::BIGINT(NumericCast<int64_t>(r.plan_tasks.size())));
	output.SetChildCardinality(1);
}

UCInternalPlanTableScanFunction::UCInternalPlanTableScanFunction()
    : TableFunction("__internal_uc_plan_table_scan",
                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                     LogicalType::VARCHAR},
                    PlanScanFunction, PlanScanBind, PlanScanInit) {
	named_parameters["filter"] = LogicalType::VARCHAR;
}

} // namespace duckdb
