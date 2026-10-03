#include "modern_sqlite/diagnostics/storage_inspector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kPageSize = 512;

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

[[nodiscard]] std::filesystem::path FixtureDirectory() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "storage_diagnostics";
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return FixtureDirectory() / "sqlite-3.54.0-storage.db";
}

[[nodiscard]] std::filesystem::path AutoVacuumFixturePath() {
  return FixtureDirectory() / "sqlite-3.54.0-storage-autovacuum.db";
}

[[nodiscard]] std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to open diagnostics fixture");
  }
  const std::vector<char> characters{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes(characters.size());
  if (!characters.empty()) {
    std::memcpy(bytes.data(), characters.data(), characters.size());
  }
  return bytes;
}

void Write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + offset, sizeof(std::uint32_t)},
      value);
}

[[nodiscard]] ByteView PageAt(const std::vector<std::byte>& database, PageNumber page_number) {
  const std::size_t offset = static_cast<std::size_t>(page_number.value() - 1U) * kPageSize;
  return ByteView{database}.subspan(offset, kPageSize);
}

[[nodiscard]] std::size_t PageOffset(PageNumber page_number) {
  return static_cast<std::size_t>(page_number.value() - 1U) * kPageSize;
}

void WriteAscii(std::vector<std::byte>& bytes, std::size_t offset, std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    bytes[offset + index] = static_cast<std::byte>(text[index]);
  }
}

class TemporaryDatabase final {
 public:
  explicit TemporaryDatabase(ByteView bytes) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-storage-diagnostics-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1)) + ".db");
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("unable to create temporary diagnostics database");
    }
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
      throw std::runtime_error("unable to write temporary diagnostics database");
    }
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  // The error-code overloads are used for best-effort test cleanup.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~TemporaryDatabase() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(path_.string() + "-journal", error);
    std::filesystem::remove(path_.string() + "-wal", error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] const StorageObjectReport& FindObject(const StorageInspectionReport& report,
                                                    std::int64_t schema_rowid) {
  const auto found =
      std::ranges::find(report.objects, schema_rowid, &StorageObjectReport::schema_rowid);
  if (found == report.objects.end()) {
    throw std::runtime_error("missing schema object");
  }
  return *found;
}

[[nodiscard]] const StoragePageReport& FindPage(const StorageInspectionReport& report,
                                                PageNumber page_number) {
  const auto found = std::ranges::find(report.pages, page_number, &StoragePageReport::page_number);
  if (found == report.pages.end()) {
    throw std::runtime_error("missing page report");
  }
  return *found;
}

[[nodiscard]] bool HasIssue(const StorageInspectionReport& report, StorageIssueCode code) {
  return std::ranges::any_of(report.issues,
                             [code](const StorageIssue& issue) { return issue.code == code; });
}

[[nodiscard]] std::string BytesToString(const StorageText& text) {
  return std::string{reinterpret_cast<const char*>(text.bytes.data()), text.bytes.size()};
}

struct SchemaRowid {
  std::int64_t value;
};

struct FieldIndex {
  std::size_t value;
};

[[nodiscard]] std::size_t SchemaTextOffset(const std::vector<std::byte>& bytes,
                                           SchemaRowid schema_rowid, FieldIndex field_index) {
  const BtreePageGeometry geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{kPageSize}, ByteCount{kPageSize}));
  const BtreePageView page =
      TakeValue(BtreePageView::Parse(PageAt(bytes, PageNumber{8}), PageNumber{8}, geometry));
  for (std::size_t index = 0; index < page.cell_count(); ++index) {
    const BtreeCellView cell = TakeValue(page.cell(index));
    if (cell.rowid() != schema_rowid.value) {
      continue;
    }
    const RecordView record = TakeValue(RecordView::Parse(cell.local_payload()));
    const RecordFieldView field = TakeValue(record.field(field_index.value));
    const std::optional<Utf8View> decoded_text = field.text_value();
    if (!decoded_text.has_value()) {
      throw std::runtime_error("schema field is not text");
    }
    const Utf8View text = *decoded_text;
    return static_cast<std::size_t>(text.data() - reinterpret_cast<const char*>(bytes.data()));
  }
  throw std::runtime_error("missing schema row");
}

[[nodiscard]] std::size_t OverflowPointerOffset(const std::vector<std::byte>& bytes,
                                                PageNumber page_number, std::size_t cell_index) {
  const BtreePageGeometry geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{kPageSize}, ByteCount{kPageSize}));
  const BtreePageView page =
      TakeValue(BtreePageView::Parse(PageAt(bytes, page_number), page_number, geometry));
  const BtreeCellView cell = TakeValue(page.cell(cell_index));
  const ByteCount cell_offset = TakeValue(page.cell_offset(cell_index));
  if (!cell.first_overflow_page().has_value()) {
    throw std::runtime_error("cell has no overflow pointer");
  }
  return PageOffset(page_number) + cell_offset.value() + cell.encoded_size().value() - 4U;
}

struct CountingState {
  std::vector<std::byte> bytes;
  std::size_t reads = 0;
  std::size_t writes = 0;
  std::size_t truncates = 0;
  std::size_t syncs = 0;
  std::size_t deletes = 0;
  std::optional<std::size_t> bad_alloc_read;
  std::size_t unlocks = 0;
  std::size_t unlock_failures_remaining = 0;
};

class CountingFile final : public File {
 public:
  explicit CountingFile(std::shared_ptr<CountingState> state) : state_(std::move(state)) {}

 private:
  [[nodiscard]] Result<ByteCount> DoReadAt(MutableByteView destination,
                                           FileOffset offset) override {
    ++state_->reads;
    if (state_->bad_alloc_read == state_->reads) {
      throw std::bad_alloc{};
    }
    if (offset.value() >= state_->bytes.size()) {
      return ByteCount{0};
    }
    const auto begin = static_cast<std::size_t>(offset.value());
    const std::size_t count = std::min(destination.size(), state_->bytes.size() - begin);
    std::ranges::copy_n(state_->bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                        static_cast<std::ptrdiff_t>(count), destination.begin());
    return ByteCount{count};
  }

  [[nodiscard]] Status DoWriteAt(ByteView, FileOffset) override {
    ++state_->writes;
    return std::unexpected(Error::Create(ErrorCode::kReadOnly, "counting file is read-only"));
  }

  [[nodiscard]] Status DoTruncate(FileSize) override {
    ++state_->truncates;
    return std::unexpected(Error::Create(ErrorCode::kReadOnly, "counting file is read-only"));
  }

  [[nodiscard]] Status DoSync(SyncOptions) override {
    ++state_->syncs;
    return {};
  }

  [[nodiscard]] Result<FileSize> DoSize() override { return FileSize{state_->bytes.size()}; }
  [[nodiscard]] Status DoLock(DatabaseLock) override { return {}; }
  [[nodiscard]] Status DoUnlock(DatabaseLock) override {
    ++state_->unlocks;
    if (state_->unlock_failures_remaining > 0) {
      --state_->unlock_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected unlock failure"));
    }
    return {};
  }
  [[nodiscard]] Result<bool> DoHasReservedLock() override { return false; }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    return {
        .sector_size = ByteCount{4096},
        .device_characteristics = DeviceCharacteristics{},
    };
  }

  [[nodiscard]] Result<std::optional<MutableByteView>> DoMapSharedMemory(
      SharedMemoryRegionIndex, ByteCount, SharedMemoryMapMode) override {
    return std::optional<MutableByteView>{};
  }

  [[nodiscard]] Status DoLockSharedMemory(SharedMemoryLockRange, SharedMemoryLockOperation,
                                          SharedMemoryLockMode) override {
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override {}
  [[nodiscard]] Status DoUnmapSharedMemory(SharedMemoryUnmapMode) override { return {}; }

  std::shared_ptr<CountingState> state_;
};

class CountingVfs final : public Vfs {
 public:
  explicit CountingVfs(std::shared_ptr<CountingState> state) : state_(std::move(state)) {}

 private:
  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view>,
                                          FileOpenOptions options) override {
    return OpenedFile{
        .file = std::make_unique<CountingFile>(state_),
        .access = options.access,
    };
  }

  [[nodiscard]] Status DoDelete(std::string_view, DirectorySync) override {
    ++state_->deletes;
    return {};
  }

  [[nodiscard]] Result<bool> DoAccess(std::string_view, FileAccessQuery) override { return false; }

  [[nodiscard]] Result<std::string> DoFullPath(std::string_view path) override {
    return std::string{path};
  }

  [[nodiscard]] Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    std::ranges::fill(output, std::byte{0});
    return ByteCount{output.size()};
  }

  [[nodiscard]] Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) override {
    return duration;
  }

  [[nodiscard]] Result<WallClockTime> DoCurrentTime() override { return WallClockTime{}; }

  std::shared_ptr<CountingState> state_;
};

TEST(StorageInspector, ReportsPinnedDatabaseStructureDeterministically) {
  PosixVfs vfs;
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, FixturePath().string()));

  ASSERT_TRUE(report.header.has_value());
  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.root_discovery_complete);
  EXPECT_FALSE(report.issues_truncated);
  EXPECT_EQ(14U, report.header->snapshot_page_count);
  EXPECT_EQ(14U, report.header->header_page_count);
  EXPECT_EQ(ByteCount{512}, report.header->page_size);
  EXPECT_EQ(6U, report.header->freelist_page_count);
  EXPECT_EQ(RecordSchemaFormat::kFour, report.header->effective_schema_format);
  EXPECT_EQ(1U, report.header->effective_text_encoding);
  EXPECT_EQ(5U, report.objects.size());
  EXPECT_EQ(14U, report.pages.size());
  EXPECT_EQ(0U, report.issues.size());
  EXPECT_EQ(5U, report.summary.object_count);
  EXPECT_GT(report.summary.record_count, 0U);
  EXPECT_GT(report.summary.overflow_pages, 0U);
  EXPECT_EQ(1U, report.summary.freelist_trunk_pages);
  EXPECT_EQ(5U, report.summary.freelist_leaf_pages);

  const StorageObjectReport& alpha = FindObject(report, 1);
  EXPECT_EQ(StorageSchemaObjectKind::kTable, alpha.kind);
  EXPECT_EQ("alpha", BytesToString(alpha.name));
  EXPECT_EQ(2U, alpha.root_page);
  EXPECT_FALSE(alpha.sql_is_null);
  EXPECT_GT(alpha.sql_bytes.value(), 0U);

  const StorageObjectReport& without_rowid = FindObject(report, 3);
  EXPECT_EQ(StorageSchemaObjectKind::kTable, without_rowid.kind);
  EXPECT_EQ(7U, without_rowid.root_page);
  EXPECT_EQ(BtreePageType::kLeafIndex, FindPage(report, PageNumber{7}).btree_type);

  EXPECT_EQ(StoragePageRole::kBtree, FindPage(report, PageNumber{1}).role);
  EXPECT_EQ(StoragePageRole::kOverflow, FindPage(report, PageNumber{3}).role);
  EXPECT_FALSE(FindPage(report, PageNumber{9}).root_page.has_value());
  EXPECT_EQ(PageNumber{10}, FindPage(report, PageNumber{9}).parent_page);
  EXPECT_FALSE(FindPage(report, PageNumber{10}).root_page.has_value());
  EXPECT_FALSE(FindPage(report, PageNumber{10}).parent_page.has_value());

  const std::string first_json = TakeValue(RenderStorageInspectionJson(report));
  const std::string second_json = TakeValue(RenderStorageInspectionJson(report));
  EXPECT_EQ(first_json, second_json);
  EXPECT_NE(std::string::npos, first_json.find("\"format_version\":1"));
  EXPECT_NE(std::string::npos, first_json.find("\"root_discovery_complete\":true"));
}

TEST(StorageInspector, ValidatesAutoVacuumPointerMaps) {
  PosixVfs vfs;
  const StorageInspectionReport report =
      TakeValue(InspectDatabase(vfs, AutoVacuumFixturePath().string()));

  ASSERT_TRUE(report.header.has_value());
  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.header->auto_vacuum);
  EXPECT_EQ(StoragePageRole::kPointerMap, FindPage(report, PageNumber{2}).role);
  EXPECT_EQ(1U, report.summary.pointer_map_pages);
  EXPECT_EQ(4U, report.header->freelist_page_count);
}

TEST(StorageInspector, AcceptsEmptyDatabaseWithoutMutation) {
  const auto state = std::make_shared<CountingState>();
  CountingVfs vfs{state};
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, "empty.db"));

  EXPECT_TRUE(report.ok());
  EXPECT_FALSE(report.header.has_value());
  EXPECT_TRUE(report.pages.empty());
  EXPECT_TRUE(report.objects.empty());
  EXPECT_EQ(0U, state->writes);
  EXPECT_EQ(0U, state->truncates);
  EXPECT_EQ(0U, state->syncs);
  EXPECT_EQ(0U, state->deletes);
}

TEST(StorageInspector, UsesSQLiteCompatibleZeroHeaderValuesAndSnapshotPageCount) {
  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 44, 0);
    Write32(bytes, 56, 0);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    ASSERT_TRUE(report.header.has_value());
    EXPECT_EQ(0U, report.header->raw_schema_format);
    EXPECT_EQ(RecordSchemaFormat::kOne, report.header->effective_schema_format);
    EXPECT_EQ(0U, report.header->raw_text_encoding);
    EXPECT_EQ(1U, report.header->effective_text_encoding);
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 56, 4);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    ASSERT_TRUE(report.header.has_value());
    EXPECT_EQ(4U, report.header->raw_text_encoding);
    EXPECT_EQ(1U, report.header->effective_text_encoding);
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    bytes.resize(bytes.size() + kPageSize, std::byte{0});
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    ASSERT_TRUE(report.header.has_value());
    EXPECT_EQ(14U, report.header->snapshot_page_count);
    EXPECT_EQ(14U, report.pages.size());
  }
}

TEST(StorageInspector, RejectsUnsupportedEffectiveHeaderPolicy) {
  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 44, 5);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const auto report = InspectDatabase(vfs, database.path().string());
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(ErrorCode::kNotDatabase, report.error().code());
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 56, 2);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const auto report = InspectDatabase(vfs, database.path().string());
    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, report.error().code());
  }
}

TEST(StorageInspector, PreservesIllFormedSchemaTextAsHex) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  const std::size_t byte_offset = SchemaTextOffset(bytes, SchemaRowid{1}, FieldIndex{1});
  bytes[byte_offset] = std::byte{0x80};

  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, database.path().string()));
  EXPECT_TRUE(report.ok());
  const StorageObjectReport& object = FindObject(report, 1);
  EXPECT_FALSE(object.name.valid_utf8);
  const std::string json = TakeValue(RenderStorageInspectionJson(report));
  EXPECT_NE(std::string::npos, json.find("\"encoding\":\"hex\",\"value\":\"806c706861\""));
}

TEST(StorageInspector, ValidatesSchemaRootKindsAndSuppressesIncompleteReachability) {
  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    const std::size_t type_offset = SchemaTextOffset(bytes, SchemaRowid{1}, FieldIndex{0});
    WriteAscii(bytes, type_offset, "index");
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_FALSE(report.ok());
    EXPECT_FALSE(report.root_discovery_complete);
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kTreeKindMismatch));
    EXPECT_FALSE(HasIssue(report, StorageIssueCode::kUnusedPage));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    const std::size_t type_offset = SchemaTextOffset(bytes, SchemaRowid{1}, FieldIndex{0});
    WriteAscii(bytes, type_offset, "other");
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_FALSE(report.ok());
    EXPECT_FALSE(report.root_discovery_complete);
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kSchemaRecord));
    EXPECT_FALSE(HasIssue(report, StorageIssueCode::kUnusedPage));
  }
}

TEST(StorageInspector, AcceptsSQLiteSchemaBlobTextCoercion) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  const BtreePageGeometry geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{kPageSize}, ByteCount{kPageSize}));
  const BtreePageView page =
      TakeValue(BtreePageView::Parse(PageAt(bytes, PageNumber{8}), PageNumber{8}, geometry));
  const BtreeCellView cell = TakeValue(page.cell(0));
  const std::size_t payload_offset = static_cast<std::size_t>(
      cell.local_payload().data() - reinterpret_cast<const std::byte*>(bytes.data()));
  ASSERT_EQ(std::byte{23}, bytes[payload_offset + 1U]);
  bytes[payload_offset + 1U] = std::byte{22};

  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, database.path().string()));

  EXPECT_TRUE(report.ok());
  EXPECT_EQ(StorageSchemaObjectKind::kTable, FindObject(report, 1).kind);
  EXPECT_EQ("table", BytesToString(FindObject(report, 1).declared_type));
}

TEST(StorageInspector, MarksSchemaDiscoveryIncompleteWhenCellsCannotBeDecoded) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  const std::size_t pointer_array = PageOffset(PageNumber{8}) + 8U;
  bytes[pointer_array] = std::byte{0};
  bytes[pointer_array + 1U] = std::byte{0};

  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, database.path().string()));

  EXPECT_FALSE(report.root_discovery_complete);
  EXPECT_TRUE(HasIssue(report, StorageIssueCode::kPageDecode));
  EXPECT_FALSE(HasIssue(report, StorageIssueCode::kUnusedPage));
}

TEST(StorageInspector, RejectsZeroLengthRecordPayloads) {
  std::vector<std::byte> bytes = ReadBytes(FixturePath());
  const BtreePageGeometry geometry =
      TakeValue(BtreePageGeometry::Create(ByteCount{kPageSize}, ByteCount{kPageSize}));
  const BtreePageView page =
      TakeValue(BtreePageView::Parse(PageAt(bytes, PageNumber{8}), PageNumber{8}, geometry));
  const ByteCount cell_offset = TakeValue(page.cell_offset(0));
  bytes[PageOffset(PageNumber{8}) + cell_offset.value()] = std::byte{0};

  const TemporaryDatabase database(bytes);
  PosixVfs vfs;
  const StorageInspectionReport report = TakeValue(InspectDatabase(vfs, database.path().string()));

  EXPECT_FALSE(report.root_discovery_complete);
  EXPECT_TRUE(HasIssue(report, StorageIssueCode::kRecordDecode));
  EXPECT_FALSE(HasIssue(report, StorageIssueCode::kUnusedPage));
}

TEST(StorageInspector, DetectsCellOverlapAndFragmentationMismatch) {
  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    const std::size_t pointer_array = PageOffset(PageNumber{2}) + 8U;
    bytes[pointer_array + 2U] = bytes[pointer_array];
    bytes[pointer_array + 3U] = bytes[pointer_array + 1U];
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kPageCoverage));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    bytes[PageOffset(PageNumber{2}) + 7U] = std::byte{1};
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kPageCoverage));
  }
}

TEST(StorageInspector, ReportsFreelistOverflowPointerMapAndUnusedPageCorruption) {
  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 36, 5);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kFreelist));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, (3U - 1U) * kPageSize, 3);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kOverflowChain));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, PageOffset(PageNumber{5}), 0);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kOverflowChain));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, OverflowPointerOffset(bytes, PageNumber{4}, 2), 15);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kInvalidPageReference));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, OverflowPointerOffset(bytes, PageNumber{4}, 2), 3);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kDuplicatePageReference));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(AutoVacuumFixturePath());
    bytes[(2U - 1U) * kPageSize] = std::byte{2};
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report =
        TakeValue(InspectDatabase(vfs, database.path().string()));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasIssue(report, StorageIssueCode::kPointerMap));
  }

  {
    std::vector<std::byte> bytes = ReadBytes(FixturePath());
    Write32(bytes, 32, 0);
    Write32(bytes, 36, 0);
    const TemporaryDatabase database(bytes);
    PosixVfs vfs;
    const StorageInspectionReport report = TakeValue(
        InspectDatabase(vfs, database.path().string(), StorageInspectionOptions{.max_issues = 1}));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(report.issues_truncated);
    ASSERT_EQ(1U, report.issues.size());
    EXPECT_EQ(StorageIssueCode::kUnusedPage, report.issues.front().code);
  }
}

TEST(StorageInspector, ReturnsUnlockFailureInsteadOfCompletedInspection) {
  const auto state = std::make_shared<CountingState>();
  state->bytes = ReadBytes(FixturePath());
  state->unlock_failures_remaining = 1;
  CountingVfs vfs{state};

  const auto report = InspectDatabase(vfs, "database.db");

  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(ErrorCode::kIo, report.error().code());
}

TEST(StorageInspector, CleansUpAfterAllocationFailureAndPrioritizesUnlockFailure) {
  {
    const auto state = std::make_shared<CountingState>();
    state->bytes = ReadBytes(FixturePath());
    state->bad_alloc_read = 2;
    CountingVfs vfs{state};

    const auto report = InspectDatabase(vfs, "database.db");

    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(ErrorCode::kOutOfMemory, report.error().code());
    EXPECT_EQ(1U, state->unlocks);
  }

  {
    const auto state = std::make_shared<CountingState>();
    state->bytes = ReadBytes(FixturePath());
    state->bad_alloc_read = 2;
    state->unlock_failures_remaining = 1;
    CountingVfs vfs{state};

    const auto report = InspectDatabase(vfs, "database.db");

    ASSERT_FALSE(report.has_value());
    EXPECT_EQ(ErrorCode::kIo, report.error().code());
    EXPECT_EQ(1U, state->unlocks);
  }
}

TEST(StorageInspector, RendersStablePathFreeFatalErrors) {
  EXPECT_EQ("{\"format_version\":1,\"ok\":false,\"fatal_error\":{\"code\":\"cannot_open\"}}",
            TakeValue(RenderStorageInspectionErrorJson(ErrorCode::kCannotOpen)));
}

}  // namespace
}  // namespace modern_sqlite
