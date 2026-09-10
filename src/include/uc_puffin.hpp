//===----------------------------------------------------------------------===//
//                         DuckDB
//
// uc_puffin.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/unordered_map.hpp"
#include <roaring/roaring.hh>

namespace duckdb {

// Reads Iceberg deletion vectors (puffin-spec.md, "deletion-vector-v1" blob type). A puffin
// file is Magic | Blob1 ... BlobN | Footer (JSON blob metadata); a lone deletion vector may
// also appear as a bare blob with no container/footer (the shape UC's scan-plan response
// points at via content-offset/content-size-in-bytes). Ported from ducklake's
// storage/ducklake_puffin.{hpp,cpp} (same license, same org) — only the reader half; UC never
// writes deletion vectors.
struct UCPuffinBlob {
	idx_t offset = 0;
	idx_t length = 0;
};

// Deserialized deletion-vector-v1 blob: high-32-bits -> roaring bitmap of low-32-bits, so one
// blob can cover positions across the full int64 range in 2^32-sized buckets.
struct UCDeletionVectorData {
public:
	static constexpr data_t DELETION_VECTOR_MAGIC[4] = {0xD1, 0xD3, 0x39, 0x64};

public:
	static unique_ptr<UCDeletionVectorData> FromBlob(data_ptr_t blob_start, idx_t blob_length, const string &path);
	//! Positions this blob marks deleted, as absolute int64 row positions.
	void ToSet(set<idx_t> &out) const;

public:
	unordered_map<int32_t, roaring::Roaring> bitmaps;
};

class UCPuffinReader {
public:
	UCPuffinReader(data_ptr_t data, idx_t size, const string &path);

	const vector<UCPuffinBlob> &Blobs() const {
		return blobs;
	}
	unique_ptr<UCDeletionVectorData> DecodeBlob(const UCPuffinBlob &blob) const;

private:
	void ParseFooter();

private:
	data_ptr_t data;
	idx_t size;
	string path;
	vector<UCPuffinBlob> blobs;
};

} // namespace duckdb
