// Server-free unit coverage of ApplyVendedCredentials (src/uc_utils.cpp): the mapping from a
// UC-vended credential plus a storage location to the DuckDB secret that gets injected. Isolated
// here because exercising it through SQL needs a live catalog on each of three clouds, while the
// decision it makes is pure: scheme in, secret type and options out.
//
// Runner: the standalone `unittest_cpp` Catch executable — see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][credentials]").

#include "catch.hpp"

#include "uc_utils.hpp"

#include "duckdb.hpp"

using namespace duckdb;

namespace {

UCAPITableCredentials AwsCreds() {
	UCAPITableCredentials c;
	c.key_id = "AKIAEXAMPLE";
	c.secret = "s3-secret";
	c.session_token = "s3-session";
	return c;
}

UCAPITableCredentials GcpCreds() {
	UCAPITableCredentials c;
	c.bearer_token = "ya29.example";
	return c;
}

UCAPITableCredentials AzureCreds() {
	UCAPITableCredentials c;
	c.sas_token = "sv=2021-01-01&sig=example";
	return c;
}

// ApplyVendedCredentials logs through the extension's logger, so it needs a live context.
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

} // namespace

TEST_CASE("AWS storage keeps producing an s3 secret", "[uc][credentials]") {
	TestContext ctx;
	CreateSecretInput input;
	ApplyVendedCredentials(ctx.Get(), input, "s3://bucket/table", AwsCreds(), "us-east-1");

	REQUIRE(input.type == "s3");
	REQUIRE(OptionOf(input, "key_id") == "AKIAEXAMPLE");
	REQUIRE(OptionOf(input, "secret") == "s3-secret");
	REQUIRE(OptionOf(input, "session_token") == "s3-session");
	// UC vends no region of its own, so it has to come from the catalog secret.
	REQUIRE(OptionOf(input, "region") == "us-east-1");
}

TEST_CASE("GCS storage produces a gcs secret carrying the bearer token", "[uc][credentials]") {
	TestContext ctx;
	for (auto &location : {"gs://bucket/table", "gcs://bucket/table"}) {
		CreateSecretInput input;
		ApplyVendedCredentials(ctx.Get(), input, location, GcpCreds(), "us-east-1");

		REQUIRE(input.type == "gcs");
		REQUIRE(OptionOf(input, "bearer_token") == "ya29.example");
		// The AWS region is meaningless here and must not leak into the secret.
		REQUIRE(OptionOf(input, "region").empty());
	}
}

TEST_CASE("A GCS table without a GCP credential keeps the old s3 secret", "[uc][credentials]") {
	TestContext ctx;
	CreateSecretInput input;
	// Same reasoning as the Azure case: an s3 secret is useless for GCS, but it is the wrong type
	// to match a gs:// lookup, so any gcs secret the user created himself (HMAC interoperability
	// keys, say) is still found and still works. Throwing here would break that.
	ApplyVendedCredentials(ctx.Get(), input, "gs://bucket/table", AwsCreds(), "us-east-1");

	REQUIRE(input.type == "s3");
	REQUIRE(OptionOf(input, "bearer_token").empty());
}

TEST_CASE("Azure storage still produces the s3 secret it always has", "[uc][credentials]") {
	TestContext ctx;
	// Not an endorsement of the result -- an s3 secret is useless for Azure. It is preserved
	// because any azure secret the user created himself is still found in preference to it, and
	// that is the only way these tables can be read today. Failing here would remove that.
	for (auto &location : {"abfss://c@a.dfs.core.windows.net/t", "abfs://c@a.dfs.core.windows.net/t",
	                       "azure://container/t", "az://container/t"}) {
		CreateSecretInput input;
		ApplyVendedCredentials(ctx.Get(), input, location, AzureCreds(), "us-east-1");

		REQUIRE(input.type == "s3");
		// The SAS is parsed and carried, but deliberately not used to build the secret yet.
		REQUIRE(OptionOf(input, "bearer_token").empty());
	}
}

TEST_CASE("An unrecognised scheme falls back to s3", "[uc][credentials]") {
	TestContext ctx;
	// file:// never reaches this function (RefreshCredentials returns early), but anything else
	// unknown should behave as it did before rather than throw.
	CreateSecretInput input;
	ApplyVendedCredentials(ctx.Get(), input, "r2://bucket/table", AwsCreds(), "us-east-1");
	REQUIRE(input.type == "s3");
}
