#include "modern_sqlite/storage/journal/journal.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kPendingByte = 0x40000000ULL;

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Internal(std::string message) {
  return Error::Create(ErrorCode::kInternal, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return Error::Create(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] bool IsValidPageSize(ByteCount page_size) noexcept {
  return page_size.value() >= 512U && page_size.value() <= 65536U &&
         std::has_single_bit(page_size.value());
}

[[nodiscard]] bool IsValidSectorSize(ByteCount sector_size) noexcept {
  return sector_size.value() >= 32U && sector_size.value() <= 65536U &&
         std::has_single_bit(sector_size.value());
}

[[nodiscard]] std::uint32_t LockingPage(ByteCount page_size) noexcept {
  return static_cast<std::uint32_t>((kPendingByte / static_cast<std::uint64_t>(page_size.value())) +
                                    1U);
}

[[nodiscard]] Result<std::uint64_t> AllocateTransactionToken() {
  static std::atomic<std::uint64_t> next_token{1};

  std::uint64_t token = next_token.load(std::memory_order_relaxed);
  while (token != std::numeric_limits<std::uint64_t>::max()) {
    if (next_token.compare_exchange_weak(token, token + 1U, std::memory_order_relaxed)) {
      return token;
    }
  }
  return std::unexpected(TooLarge("journal transaction identifier space is exhausted"));
}

struct PlaybackExpectation {
  JournalPlaybackKind kind;
  std::optional<ByteCount> page_size;
  std::optional<std::uint32_t> original_page_count;
};

[[nodiscard]] Status ValidatePlaybackInfo(JournalPlaybackInfo info,
                                          const PlaybackExpectation& expected) {
  if (info.kind != expected.kind) {
    return std::unexpected(Internal("journal backend returned the wrong playback kind"));
  }
  if (!IsValidPageSize(info.page_size)) {
    return std::unexpected(Internal("journal backend returned an invalid playback page size"));
  }
  if (expected.page_size.has_value() && info.page_size != *expected.page_size) {
    return std::unexpected(
        Internal("journal backend playback page size does not match the transaction"));
  }
  if (expected.original_page_count.has_value() &&
      info.original_page_count != *expected.original_page_count) {
    return std::unexpected(
        Internal("journal backend playback size does not match the rollback scope"));
  }
  return {};
}

[[nodiscard]] Status ApplyPlayback(JournalPlayback& playback, JournalRecoveryTarget& target,
                                   const PlaybackExpectation& expected, bool sync_database,
                                   bool& target_started) try {
  target_started = false;
  const std::optional<JournalPlaybackInfo> info = playback.info();
  if (!info.has_value()) {
    if (expected.kind == JournalPlaybackKind::kSavepointRollback) {
      return std::unexpected(
          Internal("journal backend omitted required savepoint playback metadata"));
    }
    return {};
  }

  auto valid_info = ValidatePlaybackInfo(*info, expected);
  if (!valid_info.has_value()) {
    return valid_info;
  }

  target_started = true;
  auto prepared = target.PreparePlayback(*info);
  if (!prepared.has_value()) {
    return std::unexpected(std::move(prepared.error()));
  }
  auto resized = target.ResizeDatabase(info->original_page_count);
  if (!resized.has_value()) {
    return std::unexpected(std::move(resized.error()));
  }

  std::unordered_set<std::uint32_t> restored_pages;
  const bool suppress_duplicates = info->kind == JournalPlaybackKind::kSavepointRollback;
  while (true) {
    auto next = playback.Next();
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    if (!next->has_value()) {
      break;
    }

    const JournalPageImage image = **next;
    const std::uint32_t page_number = image.page_number.value();
    if (page_number == 0U || page_number == LockingPage(info->page_size) ||
        image.bytes.size() != info->page_size.value()) {
      return std::unexpected(Internal("journal backend returned an invalid page image"));
    }
    if (page_number > info->original_page_count) {
      continue;
    }

    if (suppress_duplicates) {
      bool inserted = false;
      try {
        inserted = restored_pages.insert(page_number).second;
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      }
      if (!inserted) {
        continue;
      }
    }

    auto restored = target.RestorePage(image);
    if (!restored.has_value()) {
      return std::unexpected(std::move(restored.error()));
    }
  }

  if (sync_database) {
    auto synced = target.SyncDatabase();
    if (!synced.has_value()) {
      return std::unexpected(std::move(synced.error()));
    }
  }
  auto completed = target.CompletePlayback(*info);
  if (!completed.has_value()) {
    return std::unexpected(std::move(completed.error()));
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
}

}  // namespace

Result<std::unique_ptr<JournalTransaction>> JournalTransaction::Begin(JournalBackend& backend,
                                                                      JournalTransactionInfo info) {
  try {
    if (!IsValidPageSize(info.page_size)) {
      return std::unexpected(
          Misuse("journal page size must be a power of two from 512 through 65536"));
    }
    if (!IsValidSectorSize(info.sector_size)) {
      return std::unexpected(
          Misuse("journal sector size must be a power of two from 32 through 65536"));
    }

    auto owner_token = AllocateTransactionToken();
    if (!owner_token.has_value()) {
      return std::unexpected(std::move(owner_token.error()));
    }

    auto transaction =
        std::make_unique<JournalTransaction>(ConstructionKey{}, backend, info, *owner_token);
    auto begun = backend.DoBegin(info);
    if (!begun.has_value()) {
      return std::unexpected(std::move(begun.error()));
    }
    return transaction;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

JournalTransaction::JournalTransaction(ConstructionKey, JournalBackend& backend,
                                       JournalTransactionInfo info,
                                       std::uint64_t owner_token) noexcept
    : backend_(&backend), info_(info), owner_token_(owner_token) {}

Status JournalTransaction::CapturePage(JournalPageImage image) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (!IsValidPageNumber(image.page_number)) {
    return std::unexpected(Misuse("journal capture requires a valid database page number"));
  }
  if (image.bytes.size() != info_.page_size.value()) {
    return std::unexpected(Misuse("journal page image size does not match the transaction"));
  }

  const std::uint32_t page_number = image.page_number.value();
  const bool needs_transaction =
      page_number <= info_.original_page_count && !transaction_pages_.contains(page_number);
  std::optional<std::size_t> first_savepoint_needing_image;
  for (std::size_t index = 0; index < savepoints_.size(); ++index) {
    const SavepointState& state = savepoints_[index];
    if (page_number <= state.savepoint.original_page_count && !state.pages.contains(page_number)) {
      first_savepoint_needing_image = index;
      break;
    }
  }

  if (needs_transaction) {
    main_journal_dirty_ = true;
    database_synced_ = false;
    auto appended = backend_->DoAppendTransactionPage(image);
    if (!appended.has_value()) {
      const ErrorCode code = appended.error().code();
      EnterFailed(code);
      return std::unexpected(std::move(appended.error()));
    }

    try {
      transaction_pages_.insert(page_number);
      for (SavepointState& state : savepoints_) {
        if (page_number <= state.savepoint.original_page_count) {
          state.pages.insert(page_number);
        }
      }
    } catch (const std::bad_alloc&) {
      EnterFailed(ErrorCode::kOutOfMemory);
      return std::unexpected(Error::OutOfMemory());
    }
    return {};
  }

  if (!first_savepoint_needing_image.has_value()) {
    return {};
  }

  auto appended = backend_->DoAppendSavepointPage(image);
  if (!appended.has_value()) {
    const ErrorCode code = appended.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(appended.error()));
  }
  for (std::size_t index = *first_savepoint_needing_image + 1U; index < savepoints_.size();
       ++index) {
    savepoints_[index].rewind_subjournal_on_release = false;
  }
  try {
    for (SavepointState& state : savepoints_) {
      if (page_number <= state.savepoint.original_page_count) {
        state.pages.insert(page_number);
      }
    }
  } catch (const std::bad_alloc&) {
    EnterFailed(ErrorCode::kOutOfMemory);
    return std::unexpected(Error::OutOfMemory());
  }
  return {};
}

bool JournalTransaction::NeedsCapture(PageNumber page_number) const noexcept {
  if (state_ != JournalTransactionState::kActive || !IsValidPageNumber(page_number)) {
    return false;
  }

  const std::uint32_t page = page_number.value();
  if (page <= info_.original_page_count && !transaction_pages_.contains(page)) {
    return true;
  }
  return std::ranges::any_of(savepoints_, [page](const SavepointState& state) {
    return page <= state.savepoint.original_page_count && !state.pages.contains(page);
  });
}

Result<JournalSavepointId> JournalTransaction::CreateSavepoint(std::uint32_t current_page_count) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (next_savepoint_id_ == 0U) {
    return std::unexpected(TooLarge("journal savepoint identifier space is exhausted"));
  }

  const JournalSavepoint savepoint{
      .id = JournalSavepointId{owner_token_, next_savepoint_id_},
      .original_page_count = current_page_count,
  };
  try {
    savepoints_.push_back(SavepointState{
        .savepoint = savepoint,
        .pages = {},
        .rewind_subjournal_on_release = true,
    });
  } catch (const std::bad_alloc&) {
    EnterFailed(ErrorCode::kOutOfMemory);
    return std::unexpected(Error::OutOfMemory());
  }

  auto created = backend_->DoCreateSavepoint(savepoint);
  if (!created.has_value()) {
    savepoints_.pop_back();
    const ErrorCode code = created.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(created.error()));
  }

  if (next_savepoint_id_ == std::numeric_limits<std::uint64_t>::max()) {
    next_savepoint_id_ = 0;
  } else {
    ++next_savepoint_id_;
  }
  return savepoint.id;
}

Status JournalTransaction::ReleaseSavepoint(JournalSavepointId savepoint) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  const std::optional<std::size_t> index = FindSavepoint(savepoint);
  if (!index.has_value()) {
    return std::unexpected(Misuse("journal savepoint is not active"));
  }

  auto released =
      backend_->DoReleaseSavepoint(savepoint, savepoints_[*index].rewind_subjournal_on_release);
  if (!released.has_value()) {
    const ErrorCode code = released.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(released.error()));
  }
  savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(*index), savepoints_.end());
  return {};
}

Status JournalTransaction::RollbackToSavepoint(JournalSavepointId savepoint,
                                               JournalRecoveryTarget& target) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  const std::optional<std::size_t> index = FindSavepoint(savepoint);
  if (!index.has_value()) {
    return std::unexpected(Misuse("journal savepoint is not active"));
  }

  if (main_journal_dirty_) {
    auto synced = backend_->DoSync();
    if (!synced.has_value()) {
      const ErrorCode code = synced.error().code();
      EnterFailed(code);
      return std::unexpected(std::move(synced.error()));
    }
    main_journal_dirty_ = false;
  }

  const JournalSavepoint scope = savepoints_[*index].savepoint;
  auto opened = backend_->DoOpenSavepointPlayback(scope);
  if (!opened.has_value()) {
    const ErrorCode code = opened.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(opened.error()));
  }
  if (*opened == nullptr) {
    EnterFailed(ErrorCode::kInternal);
    return std::unexpected(Internal("journal backend returned a null savepoint playback"));
  }

  bool target_started = false;
  auto applied = ApplyPlayback(**opened, target,
                               PlaybackExpectation{
                                   .kind = JournalPlaybackKind::kSavepointRollback,
                                   .page_size = info_.page_size,
                                   .original_page_count = scope.original_page_count,
                               },
                               false, target_started);
  if (!applied.has_value()) {
    const ErrorCode code = applied.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(applied.error()));
  }

  opened->reset();
  auto completed = backend_->DoCompleteSavepointPlayback(scope);
  if (!completed.has_value()) {
    const ErrorCode code = completed.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(completed.error()));
  }

  savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
                    savepoints_.end());
  database_may_be_modified_ = true;
  database_synced_ = false;
  return {};
}

Status JournalTransaction::SyncJournal() {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (!main_journal_dirty_) {
    return {};
  }

  auto synced = backend_->DoSync();
  if (!synced.has_value()) {
    const ErrorCode code = synced.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(synced.error()));
  }
  main_journal_dirty_ = false;
  return {};
}

Status JournalTransaction::AuthorizeDatabaseWrite(PageNumber page_number) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (!IsValidPageNumber(page_number)) {
    return std::unexpected(Misuse("database write requires a valid page number"));
  }
  if (main_journal_dirty_) {
    return std::unexpected(Misuse("database write requires a synchronized main journal"));
  }

  const std::uint64_t page_size = info_.page_size.value();
  const std::uint64_t sector_size = info_.sector_size.value();
  const std::uint64_t pages_per_sector = sector_size > page_size ? sector_size / page_size : 1U;
  const std::uint64_t requested = page_number.value();
  const std::uint64_t first = ((requested - 1U) / pages_per_sector) * pages_per_sector + 1U;
  const std::uint64_t last = first + pages_per_sector - 1U;
  const std::uint64_t upper = std::min<std::uint64_t>(last, info_.original_page_count);
  const std::uint32_t locking_page = LockingPage(info_.page_size);

  for (std::uint64_t current = first; current <= upper; ++current) {
    if (current == locking_page) {
      continue;
    }
    const auto current_page = static_cast<std::uint32_t>(current);
    if (!transaction_pages_.contains(current_page)) {
      return std::unexpected(
          Misuse("database write sector contains a page absent from the main journal"));
    }
  }

  database_may_be_modified_ = true;
  database_synced_ = false;
  return {};
}

Status JournalTransaction::AuthorizeDatabaseResize(std::span<const PageNumber> rollback_pages) {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (main_journal_dirty_) {
    return std::unexpected(Misuse("database resize requires a synchronized main journal"));
  }

  for (const PageNumber page_number : rollback_pages) {
    if (!IsValidPageNumber(page_number)) {
      return std::unexpected(Misuse("database resize requires valid rollback page numbers"));
    }
    const std::uint32_t page = page_number.value();
    if (page <= info_.original_page_count && !transaction_pages_.contains(page)) {
      return std::unexpected(
          Misuse("database resize rollback page is absent from the main journal"));
    }
    for (const SavepointState& state : savepoints_) {
      if (page <= state.savepoint.original_page_count && !state.pages.contains(page)) {
        return std::unexpected(
            Misuse("database resize rollback page is absent from a savepoint journal"));
      }
    }
  }

  database_may_be_modified_ = true;
  database_synced_ = false;
  return {};
}

void JournalTransaction::ReportDatabaseFailure(ErrorCode code) noexcept {
  if (state_ == JournalTransactionState::kActive) {
    EnterFailed(code);
  }
}

Status JournalTransaction::MarkDatabaseSynced() {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (main_journal_dirty_) {
    return std::unexpected(Misuse("database sync cannot complete with an unsynced main journal"));
  }
  if (!database_may_be_modified_) {
    return std::unexpected(Misuse("database sync requires an authorized database mutation"));
  }
  database_synced_ = true;
  return {};
}

Status JournalTransaction::Commit() {
  auto active = RequireActive();
  if (!active.has_value()) {
    return active;
  }
  if (main_journal_dirty_ || !database_synced_) {
    return std::unexpected(
        Misuse("journal commit requires synchronized journal and database contents"));
  }

  Status finalized;
  try {
    finalized = backend_->DoFinalizeCommit();
  } catch (const std::bad_alloc&) {
    EnterError(ErrorCode::kOutOfMemory);
    return std::unexpected(Error::OutOfMemory());
  }
  if (!finalized.has_value()) {
    const ErrorCode code = finalized.error().code();
    EnterError(code);
    return std::unexpected(std::move(finalized.error()));
  }
  state_ = JournalTransactionState::kFinished;
  failure_code_.reset();
  return {};
}

Status JournalTransaction::Rollback(JournalRecoveryTarget& target) {
  if (state_ == JournalTransactionState::kError) {
    return StoredFailure();
  }
  if (state_ == JournalTransactionState::kFinished) {
    return std::unexpected(Misuse("journal transaction is already finished"));
  }

  if (main_journal_dirty_) {
    auto synced = backend_->DoSync();
    if (!synced.has_value()) {
      const ErrorCode code = synced.error().code();
      EnterFailed(code);
      return std::unexpected(std::move(synced.error()));
    }
    main_journal_dirty_ = false;
  }

  auto opened = backend_->DoOpenTransactionPlayback();
  if (!opened.has_value()) {
    const ErrorCode code = opened.error().code();
    EnterFailed(code);
    return std::unexpected(std::move(opened.error()));
  }
  if (*opened == nullptr) {
    EnterError(ErrorCode::kInternal);
    return std::unexpected(Internal("journal backend returned a null transaction playback"));
  }

  bool target_started = false;
  auto applied = ApplyPlayback(**opened, target,
                               PlaybackExpectation{
                                   .kind = JournalPlaybackKind::kTransactionRollback,
                                   .page_size = info_.page_size,
                                   .original_page_count = info_.original_page_count,
                               },
                               true, target_started);
  if (!applied.has_value()) {
    const ErrorCode code = applied.error().code();
    if (target_started || code == ErrorCode::kInternal) {
      EnterError(code);
    } else {
      EnterFailed(code);
    }
    return std::unexpected(std::move(applied.error()));
  }

  opened->reset();
  Status finalized;
  try {
    finalized = backend_->DoFinalizeRollback();
  } catch (const std::bad_alloc&) {
    EnterError(ErrorCode::kOutOfMemory);
    return std::unexpected(Error::OutOfMemory());
  }
  if (!finalized.has_value()) {
    const ErrorCode code = finalized.error().code();
    EnterError(code);
    return std::unexpected(std::move(finalized.error()));
  }

  state_ = JournalTransactionState::kFinished;
  failure_code_.reset();
  database_synced_ = false;
  return {};
}

Status JournalTransaction::RequireActive() const {
  switch (state_) {
    case JournalTransactionState::kActive:
      return {};
    case JournalTransactionState::kFailed:
    case JournalTransactionState::kError:
      return StoredFailure();
    case JournalTransactionState::kFinished:
      return std::unexpected(Misuse("journal transaction is already finished"));
  }
  return std::unexpected(Internal("journal transaction has an unknown state"));
}

Status JournalTransaction::StoredFailure() const {
  const ErrorCode code = failure_code_.value_or(ErrorCode::kInternal);
  return std::unexpected(
      Error::Create(code, state_ == JournalTransactionState::kFailed
                              ? "journal transaction requires rollback"
                              : "journal transaction is in a persistent error state"));
}

bool JournalTransaction::IsValidPageNumber(PageNumber page_number) const noexcept {
  return page_number.value() != 0U && page_number.value() != LockingPage(info_.page_size);
}

std::optional<std::size_t> JournalTransaction::FindSavepoint(
    JournalSavepointId savepoint) const noexcept {
  if (savepoint.value() == 0U) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < savepoints_.size(); ++index) {
    if (savepoints_[index].savepoint.id == savepoint) {
      return index;
    }
  }
  return std::nullopt;
}

void JournalTransaction::EnterFailed(ErrorCode code) noexcept {
  if (!failure_code_.has_value()) {
    failure_code_ = code;
  }
  if (state_ != JournalTransactionState::kError) {
    state_ = JournalTransactionState::kFailed;
  }
  database_synced_ = false;
}

void JournalTransaction::EnterError(ErrorCode code) noexcept {
  if (!failure_code_.has_value()) {
    failure_code_ = code;
  }
  state_ = JournalTransactionState::kError;
  database_synced_ = false;
}

Status RecoverHotJournal(JournalBackend& backend, JournalRecoveryTarget& target) {
  auto prepared = backend.DoPrepareHotRecovery();
  if (!prepared.has_value()) {
    return std::unexpected(std::move(prepared.error()));
  }

  auto opened = backend.DoOpenHotPlayback();
  if (!opened.has_value()) {
    return std::unexpected(std::move(opened.error()));
  }
  if (*opened == nullptr) {
    return std::unexpected(Internal("journal backend returned a null hot-recovery playback"));
  }

  bool target_started = false;
  auto applied = ApplyPlayback(**opened, target,
                               PlaybackExpectation{
                                   .kind = JournalPlaybackKind::kHotRecovery,
                                   .page_size = std::nullopt,
                                   .original_page_count = std::nullopt,
                               },
                               true, target_started);
  if (!applied.has_value()) {
    return applied;
  }

  opened->reset();
  Status finalized;
  try {
    finalized = backend.DoFinalizeRollback();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  if (!finalized.has_value()) {
    return std::unexpected(std::move(finalized.error()));
  }
  return {};
}

}  // namespace modern_sqlite
