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

struct UCTableInfo {
	UCTableInfo() {
		create_info = make_uniq<CreateTableInfo>();
	}
	UCTableInfo(const string &schema, const string &table) {
		create_info = make_uniq<CreateTableInfo>(string(), schema, table);
	}
	UCTableInfo(const SchemaCatalogEntry &schema, const string &table) {
		create_info = make_uniq<CreateTableInfo>((SchemaCatalogEntry &)schema, table);
	}

	const string &GetTableName() const {
		return create_info->table;
	}

	unique_ptr<CreateTableInfo> create_info;
};

class UCTableEntry : public TableCatalogEntry {
public:
	UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);
	UCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, UCTableInfo &info);

	unique_ptr<UCAPITable> table_data;

	shared_ptr<AttachedDatabase> internal_attached_database;

public:
	optional_ptr<Catalog> GetInternalCatalog();
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	virtual_column_map_t GetVirtualColumns() const override;
	vector<column_t> GetRowIdColumns() const override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;
};

} // namespace duckdb
