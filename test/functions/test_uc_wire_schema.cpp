// Server-free unit coverage of UCAPI::WireSchema (src/uc_api.cpp), which turns the schema a create
// wrote to its Delta log into the `columns` UC registers. UC spells the parameters of array and map
// with dashes and silently drops keys it does not know, so a key left in camelCase reaches the server
// as a missing field. The nested shapes would need a live server per case to reach otherwise.
//
// Runner: the standalone `unittest_cpp` Catch executable -- see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][wire]").

#include "catch.hpp"

#include "uc_api.hpp"

using namespace duckdb;

namespace {

// Delta type strings such as decimal(18,4) end in `)"`, hence the `j` raw-string delimiter throughout
string Field(const string &name, const string &type) {
	return R"j({"name":")j" + name + R"j(","type":)j" + type + R"j(,"nullable":true,"metadata":{}})j";
}

string Schema(const string &fields) {
	return R"j({"type":"struct","fields":[)j" + fields + "]}";
}

} // namespace

TEST_CASE("uc wire schema: flat columns pass through", "[uc][wire]") {
	auto schema = Schema(Field("id", R"j("integer")j") + "," + Field("name", R"j("string")j"));
	CHECK(UCAPI::WireSchema(schema) == schema);
}

TEST_CASE("uc wire schema: array and map parameters take dashes", "[uc][wire]") {
	auto list = Field("tags", R"j({"type":"array","elementType":"string","containsNull":true})j");
	auto map = Field("scores", R"j({"type":"map","keyType":"string","valueType":"integer","valueContainsNull":true})j");
	auto wire_list = Field("tags", R"j({"type":"array","element-type":"string","contains-null":true})j");
	auto wire_map =
	    Field("scores", R"j({"type":"map","key-type":"string","value-type":"integer","value-contains-null":true})j");
	CHECK(UCAPI::WireSchema(Schema(list + "," + map)) == Schema(wire_list + "," + wire_map));
}

TEST_CASE("uc wire schema: renames reach every level of nesting", "[uc][wire]") {
	// map<string, struct<xs: array<decimal(18,4)>>>: a struct in the map's value, an array in the struct
	auto inner = Schema(Field("xs", R"j({"type":"array","elementType":"decimal(18,4)","containsNull":true})j"));
	auto wire_inner = Schema(Field("xs", R"j({"type":"array","element-type":"decimal(18,4)","contains-null":true})j"));
	auto col =
	    Field("m", R"j({"type":"map","keyType":"string","valueType":)j" + inner + R"j(,"valueContainsNull":false})j");
	auto wire_col = Field("m", R"j({"type":"map","key-type":"string","value-type":)j" + wire_inner +
	                               R"j(,"value-contains-null":false})j");
	CHECK(UCAPI::WireSchema(Schema(col)) == Schema(wire_col));
}

TEST_CASE("uc wire schema: names that look like keys are values and stay", "[uc][wire]") {
	auto schema = Schema(Field("elementType", R"j("integer")j") + "," + Field("valueContainsNull", R"j("string")j"));
	CHECK(UCAPI::WireSchema(schema) == schema);
}

TEST_CASE("uc wire schema: an unreadable schema is an error", "[uc][wire]") {
	REQUIRE_THROWS(UCAPI::WireSchema("{not json"));
}
