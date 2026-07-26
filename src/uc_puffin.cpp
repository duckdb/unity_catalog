// Ported from ducklake's storage/ducklake_puffin.cpp (reader half only; same license/org).
#include "uc_puffin.hpp"

#include "duckdb/common/bswap.hpp"
#include "yyjson.hpp"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

struct YyjsonDocHolder {
	explicit YyjsonDocHolder(yyjson_doc *doc) : doc(doc) {
	}
	~YyjsonDocHolder() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
	yyjson_doc *doc;
};

class CRC32 {
public:
	CRC32() : crc(0xFFFFFFFF) {
		InitTable();
	}

public:
	static void InitTable() {
		if (table_initialized) {
			return;
		}
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int j = 0; j < 8; j++) {
				c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
			}
			crc_table[i] = c;
		}
		table_initialized = true;
	}

public:
	void Update(const data_t *data, idx_t length) {
		for (idx_t i = 0; i < length; i++) {
			crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		}
	}
	uint32_t GetValue() const {
		return crc ^ 0xFFFFFFFF;
	}

private:
	uint32_t crc;
	static uint32_t crc_table[256];
	static bool table_initialized;
};

uint32_t CRC32::crc_table[256];
bool CRC32::table_initialized = false;

} // namespace

// https://iceberg.apache.org/puffin-spec/#deletion-vector-v1-blob-type
// Blob: vector_size (4, LE) | magic (4) | n_bitmaps (8, LE) | {key (4, LE) | portable-roaring}* | CRC32 (4, BE)
unique_ptr<UCDeletionVectorData> UCDeletionVectorData::FromBlob(data_ptr_t blob_start, idx_t blob_length,
                                                                const string &path) {
	// Smallest legal blob = vector_size(4) + magic(4) + n_bitmaps(8) + CRC32(4) = 20 (zero bitmaps).
	if (blob_length < 20) {
		throw InvalidInputException("Deletion vector blob in \"%s\" is too small (%d bytes)", path, blob_length);
	}
	auto blob_end = blob_start + blob_length;
	auto vector_size = BSwap(Load<uint32_t>(blob_start));
	blob_start += sizeof(uint32_t);

	auto checksummed_data_start = blob_start;
	if (memcmp(DELETION_VECTOR_MAGIC, blob_start, 4) != 0) {
		throw InvalidInputException("Deletion vector blob in \"%s\" has a magic mismatch", path);
	}
	blob_start += 4;
	vector_size -= 4;
	if (blob_start + sizeof(int64_t) > blob_end) {
		throw InvalidInputException("Deletion vector blob in \"%s\" is corrupt (truncated bitmap count)", path);
	}

	int64_t amount_of_bitmaps = Load<int64_t>(blob_start);
	blob_start += sizeof(int64_t);
	vector_size -= sizeof(int64_t);
	if (amount_of_bitmaps < 0 || blob_start > blob_end) {
		throw InvalidInputException("Deletion vector blob in \"%s\" is corrupt (bad bitmap count)", path);
	}

	auto result = make_uniq<UCDeletionVectorData>();
	result->bitmaps.reserve(amount_of_bitmaps);
	for (int64_t i = 0; i < amount_of_bitmaps; i++) {
		if (blob_start + sizeof(int32_t) > blob_end) {
			throw InvalidInputException("Deletion vector blob in \"%s\" is corrupt (truncated bitmap key)", path);
		}
		auto key = Load<int32_t>(blob_start);
		blob_start += sizeof(int32_t);
		vector_size -= sizeof(int32_t);

		size_t bitmap_size =
		    roaring::api::roaring_bitmap_portable_deserialize_size((const char *)blob_start, vector_size);
		if (bitmap_size == 0 || blob_start + bitmap_size > blob_end) {
			throw InvalidInputException("Deletion vector blob in \"%s\" is corrupt (bad bitmap payload)", path);
		}
		auto bitmap = roaring::Roaring::readSafe((const char *)blob_start, bitmap_size);
		blob_start += bitmap_size;
		vector_size -= bitmap_size;
		result->bitmaps.emplace(key, std::move(bitmap));
	}

	auto checksummed_data_length = blob_start - checksummed_data_start;
	if (blob_start + sizeof(uint32_t) != blob_end) {
		throw InvalidInputException("Deletion vector blob in \"%s\" is corrupt (trailing bytes after checksum)", path);
	}
	auto stored_checksum = BSwap(Load<uint32_t>(blob_start));

	CRC32 crc;
	crc.Update(checksummed_data_start, checksummed_data_length);
	if (crc.GetValue() != stored_checksum) {
		throw InvalidInputException("Deletion vector blob in \"%s\" failed its checksum (corrupt or truncated)", path);
	}
	return result;
}

namespace {
struct RoaringIterateContext {
	set<idx_t> *out;
	idx_t high;
};
} // namespace

void UCDeletionVectorData::ToSet(set<idx_t> &out) const {
	for (auto &entry : bitmaps) {
		RoaringIterateContext ctx {&out, static_cast<idx_t>(entry.first)};
		entry.second.iterate(
		    [](uint32_t value, void *ptr) -> bool {
			    auto *ctx = static_cast<RoaringIterateContext *>(ptr);
			    ctx->out->insert((ctx->high << 32) | static_cast<idx_t>(value));
			    return true;
		    },
		    &ctx);
	}
}

//===--------------------------------------------------------------------===//
// Puffin container (only needed for the multi-blob/footer form; UC's scan-plan
// content-offset/content-size-in-bytes already locates a single bare blob directly,
// but a full puffin container is also handled in case a server ever points there).
//===--------------------------------------------------------------------===//

static constexpr const data_t PUFFIN_MAGIC[4] = {'P', 'F', 'A', '1'};
static constexpr idx_t PUFFIN_MAGIC_SIZE = 4;
static constexpr idx_t PUFFIN_FOOTER_SIZE_FIELD_SIZE = 4;
static constexpr idx_t PUFFIN_FOOTER_FLAGS_SIZE = 4;
static constexpr idx_t PUFFIN_FOOTER_TAIL_SIZE =
    PUFFIN_FOOTER_SIZE_FIELD_SIZE + PUFFIN_FOOTER_FLAGS_SIZE + PUFFIN_MAGIC_SIZE;
static constexpr idx_t PUFFIN_FOOTER_STRUCT_SIZE = PUFFIN_MAGIC_SIZE + PUFFIN_FOOTER_TAIL_SIZE;
static constexpr idx_t PUFFIN_MIN_FILE_SIZE = PUFFIN_MAGIC_SIZE + PUFFIN_FOOTER_STRUCT_SIZE;
static constexpr uint32_t PUFFIN_FOOTER_COMPRESSED_FLAG = 1;
static constexpr const char *DELETION_VECTOR_BLOB_TYPE = "deletion-vector-v1";

static bool IsBareDeletionVector(data_ptr_t data, idx_t size) {
	auto &magic = UCDeletionVectorData::DELETION_VECTOR_MAGIC;
	return size >= sizeof(uint32_t) + sizeof(magic) && memcmp(data + sizeof(uint32_t), magic, sizeof(magic)) == 0;
}

// Footer: Magic | FooterPayload | FooterPayloadSize (LE) | Flags | Magic. Returns the payload offset.
static idx_t ValidateFooter(data_ptr_t data, idx_t size, const string &path) {
	if (memcmp(data + size - PUFFIN_MAGIC_SIZE, PUFFIN_MAGIC, PUFFIN_MAGIC_SIZE) != 0) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - trailing magic mismatch", path);
	}
	auto flags = Load<uint32_t>(data + size - PUFFIN_MAGIC_SIZE - PUFFIN_FOOTER_FLAGS_SIZE);
	if (flags & PUFFIN_FOOTER_COMPRESSED_FLAG) {
		throw InvalidInputException("Puffin file \"%s\" has a compressed footer, which is not supported", path);
	}
	if (flags != 0) {
		throw InvalidInputException("Puffin file \"%s\" has unsupported footer flags", path);
	}
	idx_t payload_size = Load<uint32_t>(data + size - PUFFIN_FOOTER_TAIL_SIZE);
	if (payload_size > size - PUFFIN_MIN_FILE_SIZE) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - footer payload size out of range", path);
	}
	auto payload_start = size - PUFFIN_FOOTER_TAIL_SIZE - payload_size;
	if (memcmp(data + payload_start - PUFFIN_MAGIC_SIZE, PUFFIN_MAGIC, PUFFIN_MAGIC_SIZE) != 0) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - footer magic mismatch", path);
	}
	return payload_start;
}

static bool TryParseBlobMetadata(yyjson_val *blob_val, idx_t blob_section_end, const string &path, UCPuffinBlob &blob) {
	auto type_val = yyjson_obj_get(blob_val, "type");
	if (!type_val || !yyjson_is_str(type_val)) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - blob without a type", path);
	}
	string blob_type(yyjson_get_str(type_val), yyjson_get_len(type_val));
	if (blob_type != DELETION_VECTOR_BLOB_TYPE) {
		return false; // not a deletion vector (e.g. a stats blob) - ignore
	}
	auto offset_val = yyjson_obj_get(blob_val, "offset");
	auto length_val = yyjson_obj_get(blob_val, "length");
	if (!offset_val || !yyjson_is_int(offset_val) || !length_val || !yyjson_is_int(length_val)) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - blob without offset/length", path);
	}
	auto raw_offset = yyjson_get_sint(offset_val);
	auto raw_length = yyjson_get_sint(length_val);
	if (raw_offset < 0 || raw_length < 0) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - blob offset/length out of range", path);
	}
	blob.offset = NumericCast<idx_t>(raw_offset);
	blob.length = NumericCast<idx_t>(raw_length);
	if (blob.offset < PUFFIN_MAGIC_SIZE || blob.length > blob_section_end ||
	    blob.offset > blob_section_end - blob.length) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - blob offset/length out of range", path);
	}
	return true;
}

static vector<UCPuffinBlob> ParseFileMetadata(data_ptr_t payload, idx_t payload_size, idx_t blob_section_end,
                                              const string &path) {
	YyjsonDocHolder doc_holder(yyjson_read(const_char_ptr_cast(payload), payload_size, 0));
	if (!doc_holder.doc) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - failed to parse footer payload", path);
	}
	auto root = yyjson_doc_get_root(doc_holder.doc);
	auto blobs_val = yyjson_obj_get(root, "blobs");
	if (!blobs_val || !yyjson_is_arr(blobs_val)) {
		throw InvalidInputException("Puffin file \"%s\" is corrupt - footer has no \"blobs\" list", path);
	}
	vector<UCPuffinBlob> result;
	size_t arr_idx, arr_max;
	yyjson_val *blob_val;
	yyjson_arr_foreach(blobs_val, arr_idx, arr_max, blob_val) {
		UCPuffinBlob blob;
		if (TryParseBlobMetadata(blob_val, blob_section_end, path, blob)) {
			result.push_back(blob);
		}
	}
	return result;
}

UCPuffinReader::UCPuffinReader(data_ptr_t data, idx_t size, const string &path) : data(data), size(size), path(path) {
	ParseFooter();
}

void UCPuffinReader::ParseFooter() {
	if (size >= PUFFIN_MIN_FILE_SIZE && memcmp(data, PUFFIN_MAGIC, PUFFIN_MAGIC_SIZE) == 0) {
		auto payload_start = ValidateFooter(data, size, path);
		auto payload_size = size - PUFFIN_FOOTER_TAIL_SIZE - payload_start;
		auto blob_section_end = payload_start - PUFFIN_MAGIC_SIZE;
		blobs = ParseFileMetadata(data + payload_start, payload_size, blob_section_end, path);
		return;
	}
	if (IsBareDeletionVector(data, size)) {
		UCPuffinBlob blob;
		blob.offset = 0;
		blob.length = size;
		blobs.push_back(blob);
		return;
	}
	throw InvalidInputException("File \"%s\" is not a valid deletion vector - magic mismatch", path);
}

unique_ptr<UCDeletionVectorData> UCPuffinReader::DecodeBlob(const UCPuffinBlob &blob) const {
	return UCDeletionVectorData::FromBlob(data + blob.offset, blob.length, path);
}

} // namespace duckdb
