#include "uc_multi_file_list.hpp"
#include "uc_position_delete_filter.hpp"
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
	// Must return "added a file", not "tokens remain": a false latches all_files_expanded, and
	// MultiFileList::Scan reads the resulting empty OpenFileInfo as end-of-list.
	while (!remaining_tokens.empty()) {
		string token = std::move(remaining_tokens.front());
		remaining_tokens.erase(remaining_tokens.begin());

		auto &ctx = *context.get_mutable();
		auto result =
		    UCAPI::FetchScanTasks(ctx, catalog_name, schema_name, table_name, token, credentials, scan_plan_endpoint);
		// fetchScanTasks may itself hand back more plan-tasks.
		for (auto &new_token : result.plan_tasks) {
			remaining_tokens.push_back(std::move(new_token));
		}
		if (result.file_scan_tasks.empty()) {
			continue; // this token carried no files -- drain the next rather than reporting "done"
		}
		for (auto &task : result.file_scan_tasks) {
			AddFileScanTask(task, result.delete_files);
		}
		return true;
	}
	return false;
}

// The only channel into get_multi_file_reader, which takes just a TableFunction: set before
// parquet_fn.bind(), moved out by the factory on the same thread.
thread_local shared_ptr<UCMultiFileList> tl_uc_file_list;

unique_ptr<MultiFileReader> UCMultiFileReaderFactory(const TableFunction &) {
	D_ASSERT(tl_uc_file_list);
	return make_uniq<UCMultiFileReader>(std::move(tl_uc_file_list));
}

// ---------------------------------------------------------------------------
// Delete-file application
// ---------------------------------------------------------------------------

static TableFunction GetParquetScanFunction(ClientContext &context) {
	auto &sys_cat = Catalog::GetSystemCatalog(context);
	auto &parquet_entry = sys_cat.GetEntry<TableFunctionCatalogEntry>(
	    context, QualifiedName({Identifier(DEFAULT_SCHEMA)}, Identifier("parquet_scan")));
	return parquet_entry.functions.GetFunctionByArguments(context, {LogicalType::VARCHAR});
}

// Fully materializing is fine here: delete files run KB to low MB.
static void ScanParquetFile(ClientContext &context, const string &path,
                            const std::function<void(DataChunk &)> &consume) {
	auto parquet_fn = GetParquetScanFunction(context);

	vector<Value> inputs = {Value(path)};
	named_parameter_map_t named_params;
	vector<LogicalType> input_table_types;
	vector<Identifier> input_table_names;
	TableFunctionRef dummy_ref;
	vector<LogicalType> return_types;
	vector<Identifier> return_names;
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
	// Which delete sub-path ran is otherwise invisible; no test asserts this today.
	UC_LOG_DEBUG(context, "api-irc.PositionalDelete file=%s data_file=%s", delete_file.file_path, data_file_path);
}

// Iceberg v3 deletion vector: a `deletion-vector-v1` puffin blob at a byte range in the file.
static void ScanDeletionVectorFile(ClientContext &context, const UCScanDeleteFile &delete_file,
                                   unordered_set<int64_t> &out) {
	// The spec requires content-size-in-bytes wherever content-offset is present, so a missing one
	// is a malformed response — say so rather than failing later inside NumericCast.
	if (delete_file.content_size_in_bytes < 0) {
		throw IOException("scan-plan: deletion-vector delete file '%s' has content-offset %d but no "
		                  "content-size-in-bytes — server response is malformed",
		                  delete_file.file_path, delete_file.content_offset);
	}
	// Swallowing here would mean wrong rows, so rethrow — but name the delete file first, since
	// FromBlob has no context to log from.
	try {
		auto &fs = FileSystem::GetFileSystem(context);
		auto handle = fs.OpenFile(delete_file.file_path, FileOpenFlags::FILE_FLAGS_READ);
		auto length = NumericCast<idx_t>(delete_file.content_size_in_bytes);
		auto buffer = make_unsafe_uniq_array<data_t>(length);
		handle->Read(buffer.get(), length, NumericCast<idx_t>(delete_file.content_offset));

		auto blob = UCDeletionVectorData::FromBlob(buffer.get(), length, delete_file.file_path);
		set<idx_t> positions;
		blob->ToSet(positions);
		UC_LOG_DEBUG(context, "api-irc.DeletionVector file=%s offset=%lld size=%lld positions=%zu",
		             delete_file.file_path, (long long)delete_file.content_offset,
		             (long long)delete_file.content_size_in_bytes, positions.size());
		for (auto pos : positions) {
			out.insert(NumericCast<int64_t>(pos));
		}
	} catch (const std::exception &e) {
		UC_LOG_WARNING(context, "api-irc.DeletionVector file=%s offset=%lld size=%lld failed: %s",
		               delete_file.file_path, (long long)delete_file.content_offset,
		               (long long)delete_file.content_size_in_bytes, e.what());
		throw;
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
