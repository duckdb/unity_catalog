#include "functions/uc_table_functions.hpp"
#include "storage/uc_transaction.hpp"
#include "storage/unity_catalog.hpp"
#include "storage/uc_table_entry.hpp"
#include "storage/uc_table_set.hpp"

namespace duckdb {

struct UCDeltaCCV2CommitState final : public GlobalTableFunctionState {
	UCDeltaCCV2CommitState() {
	}
};

unique_ptr<GlobalTableFunctionState> UCDeltaCCV2CommitInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	return make_uniq<UCDeltaCCV2CommitState>();
}

static unique_ptr<FunctionData> UCDeltaCCV2CommitBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types, vector<string> &names) {
	throw InternalException("__internal_delta_ccv2_commit_staged is only for internal use and should not be called directly");
}

// Note: This is a quirky function ONLY for internal use, it should not be called directly. It will be called by the delta extension
//       manually where it will place the special value in the output chunk on row 0 and expect the output on row 1
void UCDeltaCCV2CommitExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto val = output.GetValue(0,0);

	auto res = StructValue::GetChildren(val);

	// Read commit values from input
	// TODO (samansmink): clean this API up?
	string commit_file_path = res[0].GetValue<string>();
	idx_t commit_file_size = res[1].GetValue<idx_t>();
	idx_t commit_timestamp = res[2].GetValue<idx_t>();
	idx_t version = res[3].GetValue<idx_t>();
	auto table_entry = reinterpret_cast<UCTableEntry *>(res[4].GetPointer());
	idx_t file_modification_timestamp = res[5].GetValue<idx_t>();

	string table_id = table_entry->table.table_data->table_id;
	string table_location = table_entry->table.table_data->storage_location;

	UCCredentials & credentials = table_entry->table.catalog.Cast<UnityCatalog>().credentials;

	// Get relative path
	string commit_file_name = commit_file_path.substr(commit_file_path.find_last_of("/\\") + 1);

	UCAPI::PostCommit(context, table_id, table_location, credentials, version, commit_timestamp, commit_file_name, commit_file_size, file_modification_timestamp);

	// Mark dirty after a successful commit so the next read re-attaches with a fresh log tail
	table_entry->table.MarkDirty();

	output.SetCardinality(1);
	output.SetValue(1,0, Value::BOOLEAN(true));
}

UCDeltaCCV2Commit::UCDeltaCCV2Commit()
    : TableFunction("__internal_delta_ccv2_commit_staged", {LogicalType::VARCHAR}, UCDeltaCCV2CommitExecute, UCDeltaCCV2CommitBind, UCDeltaCCV2CommitInit) {

}
} // namespace duckdb
