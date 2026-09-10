// Server-free unit coverage of SerializeFiltersToIRC (src/uc_irc_expression.cpp): the pure
// bound-expression -> IRC-filter-JSON mapping the live scan-plan suite (duckdb-iceberg #1204)
// never exercised. Pins the exact JSON for each node kind, the flip/float rules, and -- most
// importantly -- the OR-drop rule (an OR with any unsupported child is vacuously true, so it must
// be dropped whole rather than serialized as a tighter predicate the server would over-prune with).
//
// Runner: the standalone `unittest_cpp` Catch executable (test/functions/CMakeLists.txt),
// mirroring httpfs's test/unittest pattern -- no duckdb-submodule edit. Built on demand:
//   cmake --build build/<variant> --target unittest_cpp
//   build/<variant>/test/unittest_cpp "[uc][irc]"

#include "catch.hpp"

#include "uc_irc_expression.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

using namespace duckdb;

namespace {

unique_ptr<Expression> Col(const string &name, LogicalType type = LogicalType::INTEGER) {
	return make_uniq<BoundColumnRefExpression>(Identifier(name), std::move(type),
	                                           ColumnBinding(TableIndex(0), ProjectionIndex(0)));
}

unique_ptr<Expression> Const(Value v) {
	return make_uniq<BoundConstantExpression>(std::move(v));
}

unique_ptr<Expression> Cmp(ExpressionType type, unique_ptr<Expression> left, unique_ptr<Expression> right) {
	return BoundComparisonExpression::Create(type, std::move(left), std::move(right));
}

unique_ptr<Expression> IsNull(ExpressionType type, unique_ptr<Expression> child) {
	auto op = make_uniq<BoundOperatorExpression>(type, LogicalType::BOOLEAN);
	op->GetChildrenMutable().push_back(std::move(child));
	return std::move(op);
}

// Serialize a single filter expression (the common case).
string One(unique_ptr<Expression> e) {
	vector<unique_ptr<Expression>> filters;
	filters.push_back(std::move(e));
	return SerializeFiltersToIRC(filters);
}

} // namespace

TEST_CASE("uc irc: comparison terms", "[uc][irc]") {
	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"eq","term":"age","value":30})");
	CHECK(One(Cmp(ExpressionType::COMPARE_LESSTHAN, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"lt","term":"age","value":30})");
	CHECK(One(Cmp(ExpressionType::COMPARE_GREATERTHAN, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"gt","term":"age","value":30})");
	CHECK(One(Cmp(ExpressionType::COMPARE_NOTEQUAL, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"not-eq","term":"age","value":30})");
	CHECK(One(Cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"lt-eq","term":"age","value":30})");
	CHECK(One(Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Col("age"), Const(Value::INTEGER(30)))) ==
	      R"({"type":"gt-eq","term":"age","value":30})");
}

TEST_CASE("uc irc: constant-on-left is flipped", "[uc][irc]") {
	// `30 > age` must serialize as `age < 30` (the term always names the column).
	CHECK(One(Cmp(ExpressionType::COMPARE_GREATERTHAN, Const(Value::INTEGER(30)), Col("age"))) ==
	      R"({"type":"lt","term":"age","value":30})");
}

TEST_CASE("uc irc: value formatting", "[uc][irc]") {
	// %.17g round-trips a double exactly (not std::to_string's 6 fixed decimals).
	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("p", LogicalType::DOUBLE), Const(Value::DOUBLE(1.5)))) ==
	      R"({"type":"eq","term":"p","value":1.5})");
	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("p", LogicalType::DOUBLE), Const(Value::DOUBLE(0.1)))) ==
	      R"({"type":"eq","term":"p","value":0.10000000000000001})");

	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("f", LogicalType::BOOLEAN), Const(Value::BOOLEAN(true)))) ==
	      R"({"type":"eq","term":"f","value":true})");

	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("name", LogicalType::VARCHAR), Const(Value("bob")))) ==
	      R"({"type":"eq","term":"name","value":"bob"})");
	// String escaping.
	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col("name", LogicalType::VARCHAR), Const(Value("a\"b")))) ==
	      R"({"type":"eq","term":"name","value":"a\"b"})");
}

TEST_CASE("uc irc: non-finite double drops the term", "[uc][irc]") {
	auto inf = std::numeric_limits<double>::infinity();
	// A non-finite bound can't prune -> the term (and here the whole filter) is dropped.
	CHECK(One(Cmp(ExpressionType::COMPARE_LESSTHAN, Col("p", LogicalType::DOUBLE), Const(Value::DOUBLE(inf)))).empty());
}

TEST_CASE("uc irc: a column name needing escapes is quoted", "[uc][irc]") {
	// The term was concatenated in raw while values went through the escaper, so a quote in a
	// column name produced a request body that is not valid JSON at all.
	CHECK(One(Cmp(ExpressionType::COMPARE_EQUAL, Col(R"(we"ird)"), Const(Value::INTEGER(1)))) ==
	      R"({"type":"eq","term":"we\"ird","value":1})");
}

TEST_CASE("uc irc: is-null / not-null", "[uc][irc]") {
	CHECK(One(IsNull(ExpressionType::OPERATOR_IS_NULL, Col("age"))) == R"({"type":"is-null","term":"age"})");
	CHECK(One(IsNull(ExpressionType::OPERATOR_IS_NOT_NULL, Col("age"))) == R"({"type":"not-null","term":"age"})");
}

TEST_CASE("uc irc: AND / OR conjunctions", "[uc][irc]") {
	auto gt = Cmp(ExpressionType::COMPARE_GREATERTHAN, Col("age"), Const(Value::INTEGER(10)));
	auto lt = Cmp(ExpressionType::COMPARE_LESSTHAN, Col("age"), Const(Value::INTEGER(20)));
	auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(gt), std::move(lt));
	CHECK(
	    One(std::move(conj)) ==
	    R"({"type":"and","left":{"type":"gt","term":"age","value":10},"right":{"type":"lt","term":"age","value":20}})");

	auto e1 = Cmp(ExpressionType::COMPARE_EQUAL, Col("age"), Const(Value::INTEGER(1)));
	auto e2 = Cmp(ExpressionType::COMPARE_EQUAL, Col("age"), Const(Value::INTEGER(2)));
	auto disj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(e1), std::move(e2));
	CHECK(One(std::move(disj)) ==
	      R"({"type":"or","left":{"type":"eq","term":"age","value":1},"right":{"type":"eq","term":"age","value":2}})");
}

TEST_CASE("uc irc: OR with an unsupported child is dropped whole", "[uc][irc]") {
	// col-vs-col is not serializable; inside an OR that makes the OR vacuously true, so the whole
	// OR must drop (serializing only the supported side would wrongly narrow the predicate).
	auto ok = Cmp(ExpressionType::COMPARE_EQUAL, Col("age"), Const(Value::INTEGER(1)));
	auto bad = Cmp(ExpressionType::COMPARE_EQUAL, Col("a"), Col("b"));
	auto disj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(ok), std::move(bad));
	CHECK(One(std::move(disj)).empty());
}

TEST_CASE("uc irc: AND skips an unsupported child", "[uc][irc]") {
	// The AND counterpart: an unsupported child is dropped, the supported side survives.
	auto ok = Cmp(ExpressionType::COMPARE_EQUAL, Col("age"), Const(Value::INTEGER(1)));
	auto bad = Cmp(ExpressionType::COMPARE_EQUAL, Col("a"), Col("b"));
	auto conj = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(ok), std::move(bad));
	CHECK(One(std::move(conj)) == R"({"type":"eq","term":"age","value":1})");
}

TEST_CASE("uc irc: top-level filters are ANDed; empty -> empty", "[uc][irc]") {
	vector<unique_ptr<Expression>> filters;
	CHECK(SerializeFiltersToIRC(filters).empty());

	filters.push_back(Cmp(ExpressionType::COMPARE_GREATERTHAN, Col("age"), Const(Value::INTEGER(10))));
	filters.push_back(Cmp(ExpressionType::COMPARE_LESSTHAN, Col("age"), Const(Value::INTEGER(20))));
	CHECK(
	    SerializeFiltersToIRC(filters) ==
	    R"({"type":"and","left":{"type":"gt","term":"age","value":10},"right":{"type":"lt","term":"age","value":20}})");
}
