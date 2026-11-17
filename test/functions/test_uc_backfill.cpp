// Server-free unit coverage of CopyStagedCommitToDeltaLog (src/storage/uc_table_set.cpp): both the non-Azure
// NULL_IF_EXISTS branch (against LocalFileSystem) and the Azure branch (against an in-memory FileSystem mirroring
// duckdb-azure's OpenFile / LoadFileInfo / LoadRemoteFileInfo semantics).
//
// Runner: the standalone `unittest_cpp` Catch executable (tag "[uc][backfill]").

#include "catch.hpp"

#include "storage/uc_table_set.hpp"
#include "uc_utils.hpp"

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct BackfillTestContext {
	DuckDB db;
	Connection con;
	BackfillTestContext() : db(nullptr), con(db) {
	}
	ClientContext &Get() {
		return *con.context;
	}
};

string ReadLocalFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	auto size = NumericCast<size_t>(handle->GetFileSize());
	string out(size, '\0');
	if (size > 0) {
		handle->Read((void *)out.data(), NumericCast<int64_t>(size));
	}
	return out;
}

// In-memory Azure filesystem mirroring duckdb-azure's OpenFile / LoadFileInfo / LoadRemoteFileInfo behavior:
//   - On missing write target: creates a 0-byte entry during OpenFile, then if ReturnNullIfExists() is set
//     returns nullptr (leaving the 0-byte entry behind unless NULL_IF_EXISTS was omitted).
//   - On existing write target with ExclusiveCreate(): throws `exists_error` before checking ReturnNullIfExists().
//   - On existing write target with OverwriteExistingFile() (FILE_CREATE_NEW): truncates to 0 and commits on Close().
//   - Writes stage into a buffer and publish to `files[path]` only on Close().
struct MockAzureStorage {
	unordered_map<string, string> files;
	unordered_map<string, timestamp_t> mtimes;
	string exists_error =
	    "AzureDfsStorageFileSystem will not open file: '%s', ExclusiveCreate specified while file already exists.";
	string open_write_error;
	bool throw_foreign_rtti = false;
	bool fail_on_read = false;
	bool fail_on_close = false;
};

class MockAzureFileHandle : public FileHandle {
public:
	MockAzureFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, MockAzureStorage &storage_p)
	    : FileHandle(fs, path, flags), storage(storage_p) {
	}

	void Close() override {
		if (flags.OpenForWriting() && !closed) {
			if (storage.fail_on_close) {
				throw IOException("MockAzureFileSystem simulated flush failure");
			}
			storage.files[path] = staged;
			closed = true;
		}
	}

	MockAzureStorage &storage;
	string staged;
	idx_t read_offset = 0;
	bool closed = false;
};

class MockAzureFileSystem : public FileSystem {
public:
	explicit MockAzureFileSystem(MockAzureStorage &storage_p) : storage(storage_p) {
	}

	string GetName() const override {
		return "MockAzureFileSystem";
	}

	bool CanHandleFile(const string &fpath) override {
		return IsAzureLocation(fpath);
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		auto it = storage.files.find(path);
		if (flags.OpenForReading()) {
			if (it == storage.files.end()) {
				if (flags.ReturnNullIfNotExists()) {
					return nullptr;
				}
				throw IOException("MockAzureFileSystem 404: %s", path);
			}
			return make_uniq<MockAzureFileHandle>(*this, path, flags, storage);
		}
		if (!storage.open_write_error.empty()) {
			throw IOException("%s", storage.open_write_error);
		}
		if (it != storage.files.end()) {
			if (flags.ExclusiveCreate()) {
				if (storage.throw_foreign_rtti) {
					throw std::runtime_error(IOException("%s", storage.exists_error).what());
				}
				throw IOException("%s", storage.exists_error);
			}
			if (flags.OverwriteExistingFile()) {
				storage.files[path] = "";
				storage.mtimes[path] = Timestamp::GetCurrentTimestamp();
			}
		} else if (flags.CreateFileIfNotExists() || flags.OverwriteExistingFile()) {
			storage.files[path] = "";
			storage.mtimes[path] = Timestamp::GetCurrentTimestamp();
		}
		if (flags.ReturnNullIfExists()) {
			return nullptr;
		}
		return make_uniq<MockAzureFileHandle>(*this, path, flags, storage);
	}

	int64_t GetFileSize(FileHandle &handle) override {
		auto &afh = handle.Cast<MockAzureFileHandle>();
		return NumericCast<int64_t>(storage.files.at(afh.path).size());
	}

	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		auto &afh = handle.Cast<MockAzureFileHandle>();
		auto it = storage.mtimes.find(afh.path);
		return it == storage.mtimes.end() ? timestamp_t(0) : it->second;
	}

	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &afh = handle.Cast<MockAzureFileHandle>();
		if (storage.fail_on_read) {
			throw IOException("MockAzureFileSystem simulated read failure");
		}
		const auto &content = storage.files.at(afh.path);
		idx_t remaining = content.size() - afh.read_offset;
		idx_t to_read = MinValue<idx_t>(remaining, NumericCast<idx_t>(nr_bytes));
		if (to_read > 0) {
			memcpy(buffer, content.data() + afh.read_offset, to_read);
			afh.read_offset += to_read;
		}
		return NumericCast<int64_t>(to_read);
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &afh = handle.Cast<MockAzureFileHandle>();
		afh.staged.append(static_cast<const char *>(buffer), NumericCast<size_t>(nr_bytes));
		return nr_bytes;
	}

private:
	MockAzureStorage &storage;
};

} // namespace

TEST_CASE("CopyStagedCommitToDeltaLog non-Azure path copies missing commits and skips existing ones",
          "[uc][backfill]") {
	BackfillTestContext ctx;
	auto &fs = FileSystem::GetFileSystem(ctx.Get());
	string dir = TestDirectoryPath() + "/uc_backfill_local";
	fs.CreateDirectoriesRecursive(dir);

	const string src = dir + "/staged_0001.json";
	const string dst = dir + "/00000000000000000001.json";
	const string commit_json = "{\"commitInfo\":{\"timestamp\":1}}\n{\"add\":{\"path\":\"part-0.parquet\"}}\n";
	fs.TryRemoveFile(src);
	fs.TryRemoveFile(dst);

	// Missing src throws IOException without creating dst.
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst), IOException);
	REQUIRE_FALSE(fs.FileExists(dst));

	{
		auto f = fs.OpenFile(src, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);
		f->Write((void *)commit_json.data(), NumericCast<int64_t>(commit_json.size()));
		f->Close();
	}

	// First backfill creates dst and returns true.
	REQUIRE(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst));
	REQUIRE(ReadLocalFile(fs, dst) == commit_json);

	// Second backfill finds dst via NULL_IF_EXISTS, leaves its bytes untouched, and returns false.
	const string marker = "{\"commitInfo\":{\"timestamp\":99}}\n";
	{
		auto f = fs.OpenFile(dst, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);
		f->Write((void *)marker.data(), NumericCast<int64_t>(marker.size()));
		f->Close();
	}
	REQUIRE_FALSE(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst));
	REQUIRE(ReadLocalFile(fs, dst) == marker);
}

TEST_CASE("CopyStagedCommitToDeltaLog handles Azure schemes, existing commits, and 0-byte placeholders",
          "[uc][backfill]") {
	MockAzureStorage storage;
	BackfillTestContext ctx;
	FileSystem::GetFileSystem(*ctx.db.instance).RegisterSubSystem(make_uniq<MockAzureFileSystem>(storage));

	const string src =
	    "abfss://c@account.dfs.core.windows.net/t/_delta_log/_staged_commits/00000000000000000001.uuid.json";
	const string commit_json = "{\"commitInfo\":{\"timestamp\":1}}\n{\"add\":{\"path\":\"part-0.parquet\"}}\n";
	storage.files[src] = commit_json;

	// Missing dst across all four Azure schemes: writes full commit JSON (never 0 bytes) and returns true.
	for (auto *dst : {"abfss://c@account.dfs.core.windows.net/t/_delta_log/00000000000000000001.json",
	                  "abfs://c@account.dfs.core.windows.net/t/_delta_log/00000000000000000001.json",
	                  "az://c/t/_delta_log/00000000000000000001.json",
	                  "azure://account.blob.core.windows.net/c/t/_delta_log/00000000000000000001.json"}) {
		REQUIRE(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst));
		REQUIRE(storage.files.at(dst) == commit_json);
	}

	// Non-empty existing dst: DFS and Blob ExclusiveCreate messages, conditional-create codes, and foreign RTTI all
	// return false without clobbering dst.
	const string dst = "abfss://c@account.dfs.core.windows.net/t/_delta_log/00000000000000000001.json";
	storage.files[dst] = "already-backfilled";
	for (auto *err :
	     {"AzureDfsStorageFileSystem will not open file: '%s', ExclusiveCreate specified while file already exists.",
	      "AzureBlobStorageFileSystem will not open file: '%s', ExclusiveCreate specified while file already exists.",
	      "AzureStorageFileSystem open file 'dst' failed with code 'PathAlreadyExists', Reason Phrase: 'Conflict'",
	      "AzureStorageFileSystem open file 'dst' failed with code 'BlobAlreadyExists', Reason Phrase: 'Conflict'"}) {
		storage.exists_error = err;
		REQUIRE_FALSE(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst));
		REQUIRE(storage.files.at(dst) == "already-backfilled");
	}
	storage.throw_foreign_rtti = true;
	REQUIRE_FALSE(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst));
	REQUIRE(storage.files.at(dst) == "already-backfilled");
	storage.throw_foreign_rtti = false;

	// A 0-byte file from an interrupted/in-flight OpenFile is NEVER accepted as backfilled: while fresh (<60s) it
	// throws (avoiding truncating an in-flight peer or advancing the watermark); once stale (>60s) retry heals it.
	const string interrupted = "abfss://c@account.dfs.core.windows.net/t/_delta_log/00000000000000000002.json";
	storage.fail_on_close = true;
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, interrupted), IOException);
	REQUIRE(storage.files.at(interrupted).empty());
	storage.fail_on_close = false;
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, interrupted), IOException);
	storage.mtimes[interrupted] = timestamp_t(Timestamp::GetCurrentTimestamp().value - 120LL * 1000000LL);
	REQUIRE(CopyStagedCommitToDeltaLog(ctx.Get(), src, interrupted));
	REQUIRE(storage.files.at(interrupted) == commit_json);

	// Source read failure happens before dst is opened: neither creates a missing dst nor deletes a peer's dst.
	const string untouched = "abfss://c@account.dfs.core.windows.net/t/_delta_log/00000000000000000003.json";
	storage.fail_on_read = true;
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, untouched), IOException);
	REQUIRE(storage.files.find(untouched) == storage.files.end());
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, dst), IOException);
	REQUIRE(storage.files.at(dst) == "already-backfilled");
	storage.fail_on_read = false;

	// A table path containing 'PathAlreadyExists' or 'BlobAlreadyExists' on an auth failure must rethrow, not match.
	const string tricky_dst =
	    "abfss://c@account.dfs.core.windows.net/PathAlreadyExists_tbl/_delta_log/00000000000000000004.json";
	storage.open_write_error = StringUtil::Format(
	    "AzureStorageFileSystem open file '%s' failed with code 'AuthorizationPermissionMismatch'", tricky_dst);
	REQUIRE_THROWS_AS(CopyStagedCommitToDeltaLog(ctx.Get(), src, tricky_dst), IOException);
	REQUIRE(storage.files.find(tricky_dst) == storage.files.end());
}
