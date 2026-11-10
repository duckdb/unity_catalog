//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_create_table_as.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "storage/uc_table_set.hpp"

namespace duckdb {

//! What a CREATE TABLE AS holds from its plan to its registration
struct UCCreateTableAsState {
	UCExternalCreate create;
};

//! Registers the EXTERNAL table its child -- delta's CREATE TABLE AS write -- committed as version 0, once that commit
//! is done. Passes the child's row count on. Delta undoes a write that fails before its commit; a registration that
//! fails removes what the commit wrote.
class UCCreateTableAs : public PhysicalOperator {
public:
	UCCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types, idx_t estimated_cardinality, UCTableSet &tables,
	                shared_ptr<UCCreateTableAsState> state);

	UCTableSet &tables;
	shared_ptr<UCCreateTableAsState> state;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
