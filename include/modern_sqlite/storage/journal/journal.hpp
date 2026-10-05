#ifndef MODERN_SQLITE_STORAGE_JOURNAL_JOURNAL_HPP_
#define MODERN_SQLITE_STORAGE_JOURNAL_JOURNAL_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

struct JournalTransactionInfo {
  ByteCount page_size;
  ByteCount sector_size;
  std::uint32_t original_page_count = 0;

  constexpr bool operator==(const JournalTransactionInfo&) const noexcept = default;
};

struct JournalPageImage {
  PageNumber page_number;
  ByteView bytes;
};

class JournalSavepointId final {
 public:
  constexpr JournalSavepointId(const JournalSavepointId&) noexcept = default;
  constexpr JournalSavepointId& operator=(const JournalSavepointId&) noexcept = default;

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const JournalSavepointId&) const noexcept = default;

 private:
  friend class JournalTransaction;

  constexpr JournalSavepointId(std::uint64_t owner_token, std::uint64_t value) noexcept
      : owner_token_(owner_token), value_(value) {}

  std::uint64_t owner_token_;
  std::uint64_t value_ = 0;
};

struct JournalSavepoint {
  JournalSavepointId id;
  std::uint32_t original_page_count = 0;

  constexpr bool operator==(const JournalSavepoint&) const noexcept = default;
};

enum class JournalPlaybackKind : std::uint8_t {
  kTransactionRollback,
  kSavepointRollback,
  kHotRecovery,
};

struct JournalPlaybackInfo {
  JournalPlaybackKind kind = JournalPlaybackKind::kTransactionRollback;
  ByteCount page_size;
  std::uint32_t original_page_count = 0;

  constexpr bool operator==(const JournalPlaybackInfo&) const noexcept = default;
};

class JournalPlayback {
 public:
  JournalPlayback() = default;
  JournalPlayback(const JournalPlayback&) = delete;
  JournalPlayback& operator=(const JournalPlayback&) = delete;
  JournalPlayback(JournalPlayback&&) = delete;
  JournalPlayback& operator=(JournalPlayback&&) = delete;
  virtual ~JournalPlayback() = default;

  [[nodiscard]] virtual std::optional<JournalPlaybackInfo> info() const noexcept = 0;
  [[nodiscard]] virtual Result<std::optional<JournalPageImage>> Next() = 0;
};

class JournalRecoveryTarget {
 public:
  JournalRecoveryTarget() = default;
  JournalRecoveryTarget(const JournalRecoveryTarget&) = delete;
  JournalRecoveryTarget& operator=(const JournalRecoveryTarget&) = delete;
  JournalRecoveryTarget(JournalRecoveryTarget&&) = delete;
  JournalRecoveryTarget& operator=(JournalRecoveryTarget&&) = delete;
  virtual ~JournalRecoveryTarget() = default;

  [[nodiscard]] virtual Status PreparePlayback(JournalPlaybackInfo info) = 0;
  [[nodiscard]] virtual Status ResizeDatabase(std::uint32_t page_count) = 0;
  [[nodiscard]] virtual Status RestorePage(JournalPageImage image) = 0;
  [[nodiscard]] virtual Status SyncDatabase() = 0;
  [[nodiscard]] virtual Status CompletePlayback(JournalPlaybackInfo info) = 0;
};

class JournalTransaction;
class JournalBackend;

[[nodiscard]] Status RecoverHotJournal(JournalBackend& backend, JournalRecoveryTarget& target);

class JournalBackend {
 public:
  JournalBackend() = default;
  JournalBackend(const JournalBackend&) = delete;
  JournalBackend& operator=(const JournalBackend&) = delete;
  JournalBackend(JournalBackend&&) = delete;
  JournalBackend& operator=(JournalBackend&&) = delete;
  virtual ~JournalBackend() = default;

 protected:
  friend class JournalTransaction;
  friend Status RecoverHotJournal(JournalBackend& backend, JournalRecoveryTarget& target);

  [[nodiscard]] virtual Status DoBegin(JournalTransactionInfo info) = 0;
  [[nodiscard]] virtual Status DoAppendTransactionPage(JournalPageImage image) = 0;
  [[nodiscard]] virtual Status DoAppendSavepointPage(JournalPageImage image) = 0;
  [[nodiscard]] virtual Status DoCreateSavepoint(JournalSavepoint savepoint) = 0;
  [[nodiscard]] virtual Status DoReleaseSavepoint(JournalSavepointId savepoint,
                                                  bool rewind_subjournal) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<JournalPlayback>> DoOpenSavepointPlayback(
      JournalSavepoint savepoint) = 0;
  [[nodiscard]] virtual Status DoCompleteSavepointPlayback(JournalSavepoint savepoint) = 0;
  [[nodiscard]] virtual Status DoSync() = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<JournalPlayback>> DoOpenTransactionPlayback() = 0;
  [[nodiscard]] virtual Status DoPrepareHotRecovery() = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<JournalPlayback>> DoOpenHotPlayback() = 0;
  [[nodiscard]] virtual Status DoFinalizeCommit() = 0;
  [[nodiscard]] virtual Status DoFinalizeRollback() = 0;
};

enum class JournalTransactionState : std::uint8_t {
  kActive,
  kFailed,
  kFinished,
  kError,
};

class JournalTransaction final {
 private:
  struct ConstructionKey final {};

  struct SavepointState {
    JournalSavepoint savepoint;
    std::unordered_set<std::uint32_t> pages;
    bool rewind_subjournal_on_release = true;
  };

 public:
  [[nodiscard]] static Result<std::unique_ptr<JournalTransaction>> Begin(
      JournalBackend& backend, JournalTransactionInfo info);

  JournalTransaction(ConstructionKey, JournalBackend& backend, JournalTransactionInfo info,
                     std::uint64_t owner_token) noexcept;
  JournalTransaction(const JournalTransaction&) = delete;
  JournalTransaction& operator=(const JournalTransaction&) = delete;
  JournalTransaction(JournalTransaction&&) = delete;
  JournalTransaction& operator=(JournalTransaction&&) = delete;
  ~JournalTransaction() = default;

  [[nodiscard]] Status CapturePage(JournalPageImage image);
  [[nodiscard]] Result<JournalSavepointId> CreateSavepoint(std::uint32_t current_page_count);
  [[nodiscard]] Status ReleaseSavepoint(JournalSavepointId savepoint);
  [[nodiscard]] Status RollbackToSavepoint(JournalSavepointId savepoint,
                                           JournalRecoveryTarget& target);

  [[nodiscard]] Status SyncJournal();
  [[nodiscard]] Status AuthorizeDatabaseWrite(PageNumber page_number);
  [[nodiscard]] Status AuthorizeDatabaseResize(std::span<const PageNumber> rollback_pages);
  void ReportDatabaseFailure(ErrorCode code) noexcept;
  [[nodiscard]] Status MarkDatabaseSynced();

  [[nodiscard]] Status Commit();
  [[nodiscard]] Status Rollback(JournalRecoveryTarget& target);

  [[nodiscard]] JournalTransactionState state() const noexcept { return state_; }
  [[nodiscard]] JournalTransactionInfo info() const noexcept { return info_; }
  [[nodiscard]] bool main_journal_dirty() const noexcept { return main_journal_dirty_; }
  [[nodiscard]] bool database_may_be_modified() const noexcept { return database_may_be_modified_; }
  [[nodiscard]] bool database_synced() const noexcept { return database_synced_; }
  [[nodiscard]] std::size_t savepoint_count() const noexcept { return savepoints_.size(); }
  [[nodiscard]] std::optional<ErrorCode> failure_code() const noexcept { return failure_code_; }

 private:
  [[nodiscard]] Status RequireActive() const;
  [[nodiscard]] Status StoredFailure() const;
  [[nodiscard]] bool IsValidPageNumber(PageNumber page_number) const noexcept;
  [[nodiscard]] std::optional<std::size_t> FindSavepoint(
      JournalSavepointId savepoint) const noexcept;
  void EnterFailed(ErrorCode code) noexcept;
  void EnterError(ErrorCode code) noexcept;

  JournalBackend* backend_;
  JournalTransactionInfo info_;
  std::uint64_t owner_token_;
  std::unordered_set<std::uint32_t> transaction_pages_;
  std::vector<SavepointState> savepoints_;
  std::uint64_t next_savepoint_id_ = 1;
  JournalTransactionState state_ = JournalTransactionState::kActive;
  std::optional<ErrorCode> failure_code_;
  bool main_journal_dirty_ = true;
  bool database_may_be_modified_ = false;
  bool database_synced_ = false;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_JOURNAL_JOURNAL_HPP_
