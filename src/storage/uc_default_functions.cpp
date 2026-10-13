#include "duckdb/catalog/default/default_table_functions.hpp"
#include "storage/uc_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "functions/uc_table_functions.hpp"
#include "storage/uc_transaction_manager.hpp"

namespace duckdb {

// clang-format off
static const DefaultTableMacro uc_table_macros[] = {
	{nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}
};
// clang-format on

optional_ptr<CatalogEntry> UCSchemaEntry::LoadBuiltInFunction(DefaultTableMacro macro) {
	string macro_def = macro.macro;
	macro_def = StringUtil::Replace(macro_def, "{CATALOG}", SQLString::ToString(catalog.GetName().GetIdentifierName()));
	macro_def = StringUtil::Replace(macro_def, "{SCHEMA}", SQLString::ToString(name.GetIdentifierName()));
	macro.macro = macro_def.c_str();
	auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(macro);
	auto table_macro =
	    make_uniq_base<CatalogEntry, TableMacroCatalogEntry>(catalog, *this, info->Cast<CreateMacroInfo>());
	auto result = table_macro.get();
	default_function_map.emplace(macro.name, std::move(table_macro));
	return result;
}

optional_ptr<CatalogEntry> UCSchemaEntry::TryLoadBuiltInFunction(const string &entry_name) {
	lock_guard<mutex> guard(default_function_lock);
	auto entry = default_function_map.find(entry_name);
	if (entry != default_function_map.end()) {
		return entry->second.get();
	}
	for (idx_t index = 0; uc_table_macros[index].name != nullptr; index++) {
		if (StringUtil::CIEquals(uc_table_macros[index].name, entry_name)) {
			return LoadBuiltInFunction(uc_table_macros[index]);
		}
	}

	if (entry_name == "__internal_delta_ccv2_commit_staged") {
		auto info = CreateTableFunctionInfo(UCDeltaCCV2Commit());
		default_function_map[entry_name] =
		    make_uniq_base<CatalogEntry, TableFunctionCatalogEntry>(catalog, *this, info);
		return default_function_map[entry_name].get();
	}

	if (entry_name == "table_data_path") {
		auto info = CreateTableFunctionInfo(UCTableDataPath(*this));
		default_function_map[entry_name] =
		    make_uniq_base<CatalogEntry, TableFunctionCatalogEntry>(catalog, *this, info);
		return default_function_map[entry_name].get();
	}

	return nullptr;
}

} // namespace duckdb
