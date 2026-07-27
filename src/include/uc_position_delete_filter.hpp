#pragma once

// Split out of uc_multi_file_list.cpp only so the position-set -> surviving-rows logic can be
// unit-tested in isolation (test/functions/test_uc_position_delete_filter.cpp).

#include "duckdb/common/multi_file/multi_file_data.hpp" // DeleteFilter (+ row_t/idx_t/SelectionVector)
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {

// What both classic (file_path, pos) delete files and decoded deletion-vector bitmaps reduce to.
// Same shape as delta's DeltaDeleteFilter and iceberg's IcebergPositionalDeleteFilter.
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
