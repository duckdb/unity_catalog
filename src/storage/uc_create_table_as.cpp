#include "storage/uc_create_table_as.hpp"

#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

UCCreateTableAs::UCCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality,
                                 UCTableSet &tables_p, shared_ptr<UCCreateTableAsState> state_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      tables(tables_p), state(std::move(state_p)) {
}

class UCCreateTableAsGlobalState : public GlobalSinkState {
public:
	//! Rows the write reported
	int64_t row_count = 0;
	//! False when IF NOT EXISTS found the name taken at registration: then nothing was created
	bool created = false;
};

unique_ptr<GlobalSinkState> UCCreateTableAs::GetGlobalSinkState(ClientContext &context) const {
	if (state->managed && state->managed_create.staging_used) {
		// The statement runs with this staging: no later plan may reuse it
		*state->managed_create.staging_used = true;
	}
	return make_uniq<UCCreateTableAsGlobalState>();
}

SinkResultType UCCreateTableAs::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<UCCreateTableAsGlobalState>();
	for (idx_t row = 0; row < chunk.size(); row++) {
		auto count = chunk.GetValue(0, row);
		if (!count.IsNull()) {
			gstate.row_count += count.GetValue<int64_t>();
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType UCCreateTableAs::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                           OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<UCCreateTableAsGlobalState>();
	// The child's commit is done: its pipeline finished before this one started
	gstate.created = state->managed ? tables.FinishManagedCreateTableAs(context, state->managed_create)
	                                : tables.FinishExternalCreateTableAs(context, state->create);
	return SinkFinalizeType::READY;
}

SourceResultType UCCreateTableAs::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                  OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<UCCreateTableAsGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(gstate.created ? gstate.row_count : 0));
	return SourceResultType::FINISHED;
}

string UCCreateTableAs::GetName() const {
	return "UC_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> UCCreateTableAs::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	if (state->managed) {
		auto &create = state->managed_create;
		result["Table Name"] = create.catalog_name + "." + create.schema_name + "." + create.table_name;
		result["Location"] = create.staging.location;
		result["Staged Table Id"] = create.staging.table_id;
		return result;
	}
	result["Table Name"] =
	    state->create.catalog_name + "." + state->create.schema_name + "." + state->create.table_name;
	result["Location"] = state->create.location;
	return result;
}

} // namespace duckdb
