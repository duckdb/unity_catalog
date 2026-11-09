// Server-free unit coverage of UCUtils::PathIsInside (src/uc_utils.cpp), which keeps the cleanup of a refused
// EXTERNAL create to paths inside the table: a table at a storage root contains everything below it, and `..`
// cannot climb out.
//
// Runner: the standalone `unittest_cpp` Catch executable -- see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][path]").

#include "catch.hpp"

#include "uc_utils.hpp"

using namespace duckdb;

TEST_CASE("uc path: a file under a table is inside it", "[uc][path]") {
	CHECK(UCUtils::PathIsInside("/tmp/table", "/tmp/table/part-0.parquet"));
	CHECK(UCUtils::PathIsInside("/tmp/table/", "/tmp/table/p=a/part-0.parquet"));
	CHECK(UCUtils::PathIsInside("s3://bucket/table", "s3://bucket/table/part-0.parquet"));
	CHECK(UCUtils::PathIsInside("file:///tmp/table", "file:///tmp/table/part-0.parquet"));
}

TEST_CASE("uc path: a table at a root contains what is below it", "[uc][path]") {
	CHECK(UCUtils::PathIsInside("/", "/part-0.parquet"));
	CHECK(UCUtils::PathIsInside("s3://bucket", "s3://bucket/part-0.parquet"));
	CHECK(UCUtils::PathIsInside("s3://bucket/", "s3://bucket/p=a/part-0.parquet"));
}

TEST_CASE("uc path: the table itself, a sibling, and a climb out are not inside", "[uc][path]") {
	CHECK_FALSE(UCUtils::PathIsInside("/tmp/table", "/tmp/table"));
	CHECK_FALSE(UCUtils::PathIsInside("/tmp/table", "/tmp/table2/part-0.parquet"));
	CHECK_FALSE(UCUtils::PathIsInside("/tmp/table", "/tmp/table/../other/part-0.parquet"));
	CHECK_FALSE(UCUtils::PathIsInside("s3://bucket", "s3://bucket"));
	CHECK_FALSE(UCUtils::PathIsInside("s3://bucket/table", "s3://other/table/part-0.parquet"));
}
