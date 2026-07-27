#pragma once

#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/table_function.hpp"
#include "uc_api.hpp"
#include "storage/unity_catalog.hpp"

namespace duckdb {

// Lazy MultiFileList driven by a UC scan plan result: inline file-scan-tasks up front, plan-task
// tokens exchanged on demand.
//
// delete_file_references index into the delete_files array of the response that carried them —
// they are response-scoped, not global — so they must be resolved as each task is ingested, while
// that array is still in scope.
class UCMultiFileList : public LazyMultiFileList {
public:
	UCMultiFileList(ClientContext &context, const UCScanPlanResult &plan, UCCredentials credentials,
	                string catalog_name, string schema_name, string table_name, string scan_plan_endpoint);

public:
	//! Only valid for an already-expanded index.
	const vector<UCScanDeleteFile> &GetDeleteFilesForFile(idx_t file_idx) const;
	//! The server's literal file_path, NOT BaseFileReader::GetFileName(): a positional delete file
	//! cross-references this string, and a normalized path would silently fail to match.
	const string &GetDataFilePath(idx_t file_idx) const;

protected:
	bool ExpandNextPath() const override;

private:
	//! `all_delete_files` must be the delete_files array of the response `task` came from.
	void AddFileScanTask(const UCScanPlanFileScanTask &task, const vector<UCScanDeleteFile> &all_delete_files) const;

private:
	mutable vector<string> remaining_tokens;
	//! Parallel to expanded_files (LazyMultiFileList); file_deletes[i] is expanded_files[i]'s deletes.
	mutable vector<vector<UCScanDeleteFile>> file_deletes;
	UCCredentials credentials;
	string catalog_name;
	string schema_name;
	string table_name;

	// TODO: remove once gating/automation to IRC API in place
	string scan_plan_endpoint;
};

// Feeds a pre-built file list to an otherwise stock parquet_scan, so parquet's own bind flow still
// does schema detection and projection. FinalizeBind hangs each file's DeleteFilter off the reader
// — the same seam delta uses (DeltaMultiFileReader::FinalizeBind).
class UCMultiFileReader : public MultiFileReader {
public:
	explicit UCMultiFileReader(shared_ptr<UCMultiFileList> list_p) : file_list(std::move(list_p)) {
	}

	shared_ptr<MultiFileList> CreateFileList(ClientContext &context, const vector<string> &paths,
	                                         const FileGlobInput &glob_input) override {
		return file_list;
	}

	void FinalizeBind(MultiFileReaderData &reader_data, const MultiFileOptions &file_options,
	                  const MultiFileReaderBindData &options, const vector<MultiFileColumnDefinition> &global_columns,
	                  const vector<ColumnIndex> &global_column_ids, ClientContext &context,
	                  optional_ptr<MultiFileReaderGlobalState> global_state) override;

private:
	shared_ptr<UCMultiFileList> file_list;
};

// Thread-local staging area: set immediately before parquet_fn.bind(), consumed by the factory.
extern thread_local shared_ptr<UCMultiFileList> tl_uc_file_list;

// Registered as get_multi_file_reader on a parquet_scan copy; consumes tl_uc_file_list.
unique_ptr<MultiFileReader> UCMultiFileReaderFactory(const TableFunction &function);

// The DeleteFilter for one data file, or nullptr if it has no deletes. Positional deletes are
// supported in both physical forms (parquet (file_path, pos) files and deletion-vector puffin
// blobs); equality deletes throw, for want of a field-id -> output-column mapping this extension
// never builds. See docs/sp/scan-plan-design.md §4.
unique_ptr<DeleteFilter> BuildUCDeleteFilter(ClientContext &context, const string &data_file_path,
                                             const vector<UCScanDeleteFile> &deletes);

} // namespace duckdb
