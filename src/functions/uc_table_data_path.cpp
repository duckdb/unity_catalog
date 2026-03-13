#include "functions/uc_table_functions.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_table_entry.hpp"
#include "storage/uc_table_set.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"

namespace duckdb {

struct UCTableDataPathInfo : public TableFunctionInfo {
	explicit UCTableDataPathInfo(UCSchemaEntry &schema) : schema(schema) {
	}
	UCSchemaEntry &schema;
};

struct UCTableDataPathBindData : public TableFunctionData {
	string storage_location;
	bool finished = false;
};

static unique_ptr<FunctionData> UCTableDataPathBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	if (!input.info) {
		throw InternalException("table_data_path: missing function info");
	}
	auto &func_info = input.info->Cast<UCTableDataPathInfo>();
	auto &schema = func_info.schema;

	auto table_name = input.inputs[0].GetValue<string>();

	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, table_name);
	auto entry = schema.tables.GetEntry(context, lookup_info);
	if (!entry) {
		throw BinderException("table_data_path: table '%s' not found", table_name);
	}

	auto &table_entry = entry->Cast<UCTableEntry>();
	if (!table_entry.table.table_data) {
		throw InternalException("table_data_path: table_data is null for '%s'", table_name);
	}

	auto result = make_uniq<UCTableDataPathBindData>();
	result->storage_location = table_entry.table.table_data->storage_location;

	return_types.push_back(LogicalType::VARCHAR);
	names.emplace_back("storage_location");
	return std::move(result);
}

static void UCTableDataPathExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<UCTableDataPathBindData>();
	if (data.finished) {
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(data.storage_location));
	data.finished = true;
}

UCTableDataPath::UCTableDataPath(UCSchemaEntry &schema)
    : TableFunction("table_data_path", {LogicalType::VARCHAR}, UCTableDataPathExecute, UCTableDataPathBind) {
	function_info = make_shared_ptr<UCTableDataPathInfo>(schema);
}

} // namespace duckdb
