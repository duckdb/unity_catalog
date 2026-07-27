// Server-free unit coverage of UCPositionDeleteFilter (src/include/uc_position_delete_filter.hpp):
// the position-set -> surviving-row-selection every delete path (classic position-delete files and
// decoded deletion vectors) funnels through. Isolated here because its input is an in-memory
// position set + selection vector, not anything a SQL query can hand it directly.
//
// Runner: the standalone `unittest_cpp` Catch executable — see test_uc_irc_expression.cpp
// (build with `--target unittest_cpp`, filter tag "[uc][delete]").

#include "catch.hpp"

#include "uc_position_delete_filter.hpp"

#include "duckdb/common/types/selection_vector.hpp"

using namespace duckdb;

namespace {

// Run the filter over rows [start, start+count) and return the surviving LOCAL indices (0..count-1).
vector<idx_t> Surviving(unordered_set<int64_t> deleted, row_t start, idx_t count) {
	UCPositionDeleteFilter filter(std::move(deleted));
	SelectionVector sel;
	idx_t n = filter.Filter(start, count, sel);
	vector<idx_t> out;
	for (idx_t i = 0; i < n; i++) {
		out.push_back(sel.get_index(i));
	}
	return out;
}

} // namespace

TEST_CASE("uc position-delete: no deletes keeps every row", "[uc][delete]") {
	CHECK(Surviving({}, 0, 4) == vector<idx_t>({0, 1, 2, 3}));
}

TEST_CASE("uc position-delete: deleted rows are dropped, surviving order preserved", "[uc][delete]") {
	// Absolute positions 1 and 3 deleted, scanning [0,5) -> local 0,2,4 survive.
	CHECK(Surviving({1, 3}, 0, 5) == vector<idx_t>({0, 2, 4}));
}

TEST_CASE("uc position-delete: start_row_index offsets the absolute position", "[uc][delete]") {
	// The set is ABSOLUTE. Scanning [100,105): absolute 102 deleted -> local index 2 dropped.
	CHECK(Surviving({102}, 100, 5) == vector<idx_t>({0, 1, 3, 4}));
	// A deleted position outside the scanned range has no effect.
	CHECK(Surviving({7}, 100, 3) == vector<idx_t>({0, 1, 2}));
}

TEST_CASE("uc position-delete: empty chunk and all-deleted return zero", "[uc][delete]") {
	CHECK(Surviving({5, 6, 7}, 0, 0).empty());         // count == 0
	CHECK(Surviving({100, 101, 102}, 100, 3).empty()); // every scanned row deleted
}
