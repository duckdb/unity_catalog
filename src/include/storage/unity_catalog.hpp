//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/unity_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "storage/uc_schema_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/common/mutex.hpp"

#include <chrono>

namespace duckdb {
class UCSchemaEntry;

struct UCCredentials {
	string endpoint;
	string token;
	string aws_region;              // not really credentials; required to query S3 tables
	bool use_irc_scan_plan = false; // opt-in to the IRC server-side scan-plan read path
	string irc_endpoint_override;   // hidden test override; empty => derive from endpoint
};

class UCClearCacheFunction : public TableFunction {
public:
	UCClearCacheFunction();

	static void ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter);
};

class UnityCatalog : public Catalog {
public:
	explicit UnityCatalog(AttachedDatabase &db_p, const string &internal_name, AttachOptions &attach_options,
	                      UCCredentials credentials, const string &default_schema,
	                      string catalog_name = "unity_catalog");
	~UnityCatalog() override;

	string internal_name;
	AccessMode access_mode;
	UCCredentials credentials;

	string catalog_name;

public:
	void Initialize(bool load_builtin) override;

	string GetCatalogType() override {
		return catalog_name;
	}
	static bool IsUnityCatalog(Catalog &cat) {
		const auto &t = cat.GetCatalogType();
		// be paranoid, support old aliases
		return (t == "unity_catalog" || t == "uc_catalog" || t == "uc");
	}

	bool SupportsTimeTravel() const override {
		return true;
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
	void OnDetach(ClientContext &context) override;

	//! Whether or not this is an in-memory UC database
	bool InMemory() override;
	string GetDBPath() override;

	void ClearCache();

	// --- IRC scan-plan gating (opt-in; see docs/scan-plan/scan-plan-gating.md) ---

	// The IRC base URL for this catalog: the hidden override, else derived from the UC endpoint.
	// Only meaningful when credentials.use_irc_scan_plan is set.
	string GetIRCEndpoint() const {
		if (!credentials.irc_endpoint_override.empty()) {
			return credentials.irc_endpoint_override;
		}
		return credentials.endpoint + "/api/2.1/unity-catalog/iceberg-rest";
	}

	// Whether a scan should attempt the scan-plan path: opt-in AND not currently known-unavailable.
	// A UNAVAILABLE that has aged past the re-probe window decays back to UNKNOWN here.
	bool ShouldTryScanPlan();
	// The `/plan` call is the probe -- record its outcome. AVAILABLE is sticky; UNAVAILABLE is
	// re-probed after SCAN_PLAN_RE_PROBE.
	void MarkScanPlanAvailable();
	void MarkScanPlanUnavailable();

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	UCSchemaSet schemas;
	string default_schema;

	// Per-ATTACH scan-plan availability, guarded by scan_plan_lock.
	enum class ScanPlanAvailability : uint8_t { UNKNOWN, AVAILABLE, UNAVAILABLE };
	static constexpr std::chrono::minutes SCAN_PLAN_RE_PROBE {15};
	mutable mutex scan_plan_lock;
	ScanPlanAvailability scan_plan_state = ScanPlanAvailability::UNKNOWN;
	std::chrono::steady_clock::time_point scan_plan_unavailable_since {};
};

} // namespace duckdb
