#include "functions/uc_checkpoint.hpp"

#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include "storage/uc_schema_entry.hpp"
#include "storage/unity_catalog.hpp"

namespace duckdb {

struct CheckpointTableBindData : public TableFunctionData {
	string catalog_name;
	string schema_name;
	string table_name;
	bool force = false;
	bool finished = false;
};

// Resolve a table name (catalog.schema.table, schema.table, or table)
// against the client's current search path defaults.
static CheckpointTableBindData ResolveTableName(ClientContext &context, const string &input) {
	auto qname = QualifiedName::Parse(input);
	auto &search_path = ClientData::Get(context).catalog_search_path->GetDefault();

	CheckpointTableBindData result;
	result.catalog_name = qname.catalog.empty() ? search_path.catalog : qname.catalog;
	result.schema_name = qname.schema.empty() ? search_path.schema : qname.schema;
	result.table_name = qname.name;

	if (result.catalog_name.empty()) {
		throw InvalidInputException(
		    "No catalog specified and no default catalog set (use 'USE <catalog>' or provide fully-qualified name)");
	}
	if (result.schema_name.empty()) {
		throw InvalidInputException(
		    "No schema specified and no default schema set (use 'USE <catalog>.<schema>' or provide qualified name)");
	}
	if (result.table_name.empty()) {
		throw InvalidInputException("Table name must not be empty");
	}
	return result;
}

template <bool FORCE>
static unique_ptr<FunctionData> CheckpointTableBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<CheckpointTableBindData>(ResolveTableName(context, input.inputs[0].ToString()));
	result->force = FORCE;
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static void CheckpointTableFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<CheckpointTableBindData>();
	if (data.finished) {
		return;
	}
	data.finished = true;

	CatalogEntryRetriever catalog_entry_retriever(context);
	EntryLookupInfo lookup_table = {CatalogType::TABLE_ENTRY, data.table_name};
	auto tbl_entry = catalog_entry_retriever.GetEntry(data.catalog_name, data.schema_name, lookup_table);
	if (!tbl_entry) {
		throw InvalidInputException("Unity Catalog table not found: '%s.%s.%s'", data.catalog_name, data.schema_name,
		                            data.table_name);
	}

	if (!UnityCatalog::IsUnityCatalog(tbl_entry->ParentCatalog())) {
		throw InvalidInputException("Catalog not a Unity Catalog: '%s'", data.catalog_name);
	}

	tbl_entry->Cast<UCTableEntry>().table.InternalCheckpoint(context, data.force);
}

UCCheckpointTableFunction::UCCheckpointTableFunction()
    : TableFunction("unity_catalog_checkpoint_table", {LogicalType::VARCHAR}, CheckpointTableFunction,
                    CheckpointTableBind<false>) {
}

UCForceCheckpointTableFunction::UCForceCheckpointTableFunction()
    : TableFunction("unity_catalog_force_checkpoint_table", {LogicalType::VARCHAR}, CheckpointTableFunction,
                    CheckpointTableBind<true>) {
}

} // namespace duckdb
