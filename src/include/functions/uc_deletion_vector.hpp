#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

// uc_read_deletion_vector(path [, content_offset => <b>, content_size => <b>]) -> table(pos BIGINT)
//
// The deleted row positions an Iceberg deletion vector marks: a bare `deletion-vector-v1` blob at
// the given byte range (what a scan-plan response points at), or the whole file as a puffin
// container when the range is omitted. Makes the delete path inspectable from SQL with no server.
// Deliberately public rather than `__internal_` — nothing about it is UC-specific.
class UCReadDeletionVectorFunction : public TableFunction {
public:
	UCReadDeletionVectorFunction();
};

} // namespace duckdb
