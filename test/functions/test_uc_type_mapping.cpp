// Server-free unit coverage of UCUtils::TypeToLogicalType (src/uc_utils.cpp), the type text map a
// column falls back to when the catalog sends no type_json, and of UCUtils::ColumnTypeFromDefinition
// for a bare `decimal` whose precision and scale are in the column's fields. The nested spellings need a table per
// shape to reach from a live catalog, so they are pinned here.
//
// Runner: the standalone `unittest_cpp` Catch executable -- see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][types]").

#include "catch.hpp"

#include "uc_utils.hpp"

using namespace duckdb;

namespace {

LogicalType Map(const string &type_text) {
	return UCUtils::TypeToLogicalType(type_text);
}

} // namespace

TEST_CASE("uc types: scalars", "[uc][types]") {
	CHECK(Map("tinyint") == LogicalType::TINYINT);
	CHECK(Map("smallint") == LogicalType::SMALLINT);
	CHECK(Map("int") == LogicalType::INTEGER);
	CHECK(Map("bigint") == LogicalType::BIGINT);
	CHECK(Map("long") == LogicalType::BIGINT);
	CHECK(Map("float") == LogicalType::FLOAT);
	CHECK(Map("double") == LogicalType::DOUBLE);
	CHECK(Map("boolean") == LogicalType::BOOLEAN);
	CHECK(Map("date") == LogicalType::DATE);
	CHECK(Map("binary") == LogicalType::BLOB);
	CHECK(Map("void") == LogicalType::SQLNULL);
	CHECK(Map("timestamp") == LogicalType::TIMESTAMP_TZ);
	CHECK(Map("timestamp_ntz") == LogicalType::TIMESTAMP);
	CHECK(Map("variant") == LogicalType::VARIANT());
}

TEST_CASE("uc types: every string spelling collapses to VARCHAR", "[uc][types]") {
	CHECK(Map("string") == LogicalType::VARCHAR);
	CHECK(Map("char") == LogicalType::VARCHAR);
	CHECK(Map("char(10)") == LogicalType::VARCHAR);
	CHECK(Map("varchar(255)") == LogicalType::VARCHAR);
}

TEST_CASE("uc types: decimal carries precision and scale", "[uc][types]") {
	CHECK(Map("decimal(10,2)") == LogicalType::DECIMAL(10, 2));
	CHECK(Map("decimal(38,0)") == LogicalType::DECIMAL(38, 0));
	CHECK(Map("decimal(1,1)") == LogicalType::DECIMAL(1, 1));
}

TEST_CASE("uc types: array", "[uc][types]") {
	CHECK(Map("array<int>") == LogicalType::LIST(LogicalType::INTEGER));
	CHECK(Map("array<variant>") == LogicalType::LIST(LogicalType::VARIANT()));
	CHECK(Map("array<array<string>>") == LogicalType::LIST(LogicalType::LIST(LogicalType::VARCHAR)));
}

TEST_CASE("uc types: map", "[uc][types]") {
	CHECK(Map("map<string,int>") == LogicalType::MAP(LogicalType::VARCHAR, LogicalType::INTEGER));
	CHECK(Map("map<string,variant>") == LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARIANT()));
	CHECK(Map("map<string,map<string,int>>") ==
	      LogicalType::MAP(LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::INTEGER)));
	CHECK(Map("map<string,decimal(10,2)>") == LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DECIMAL(10, 2)));
}

TEST_CASE("uc types: struct", "[uc][types]") {
	CHECK(Map("struct<a:int,b:string>") ==
	      LogicalType::STRUCT({{"a", LogicalType::INTEGER}, {"b", LogicalType::VARCHAR}}));
	CHECK(Map("struct<v:variant>") == LogicalType::STRUCT({{"v", LogicalType::VARIANT()}}));
	CHECK(Map("struct<a:decimal(10,2),b:varchar(5)>") ==
	      LogicalType::STRUCT({{"a", LogicalType::DECIMAL(10, 2)}, {"b", LogicalType::VARCHAR}}));
	CHECK(Map("struct<a:struct<b:int,c:string>,d:decimal(9,3)>") ==
	      LogicalType::STRUCT({{"a", LogicalType::STRUCT({{"b", LogicalType::INTEGER}, {"c", LogicalType::VARCHAR}})},
	                           {"d", LogicalType::DECIMAL(9, 3)}}));
}

TEST_CASE("uc types: variant nested in every container", "[uc][types]") {
	CHECK(Map("array<struct<v:variant>>") == LogicalType::LIST(LogicalType::STRUCT({{"v", LogicalType::VARIANT()}})));
	CHECK(Map("map<string,array<variant>>") ==
	      LogicalType::MAP(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARIANT())));
	CHECK(Map("struct<vs:array<variant>>") == LogicalType::STRUCT({{"vs", LogicalType::LIST(LogicalType::VARIANT())}}));
}

TEST_CASE("uc types: unmapped or malformed type text throws", "[uc][types]") {
	CHECK_THROWS_AS(Map(""), NotImplementedException);
	CHECK_THROWS_AS(Map("interval"), NotImplementedException);
	CHECK_THROWS_AS(Map("decimal(10"), NotImplementedException);
	CHECK_THROWS_AS(Map("array<int"), NotImplementedException);
	CHECK_THROWS_AS(Map("struct<a>"), NotImplementedException);
	CHECK_THROWS_AS(Map("map<int>"), NotImplementedException);
	CHECK_THROWS_AS(Map("array<nosuch>"), NotImplementedException);
	// An empty child list must throw, not scan past the closing '>'.
	CHECK_THROWS_AS(Map("struct<>"), NotImplementedException);
	CHECK_THROWS_AS(Map("map<>"), NotImplementedException);
}

namespace {

//! A column with no type_precision or type_scale passes optional_idx(), as the parser leaves them when absent
LogicalType Column(const string &type_text, optional_idx precision, optional_idx scale, const string &type_json = "") {
	UCAPIColumnDefinition column;
	column.name = "c";
	column.type_text = type_text;
	column.type_json = type_json;
	column.precision = precision;
	column.scale = scale;
	return UCUtils::ColumnTypeFromDefinition(column);
}

} // namespace

// A bare `decimal` type text, with p and s in type_precision/type_scale (duckdb/unity_catalog#14)
TEST_CASE("uc types: a bare decimal takes the column's type_precision and type_scale", "[uc][types]") {
	CHECK(Column("decimal", 32, 6) == LogicalType::DECIMAL(32, 6));
	CHECK(Column("decimal", 38, 0) == LogicalType::DECIMAL(38, 0));
	CHECK(Column("decimal", 1, 1) == LogicalType::DECIMAL(1, 1));
	// A spelled-out (p,s) wins over the fields, and type_json over both. (The raw string stays out of CHECK:
	// MSVC cannot take one inside a macro argument.)
	CHECK(Column("decimal(10,2)", 32, 6) == LogicalType::DECIMAL(10, 2));
	const string type_json = R"j({"name":"c","type":"decimal(9,3)","nullable":true,"metadata":{}})j";
	CHECK(Column("decimal", 32, 6, type_json) == LogicalType::DECIMAL(9, 3));
	// The fields belong to the column, not to a decimal nested inside it
	CHECK_THROWS_AS(Column("array<decimal>", 32, 6), NotImplementedException);
	CHECK_THROWS_AS(Column("struct<a:decimal>", 32, 6), NotImplementedException);
}

TEST_CASE("uc types: a bare decimal without a valid precision and scale throws", "[uc][types]") {
	CHECK_THROWS_AS(Column("decimal", optional_idx(), optional_idx()), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", 10, optional_idx()), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", optional_idx(), 2), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", 0, 0), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", 39, 0), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", 300, 2), NotImplementedException);
	CHECK_THROWS_AS(Column("decimal", 5, 6), NotImplementedException);
	// Without the fields there is nothing to go on: still unmapped as plain type text
	CHECK_THROWS_AS(Map("decimal"), NotImplementedException);
}
