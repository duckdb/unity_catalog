#pragma once

// A DeleteFilter over a set of absolute deleted row positions. Split into its own header (rather
// than file-local in uc_multi_file_list.cpp) so the position-set -> surviving-rows logic can be
// unit-tested in isolation. See test/cpp/test_uc_position_delete_filter.cpp.

#include "duckdb/common/multi_file/multi_file_data.hpp" // DeleteFilter (+ row_t/idx_t/SelectionVector)
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {

// The common shape both classic (file_path, pos) delete files and decoded deletion-vector bitmaps
// reduce to. Mirrors the vendored delta extension's DeltaDeleteFilter / the iceberg extension's
// IcebergPositionalDeleteFilter (same DeleteFilter contract, same O(1)-membership-check shape).
struct UCPositionDeleteFilter : public DeleteFilter {
	explicit UCPositionDeleteFilter(unordered_set<int64_t> positions_p) : positions(std::move(positions_p)) {
	}

	idx_t Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) override {
		if (count == 0) {
			return 0;
		}
		result_sel.Initialize(STANDARD_VECTOR_SIZE);
		idx_t selected = 0;
		for (idx_t i = 0; i < count; i++) {
			if (!positions.count(start_row_index + i)) {
				result_sel.set_index(selected++, i);
			}
		}
		return selected;
	}

	unordered_set<int64_t> positions;
};

} // namespace duckdb
