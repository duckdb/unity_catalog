#include "uc_irc_expression.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

static string ValueToIRCJson(const Value &val) {
	switch (val.type().id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return to_string(val.GetValue<int64_t>());
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		// %.17g round-trips an IEEE-754 double exactly. std::to_string() would emit 6 fixed
		// decimals, which can *tighten* a bound (a `>`/`<` term serialized more restrictively
		// than the real predicate makes the server over-prune files → missing rows, since the
		// IRC filter is a server-side pruning hint the client can't recover files from). It also
		// avoids emitting non-JSON `inf`/`nan` tokens — a non-finite bound can't prune, so drop
		// the term (return "") and let DuckDB's own filter handle it.
		double d = val.GetValue<double>();
		if (!Value::DoubleIsFinite(d)) {
			return "";
		}
		return StringUtil::Format("%.17g", d);
	}
	case LogicalTypeId::BOOLEAN:
		return val.GetValue<bool>() ? "true" : "false";
	case LogicalTypeId::VARCHAR: {
		string s = val.ToString();
		string result = "\"";
		for (char c : s) {
			if (c == '"') {
				result += "\\\"";
			} else if (c == '\\') {
				result += "\\\\";
			} else if (c == '\n') {
				result += "\\n";
			} else if (c == '\r') {
				result += "\\r";
			} else if (c == '\t') {
				result += "\\t";
			} else {
				result += c;
			}
		}
		result += "\"";
		return result;
	}
	default:
		return "";
	}
}

// Returns an IRC Expression JSON string, or "" if the expression cannot be
// serialized (caller treats "" as "omit this term").
static string ExprToIRCJson(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		// Comparisons bind to a BOUND_FUNCTION node (COMPARE_EQUAL, etc. live as a scalar function
		// call, not a dedicated node class) — BOUND_COMPARISON is legacy/deserialize-only now.
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return "";
		}
		auto &cmp = expr.Cast<BoundFunctionExpression>();
		const Expression &left_expr = BoundComparisonExpression::Left(cmp);
		const Expression &right_expr = BoundComparisonExpression::Right(cmp);

		const BoundColumnRefExpression *col_ref = nullptr;
		const BoundConstantExpression *const_ = nullptr;
		bool flipped = false;

		if (left_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		    right_expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			col_ref = &left_expr.Cast<BoundColumnRefExpression>();
			const_ = &right_expr.Cast<BoundConstantExpression>();
		} else if (right_expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
		           left_expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			col_ref = &right_expr.Cast<BoundColumnRefExpression>();
			const_ = &left_expr.Cast<BoundConstantExpression>();
			flipped = true;
		} else {
			return "";
		}

		ExpressionType effective_type =
		    flipped ? FlipComparisonExpression(expr.GetExpressionType()) : expr.GetExpressionType();
		const char *irc_type = nullptr;
		switch (effective_type) {
		case ExpressionType::COMPARE_EQUAL:
			irc_type = "eq";
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			irc_type = "not-eq";
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			irc_type = "lt";
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			irc_type = "gt";
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			irc_type = "lt-eq";
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			irc_type = "gt-eq";
			break;
		default:
			return "";
		}

		string col_name = col_ref->GetName().GetIdentifierName();
		if (col_name.empty()) {
			return "";
		}
		string val_json = ValueToIRCJson(const_->GetValue());
		if (val_json.empty()) {
			return "";
		}
		return string("{\"type\":\"") + irc_type + "\",\"term\":\"" + col_name + "\",\"value\":" + val_json + "}";
	}

	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = reinterpret_cast<const BoundConjunctionExpression &>(expr);
		bool is_and = (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
		const char *irc_type = is_and ? "and" : "or";

		vector<string> parts;
		for (auto &child : conj.GetChildren()) {
			string s = ExprToIRCJson(*child);
			if (s.empty()) {
				if (!is_and) {
					return ""; // OR with an unsupported child = vacuously true; drop whole OR
				}
				// AND with an unsupported child: skip it
			} else {
				parts.push_back(std::move(s));
			}
		}
		if (parts.empty()) {
			return "";
		}
		if (parts.size() == 1) {
			return parts[0];
		}
		string result = parts[0];
		for (idx_t i = 1; i < parts.size(); i++) {
			result = string("{\"type\":\"") + irc_type + "\",\"left\":" + result + ",\"right\":" + parts[i] + "}";
		}
		return result;
	}

	case ExpressionClass::BOUND_OPERATOR: {
		if (expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NULL &&
		    expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL) {
			return "";
		}
		auto &op = reinterpret_cast<const BoundOperatorExpression &>(expr);
		auto &op_children = op.GetChildren();
		if (op_children.size() != 1 || op_children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return "";
		}
		auto &col = reinterpret_cast<const BoundColumnRefExpression &>(*op_children[0]);
		string col_name = col.GetName().GetIdentifierName();
		if (col_name.empty()) {
			return "";
		}
		const char *irc_type = (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) ? "is-null" : "not-null";
		return string("{\"type\":\"") + irc_type + "\",\"term\":\"" + col_name + "\"}";
	}

	default:
		return "";
	}
}

string SerializeFiltersToIRC(const vector<unique_ptr<Expression>> &filters) {
	if (filters.empty()) {
		return "";
	}
	vector<string> parts;
	for (auto &f : filters) {
		string s = ExprToIRCJson(*f);
		if (!s.empty()) {
			parts.push_back(std::move(s));
		}
	}
	if (parts.empty()) {
		return "";
	}
	if (parts.size() == 1) {
		return parts[0];
	}
	string result = parts[0];
	for (idx_t i = 1; i < parts.size(); i++) {
		result = "{\"type\":\"and\",\"left\":" + result + ",\"right\":" + parts[i] + "}";
	}
	return result;
}

} // namespace duckdb
