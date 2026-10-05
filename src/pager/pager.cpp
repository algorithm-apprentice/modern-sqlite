#include "modern_sqlite/pager/pager.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kDatabaseHeaderBytes = 100;
constexpr std::uint64_t kPendingByte = 0x40000000ULL;

constexpr std::array<std::byte, 16> kDatabaseMagic{
    std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
    std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
    std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
    std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
};

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error NotDatabase(std::string message) {
  return Error::Create(ErrorCode::kNotDatabase, std::move(message));
}

[[nodiscard]] Error Corruption(std::string message) {
  return Error::Create(ErrorCode::kCorruption, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return Error::Create(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] Error Protocol(std::string message) {
  return Error::Create(ErrorCode::kProtocol, std::move(message));
}

[[nodiscard]] Error Busy(std::string message) {
  return Error::Create(ErrorCode::kBusy, std::move(message));
}

[[nodiscard]] bool IsValidPageSize(ByteCount page_size) noexcept {
  return page_size.value() >= 512 && page_size.value() <= 65536 &&
         std::has_single_bit(page_size.value());
}

[[nodiscard]] std::uint8_t ByteAt(ByteView bytes, std::size_t offset) noexcept {
  return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
}

[[nodiscard]] Result<std::uint32_t> PhysicalPageCount(FileSize file_size, ByteCount page_size) {
  const auto divisor = static_cast<std::uint64_t>(page_size.value());
  const std::uint64_t quotient = file_size.value() / divisor;
  const std::uint64_t remainder = file_size.value() % divisor;
  const std::uint64_t pages = quotient + static_cast<std::uint64_t>(remainder != 0);
  if (pages > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(TooLarge("database page count exceeds the pager page-number range"));
  }
  return static_cast<std::uint32_t>(pages);
}

}  // namespace

Result<DatabaseHeader> ParseDatabaseHeader(ByteView bytes) {
  if (bytes.size() < kDatabaseHeaderBytes) {
    return std::unexpected(NotDatabase("database header is shorter than 100 bytes"));
  }
  if (!std::ranges::equal(kDatabaseMagic, bytes.first(kDatabaseMagic.size()))) {
    return std::unexpected(NotDatabase("database header magic is not SQLite format 3"));
  }

  const auto encoded_page_size = LoadBigEndian<std::uint16_t>(
      std::span<const std::byte, sizeof(std::uint16_t)>{bytes.data() + 16, sizeof(std::uint16_t)});
  const std::size_t page_size =
      encoded_page_size == 1 ? 65536 : static_cast<std::size_t>(encoded_page_size);
  if (!IsValidPageSize(ByteCount{page_size})) {
    return std::unexpected(NotDatabase("database header page size is invalid"));
  }

  const std::uint8_t read_version = ByteAt(bytes, 19);
  if (read_version > 2) {
    return std::unexpected(NotDatabase("database header read version is invalid"));
  }
  if (ByteAt(bytes, 21) != 64 || ByteAt(bytes, 22) != 32 || ByteAt(bytes, 23) != 32) {
    return std::unexpected(NotDatabase("database header payload fractions are invalid"));
  }

  const std::size_t reserved_bytes = ByteAt(bytes, 20);
  if (reserved_bytes > page_size || page_size - reserved_bytes < 480) {
    return std::unexpected(NotDatabase("database header usable page size is too small"));
  }

  DatabaseHeader header;
  header.page_size_ = ByteCount{page_size};
  header.reserved_bytes_ = ByteCount{reserved_bytes};
  header.usable_size_ = ByteCount{page_size - reserved_bytes};
  header.write_version_ = ByteAt(bytes, 18);
  header.read_version_ = read_version;
  header.file_change_counter_ = Load32(bytes, 24);
  header.header_page_count_ = Load32(bytes, 28);
  header.first_freelist_trunk_ = PageNumber{Load32(bytes, 32)};
  header.freelist_page_count_ = Load32(bytes, 36);
  header.schema_cookie_ = Load32(bytes, 40);
  header.schema_format_ = Load32(bytes, 44);
  header.suggested_cache_size_ = std::bit_cast<std::int32_t>(Load32(bytes, 48));
  header.largest_root_page_ = PageNumber{Load32(bytes, 52)};
  header.text_encoding_ = Load32(bytes, 56);
  header.user_version_ = Load32(bytes, 60);
  header.incremental_vacuum_ = Load32(bytes, 64);
  header.application_id_ = Load32(bytes, 68);
  header.version_valid_for_ = Load32(bytes, 92);
  header.sqlite_version_ = Load32(bytes, 96);
  std::ranges::copy(bytes.subspan(24, header.change_token_.size()), header.change_token_.begin());
  return header;
}

MutableByteView WritePagePin::mutable_bytes() noexcept {
  auto bytes = pin_.mutable_bytes();
  assert(bytes.has_value());
  return *bytes;
}

Result<std::unique_ptr<Pager>> Pager::Open(Vfs& vfs, std::string_view path, PagerOptions options) {
  try {
    if (!IsValidPageSize(options.empty_database_page_size)) {
      return std::unexpected(
          Misuse("empty-database page size must be a power of two from 512 through 65536"));
    }

    auto full_path = vfs.FullPath(path);
    if (!full_path.has_value()) {
      return std::unexpected(std::move(full_path.error()));
    }
    auto opened = vfs.Open(*full_path, FileOpenOptions{
                                           .kind = FileKind::kMainDatabase,
                                           .access = FileAccessMode::kReadOnly,
                                       });
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }

    return std::make_unique<Pager>(ConstructionKey{}, vfs, std::move(*full_path),
                                   std::move(opened->file), options);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::unique_ptr<Pager>> Pager::OpenWritable(Vfs& vfs, std::string_view path,
                                                   WritablePagerOptions options) {
  try {
    if (!IsValidPageSize(options.pager.empty_database_page_size)) {
      return std::unexpected(
          Misuse("empty-database page size must be a power of two from 512 through 65536"));
    }

    auto full_path = vfs.FullPath(path);
    if (!full_path.has_value()) {
      return std::unexpected(std::move(full_path.error()));
    }
    auto opened = vfs.Open(*full_path, FileOpenOptions{
                                           .kind = FileKind::kMainDatabase,
                                           .access = FileAccessMode::kReadWrite,
                                           .create = true,
                                       });
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    auto properties = opened->file->Properties();
    if (!properties.has_value()) {
      return std::unexpected(std::move(properties.error()));
    }
    auto sector_size = ResolveRollbackJournalSectorSize(*properties);
    if (!sector_size.has_value()) {
      return std::unexpected(std::move(sector_size.error()));
    }
    auto rollback = RollbackJournal::Create(vfs, *full_path, *properties, options.journal);
    if (!rollback.has_value()) {
      return std::unexpected(std::move(rollback.error()));
    }

    return std::make_unique<Pager>(ConstructionKey{}, vfs, std::move(*full_path),
                                   std::move(opened->file), options.pager, *properties,
                                   std::move(*rollback), *sector_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Pager::Pager(ConstructionKey, Vfs& vfs, std::string path, std::unique_ptr<File> file,
             PagerOptions options, std::optional<FileProperties> file_properties,
             std::unique_ptr<RollbackJournal> rollback_journal, ByteCount journal_sector_size)
    : vfs_(&vfs),
      path_(std::move(path)),
      journal_path_(path_ + "-journal"),
      wal_path_(path_ + "-wal"),
      file_(std::move(file)),
      options_(options),
      file_properties_(file_properties),
      rollback_journal_(std::move(rollback_journal)),
      journal_sector_size_(journal_sector_size) {}

Pager::~Pager() { assert(cache_ == nullptr || cache_->pin_count() == 0); }

Status Pager::BeginRead() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (state_ != PagerState::kOpen) {
    return std::unexpected(Misuse("a read transaction is already active"));
  }
  auto retained_cleanup = ReleaseRetainedLock();
  if (!retained_cleanup.has_value()) {
    return retained_cleanup;
  }

  auto locked = file_->Lock(DatabaseLock::kShared);
  if (!locked.has_value()) {
    return std::unexpected(std::move(locked.error()));
  }
  database_lock_ = DatabaseLock::kShared;

  try {
    auto recovered = RecoverHotJournalIfNeeded();
    if (!recovered.has_value()) {
      return FailBegin(std::move(recovered.error()));
    }
    auto snapshot = ReadSnapshot();
    if (!snapshot.has_value()) {
      return FailBegin(std::move(snapshot.error()));
    }
    auto applied = ApplySnapshot(*snapshot);
    if (!applied.has_value()) {
      return FailBegin(std::move(applied.error()));
    }
  } catch (const std::bad_alloc&) {
    return FailBegin(Error::OutOfMemory());
  }

  state_ = PagerState::kReader;
  return {};
}

Status Pager::EndRead() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (state_ != PagerState::kReader) {
    return std::unexpected(Misuse("no read transaction is active"));
  }
  assert(cache_ != nullptr);
  if (cache_->pin_count() != 0) {
    return std::unexpected(Busy("cannot end a read transaction with pinned pages"));
  }

  return CleanupReadState();
}

Status Pager::CleanupReadState() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (in_write_transaction()) {
    return std::unexpected(Misuse("cannot clean up read state during a write transaction"));
  }
  if (database_lock_ == DatabaseLock::kNone) {
    return {};
  }
  if (cache_ != nullptr && cache_->pin_count() != 0) {
    return std::unexpected(Busy("cannot clean up read state with pinned pages"));
  }

  auto unlocked = file_->Unlock(DatabaseLock::kNone);
  if (!unlocked.has_value()) {
    return std::unexpected(std::move(unlocked.error()));
  }
  database_lock_ = DatabaseLock::kNone;
  state_ = PagerState::kOpen;
  current_header_.reset();
  current_page_count_ = 0;
  return {};
}

Status Pager::ValidatePageNumber(PageNumber page_number) const {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (state_ == PagerState::kWriterFinished) {
    return std::unexpected(Misuse("page validation is unavailable while write cleanup is pending"));
  }
  if (!in_read_transaction()) {
    return std::unexpected(Misuse("page validation requires an active read transaction"));
  }
  if (page_number.value() == 0 || page_number.value() > current_page_count_) {
    return std::unexpected(Corruption("database page number is outside the current snapshot"));
  }

  const std::uint64_t page_size_value = static_cast<std::uint64_t>(page_size().value());
  const std::uint64_t locking_page = (kPendingByte / page_size_value) + 1;
  if (page_number.value() == locking_page) {
    return std::unexpected(Corruption("database references the reserved locking page"));
  }
  return {};
}

Result<ReadPagePin> Pager::ReadPage(PageNumber page_number) {
  auto valid = ValidatePageNumber(page_number);
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto pressure = MaintainCachePressure();
  if (!pressure.has_value()) {
    return std::unexpected(std::move(pressure.error()));
  }
  auto pin = AcquirePage(page_number, false);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  return ReadPagePin{std::move(*pin)};
}

const DatabaseHeader* Pager::header() const noexcept {
  if (!in_read_transaction() || !current_header_.has_value()) {
    return nullptr;
  }
  return &*current_header_;
}

ByteCount Pager::page_size() const noexcept {
  if (cache_ == nullptr) {
    return options_.empty_database_page_size;
  }
  return cache_->page_size();
}

Result<Pager::Snapshot> Pager::ReadSnapshot() {
  auto database_size = file_->Size();
  if (!database_size.has_value()) {
    return std::unexpected(std::move(database_size.error()));
  }

  if (database_size->value() == 0) {
    auto wal = CheckWal();
    if (!wal.has_value()) {
      return std::unexpected(std::move(wal.error()));
    }
    return Snapshot{
        .header = std::nullopt,
        .page_size = options_.empty_database_page_size,
        .page_count = 0,
        .change_token = std::nullopt,
    };
  }

  std::array<std::byte, kDatabaseHeaderBytes> header_bytes{};
  auto read = file_->ReadAt(header_bytes, FileOffset{0});
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  if (read->bytes_read().value() < kDatabaseHeaderBytes) {
    return std::unexpected(NotDatabase("nonempty database has a truncated header"));
  }

  auto header = ParseDatabaseHeader(header_bytes);
  if (!header.has_value()) {
    return std::unexpected(std::move(header.error()));
  }
  if (header->read_version() == 2) {
    return std::unexpected(Protocol("WAL database snapshots are not implemented"));
  }

  auto wal = CheckWal();
  if (!wal.has_value()) {
    return std::unexpected(std::move(wal.error()));
  }

  auto physical_pages = PhysicalPageCount(*database_size, header->page_size());
  if (!physical_pages.has_value()) {
    return std::unexpected(std::move(physical_pages.error()));
  }
  std::uint32_t page_count = *physical_pages;
  if (header->header_page_count() != 0 &&
      header->file_change_counter() == header->version_valid_for()) {
    if (header->header_page_count() > *physical_pages) {
      return std::unexpected(
          Corruption("database header page count exceeds the physical file page count"));
    }
    page_count = header->header_page_count();
  }

  const ByteCount page_size = header->page_size();
  const std::array<std::byte, 16> change_token = header->change_token_;
  return Snapshot{
      .header = *header,
      .page_size = page_size,
      .page_count = page_count,
      .change_token = change_token,
  };
}

Result<bool> Pager::CheckHotJournal(FileSize database_size) {
  if (database_size.value() == 0) {
    return false;
  }

  auto exists = vfs_->Access(journal_path_, FileAccessQuery::kExists);
  if (!exists.has_value()) {
    return std::unexpected(std::move(exists.error()));
  }
  if (!*exists) {
    return false;
  }

  auto reserved = file_->HasReservedLock();
  if (!reserved.has_value()) {
    return std::unexpected(std::move(reserved.error()));
  }
  if (*reserved) {
    return false;
  }

  auto journal = vfs_->Open(journal_path_, FileOpenOptions{
                                               .kind = FileKind::kMainJournal,
                                               .access = FileAccessMode::kReadOnly,
                                           });
  if (!journal.has_value()) {
    return std::unexpected(std::move(journal.error()));
  }
  std::array<std::byte, 1> first{};
  auto read = journal->file->ReadAt(first, FileOffset{0});
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  return first.front() != std::byte{0};
}

Status Pager::CheckWal() {
  auto exists = vfs_->Access(wal_path_, FileAccessQuery::kExists);
  if (!exists.has_value()) {
    return std::unexpected(std::move(exists.error()));
  }
  if (!*exists) {
    return {};
  }

  auto wal = vfs_->Open(wal_path_, FileOpenOptions{
                                       .kind = FileKind::kWriteAheadLog,
                                       .access = FileAccessMode::kReadOnly,
                                   });
  if (!wal.has_value()) {
    return std::unexpected(std::move(wal.error()));
  }
  auto size = wal->file->Size();
  if (!size.has_value()) {
    return std::unexpected(std::move(size.error()));
  }
  if (size->value() != 0) {
    return std::unexpected(Protocol("WAL sidecar requires WAL snapshot support"));
  }
  return {};
}

Status Pager::ApplySnapshot(Snapshot snapshot) {
  const bool page_size_changed = cache_ != nullptr && cache_->page_size() != snapshot.page_size;
  const bool identity_changed = has_seen_snapshot_ && last_change_token_ != snapshot.change_token;
  const bool invalidated = page_size_changed || identity_changed;

  if (cache_ == nullptr || page_size_changed) {
    auto replacement = PageCache::Create(PageCacheOptions{
        .page_size = snapshot.page_size,
        .capacity_pages = options_.cache_capacity_pages,
    });
    if (!replacement.has_value()) {
      return std::unexpected(std::move(replacement.error()));
    }
    cache_ = std::move(*replacement);
  } else if (identity_changed) {
    assert(cache_->pin_count() == 0);
    assert(cache_->dirty_page_count() == 0);
    static_cast<void>(cache_->ReclaimClean());
    assert(cache_->page_count() == 0);
  }

  if (invalidated) {
    ++data_version_;
  }
  has_seen_snapshot_ = true;
  last_change_token_ = snapshot.change_token;
  current_header_ = snapshot.header;
  current_page_count_ = snapshot.page_count;
  return {};
}

Status Pager::FailBegin(Error setup_error) {
  if (state_ == PagerState::kError || database_lock_ == DatabaseLock::kNone) {
    return std::unexpected(std::move(setup_error));
  }
  auto unlocked = file_->Unlock(DatabaseLock::kNone);
  if (unlocked.has_value()) {
    database_lock_ = DatabaseLock::kNone;
    return std::unexpected(std::move(setup_error));
  }
  if (setup_error.code() == ErrorCode::kOutOfMemory) {
    return std::unexpected(std::move(unlocked.error()));
  }

  std::string message{"read transaction setup failed with "};
  message.append(setup_error.ToString());
  message.append("; shared-lock cleanup failed with ");
  message.append(unlocked.error().ToString());
  return std::unexpected(Error::Create(unlocked.error().code(), std::move(message)));
}

Status Pager::ReleaseRetainedLock() {
  assert(state_ == PagerState::kOpen);
  return CleanupReadState();
}

Result<PageCache::Pin> Pager::AcquirePage(PageNumber page_number, bool exclusive) try {
  assert(cache_ != nullptr);
  auto found = exclusive ? cache_->LookupExclusive(page_number) : cache_->Lookup(page_number);
  if (!found.has_value()) {
    return std::unexpected(std::move(found.error()));
  }
  if (found->has_value()) {
    return std::move(found->value());
  }

  const std::uint64_t page_size_value = static_cast<std::uint64_t>(cache_->page_size().value());
  const auto page_index = static_cast<std::uint64_t>(page_number.value() - 1U);
  if (page_index > std::numeric_limits<std::uint64_t>::max() / page_size_value) {
    return std::unexpected(TooLarge("database page offset is not representable"));
  }
  const std::uint64_t offset = page_index * page_size_value;
  ByteBuffer bytes{cache_->page_size()};
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPagesRead, 1U);
  auto read = file_->ReadAt(bytes.mutable_view(), FileOffset{offset});
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  auto inserted = exclusive ? cache_->InsertExclusive(page_number, std::move(bytes))
                            : cache_->Insert(page_number, std::move(bytes));
  if (!inserted.has_value()) {
    return std::unexpected(std::move(inserted.error()));
  }
  return std::move(*inserted);
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
}

PagerState Pager::state() const noexcept { return state_; }

bool Pager::writable() const noexcept { return rollback_journal_ != nullptr; }

bool Pager::in_read_transaction() const noexcept {
  return state_ != PagerState::kOpen && state_ != PagerState::kError;
}

bool Pager::in_write_transaction() const noexcept {
  return state_ == PagerState::kWriterLocked || state_ == PagerState::kWriterCacheModified ||
         state_ == PagerState::kWriterDatabaseModified || state_ == PagerState::kWriterFinished;
}

Status Pager::StoredError() const {
  return std::unexpected(Error::Create(persistent_error_.value_or(ErrorCode::kInternal),
                                       "pager is in a persistent error state"));
}

}  // namespace modern_sqlite
