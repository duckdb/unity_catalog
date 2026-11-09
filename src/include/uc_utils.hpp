//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mysql_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "uc_api.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
class UCSchemaEntry;
class UCTransaction;

enum class UCTypeAnnotation { STANDARD, CAST_TO_VARCHAR, NUMERIC_AS_DOUBLE, CTID, JSONB, FIXED_LENGTH_CHAR };

struct UCType {
	idx_t oid = 0;
	UCTypeAnnotation info = UCTypeAnnotation::STANDARD;
	vector<UCType> children;
};

//! The credentials in a storage credential's `config` map, as delta/v1 and Iceberg REST both vend them: `s3.*`,
//! `gcs.oauth2.token` (Iceberg REST) or `gcs.oauth-token` (delta/v1), `adls.sas-token.<host>` (Iceberg REST) or
//! `azure.sas-token` (delta/v1), and `client.region`. Of several Azure hosts, the one in `prefix` is taken.
UCAPITableCredentials CredentialsFromConfig(const unordered_map<string, string> &config, const string &prefix);

//! The scope for an Iceberg REST storage credential's `prefix`: a bare scheme ("gs") widened to "gs://", the least
//! specific scope for it; a lone "/" (which matches no path) the table's location, as duckdb-iceberg does; else the
//! prefix itself.
string PlanCredentialScope(const string &prefix, const string &table_location);

//! The secret UC-vended credentials make for `location`, chosen by its scheme: gcs (bearer token) for gs:// and
//! gcs://, azure (a SAS connection string) for abfss://, abfs://, az:// and azure://, else s3. Sets `input.type`
//! and `input.options` only. False, with a warning, when nothing usable for that cloud was vended: then no secret
//! should exist, so that the user's own secrets for the path are found. `aws_region` is the s3 secret's region
//! when the credential brings none.
bool BuildVendedSecret(ClientContext &context, const string &location, const UCAPITableCredentials &credentials,
                       const string &aws_region, CreateSecretInput &input);

//! Create the internal secret `name`, scoped to `location`, from vended credentials, for this transaction only;
//! it replaces an earlier one of that name. When nothing usable was vended, drop any earlier one instead.
void ApplyVendedCredentials(ClientContext &context, const string &name, const string &location,
                            const UCAPITableCredentials &credentials, const string &aws_region);

class UCUtils {
public:
	static LogicalType ToUCType(const LogicalType &input);
	static LogicalType ColumnTypeFromDefinition(const UCAPIColumnDefinition &column);
	static LogicalType TypeFromJson(const string &type_json);
	static LogicalType TypeToLogicalType(const string &columnDefinition);
	static string TypeToString(const LogicalType &input);
	//! Whether `path` lies strictly inside `directory`, both normalized (so `..` cannot climb out); a root such as
	//! `/` or `s3://bucket` contains everything below it
	static bool PathIsInside(const string &directory, const string &path);
};

} // namespace duckdb
