//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "uc_api.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

class UnityCatalog;
class TableInformation;

struct UCTableInfo {
	UCTableInfo() {
		create_info = make_uniq<CreateTableInfo>();
	}
	UCTableInfo(const string &schema, const string &table) {
		create_info = make_uniq<CreateTableInfo>(QualifiedName(Identifier(), Identifier(schema), Identifier(table)));
	}
	UCTableInfo(const SchemaCatalogEntry &schema, const string &table) {
		create_info = make_uniq<CreateTableInfo>((SchemaCatalogEntry &)schema, Identifier(table));
	}

	const string &GetTableName() const {
		return create_info->GetTableName().GetIdentifierName();
	}

	unique_ptr<CreateTableInfo> create_info;
};

class UCTableEntry : public TableCatalogEntry {
public:
	UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, TableInformation &table, CreateTableInfo &info);

public:
	const ColumnList &GetColumns() const override;

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                              const EntryLookupInfo &lookup_info) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	virtual_column_map_t GetVirtualColumns() const override;
	vector<column_t> GetRowIdColumns() const override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

protected:
	ColumnList columns;

public:
	TableInformation &table;
};

} // namespace duckdb
