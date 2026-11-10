// Server-free unit coverage of ApplyVendedCredentials (src/uc_utils.cpp): the mapping from a
// UC-vended credential plus a storage location to the DuckDB secret that gets injected, without
// network access or cloud credentials: the decision is pure, scheme in, secret type and options out.
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

TEST_CASE("AWS storage produces an s3 secret", "[uc][credentials]") {
	TestContext ctx;
	CreateSecretInput input;
	ApplyVendedCredentials(ctx.Get(), input, "s3://bucket/table", AwsCreds(), "us-east-1");

	REQUIRE(input.type == "s3");
	REQUIRE(OptionOf(input, "key_id") == "AKIAEXAMPLE");
	REQUIRE(OptionOf(input, "secret") == "s3-secret");
	REQUIRE(OptionOf(input, "session_token") == "s3-session");
	// The caller's region: the catalog secret's, or the one /plan vends.
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

TEST_CASE("A GCS table without a GCP credential gets an s3 secret", "[uc][credentials]") {
	TestContext ctx;
	CreateSecretInput input;
	// A known limitation: httpfs looks up s3 secrets before gcs ones, so this secret is found ahead
	// of a gcs secret the user created (HMAC interoperability keys, say), and such a read goes out
	// unsigned. A follow-up stops creating it.
	ApplyVendedCredentials(ctx.Get(), input, "gs://bucket/table", AwsCreds(), "us-east-1");

	REQUIRE(input.type == "s3");
	REQUIRE(OptionOf(input, "bearer_token").empty());
}

TEST_CASE("Azure storage gets an s3 secret", "[uc][credentials]") {
	TestContext ctx;
	// UC's Azure SAS is not read yet. An Azure lookup never matches an s3 secret, so an azure
	// secret the user created is still found.
	for (auto &location : {"abfss://c@a.dfs.core.windows.net/t", "abfs://c@a.dfs.core.windows.net/t",
	                       "azure://container/t", "az://container/t"}) {
		CreateSecretInput input;
		ApplyVendedCredentials(ctx.Get(), input, location, AwsCreds(), "us-east-1");

		REQUIRE(input.type == "s3");
		REQUIRE(OptionOf(input, "bearer_token").empty());
	}
}

TEST_CASE("An unrecognised scheme falls back to s3", "[uc][credentials]") {
	TestContext ctx;
	// file:// never reaches this function (RefreshCredentials returns early); any other unknown
	// scheme gets an s3 secret rather than an error.
	CreateSecretInput input;
	ApplyVendedCredentials(ctx.Get(), input, "r2://bucket/table", AwsCreds(), "us-east-1");
	REQUIRE(input.type == "s3");
}
