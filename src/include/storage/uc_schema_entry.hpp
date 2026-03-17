//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_schema_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "uc_api.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "storage/uc_table_set.hpp"

namespace duckdb {
class UCTransaction;
struct DefaultTableMacro;

class UCSchemaEntry : public SchemaCatalogEntry {
public:
	UCSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);
	~UCSchemaEntry() override;

	unique_ptr<UCAPISchema> schema_data;

public:
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

private:
	UCTableSet &GetCatalogSet(CatalogType type);

	optional_ptr<CatalogEntry> TryLoadBuiltInFunction(const string &entry_name);
	optional_ptr<CatalogEntry> LoadBuiltInFunction(DefaultTableMacro macro);

public:
	UCTableSet tables;

	mutex default_function_lock;
	case_insensitive_map_t<unique_ptr<CatalogEntry>> default_function_map;
};

} // namespace duckdb
