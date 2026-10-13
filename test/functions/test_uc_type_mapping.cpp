// Server-free unit coverage of UCUtils::TypeToLogicalType (src/uc_utils.cpp), the type text map a
// column falls back to when the catalog sends no type_json. The nested spellings need a table per
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
