#pragma once

#include "duckdb/logging/logger.hpp"

// Extension-scoped logging macros. Log type "unity_catalog" lets users filter
// UC extension messages independently of other DuckDB subsystems.
//
// Message convention: start every line with a dotted tag `<subsystem>.<Op>` (the
// "unity_catalog" scope already comes from the log type above, so don't repeat it).
// Subsystems:
//   api      — the Unity Catalog REST API (LoadTable, GetTables, BackfillCommits, ...)
//   api-irc  — the Iceberg REST Catalog scan-plan path, incl. delete-file application
//             (PlanTableScan, FetchScanTasks, DeletionVector, ...)
#define UC_LOG_DEBUG(SOURCE, ...)   DUCKDB_LOG_INTERNAL(SOURCE, "unity_catalog", LogLevel::LOG_DEBUG, __VA_ARGS__)
#define UC_LOG_WARNING(SOURCE, ...) DUCKDB_LOG_INTERNAL(SOURCE, "unity_catalog", LogLevel::LOG_WARNING, __VA_ARGS__)
#define UC_LOG_ERROR(SOURCE, ...)   DUCKDB_LOG_INTERNAL(SOURCE, "unity_catalog", LogLevel::LOG_ERROR, __VA_ARGS__)
