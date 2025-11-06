//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/uc_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "storage/uc_schema_set.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {
class UCSchemaEntry;

struct UCCredentials {
	string endpoint;
	string token;

	// Not really part of the credentials, but required to query s3 tables
	string aws_region;
};

class UCClearCacheFunction : public TableFunction {
public:
	UCClearCacheFunction();

	static void ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter);
};

class UCCatalog : public Catalog {
public:
	explicit UCCatalog(AttachedDatabase &db_p, const string &internal_name, AttachOptions &attach_options,
	                   UCCredentials credentials, const string &default_schema);
	~UCCatalog();

	string internal_name;
	AccessMode access_mode;
	UCCredentials credentials;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "uc";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	string GetDefaultSchema() const override;

	//! Whether or not this is an in-memory UC database
	bool InMemory() override;
	string GetDBPath() override;

	void ClearCache();

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	UCSchemaSet schemas;
	string default_schema;
};

} // namespace duckdb
