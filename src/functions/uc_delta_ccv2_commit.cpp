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

unique_ptr<GlobalTableFunctionState> UCDeltaCCV2CommitInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<UCDeltaCCV2CommitState>();
}

static unique_ptr<FunctionData> UCDeltaCCV2CommitBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	throw InternalException(
	    "__internal_delta_ccv2_commit_staged is only for internal use and should not be called directly");
}

// Note: This is a quirky function ONLY for internal use, it should not be called directly. It will be called by the
// delta extension
//       manually where it will place the special value in the output chunk on row 0 and expect the output on row 1
void UCDeltaCCV2CommitExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto val = output.GetValue(0, 0);

	auto res = StructValue::GetChildren(val);

	// Read commit values from input
	// TODO (samansmink): clean this API up?
	string commit_file_path = res[0].GetValue<string>();
	idx_t commit_file_size = res[1].GetValue<idx_t>();
	idx_t commit_timestamp = res[2].GetValue<idx_t>();
	idx_t version = res[3].GetValue<idx_t>();
	auto table_entry = reinterpret_cast<UCTableEntry *>(res[4].GetPointer());
	idx_t file_modification_timestamp = res[5].GetValue<idx_t>();

	UCCredentials &credentials = table_entry->table.catalog.Cast<UnityCatalog>().credentials;
	auto &td = *table_entry->table.table_data;

	// Get relative path
	string commit_file_name = commit_file_path.substr(commit_file_path.find_last_of("/\\") + 1);

	// One consistent snapshot: etag (assert-etag token) and backfilled_version must be paired from
	// the same locked read, else a concurrent re-attach could tear them. Copy out via with_locked so
	// the lock is NOT held across the UpdateTable HTTP call below.
	auto &table = table_entry->table;
	UCCommitState cs = table.commit_state.with_locked([](const UCCommitState &s) { return s; });
	string new_etag = UCAPI::UpdateTable(context, td.catalog_name, td.schema_name, td.name, td.table_id, cs.etag,
	                                     credentials, version, commit_timestamp, commit_file_name, commit_file_size,
	                                     file_modification_timestamp, cs.backfilled_version);

	// Mark dirty so the next read re-attaches with a fresh log tail; cache the new etag for the next
	// commit's assert-etag. (is_dirty under attach_lock; etag under commit_state's own lock.)
	{
		lock_guard<mutex> l(table.attach_lock);
		table.MarkDirty(l);
	}
	table.commit_state.with_locked([&](UCCommitState &s) { s.etag = new_etag; });

	// Write the output boolean at row 0, then set cardinality. SetChildCardinality (not the
	// deprecated SetCardinality) sizes the child vectors via FlatVector::SetSize -- which
	// preserves the value just written -- and sets the chunk count.
	output.data[1].SetValue(0, Value::BOOLEAN(true));
	output.SetChildCardinality(1);
}

UCDeltaCCV2Commit::UCDeltaCCV2Commit()
    : TableFunction("__internal_delta_ccv2_commit_staged", {LogicalType::VARCHAR}, UCDeltaCCV2CommitExecute,
                    UCDeltaCCV2CommitBind, UCDeltaCCV2CommitInit) {
}
} // namespace duckdb
