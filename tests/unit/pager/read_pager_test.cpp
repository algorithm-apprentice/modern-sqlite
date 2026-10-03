#include "modern_sqlite/pager/read_pager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/cache/page_cache.hpp"

namespace modern_sqlite {
namespace {

static_assert(!std::is_copy_constructible_v<ReadPager>);
static_assert(!std::is_move_constructible_v<ReadPager>);
static_assert(!std::is_copy_constructible_v<ReadPagePin>);
static_assert(std::is_nothrow_move_constructible_v<ReadPagePin>);

constexpr std::string_view kInputPath = "fixture.db";
constexpr std::string_view kCanonicalPath = "/canonical/fixture.db";
constexpr std::uint32_t kApplicationId = 1'297'305'932;
constexpr std::uint32_t kSqliteVersion = 3'054'000;
constexpr std::uint64_t kPendingByte = 0x40000000ULL;

struct DatabaseImageOptions {
  std::size_t page_size = 4096;
  std::uint32_t physical_pages = 2;
  std::uint32_t header_pages = 2;
  std::uint32_t change_counter = 0x01020304U;
  std::uint32_t version_valid_for = 0x01020304U;
  std::uint8_t write_version = 1;
  std::uint8_t read_version = 1;
  std::uint8_t reserved_bytes = 0;
};

void WriteBigEndian32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + offset, 4}, value);
}

[[nodiscard]] std::vector<std::byte> MakeDatabaseImage(const DatabaseImageOptions& options = {}) {
  std::vector<std::byte> bytes(options.page_size * options.physical_pages);
  constexpr std::array<std::byte, 16> kMagic{
      std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
      std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
      std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
  };
  std::ranges::copy(kMagic, bytes.begin());
  if (options.page_size == 65536) {
    bytes[16] = std::byte{0x00};
    bytes[17] = std::byte{0x01};
  } else {
    bytes[16] = static_cast<std::byte>((options.page_size >> 8U) & 0xffU);
    bytes[17] = static_cast<std::byte>(options.page_size & 0xffU);
  }
  bytes[18] = static_cast<std::byte>(options.write_version);
  bytes[19] = static_cast<std::byte>(options.read_version);
  bytes[20] = static_cast<std::byte>(options.reserved_bytes);
  bytes[21] = std::byte{64};
  bytes[22] = std::byte{32};
  bytes[23] = std::byte{32};
  WriteBigEndian32(bytes, 24, options.change_counter);
  WriteBigEndian32(bytes, 28, options.header_pages);
  WriteBigEndian32(bytes, 32, 5);
  WriteBigEndian32(bytes, 36, 6);
  WriteBigEndian32(bytes, 40, 0x11121314U);
  WriteBigEndian32(bytes, 44, 4);
  WriteBigEndian32(bytes, 48, std::bit_cast<std::uint32_t>(std::int32_t{-2000}));
  WriteBigEndian32(bytes, 52, 7);
  WriteBigEndian32(bytes, 56, 1);
  WriteBigEndian32(bytes, 60, 8);
  WriteBigEndian32(bytes, 64, 1);
  WriteBigEndian32(bytes, 68, kApplicationId);
  WriteBigEndian32(bytes, 92, options.version_valid_for);
  WriteBigEndian32(bytes, 96, kSqliteVersion);

  if (options.page_size > 100) {
    bytes[100] = std::byte{0x0d};
  }
  if (options.physical_pages > 1) {
    bytes[options.page_size] = std::byte{0x0d};
    bytes[options.page_size + 1] = std::byte{0x42};
  }
  return bytes;
}

struct ReadRequest {
  std::uint64_t offset;
  std::size_t size;
};

struct FakeFileState {
  std::vector<std::byte> bytes;
  std::optional<std::uint64_t> size_override;
  std::vector<ReadRequest> reads;
  std::vector<DatabaseLock> lock_requests;
  std::vector<DatabaseLock> unlock_requests;
  DatabaseLock current_lock = DatabaseLock::kNone;
  bool reserved_lock = false;
  std::optional<std::uint64_t> failing_read_offset;
  int read_failures_remaining = 0;
  int lock_failures_remaining = 0;
  int unlock_failures_remaining = 0;
  int size_failures_remaining = 0;
  int reserved_check_failures_remaining = 0;
};

class FakeFile final : public File {
 public:
  explicit FakeFile(std::shared_ptr<FakeFileState> state) : state_(std::move(state)) {}

 private:
  Result<ByteCount> DoReadAt(MutableByteView destination, FileOffset offset) override {
    state_->reads.push_back(ReadRequest{.offset = offset.value(), .size = destination.size()});
    if (state_->read_failures_remaining > 0 && state_->failing_read_offset.has_value() &&
        *state_->failing_read_offset == offset.value()) {
      --state_->read_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected read failure"));
    }

    const std::uint64_t logical_size =
        state_->size_override.value_or(static_cast<std::uint64_t>(state_->bytes.size()));
    if (offset.value() >= logical_size || offset.value() >= state_->bytes.size()) {
      return ByteCount{0};
    }
    const auto start = static_cast<std::size_t>(offset.value());
    const std::size_t logical_available = static_cast<std::size_t>(
        std::min<std::uint64_t>(logical_size - offset.value(), destination.size()));
    const std::size_t stored_available =
        start < state_->bytes.size() ? std::min(destination.size(), state_->bytes.size() - start)
                                     : 0;
    const std::size_t count = std::min(logical_available, stored_available);
    std::ranges::copy_n(state_->bytes.begin() + static_cast<std::ptrdiff_t>(start),
                        static_cast<std::ptrdiff_t>(count), destination.begin());
    return ByteCount{count};
  }

  Status DoWriteAt(ByteView, FileOffset) override {
    return std::unexpected(Error::Create(ErrorCode::kReadOnly, "fake file is read-only"));
  }

  Status DoTruncate(FileSize) override {
    return std::unexpected(Error::Create(ErrorCode::kReadOnly, "fake file is read-only"));
  }

  Status DoSync(SyncOptions) override { return {}; }

  Result<FileSize> DoSize() override {
    if (state_->size_failures_remaining > 0) {
      --state_->size_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected size failure"));
    }
    return FileSize{
        state_->size_override.value_or(static_cast<std::uint64_t>(state_->bytes.size()))};
  }

  Status DoLock(DatabaseLock lock) override {
    state_->lock_requests.push_back(lock);
    if (state_->lock_failures_remaining > 0) {
      --state_->lock_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kBusy, "injected lock conflict"));
    }
    state_->current_lock = lock;
    return {};
  }

  Status DoUnlock(DatabaseLock lock) override {
    state_->unlock_requests.push_back(lock);
    if (state_->unlock_failures_remaining > 0) {
      --state_->unlock_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected unlock failure"));
    }
    state_->current_lock = lock;
    return {};
  }

  Result<bool> DoHasReservedLock() override {
    if (state_->reserved_check_failures_remaining > 0) {
      --state_->reserved_check_failures_remaining;
      return std::unexpected(Error::Create(ErrorCode::kIo, "injected reserved-lock failure"));
    }
    return state_->reserved_lock;
  }

  [[nodiscard]] FileProperties DoProperties() const noexcept override {
    return FileProperties{
        .sector_size = ByteCount{4096},
        .device_characteristics = {},
    };
  }

  Result<std::optional<MutableByteView>> DoMapSharedMemory(SharedMemoryRegionIndex, ByteCount,
                                                           SharedMemoryMapMode) override {
    return std::optional<MutableByteView>{};
  }

  Status DoLockSharedMemory(SharedMemoryLockRange, SharedMemoryLockOperation,
                            SharedMemoryLockMode) override {
    return {};
  }

  void DoSharedMemoryBarrier() noexcept override {}

  Status DoUnmapSharedMemory(SharedMemoryUnmapMode) override { return {}; }

  std::shared_ptr<FakeFileState> state_;
};

struct OpenRequest {
  std::string path;
  FileOpenOptions options;
};

struct AccessRequest {
  std::string path;
  FileAccessQuery query;
};

struct FakeVfsState {
  std::string canonical_path{std::string{kCanonicalPath}};
  std::map<std::string, std::shared_ptr<FakeFileState>, std::less<>> files;
  std::vector<std::string> full_path_requests;
  std::vector<OpenRequest> open_requests;
  std::vector<AccessRequest> access_requests;
};

class FakeVfs final : public Vfs {
 public:
  explicit FakeVfs(std::shared_ptr<FakeVfsState> state) : state_(std::move(state)) {}

 private:
  Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                            FileOpenOptions options) override {
    const std::string owned_path{path.value_or(std::string_view{})};
    state_->open_requests.push_back(OpenRequest{
        .path = owned_path,
        .options = options,
    });
    const auto iterator = state_->files.find(owned_path);
    if (iterator == state_->files.end()) {
      return std::unexpected(Error::Create(ErrorCode::kCannotOpen, "fake path is missing"));
    }
    return OpenedFile{
        .file = std::make_unique<FakeFile>(iterator->second),
        .access = FileAccessMode::kReadOnly,
    };
  }

  Status DoDelete(std::string_view, DirectorySync) override {
    return std::unexpected(Error::Create(ErrorCode::kReadOnly, "fake VFS is read-only"));
  }

  Result<bool> DoAccess(std::string_view path, FileAccessQuery query) override {
    state_->access_requests.push_back(AccessRequest{
        .path = std::string{path},
        .query = query,
    });
    return state_->files.contains(path);
  }

  Result<std::string> DoFullPath(std::string_view path) override {
    state_->full_path_requests.emplace_back(path);
    return state_->canonical_path;
  }

  Result<ByteCount> DoRandomBytes(MutableByteView output) override {
    std::ranges::fill(output, std::byte{0});
    return ByteCount{output.size()};
  }

  Result<std::chrono::microseconds> DoSleepFor(std::chrono::microseconds duration) override {
    return duration;
  }

  Result<WallClockTime> DoCurrentTime() override { return WallClockTime{}; }

  std::shared_ptr<FakeVfsState> state_;
};

class FakeEnvironment final {
 public:
  explicit FakeEnvironment(std::vector<std::byte> main_bytes)
      : state(std::make_shared<FakeVfsState>()),
        main_file(std::make_shared<FakeFileState>()),
        vfs(state) {
    main_file->bytes = std::move(main_bytes);
    state->files.emplace(state->canonical_path, main_file);
  }

  void AddSidecar(std::string_view suffix, std::vector<std::byte> bytes) {
    auto sidecar = std::make_shared<FakeFileState>();
    sidecar->bytes = std::move(bytes);
    state->files.insert_or_assign(state->canonical_path + std::string{suffix}, std::move(sidecar));
  }

  std::shared_ptr<FakeVfsState> state;
  std::shared_ptr<FakeFileState> main_file;
  FakeVfs vfs;
};

[[nodiscard]] std::size_t CountReads(const FakeFileState& file, std::uint64_t offset,
                                     std::size_t size) {
  return static_cast<std::size_t>(
      std::ranges::count_if(file.reads, [offset, size](const ReadRequest& request) {
        return request.offset == offset && request.size == size;
      }));
}

TEST(DatabaseHeader, ParsesAllStandardFieldsAndThe65536Encoding) {
  const DatabaseImageOptions options{
      .page_size = 65536,
      .physical_pages = 2,
      .header_pages = 2,
      .write_version = 3,
      .reserved_bytes = 16,
  };
  const std::vector<std::byte> image = MakeDatabaseImage(options);

  const auto header = ParseDatabaseHeader(ByteView{image}.first(100));

  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(ByteCount{65536}, header->page_size());
  EXPECT_EQ(ByteCount{16}, header->reserved_bytes());
  EXPECT_EQ(ByteCount{65520}, header->usable_size());
  EXPECT_EQ(3, header->write_version());
  EXPECT_EQ(1, header->read_version());
  EXPECT_EQ(0x01020304U, header->file_change_counter());
  EXPECT_EQ(2U, header->header_page_count());
  EXPECT_EQ(PageNumber{5}, header->first_freelist_trunk());
  EXPECT_EQ(6U, header->freelist_page_count());
  EXPECT_EQ(0x11121314U, header->schema_cookie());
  EXPECT_EQ(4U, header->schema_format());
  EXPECT_EQ(-2000, header->suggested_cache_size());
  EXPECT_EQ(PageNumber{7}, header->largest_root_page());
  EXPECT_EQ(1U, header->text_encoding());
  EXPECT_EQ(8U, header->user_version());
  EXPECT_EQ(1U, header->incremental_vacuum());
  EXPECT_EQ(kApplicationId, header->application_id());
  EXPECT_EQ(0x01020304U, header->version_valid_for());
  EXPECT_EQ(kSqliteVersion, header->sqlite_version());
}

TEST(DatabaseHeader, RejectsMalformedStructuralFields) {
  const auto expect_not_database = [](std::vector<std::byte> bytes) {
    const auto header = ParseDatabaseHeader(bytes);
    ASSERT_FALSE(header.has_value());
    EXPECT_EQ(ErrorCode::kNotDatabase, header.error().code());
  };

  std::vector<std::byte> truncated = MakeDatabaseImage();
  truncated.resize(99);
  expect_not_database(std::move(truncated));

  std::vector<std::byte> bad_magic = MakeDatabaseImage();
  bad_magic[0] = std::byte{0};
  expect_not_database(std::move(bad_magic));

  std::vector<std::byte> bad_page_size = MakeDatabaseImage();
  bad_page_size[16] = std::byte{0x03};
  bad_page_size[17] = std::byte{0x00};
  expect_not_database(std::move(bad_page_size));

  std::vector<std::byte> bad_read_version = MakeDatabaseImage();
  bad_read_version[19] = std::byte{3};
  expect_not_database(std::move(bad_read_version));

  std::vector<std::byte> bad_fractions = MakeDatabaseImage();
  bad_fractions[21] = std::byte{63};
  expect_not_database(std::move(bad_fractions));

  std::vector<std::byte> bad_usable_size = MakeDatabaseImage(DatabaseImageOptions{
      .page_size = 512,
      .physical_pages = 2,
      .header_pages = 2,
      .reserved_bytes = 33,
  });
  expect_not_database(std::move(bad_usable_size));
}

TEST(ReadPager, ValidatesOptionsBeforeUsingTheVfs) {
  FakeEnvironment environment{MakeDatabaseImage()};

  const auto pager = ReadPager::Open(environment.vfs, kInputPath,
                                     ReadPagerOptions{
                                         .empty_database_page_size = ByteCount{1000},
                                         .cache_capacity_pages = 4,
                                     });

  ASSERT_FALSE(pager.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, pager.error().code());
  EXPECT_TRUE(environment.state->full_path_requests.empty());
  EXPECT_TRUE(environment.state->open_requests.empty());
}

TEST(ReadPager, ResolvesAndOpensTheMainDatabaseReadOnlyWithoutReading) {
  FakeEnvironment environment{MakeDatabaseImage()};

  const auto pager = ReadPager::Open(environment.vfs, kInputPath);

  ASSERT_TRUE(pager.has_value());
  EXPECT_EQ(kCanonicalPath, (*pager)->path());
  ASSERT_EQ(1U, environment.state->full_path_requests.size());
  EXPECT_EQ(kInputPath, environment.state->full_path_requests.front());
  ASSERT_EQ(1U, environment.state->open_requests.size());
  EXPECT_EQ(kCanonicalPath, environment.state->open_requests.front().path);
  EXPECT_EQ(FileKind::kMainDatabase, environment.state->open_requests.front().options.kind);
  EXPECT_EQ(FileAccessMode::kReadOnly, environment.state->open_requests.front().options.access);
  EXPECT_FALSE(environment.state->open_requests.front().options.create);
  EXPECT_TRUE(environment.main_file->reads.empty());
  EXPECT_TRUE(environment.main_file->lock_requests.empty());
}

TEST(ReadPager, ReadsAnEmptyDatabaseTransaction) {
  FakeEnvironment environment{{}};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_TRUE(pager->in_read_transaction());
  EXPECT_EQ(nullptr, pager->header());
  EXPECT_EQ(ByteCount{4096}, pager->page_size());
  EXPECT_EQ(0U, pager->page_count());
  const auto page = pager->ReadPage(PageNumber{1});
  ASSERT_FALSE(page.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, page.error().code());
  EXPECT_TRUE(pager->EndRead().has_value());
  EXPECT_FALSE(pager->in_read_transaction());
  EXPECT_EQ(nullptr, pager->header());
  ASSERT_EQ(1U, environment.main_file->lock_requests.size());
  EXPECT_EQ(DatabaseLock::kShared, environment.main_file->lock_requests.front());
  ASSERT_EQ(1U, environment.main_file->unlock_requests.size());
  EXPECT_EQ(DatabaseLock::kNone, environment.main_file->unlock_requests.front());
}

TEST(ReadPager, RejectsOperationsOutsideTheirTransactionState) {
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());

  const auto page_before_begin = (*opened)->ReadPage(PageNumber{1});
  const auto end_before_begin = (*opened)->EndRead();
  ASSERT_FALSE(page_before_begin.has_value());
  ASSERT_FALSE(end_before_begin.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, page_before_begin.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, end_before_begin.error().code());

  ASSERT_TRUE((*opened)->BeginRead().has_value());
  const auto nested_begin = (*opened)->BeginRead();
  ASSERT_FALSE(nested_begin.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, nested_begin.error().code());
  EXPECT_TRUE((*opened)->EndRead().has_value());
}

TEST(ReadPager, UsesTrustedHeaderPageCountOnlyWhenChangeCountersAgree) {
  {
    FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
        .physical_pages = 3,
        .header_pages = 2,
    })};
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE((*opened)->BeginRead().has_value());
    EXPECT_EQ(2U, (*opened)->page_count());
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }

  {
    FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
        .physical_pages = 3,
        .header_pages = 2,
        .version_valid_for = 0x05060708U,
    })};
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE((*opened)->BeginRead().has_value());
    EXPECT_EQ(3U, (*opened)->page_count());
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }
}

TEST(ReadPager, RejectsTrustedPageCountBeyondThePhysicalFile) {
  FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
      .physical_pages = 2,
      .header_pages = 3,
  })};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());

  const auto begun = (*opened)->BeginRead();

  ASSERT_FALSE(begun.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, begun.error().code());
  EXPECT_FALSE((*opened)->in_read_transaction());
  EXPECT_EQ(DatabaseLock::kNone, environment.main_file->current_lock);
}

TEST(ReadPager, RejectsPhysicalPageCountsBeyondThePageNumberRange) {
  constexpr std::uint64_t kPhysicalPages =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
  constexpr std::uint64_t kPageSize = 512;
  FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
      .page_size = kPageSize,
      .header_pages = 0,
  })};
  environment.main_file->size_override = kPhysicalPages * kPageSize;
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());

  const auto begun = (*opened)->BeginRead();

  ASSERT_FALSE(begun.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, begun.error().code());
  EXPECT_EQ(DatabaseLock::kNone, environment.main_file->current_lock);
}

TEST(ReadPager, PropagatesSharedLockContentionWithoutReading) {
  FakeEnvironment environment{MakeDatabaseImage()};
  environment.main_file->lock_failures_remaining = 1;
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());

  const auto begun = (*opened)->BeginRead();

  ASSERT_FALSE(begun.has_value());
  EXPECT_EQ(ErrorCode::kBusy, begun.error().code());
  EXPECT_TRUE(environment.main_file->reads.empty());
  EXPECT_TRUE(environment.main_file->unlock_requests.empty());
}

TEST(ReadPager, CachesPageReadsAndRequiresPinsToEndTheTransaction) {
  constexpr std::size_t kPageSize = 4096;
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened =
      ReadPager::Open(environment.vfs, kInputPath, ReadPagerOptions{.cache_capacity_pages = 4});
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  std::optional<ReadPagePin> first_pin;
  {
    auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x0d}, page->frame().bytes()[0]);
    EXPECT_EQ(std::byte{0x42}, page->frame().bytes()[1]);
    first_pin.emplace(std::move(*page));
  }
  const auto busy = pager->EndRead();
  ASSERT_FALSE(busy.has_value());
  EXPECT_EQ(ErrorCode::kBusy, busy.error().code());
  first_pin.reset();
  EXPECT_TRUE(pager->EndRead().has_value());

  ASSERT_TRUE(pager->BeginRead().has_value());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x42}, page->frame().bytes()[1]);
  }
  EXPECT_TRUE(pager->EndRead().has_value());
  EXPECT_EQ(1U, CountReads(*environment.main_file, kPageSize, kPageSize));
  EXPECT_EQ(2U, CountReads(*environment.main_file, 0, 100));
}

TEST(ReadPager, ZeroFillsAPartialFinalPage) {
  constexpr std::size_t kPageSize = 4096;
  std::vector<std::byte> image = MakeDatabaseImage(DatabaseImageOptions{
      .header_pages = 0,
  });
  image.resize(kPageSize + 10);
  FakeEnvironment environment{std::move(image)};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE((*opened)->BeginRead().has_value());
  ASSERT_EQ(2U, (*opened)->page_count());

  {
    auto page = (*opened)->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(kPageSize, page->frame().bytes().size());
    EXPECT_EQ(std::byte{0x0d}, page->frame().bytes()[0]);
    EXPECT_EQ(std::byte{0}, page->frame().bytes()[10]);
    EXPECT_EQ(std::byte{0}, page->frame().bytes().back());
  }
  EXPECT_TRUE((*opened)->EndRead().has_value());
}

TEST(ReadPager, RejectsInvalidAndLockingPageNumbers) {
  constexpr std::size_t kPageSize = 65536;
  const auto locking_page = static_cast<std::uint32_t>((kPendingByte / kPageSize) + 1U);
  FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
      .page_size = kPageSize,
      .physical_pages = 1,
      .header_pages = locking_page,
  })};
  environment.main_file->size_override =
      static_cast<std::uint64_t>(locking_page) * static_cast<std::uint64_t>(kPageSize);
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE((*opened)->BeginRead().has_value());

  const auto zero = (*opened)->ReadPage(PageNumber{0});
  const auto too_large = (*opened)->ReadPage(PageNumber{locking_page + 1U});
  const auto locking = (*opened)->ReadPage(PageNumber{locking_page});

  ASSERT_FALSE(zero.has_value());
  ASSERT_FALSE(too_large.has_value());
  ASSERT_FALSE(locking.has_value());
  EXPECT_EQ(ErrorCode::kCorruption, zero.error().code());
  EXPECT_EQ(ErrorCode::kCorruption, too_large.error().code());
  EXPECT_EQ(ErrorCode::kCorruption, locking.error().code());
  EXPECT_EQ(0U, CountReads(*environment.main_file,
                           static_cast<std::uint64_t>(locking_page - 1U) * kPageSize, kPageSize));
  EXPECT_TRUE((*opened)->EndRead().has_value());
}

TEST(ReadPager, DoesNotCacheFailedPageReads) {
  constexpr std::size_t kPageSize = 4096;
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE((*opened)->BeginRead().has_value());
  environment.main_file->failing_read_offset = kPageSize;
  environment.main_file->read_failures_remaining = 1;

  const auto failed = (*opened)->ReadPage(PageNumber{2});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  {
    auto retried = (*opened)->ReadPage(PageNumber{2});
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(std::byte{0x42}, retried->frame().bytes()[1]);
  }
  EXPECT_EQ(2U, CountReads(*environment.main_file, kPageSize, kPageSize));
  EXPECT_TRUE((*opened)->EndRead().has_value());
}

TEST(ReadPager, InvalidatesCachedPagesWhenTheDatabaseIdentityChanges) {
  constexpr std::size_t kPageSize = 4096;
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x42}, page->frame().bytes()[1]);
  }
  EXPECT_EQ(0U, pager->data_version());
  EXPECT_TRUE(pager->EndRead().has_value());

  WriteBigEndian32(environment.main_file->bytes, 24, 0x11111111U);
  WriteBigEndian32(environment.main_file->bytes, 92, 0x11111111U);
  environment.main_file->bytes[kPageSize + 1] = std::byte{0x7f};

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(1U, pager->data_version());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x7f}, page->frame().bytes()[1]);
  }
  EXPECT_TRUE(pager->EndRead().has_value());

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(1U, pager->data_version());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x7f}, page->frame().bytes()[1]);
  }
  EXPECT_TRUE(pager->EndRead().has_value());
  EXPECT_EQ(2U, CountReads(*environment.main_file, kPageSize, kPageSize));
}

TEST(ReadPager, RecomputesPageCountWithoutInvalidatingAnUnchangedSnapshot) {
  constexpr std::size_t kPageSize = 4096;
  FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
      .physical_pages = 3,
      .header_pages = 0,
  })};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(3U, pager->page_count());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
  }
  EXPECT_TRUE(pager->EndRead().has_value());

  environment.main_file->bytes.resize(kPageSize * 2);
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(2U, pager->page_count());
  EXPECT_EQ(0U, pager->data_version());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
  }
  EXPECT_TRUE(pager->EndRead().has_value());
  EXPECT_EQ(1U, CountReads(*environment.main_file, kPageSize, kPageSize));
}

TEST(ReadPager, InvalidatesAcrossEmptyAndNonemptyTransitions) {
  FakeEnvironment environment{{}};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(0U, pager->data_version());
  EXPECT_TRUE(pager->EndRead().has_value());

  environment.main_file->bytes = MakeDatabaseImage();
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(1U, pager->data_version());
  EXPECT_TRUE(pager->EndRead().has_value());

  environment.main_file->bytes.clear();
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(2U, pager->data_version());
  EXPECT_TRUE(pager->EndRead().has_value());
}

TEST(ReadPager, ReplacesTheCacheWhenThePageSizeChanges) {
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<ReadPager> pager = std::move(*opened);

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(ByteCount{4096}, pager->page_size());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
  }
  EXPECT_TRUE(pager->EndRead().has_value());

  environment.main_file->bytes = MakeDatabaseImage(DatabaseImageOptions{
      .page_size = 512,
      .physical_pages = 2,
      .header_pages = 2,
  });

  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(ByteCount{512}, pager->page_size());
  EXPECT_EQ(1U, pager->data_version());
  {
    const auto page = pager->ReadPage(PageNumber{2});
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(std::byte{0x42}, page->frame().bytes()[1]);
  }
  EXPECT_TRUE(pager->EndRead().has_value());
  EXPECT_EQ(1U, CountReads(*environment.main_file, 512, 512));
}

TEST(ReadPager, LeavesTheTransactionActiveWhenUnlockFailsSoItCanBeRetried) {
  FakeEnvironment environment{MakeDatabaseImage()};
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE((*opened)->BeginRead().has_value());
  environment.main_file->unlock_failures_remaining = 1;

  const auto first_end = (*opened)->EndRead();

  ASSERT_FALSE(first_end.has_value());
  EXPECT_EQ(ErrorCode::kIo, first_end.error().code());
  EXPECT_TRUE((*opened)->in_read_transaction());
  EXPECT_NE(nullptr, (*opened)->header());
  EXPECT_TRUE((*opened)->EndRead().has_value());
  EXPECT_FALSE((*opened)->in_read_transaction());
  EXPECT_EQ(2U, environment.main_file->unlock_requests.size());
}

TEST(ReadPager, RetriesRetainedLockCleanupBeforeTheNextTransaction) {
  std::vector<std::byte> image = MakeDatabaseImage();
  image[0] = std::byte{0};
  FakeEnvironment environment{std::move(image)};
  environment.main_file->unlock_failures_remaining = 1;
  auto opened = ReadPager::Open(environment.vfs, kInputPath);
  ASSERT_TRUE(opened.has_value());

  const auto failed = (*opened)->BeginRead();

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_NE(std::string_view::npos, failed.error().message().find("not_database"));
  EXPECT_FALSE((*opened)->in_read_transaction());
  environment.main_file->bytes[0] = std::byte{0x53};

  ASSERT_TRUE((*opened)->BeginRead().has_value());
  EXPECT_TRUE((*opened)->EndRead().has_value());
  EXPECT_EQ(2U, environment.main_file->lock_requests.size());
  EXPECT_EQ(3U, environment.main_file->unlock_requests.size());
}

TEST(ReadPager, RejectsHotJournalsButAllowsActiveAndZeroHeaderJournals) {
  {
    FakeEnvironment environment{MakeDatabaseImage()};
    environment.AddSidecar("-journal", {std::byte{0xd9}});
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    const auto begun = (*opened)->BeginRead();
    ASSERT_FALSE(begun.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, begun.error().code());
  }

  {
    FakeEnvironment environment{MakeDatabaseImage()};
    environment.AddSidecar("-journal", {std::byte{0xd9}});
    environment.main_file->reserved_lock = true;
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE((*opened)->BeginRead().has_value());
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }

  {
    FakeEnvironment environment{MakeDatabaseImage()};
    environment.AddSidecar("-journal", {std::byte{0x00}});
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE((*opened)->BeginRead().has_value());
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }
}

TEST(ReadPager, RejectsWalSnapshotsButIgnoresAnEmptyWalSidecar) {
  {
    FakeEnvironment environment{MakeDatabaseImage(DatabaseImageOptions{
        .read_version = 2,
    })};
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    const auto begun = (*opened)->BeginRead();
    ASSERT_FALSE(begun.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, begun.error().code());
  }

  {
    FakeEnvironment environment{MakeDatabaseImage()};
    environment.AddSidecar("-wal", {std::byte{0x37}});
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    const auto begun = (*opened)->BeginRead();
    ASSERT_FALSE(begun.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, begun.error().code());
  }

  {
    FakeEnvironment environment{MakeDatabaseImage()};
    environment.AddSidecar("-wal", {});
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE((*opened)->BeginRead().has_value());
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }

  {
    FakeEnvironment environment{{}};
    environment.AddSidecar("-wal", {std::byte{0x37}});
    auto opened = ReadPager::Open(environment.vfs, kInputPath);
    ASSERT_TRUE(opened.has_value());
    const auto begun = (*opened)->BeginRead();
    ASSERT_FALSE(begun.has_value());
    EXPECT_EQ(ErrorCode::kProtocol, begun.error().code());
  }
}

TEST(ReadPager, ReadsPinnedSQLite354CompatibilityFixtures) {
  const std::filesystem::path fixture_directory =
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
      "read_pager";
  constexpr std::array<std::size_t, 3> kPageSizes{512, 4096, 65536};

  for (const std::size_t page_size : kPageSizes) {
    SCOPED_TRACE(page_size);
    PosixVfs vfs;
    const std::filesystem::path path =
        fixture_directory / ("sqlite-3.54.0-page-" + std::to_string(page_size) + ".db");
    auto opened = ReadPager::Open(vfs, path.string(), ReadPagerOptions{.cache_capacity_pages = 4});
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE((*opened)->BeginRead().has_value());
    ASSERT_NE(nullptr, (*opened)->header());
    EXPECT_EQ(ByteCount{page_size}, (*opened)->header()->page_size());
    EXPECT_EQ(2U, (*opened)->page_count());
    EXPECT_EQ(7U, (*opened)->header()->user_version());
    EXPECT_EQ(kApplicationId, (*opened)->header()->application_id());
    EXPECT_EQ(kSqliteVersion, (*opened)->header()->sqlite_version());
    {
      auto page1 = (*opened)->ReadPage(PageNumber{1});
      auto page2 = (*opened)->ReadPage(PageNumber{2});
      ASSERT_TRUE(page1.has_value());
      ASSERT_TRUE(page2.has_value());
      EXPECT_EQ(std::string_view{"SQLite format 3"},
                AsStringView(page1->frame().bytes().first(15)));
      EXPECT_EQ(std::byte{0x0d}, page2->frame().bytes().front());
    }
    EXPECT_TRUE((*opened)->EndRead().has_value());
  }
}

}  // namespace
}  // namespace modern_sqlite
