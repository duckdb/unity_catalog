#include "uc_multi_file_list.hpp"
#include "uc_puffin.hpp"
#include "uc_logging.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

static OpenFileInfo MakeOpenFileInfo(const UCScanPlanDataFile &df) {
	OpenFileInfo info(df.file_path);
	if (df.file_size_in_bytes > 0) {
		auto ext = make_shared_ptr<ExtendedOpenFileInfo>();
		ext->options["file_size"] = Value::UBIGINT((uint64_t)df.file_size_in_bytes);
		info.extended_info = std::move(ext);
	}
	return info;
}

void UCMultiFileList::AddFileScanTask(const UCScanPlanFileScanTask &task,
                                      const vector<UCScanDeleteFile> &all_delete_files) const {
	expanded_files.push_back(MakeOpenFileInfo(task.data_file));
	vector<UCScanDeleteFile> resolved;
	resolved.reserve(task.delete_file_references.size());
	for (auto ref : task.delete_file_references) {
		if (ref >= all_delete_files.size()) {
			throw IOException("scan-plan: file-scan-task for '%s' references delete-file index %d, but the "
			                  "response only carries %d delete file(s) — server response is malformed",
			                  task.data_file.file_path, ref, all_delete_files.size());
		}
		resolved.push_back(all_delete_files[ref]);
	}
	file_deletes.push_back(std::move(resolved));
}

const vector<UCScanDeleteFile> &UCMultiFileList::GetDeleteFilesForFile(idx_t file_idx) const {
	D_ASSERT(file_idx < file_deletes.size());
	return file_deletes[file_idx];
}

const string &UCMultiFileList::GetDataFilePath(idx_t file_idx) const {
	D_ASSERT(file_idx < expanded_files.size());
	return expanded_files[file_idx].path;
}

UCMultiFileList::UCMultiFileList(ClientContext &context, const UCScanPlanResult &plan, UCCredentials credentials_p,
                                 string catalog_name_p, string schema_name_p, string table_name_p,
                                 string scan_plan_endpoint_p)
    : LazyMultiFileList(&context), credentials(std::move(credentials_p)), catalog_name(std::move(catalog_name_p)),
      schema_name(std::move(schema_name_p)), table_name(std::move(table_name_p)),
      scan_plan_endpoint(std::move(scan_plan_endpoint_p)) {
	// Pre-seed inline file-scan-tasks — available immediately without a fetch round-trip.
	for (auto &task : plan.file_scan_tasks) {
		AddFileScanTask(task, plan.delete_files);
	}
	for (auto &token : plan.plan_tasks) {
		remaining_tokens.push_back(token);
	}
	if (remaining_tokens.empty()) {
		all_files_expanded = true;
	}
}

bool UCMultiFileList::ExpandNextPath() const {
	if (remaining_tokens.empty()) {
		return false;
	}

	string token = std::move(remaining_tokens.front());
	remaining_tokens.erase(remaining_tokens.begin());

	auto &ctx = *context.get_mutable();
	auto result =
	    UCAPI::FetchScanTasks(ctx, catalog_name, schema_name, table_name, token, credentials, scan_plan_endpoint);
	for (auto &task : result.file_scan_tasks) {
		AddFileScanTask(task, result.delete_files);
	}
	// Re-queue nested tokens (server may return additional plan-tasks from fetchScanTasks).
	for (auto &new_token : result.plan_tasks) {
		remaining_tokens.push_back(std::move(new_token));
	}

	return !remaining_tokens.empty();
}

// Thread-local staging area for UCScanPlanPushdownFilter → UCMultiFileReaderFactory handoff.
// Set immediately before parquet_fn.bind(); consumed (moved) by the factory on the same thread.
thread_local shared_ptr<UCMultiFileList> tl_uc_file_list;

unique_ptr<MultiFileReader> UCMultiFileReaderFactory(const TableFunction &) {
	D_ASSERT(tl_uc_file_list);
	return make_uniq<UCMultiFileReader>(std::move(tl_uc_file_list));
}

// ---------------------------------------------------------------------------
// Delete-file application
// ---------------------------------------------------------------------------

// A DeleteFilter backed by a plain set of absolute deleted row positions — the common shape
// both classic (file_path, pos) delete files and decoded deletion-vector bitmaps reduce to.
// Mirrors the vendored delta extension's DeltaDeleteFilter / the iceberg extension's
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

// Grab a private copy of the system `parquet_scan` table function (same lookup
// UCScanPlanPushdownFilter uses for the main data scan).
static TableFunction GetParquetScanFunction(ClientContext &context) {
	auto &sys_cat = Catalog::GetSystemCatalog(context);
	auto &parquet_entry = sys_cat.GetEntry<TableFunctionCatalogEntry>(
	    context, QualifiedName({Identifier(DEFAULT_SCHEMA)}, Identifier("parquet_scan")));
	return parquet_entry.functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
}

// Bind + fully drain `path` through parquet_scan, calling `consume` on each non-empty chunk.
// Delete files are small (a handful of KB to low MB) so this is fine to fully materialize.
static void ScanParquetFile(ClientContext &context, const string &path,
                            const std::function<void(DataChunk &)> &consume) {
	auto parquet_fn = GetParquetScanFunction(context);

	vector<Value> inputs = {Value(path)};
	named_parameter_map_t named_params;
	vector<LogicalType> input_table_types;
	vector<Identifier> input_table_names;
	TableFunctionRef dummy_ref;
	vector<LogicalType> return_types;
	vector<string> return_names;
	TableFunctionBindInput bind_input(inputs, named_params, input_table_types, input_table_names, nullptr, nullptr,
	                                  parquet_fn, dummy_ref);
	auto bind_data = parquet_fn.bind(context, bind_input, return_types, return_names);

	vector<column_t> column_ids;
	for (idx_t i = 0; i < return_types.size(); i++) {
		column_ids.push_back(i);
	}
	TableFunctionInitInput init_input(bind_data.get(), column_ids, vector<idx_t>(), nullptr);
	auto global_state = parquet_fn.init_global(context, init_input);
	ThreadContext thread_context(context);
	ExecutionContext execution_context(context, thread_context, nullptr);
	auto local_state = parquet_fn.init_local(execution_context, init_input, global_state.get());

	DataChunk result;
	result.Initialize(context, return_types, STANDARD_VECTOR_SIZE);
	do {
		TableFunctionInput function_input(bind_data.get(), local_state.get(), global_state.get());
		result.Reset();
		parquet_fn.function(context, function_input, result);
		if (result.size() > 0) {
			result.Flatten();
			consume(result);
		}
	} while (result.size() != 0);
}

// Classic Iceberg v2 positional-delete file: columns (file_path VARCHAR, pos BIGINT, [row]).
// One delete file can reference multiple data files — keep only positions for `data_file_path`.
static void ScanPositionalDeleteFile(ClientContext &context, const UCScanDeleteFile &delete_file,
                                     const string &data_file_path, unordered_set<int64_t> &out) {
	if (delete_file.file_format != "parquet") {
		throw NotImplementedException(
		    "scan-plan: positional delete file '%s' has file-format '%s' — only parquet-format position-delete "
		    "files are supported (the format Delta-via-UniForm is expected to produce); avro/orc are not.",
		    delete_file.file_path, delete_file.file_format);
	}
	ScanParquetFile(context, delete_file.file_path, [&](DataChunk &chunk) {
		if (chunk.ColumnCount() < 2) {
			throw IOException("scan-plan: positional delete file '%s' has %d column(s), expected at least "
			                  "(file_path, pos)",
			                  delete_file.file_path, chunk.ColumnCount());
		}
		auto paths = FlatVector::GetData<string_t>(chunk.data[0]);
		auto positions = FlatVector::GetData<int64_t>(chunk.data[1]);
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (paths[i].GetString() == data_file_path) {
				out.insert(positions[i]);
			}
		}
	});
	// Logged (with the DV counterpart below) so a test can assert WHICH delete sub-path a live
	// scan-plan response actually took — classic parquet position-delete vs deletion-vector blob.
	UC_LOG_DEBUG(context, "scan-plan.PositionalDelete file=%s data_file=%s", delete_file.file_path, data_file_path);
}

// Iceberg v3 deletion vector: a `deletion-vector-v1` puffin blob at [content_offset,
// content_offset + content_size_in_bytes) within delete_file.file_path.
static void ScanDeletionVectorFile(ClientContext &context, const UCScanDeleteFile &delete_file,
                                   unordered_set<int64_t> &out) {
	// content_offset >= 0 is guaranteed by BuildUCDeleteFilter's routing; the spec requires
	// content-size-in-bytes whenever content-offset is present, so a negative size here means a
	// malformed response — fail with a clear message rather than an opaque NumericCast error.
	if (delete_file.content_size_in_bytes < 0) {
		throw IOException("scan-plan: deletion-vector delete file '%s' has content-offset %d but no "
		                  "content-size-in-bytes — server response is malformed",
		                  delete_file.file_path, delete_file.content_offset);
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(delete_file.file_path, FileOpenFlags::FILE_FLAGS_READ);
	auto length = NumericCast<idx_t>(delete_file.content_size_in_bytes);
	auto buffer = make_unsafe_uniq_array<data_t>(length);
	handle->Read(buffer.get(), length, NumericCast<idx_t>(delete_file.content_offset));

	auto blob = UCDeletionVectorData::FromBlob(buffer.get(), length, delete_file.file_path);
	set<idx_t> positions;
	blob->ToSet(positions);
	// The load-bearing confirmation: this line is emitted ONLY when a delete resolves to a
	// deletion-vector-v1 puffin blob (content-offset present), so a test asserting its presence
	// proves the puffin/DV path — not just that some delete was applied. See scan_plan_deletes.test.
	UC_LOG_DEBUG(context, "scan-plan.DeletionVector file=%s offset=%lld size=%lld positions=%zu", delete_file.file_path,
	             (long long)delete_file.content_offset, (long long)delete_file.content_size_in_bytes, positions.size());
	for (auto pos : positions) {
		out.insert(NumericCast<int64_t>(pos));
	}
}

unique_ptr<DeleteFilter> BuildUCDeleteFilter(ClientContext &context, const string &data_file_path,
                                             const vector<UCScanDeleteFile> &deletes) {
	if (deletes.empty()) {
		return nullptr;
	}
	for (auto &d : deletes) {
		if (d.content == UCScanDeleteFileType::EQUALITY_DELETES) {
			throw NotImplementedException(
			    "scan-plan: table has an equality-delete file ('%s') referencing data file '%s'. Applying "
			    "equality deletes correctly requires mapping each Iceberg field-id in its equality-ids to this "
			    "table's output columns; that mapping is not resolved anywhere in this extension today (no "
			    "field-id-carrying schema fetch exists), so the safe choice is to fail loud rather than silently "
			    "match columns by name (wrong under a renamed column). See BuildUCDeleteFilter's doc comment.",
			    d.file_path, data_file_path);
		}
	}

	unordered_set<int64_t> deleted_positions;
	for (auto &d : deletes) {
		D_ASSERT(d.content == UCScanDeleteFileType::POSITION_DELETES);
		if (d.content_offset >= 0) {
			ScanDeletionVectorFile(context, d, deleted_positions);
		} else {
			ScanPositionalDeleteFile(context, d, data_file_path, deleted_positions);
		}
	}
	if (deleted_positions.empty()) {
		return nullptr;
	}
	return make_uniq<UCPositionDeleteFilter>(std::move(deleted_positions));
}

void UCMultiFileReader::FinalizeBind(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
                                     const MultiFileReaderBindData &options,
                                     const vector<MultiFileColumnDefinition> &global_columns,
                                     const vector<ColumnIndex> &global_column_ids, ClientContext &context,
                                     optional_ptr<MultiFileReaderGlobalState> global_state) {
	MultiFileReader::FinalizeBind(reader_data, file_options, options, global_columns, global_column_ids, context,
	                              global_state);

	auto file_idx = reader_data.reader->file_list_idx.GetIndex();
	auto &deletes = file_list->GetDeleteFilesForFile(file_idx);
	if (deletes.empty()) {
		return;
	}
	auto filter = BuildUCDeleteFilter(context, file_list->GetDataFilePath(file_idx), deletes);
	if (filter) {
		reader_data.reader->deletion_filter = std::move(filter);
	}
}

} // namespace duckdb
