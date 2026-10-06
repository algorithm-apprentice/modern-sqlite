#ifndef MODERN_SQLITE_PAGER_PAGER_HPP_
#define MODERN_SQLITE_PAGER_PAGER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/cache/page_cache.hpp"
#include "modern_sqlite/storage/journal/journal.hpp"
#include "modern_sqlite/storage/journal/rollback_journal.hpp"

namespace modern_sqlite {

class BtreeWriteSession;

class DatabaseHeader final {
 public:
  [[nodiscard]] ByteCount page_size() const noexcept { return page_size_; }
  [[nodiscard]] ByteCount reserved_bytes() const noexcept { return reserved_bytes_; }
  [[nodiscard]] ByteCount usable_size() const noexcept { return usable_size_; }
  [[nodiscard]] std::uint8_t write_version() const noexcept { return write_version_; }
  [[nodiscard]] std::uint8_t read_version() const noexcept { return read_version_; }
  [[nodiscard]] std::uint32_t file_change_counter() const noexcept { return file_change_counter_; }
  [[nodiscard]] std::uint32_t header_page_count() const noexcept { return header_page_count_; }
  [[nodiscard]] PageNumber first_freelist_trunk() const noexcept { return first_freelist_trunk_; }
  [[nodiscard]] std::uint32_t freelist_page_count() const noexcept { return freelist_page_count_; }
  [[nodiscard]] std::uint32_t schema_cookie() const noexcept { return schema_cookie_; }
  [[nodiscard]] std::uint32_t schema_format() const noexcept { return schema_format_; }
  [[nodiscard]] std::int32_t suggested_cache_size() const noexcept { return suggested_cache_size_; }
  [[nodiscard]] PageNumber largest_root_page() const noexcept { return largest_root_page_; }
  [[nodiscard]] std::uint32_t text_encoding() const noexcept { return text_encoding_; }
  [[nodiscard]] std::uint32_t user_version() const noexcept { return user_version_; }
  [[nodiscard]] std::uint32_t incremental_vacuum() const noexcept { return incremental_vacuum_; }
  [[nodiscard]] std::uint32_t application_id() const noexcept { return application_id_; }
  [[nodiscard]] std::uint32_t version_valid_for() const noexcept { return version_valid_for_; }
  [[nodiscard]] std::uint32_t sqlite_version() const noexcept { return sqlite_version_; }

 private:
  friend Result<DatabaseHeader> ParseDatabaseHeader(ByteView bytes);
  friend class Pager;

  DatabaseHeader() = default;

  ByteCount page_size_;
  ByteCount reserved_bytes_;
  ByteCount usable_size_;
  std::uint8_t write_version_ = 0;
  std::uint8_t read_version_ = 0;
  std::uint32_t file_change_counter_ = 0;
  std::uint32_t header_page_count_ = 0;
  PageNumber first_freelist_trunk_;
  std::uint32_t freelist_page_count_ = 0;
  std::uint32_t schema_cookie_ = 0;
  std::uint32_t schema_format_ = 0;
  std::int32_t suggested_cache_size_ = 0;
  PageNumber largest_root_page_;
  std::uint32_t text_encoding_ = 0;
  std::uint32_t user_version_ = 0;
  std::uint32_t incremental_vacuum_ = 0;
  std::uint32_t application_id_ = 0;
  std::uint32_t version_valid_for_ = 0;
  std::uint32_t sqlite_version_ = 0;
  std::array<std::byte, 16> change_token_{};
};

[[nodiscard]] Result<DatabaseHeader> ParseDatabaseHeader(ByteView bytes);

struct PagerOptions {
  ByteCount empty_database_page_size{4096};
  std::size_t cache_capacity_pages = 512;
};

struct WritablePagerOptions {
  PagerOptions pager;
  RollbackJournalOptions journal;
};

enum class PagerState : std::uint8_t {
  kOpen,
  kReader,
  kWriterLocked,
  kWriterCacheModified,
  kWriterDatabaseModified,
  kWriterFinished,
  kError,
};

class Pager;

class ReadPagePin final {
 public:
  ReadPagePin(const ReadPagePin&) = delete;
  ReadPagePin& operator=(const ReadPagePin&) = delete;
  ReadPagePin(ReadPagePin&&) noexcept = default;
  ReadPagePin& operator=(ReadPagePin&&) noexcept = default;
  ~ReadPagePin() = default;

  [[nodiscard]] const PageFrame& frame() const noexcept { return pin_.frame(); }
  [[nodiscard]] const PageFrame* operator->() const noexcept { return &frame(); }
  [[nodiscard]] const PageFrame& operator*() const noexcept { return frame(); }

 private:
  friend class Pager;

  explicit ReadPagePin(PageCache::Pin pin) noexcept : pin_(std::move(pin)) {}

  PageCache::Pin pin_;
};

class WritePagePin final {
 public:
  WritePagePin(const WritePagePin&) = delete;
  WritePagePin& operator=(const WritePagePin&) = delete;
  WritePagePin(WritePagePin&&) noexcept = default;
  WritePagePin& operator=(WritePagePin&&) noexcept = default;
  ~WritePagePin() = default;

  [[nodiscard]] const PageFrame& frame() const noexcept { return pin_.frame(); }
  [[nodiscard]] const PageFrame* operator->() const noexcept { return &frame(); }
  [[nodiscard]] const PageFrame& operator*() const noexcept { return frame(); }
  [[nodiscard]] MutableByteView mutable_bytes() noexcept;

 private:
  friend class Pager;

  explicit WritePagePin(PageCache::Pin pin) noexcept : pin_(std::move(pin)) {}

  PageCache::Pin pin_;
};

// Externally serialized. The VFS must outlive the pager, and the pager must
// outlive every page pin returned by ReadPage().
class Pager final {
 private:
  struct ConstructionKey final {};
  class RecoveryTarget;

  enum class WriteCompletion : std::uint8_t {
    kNone,
    kCommit,
    kRollback,
  };

  struct Snapshot {
    std::optional<DatabaseHeader> header;
    ByteCount page_size;
    std::uint32_t page_count = 0;
    std::optional<std::array<std::byte, 16>> change_token;
  };

 public:
  [[nodiscard]] static Result<std::unique_ptr<Pager>> Open(Vfs& vfs, std::string_view path,
                                                           PagerOptions options = {});
  [[nodiscard]] static Result<std::unique_ptr<Pager>> OpenWritable(
      Vfs& vfs, std::string_view path, WritablePagerOptions options = {});

  Pager(ConstructionKey, Vfs& vfs, std::string path, std::unique_ptr<File> file,
        PagerOptions options, std::optional<FileProperties> file_properties = std::nullopt,
        std::unique_ptr<RollbackJournal> rollback_journal = nullptr,
        ByteCount journal_sector_size = {});
  Pager(const Pager&) = delete;
  Pager& operator=(const Pager&) = delete;
  Pager(Pager&&) = delete;
  Pager& operator=(Pager&&) = delete;
  ~Pager();

  [[nodiscard]] Status BeginRead();
  [[nodiscard]] Status EndRead();
  [[nodiscard]] Status CleanupReadState();
  [[nodiscard]] Status BeginWrite();
  [[nodiscard]] Status ClaimWriteCoordinator();
  [[nodiscard]] Status Commit();
  [[nodiscard]] Status Rollback();
  [[nodiscard]] Result<JournalSavepointId> CreateSavepoint();
  [[nodiscard]] Status ReleaseSavepoint(JournalSavepointId savepoint);
  [[nodiscard]] Status RollbackToSavepoint(JournalSavepointId savepoint);
  [[nodiscard]] Result<WritePagePin> WritePage(PageNumber page_number);
  [[nodiscard]] Result<WritePagePin> AllocatePage();
  [[nodiscard]] Status TruncateImage(std::uint32_t page_count);
  [[nodiscard]] Status ValidatePageNumber(PageNumber page_number) const;
  [[nodiscard]] Result<ReadPagePin> ReadPage(PageNumber page_number);

  [[nodiscard]] bool in_read_transaction() const noexcept;
  [[nodiscard]] const DatabaseHeader* header() const noexcept;
  [[nodiscard]] ByteCount page_size() const noexcept;
  [[nodiscard]] std::uint32_t page_count() const noexcept { return current_page_count_; }
  [[nodiscard]] std::uint64_t data_version() const noexcept { return data_version_; }
  [[nodiscard]] std::uint64_t write_transaction_generation() const noexcept {
    return write_transaction_generation_;
  }
  [[nodiscard]] std::optional<ErrorCode> write_failure_code() const noexcept;
  void ReportWriteCoordinatorFailure(ErrorCode code) noexcept;
  [[nodiscard]] std::string_view path() const noexcept { return path_; }
  [[nodiscard]] PagerState state() const noexcept;
  [[nodiscard]] bool writable() const noexcept;
  [[nodiscard]] bool in_write_transaction() const noexcept;

 private:
  friend class BtreeWriteSession;

  [[nodiscard]] Result<Snapshot> ReadSnapshot();
  [[nodiscard]] Result<bool> CheckHotJournal(FileSize database_size);
  [[nodiscard]] Status CheckWal();
  [[nodiscard]] Status ApplySnapshot(Snapshot snapshot);
  [[nodiscard]] Status FailBegin(Error setup_error);
  [[nodiscard]] Status ReleaseRetainedLock();
  [[nodiscard]] Status RecoverHotJournalIfNeeded();
  [[nodiscard]] Result<PageCache::Pin> AcquirePage(PageNumber page_number, bool exclusive);
  [[nodiscard]] Status MaintainCachePressure();
  [[nodiscard]] Status EnsureJournalTransaction();
  [[nodiscard]] Status CaptureSector(PageNumber page_number, const PageCache::Pin& target,
                                     std::uint32_t logical_page_count);
  [[nodiscard]] Status EnsureExclusiveLock();
  [[nodiscard]] Status SpillPage(PageNumber page_number);
  [[nodiscard]] Status UpdateChangeCounter();
  [[nodiscard]] Status FlushDirtyPages();
  [[nodiscard]] Status CompleteWriteCleanup(WriteCompletion completion);
  [[nodiscard]] Status RefreshCurrentHeader();
  [[nodiscard]] Status EnterError(Error error);
  [[nodiscard]] Status StoredError() const;
  void AdvanceWriteTransactionGeneration() noexcept;
  void ResetWriteState() noexcept;

  Vfs* vfs_;
  std::string path_;
  std::string journal_path_;
  std::string wal_path_;
  std::unique_ptr<File> file_;
  PagerOptions options_;
  std::optional<FileProperties> file_properties_;
  std::unique_ptr<RollbackJournal> rollback_journal_;
  ByteCount journal_sector_size_;
  std::unique_ptr<PageCache> cache_;
  std::unique_ptr<JournalTransaction> journal_transaction_;
  std::optional<std::array<std::byte, 16>> last_change_token_;
  std::optional<DatabaseHeader> current_header_;
  std::optional<DatabaseHeader> transaction_start_header_;
  std::optional<std::array<std::byte, 16>> transaction_start_change_token_;
  std::uint32_t current_page_count_ = 0;
  std::uint32_t transaction_start_page_count_ = 0;
  std::uint64_t data_version_ = 0;
  std::uint64_t write_transaction_generation_ = 0;
  PagerState state_ = PagerState::kOpen;
  DatabaseLock database_lock_ = DatabaseLock::kNone;
  WriteCompletion completion_ = WriteCompletion::kNone;
  std::optional<ErrorCode> persistent_error_;
  std::optional<ErrorCode> write_coordinator_failure_;
  bool has_seen_snapshot_ = false;
  bool transaction_modified_ = false;
  bool database_bytes_modified_ = false;
  bool image_size_changed_ = false;
  bool change_counter_updated_ = false;
  bool final_image_ = false;
  bool journal_finalized_ = false;
  bool write_coordinator_claimed_ = false;
  bool write_attempt_sealed_ = false;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PAGER_PAGER_HPP_
