#include "functions/uc_deletion_vector.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include "uc_puffin.hpp"

namespace duckdb {

struct ReadDVBindData : public TableFunctionData {
	string path;
	int64_t offset = 0;
	int64_t size = -1;      // used only when whole_file == false
	bool whole_file = true; // true => read the whole file via UCPuffinReader
};

struct ReadDVGlobalState : public GlobalTableFunctionState {
	vector<int64_t> positions; // sorted (materialized once at init)
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadDVBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ReadDVBindData>();
	result->path = input.inputs[0].ToString();

	auto off_it = input.named_parameters.find("content_offset");
	auto sz_it = input.named_parameters.find("content_size");
	bool has_off = off_it != input.named_parameters.end();
	bool has_sz = sz_it != input.named_parameters.end();
	if (has_off != has_sz) {
		throw InvalidInputException("uc_read_deletion_vector: 'content_offset' and 'content_size' must be given "
		                            "together (or neither, to read the whole file)");
	}
	if (has_off) {
		result->offset = off_it->second.GetValue<int64_t>();
		result->size = sz_it->second.GetValue<int64_t>();
		if (result->offset < 0 || result->size < 0) {
			throw InvalidInputException("uc_read_deletion_vector: 'content_offset' and 'content_size' must be "
			                            "non-negative");
		}
		result->whole_file = false;
	}

	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("pos");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadDVInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadDVBindData>();
	auto state = make_uniq<ReadDVGlobalState>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(bind.path, FileOpenFlags::FILE_FLAGS_READ);

	set<idx_t> positions;
	if (bind.whole_file) {
		auto file_size = NumericCast<idx_t>(handle->GetFileSize());
		auto buffer = make_unsafe_uniq_array<data_t>(file_size);
		handle->Read(buffer.get(), file_size, 0);
		UCPuffinReader reader(buffer.get(), file_size, bind.path);
		for (auto &blob : reader.Blobs()) {
			reader.DecodeBlob(blob)->ToSet(positions);
		}
	} else {
		auto len = NumericCast<idx_t>(bind.size);
		auto buffer = make_unsafe_uniq_array<data_t>(len);
		handle->Read(buffer.get(), len, NumericCast<idx_t>(bind.offset));
		UCDeletionVectorData::FromBlob(buffer.get(), len, bind.path)->ToSet(positions);
	}

	state->positions.reserve(positions.size());
	for (auto pos : positions) {
		state->positions.push_back(NumericCast<int64_t>(pos));
	}
	return std::move(state);
}

static void ReadDVFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<ReadDVGlobalState>();
	auto out = FlatVector::GetDataMutable<int64_t>(output.data[0]);
	idx_t count = 0;
	while (state.cursor < state.positions.size() && count < STANDARD_VECTOR_SIZE) {
		out[count++] = state.positions[state.cursor++];
	}
	output.SetChildCardinality(count);
}

UCReadDeletionVectorFunction::UCReadDeletionVectorFunction()
    : TableFunction("uc_read_deletion_vector", {LogicalType::VARCHAR}, ReadDVFunction, ReadDVBind, ReadDVInit) {
	named_parameters["content_offset"] = LogicalType::BIGINT;
	named_parameters["content_size"] = LogicalType::BIGINT;
}

} // namespace duckdb
