// Server-free unit coverage of the vended-credential path (src/uc_api.cpp parsing, src/uc_utils.cpp builder): each
// endpoint's response, parsed, and the DuckDB secret BuildVendedSecret makes of it, per cloud. Without network
// access or cloud credentials: the decision is pure, response and location in, secret type and options out.
//
// Runner: the standalone `unittest_cpp` Catch executable — see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][credentials]").

#include "catch.hpp"

#include "uc_api.hpp"
#include "uc_utils.hpp"

#include "duckdb.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

using namespace duckdb;

namespace {

// BuildVendedSecret logs through the extension's logger, so it needs a live context.
struct TestContext {
	DuckDB db;
	Connection con;
	TestContext() : db(nullptr), con(db) {
	}
	ClientContext &Get() {
		return *con.context;
	}
};

string OptionOf(const CreateSecretInput &input, const string &key) {
	auto it = input.options.find(key);
	return it == input.options.end() ? string() : it->second.ToString();
}

// The secret `credentials` make for `location`, or type "none" when BuildVendedSecret makes none.
CreateSecretInput Build(TestContext &ctx, const string &location, const UCAPITableCredentials &credentials,
                        const string &aws_region = "us-east-1") {
	CreateSecretInput input;
	if (!BuildVendedSecret(ctx.Get(), location, credentials, aws_region, input)) {
		input.type = "none";
	}
	return input;
}

const char *ABFSS = "abfss://container@account.dfs.core.windows.net/table";

// One response per endpoint and cloud, as UC's api/all.yaml and api/delta.yaml give them. The delta/v1
// response is the same for a table's and a staging table's credentials.
const char *TEMPORARY_AWS = R"({"aws_temp_credentials": {"access_key_id": "AKIAEXAMPLE",
	"secret_access_key": "s3-secret", "session_token": "s3-session"}, "expiration_time": 1})";
const char *TEMPORARY_GCP = R"({"gcp_oauth_token": {"oauth_token": "ya29.example"}, "expiration_time": 1})";
const char *TEMPORARY_AZURE = R"({"azure_user_delegation_sas": {"sas_token": "sv=2026&sig=abc"},
	"expiration_time": 1})";
const char *TEMPORARY_NONE = R"({"expiration_time": 1})";

const char *DELTA_AWS = R"({"storage-credentials": [{"prefix": "s3://bucket/table", "operation": "READ",
	"expiration-time-ms": 1, "config": {"s3.access-key-id": "AKIAEXAMPLE", "s3.secret-access-key": "s3-secret",
	"s3.session-token": "s3-session", "azure.sas-token": null, "gcs.oauth-token": null}}]})";
const char *DELTA_GCP = R"({"storage-credentials": [{"prefix": "gs://bucket/table", "operation": "READ",
	"expiration-time-ms": 1, "config": {"s3.access-key-id": null, "gcs.oauth-token": "ya29.example"}}]})";
const char *DELTA_AZURE = R"({"storage-credentials": [{"prefix": "abfss://container@account.dfs.core.windows.net/table",
	"operation": "READ", "expiration-time-ms": 1, "config": {"azure.sas-token": "sv=2026&sig=abc"}}]})";
const char *DELTA_NONE = R"({"storage-credentials": []})";

} // namespace

//===--------------------------------------------------------------------===//
// The builder
//===--------------------------------------------------------------------===//

TEST_CASE("AWS credentials make an s3 secret", "[uc][credentials]") {
	TestContext ctx;
	UCAPITableCredentials c;
	c.key_id = "AKIAEXAMPLE";
	c.secret = "s3-secret";
	c.session_token = "s3-session";
	auto input = Build(ctx, "s3://bucket/table", c);
	REQUIRE(input.type == "s3");
	REQUIRE(OptionOf(input, "key_id") == "AKIAEXAMPLE");
	REQUIRE(OptionOf(input, "secret") == "s3-secret");
	REQUIRE(OptionOf(input, "session_token") == "s3-session");
	// The caller's region (the catalog secret's) when the credential brings none ...
	REQUIRE(OptionOf(input, "region") == "us-east-1");
	// ... and the credential's own when it does (Iceberg REST's client.region)
	c.region = "eu-west-1";
	REQUIRE(OptionOf(Build(ctx, "s3://bucket/table", c), "region") == "eu-west-1");
}

TEST_CASE("A GCP token makes a gcs secret carrying it", "[uc][credentials]") {
	TestContext ctx;
	UCAPITableCredentials c;
	c.bearer_token = "ya29.example";
	for (auto &location : {"gs://bucket/table", "gcs://bucket/table"}) {
		auto input = Build(ctx, location, c);
		REQUIRE(input.type == "gcs");
		REQUIRE(OptionOf(input, "bearer_token") == "ya29.example");
		// The AWS region is meaningless here and must not leak into the secret.
		REQUIRE(OptionOf(input, "region").empty());
	}
}

TEST_CASE("An Azure SAS makes an azure secret for the location's account", "[uc][credentials]") {
	TestContext ctx;
	UCAPITableCredentials c;
	c.azure_sas_token = "sv=2026&sig=abc";
	for (auto &location : {"abfss://container@account.dfs.core.windows.net/table",
	                       "abfs://container@account.dfs.core.windows.net/table"}) {
		auto input = Build(ctx, location, c);
		REQUIRE(input.type == "azure");
		REQUIRE(OptionOf(input, "account_name") == "account");
		REQUIRE(OptionOf(input, "connection_string") ==
		        "AccountName=account;SharedAccessSignature=sv=2026&sig=abc;EndpointSuffix=core.windows.net");
	}
	// A suffix other than the SDK's default is kept.
	auto onelake = Build(ctx, "abfss://ws@onelake.dfs.fabric.microsoft.com/lh/table", c);
	REQUIRE(OptionOf(onelake, "connection_string") ==
	        "AccountName=onelake;SharedAccessSignature=sv=2026&sig=abc;EndpointSuffix=fabric.microsoft.com");
	// az:// and azure:// name only the container: the account must come with the SAS (Iceberg REST's key) ...
	REQUIRE(Build(ctx, "az://container/table", c).type == "none");
	REQUIRE(Build(ctx, "azure://container/table", c).type == "none");
	c.azure_host = "account.blob.core.windows.net";
	auto az = Build(ctx, "az://container/table", c);
	REQUIRE(az.type == "azure");
	REQUIRE(OptionOf(az, "connection_string") ==
	        "AccountName=account;SharedAccessSignature=sv=2026&sig=abc;EndpointSuffix=core.windows.net");
}

TEST_CASE("Nothing vended for the location's cloud makes no secret", "[uc][credentials]") {
	TestContext ctx;
	UCAPITableCredentials none;
	UCAPITableCredentials aws;
	aws.key_id = "AKIAEXAMPLE";
	aws.secret = "s3-secret";
	UCAPITableCredentials gcp;
	gcp.bearer_token = "ya29.example";
	UCAPITableCredentials azure;
	azure.azure_sas_token = "sv=2026&sig=abc";

	// Nothing at all: an empty s3 secret would be found ahead of the user's own (for gs://, httpfs looks up s3
	// secrets before gcs ones), and a read would go out unsigned.
	for (auto &location : {"s3://bucket/table", "gs://bucket/table", "gcs://bucket/table", ABFSS, "az://c/table"}) {
		REQUIRE(Build(ctx, location, none).type == "none");
	}
	// A credential for another cloud cannot serve the location either.
	REQUIRE(Build(ctx, "gs://bucket/table", aws).type == "none");
	REQUIRE(Build(ctx, ABFSS, aws).type == "none");
	REQUIRE(Build(ctx, "s3://bucket/table", gcp).type == "none");
	REQUIRE(Build(ctx, "gs://bucket/table", azure).type == "none");
	// A key without its secret is not a credential.
	UCAPITableCredentials key_only;
	key_only.key_id = "AKIAEXAMPLE";
	REQUIRE(Build(ctx, "s3://bucket/table", key_only).type == "none");
}

TEST_CASE("An unrecognised scheme takes AWS credentials as before", "[uc][credentials]") {
	TestContext ctx;
	// file:// never reaches the builder (RefreshCredentials returns early); any other scheme is treated as s3.
	UCAPITableCredentials c;
	c.key_id = "AKIAEXAMPLE";
	c.secret = "s3-secret";
	REQUIRE(Build(ctx, "r2://bucket/table", c).type == "s3");
}

//===--------------------------------------------------------------------===//
// Each endpoint's response, parsed and built
//===--------------------------------------------------------------------===//

TEST_CASE("temporary-table-credentials: each cloud's shape", "[uc][credentials]") {
	TestContext ctx;
	auto aws = Build(ctx, "s3://bucket/table", UCAPI::ParseTemporaryCredentials(TEMPORARY_AWS));
	REQUIRE(aws.type == "s3");
	REQUIRE(OptionOf(aws, "key_id") == "AKIAEXAMPLE");
	REQUIRE(OptionOf(aws, "session_token") == "s3-session");

	auto gcp = Build(ctx, "gs://bucket/table", UCAPI::ParseTemporaryCredentials(TEMPORARY_GCP));
	REQUIRE(gcp.type == "gcs");
	REQUIRE(OptionOf(gcp, "bearer_token") == "ya29.example");

	auto azure = Build(ctx, ABFSS, UCAPI::ParseTemporaryCredentials(TEMPORARY_AZURE));
	REQUIRE(azure.type == "azure");
	REQUIRE(OptionOf(azure, "connection_string") ==
	        "AccountName=account;SharedAccessSignature=sv=2026&sig=abc;EndpointSuffix=core.windows.net");

	for (auto &location : {"s3://bucket/table", "gs://bucket/table", ABFSS}) {
		REQUIRE(Build(ctx, location, UCAPI::ParseTemporaryCredentials(TEMPORARY_NONE)).type == "none");
	}
}

TEST_CASE("delta/v1 credentials: each cloud's shape", "[uc][credentials]") {
	TestContext ctx;
	auto aws = Build(ctx, "s3://bucket/table", UCAPI::ParseDeltaCredentials(DELTA_AWS));
	REQUIRE(aws.type == "s3");
	REQUIRE(OptionOf(aws, "secret") == "s3-secret");
	REQUIRE(OptionOf(aws, "region") == "us-east-1");

	auto gcp = Build(ctx, "gs://bucket/table", UCAPI::ParseDeltaCredentials(DELTA_GCP));
	REQUIRE(gcp.type == "gcs");
	REQUIRE(OptionOf(gcp, "bearer_token") == "ya29.example");

	// delta/v1 names the SAS `azure.sas-token`, with no host: the account comes from the location.
	auto azure = Build(ctx, ABFSS, UCAPI::ParseDeltaCredentials(DELTA_AZURE));
	REQUIRE(azure.type == "azure");
	REQUIRE(OptionOf(azure, "account_name") == "account");

	for (auto &location : {"s3://bucket/table", "gs://bucket/table", ABFSS}) {
		REQUIRE(Build(ctx, location, UCAPI::ParseDeltaCredentials(DELTA_NONE)).type == "none");
	}
}

TEST_CASE("Iceberg REST /plan credentials: each cloud's shape", "[uc][credentials]") {
	TestContext ctx;
	auto aws = Build(ctx, "s3://bucket/",
	                 CredentialsFromConfig({{"s3.access-key-id", "AKIAEXAMPLE"},
	                                        {"s3.secret-access-key", "s3-secret"},
	                                        {"s3.session-token", "s3-session"},
	                                        {"client.region", "eu-west-1"}},
	                                       "s3://bucket/"),
	                 "");
	REQUIRE(aws.type == "s3");
	REQUIRE(OptionOf(aws, "region") == "eu-west-1");

	auto gcp =
	    Build(ctx, "gs://bucket/", CredentialsFromConfig({{"gcs.oauth2.token", "ya29.example"}}, "gs://bucket/"));
	REQUIRE(gcp.type == "gcs");
	REQUIRE(OptionOf(gcp, "bearer_token") == "ya29.example");

	// Iceberg REST names the host in the key; the expiry key beside it is not a token.
	auto prefix = "abfss://container@account.dfs.core.windows.net/";
	auto azure = Build(ctx, prefix,
	                   CredentialsFromConfig({{"adls.sas-token.account.dfs.core.windows.net", "sv=2026&sig=abc"},
	                                          {"adls.sas-token-expires-at-ms.account.dfs.core.windows.net", "1"}},
	                                         prefix));
	REQUIRE(azure.type == "azure");
	REQUIRE(OptionOf(azure, "connection_string") ==
	        "AccountName=account;SharedAccessSignature=sv=2026&sig=abc;EndpointSuffix=core.windows.net");

	for (auto &location : {"s3://bucket/", "gs://bucket/", prefix}) {
		REQUIRE(Build(ctx, location, CredentialsFromConfig({{"gcs.oauth2.token-expires-at", "0"}}, location)).type ==
		        "none");
	}
}

TEST_CASE("Config keys: which spelling wins", "[uc][credentials]") {
	// GCS: Iceberg REST's gcs.oauth2.token first, then delta/v1's gcs.oauth-token if it is absent or empty.
	REQUIRE(CredentialsFromConfig({{"gcs.oauth2.token", "ya29.iceberg"}, {"gcs.oauth-token", "ya29.delta"}}, "")
	            .bearer_token == "ya29.iceberg");
	REQUIRE(CredentialsFromConfig({{"gcs.oauth2.token", ""}, {"gcs.oauth-token", "ya29.delta"}}, "").bearer_token ==
	        "ya29.delta");
	REQUIRE(CredentialsFromConfig({}, "").bearer_token.empty());

	// Azure: the SAS for the prefix's host, else the first host in key order; a host-named key beats
	// delta/v1's azure.sas-token.
	unordered_map<string, string> two_hosts = {{"adls.sas-token.zeta.dfs.core.windows.net", "sig-zeta"},
	                                           {"adls.sas-token.alpha.dfs.core.windows.net", "sig-alpha"},
	                                           {"azure.sas-token", "sig-delta"}};
	auto matched = CredentialsFromConfig(two_hosts, "abfss://c@zeta.dfs.core.windows.net/t");
	REQUIRE(matched.azure_sas_token == "sig-zeta");
	REQUIRE(matched.azure_host == "zeta.dfs.core.windows.net");
	auto first = CredentialsFromConfig(two_hosts, "az://c/t");
	REQUIRE(first.azure_sas_token == "sig-alpha");
	REQUIRE(first.azure_host == "alpha.dfs.core.windows.net");
	auto delta = CredentialsFromConfig({{"azure.sas-token", "sig-delta"}}, "abfss://c@a.dfs.core.windows.net/t");
	REQUIRE(delta.azure_sas_token == "sig-delta");
	REQUIRE(delta.azure_host.empty());
}

//===--------------------------------------------------------------------===//
// Applying: create, replace, and drop when nothing is vended
//===--------------------------------------------------------------------===//

TEST_CASE("Re-applied credentials replace the secret, or drop it when nothing is vended", "[uc][credentials]") {
	TestContext ctx;
	auto &context = ctx.Get();
	auto &secret_manager = SecretManager::Get(context);
	// httpfs, which owns the s3 type, is not linked in here: a stand-in with the same name and provider.
	SecretType s3_type;
	s3_type.name = Identifier("s3");
	s3_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	s3_type.default_provider = "config";
	secret_manager.RegisterSecretType(s3_type);
	CreateSecretFunction s3_config = {
	    "s3", "config", [](ClientContext &, CreateSecretInput &input) -> unique_ptr<BaseSecret> {
		    return make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	    }};
	secret_manager.RegisterSecretFunction(s3_config, OnCreateConflict::ERROR_ON_CONFLICT);
	ctx.con.BeginTransaction();
	auto lookup_type = [&](const string &path, const string &type) {
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto match = secret_manager.LookupSecret(transaction, path, type);
		return match.HasMatch() ? match.GetSecret().GetName().GetIdentifierName() : string();
	};
	auto in_transaction_storage = [&](const string &name) {
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		return secret_manager.GetSecretByName(transaction, name, SecretManager::TRANSACTION_STORAGE_NAME) != nullptr;
	};

	UCAPITableCredentials aws;
	aws.key_id = "AKIAEXAMPLE";
	aws.secret = "s3-secret";
	ApplyVendedCredentials(context, "__internal_uc_t1", "s3://bucket/table", aws, "us-east-1");
	REQUIRE(lookup_type("s3://bucket/table/part-0.parquet", "s3") == "__internal_uc_t1");
	// Transaction-scoped (uc#134), not temporary.
	REQUIRE(in_transaction_storage("__internal_uc_t1"));

	// Applied again in the same transaction, with nothing vended: the earlier secret must not linger.
	ApplyVendedCredentials(context, "__internal_uc_t1", "s3://bucket/table", UCAPITableCredentials(), "us-east-1");
	REQUIRE(lookup_type("s3://bucket/table/part-0.parquet", "s3").empty());
	REQUIRE_FALSE(in_transaction_storage("__internal_uc_t1"));

	// And nothing to drop is not an error.
	ApplyVendedCredentials(context, "__internal_uc_t1", "s3://bucket/table", UCAPITableCredentials(), "us-east-1");
	ctx.con.Commit();
}
