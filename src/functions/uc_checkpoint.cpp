#include "functions/uc_checkpoint.hpp"

#include "storage/unity_catalog.hpp"
#include "storage/uc_schema_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

struct CheckpointTableBindData : public TableFunctionData {
	string catalog_name;
	string schema_name;
	string table_name;
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

static unique_ptr<FunctionData> CheckpointTableBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<CheckpointTableBindData>(ResolveTableName(context, input.inputs[0].ToString()));
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

	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &catalog = db_ref.get()->GetCatalog();
		if (catalog.GetName() != data.catalog_name) {
			continue;
		}
		if (catalog.GetCatalogType() != "uc" && catalog.GetCatalogType() != "unity_catalog") {
			throw InvalidInputException("Catalog '%s' is not a Unity Catalog", data.catalog_name);
		}
		auto &unity_catalog = catalog.Cast<UnityCatalog>();
		bool schema_found = false;
		unity_catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema_entry) {
			if (schema_entry.name != data.schema_name) {
				return;
			}
			schema_found = true;
			auto &schema = schema_entry.Cast<UCSchemaEntry>();
			schema.tables.CheckpointTable(context, data.table_name);
		});
		if (!schema_found) {
			throw InvalidInputException("Schema '%s' not found in catalog '%s'", data.schema_name, data.catalog_name);
		}
		return;
	}
	throw InvalidInputException("Unity Catalog '%s' not found", data.catalog_name);
}

UCCheckpointTableFunction::UCCheckpointTableFunction()
    : TableFunction("unity_catalog_checkpoint_table", {LogicalType::VARCHAR}, CheckpointTableFunction,
                    CheckpointTableBind) {
}

} // namespace duckdb
