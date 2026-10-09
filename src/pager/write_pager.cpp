#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kPendingByte = 0x40000000ULL;
constexpr std::uint32_t kSqliteVersion = 3'054'000;

[[nodiscard]] Error MakeError(ErrorCode code, std::string_view message) noexcept {
  try {
    return Error::Create(code, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error Misuse(std::string_view message) noexcept {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] Error Busy(std::string_view message) noexcept {
  return MakeError(ErrorCode::kBusy, message);
}

[[nodiscard]] Error Locked(std::string_view message) noexcept {
  return MakeError(ErrorCode::kLocked, message);
}

[[nodiscard]] Error Protocol(std::string_view message) noexcept {
  return MakeError(ErrorCode::kProtocol, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Error Internal(std::string_view message) noexcept {
  return MakeError(ErrorCode::kInternal, message);
}

[[nodiscard]] std::uint32_t LockingPage(ByteCount page_size) noexcept {
  return static_cast<std::uint32_t>((kPendingByte / static_cast<std::uint64_t>(page_size.value())) +
                                    1U);
}

[[nodiscard]] Result<FileOffset> PageOffset(PageNumber page_number, ByteCount page_size) {
  const auto page_index = static_cast<std::uint64_t>(page_number.value() - 1U);
  const auto size = static_cast<std::uint64_t>(page_size.value());
  if (page_index > std::numeric_limits<std::uint64_t>::max() / size) {
    return std::unexpected(TooLarge("database page offset is not representable"));
  }
  return FileOffset{page_index * size};
}

[[nodiscard]] Result<FileSize> DatabaseSize(std::uint32_t page_count, ByteCount page_size) {
  const auto count = static_cast<std::uint64_t>(page_count);
  const auto size = static_cast<std::uint64_t>(page_size.value());
  if (count > std::numeric_limits<std::uint64_t>::max() / size) {
    return std::unexpected(TooLarge("database file size is not representable"));
  }
  return FileSize{count * size};
}

}  // namespace

class Pager::RecoveryTarget final : public JournalRecoveryTarget {
 public:
  RecoveryTarget(Pager& pager, bool write_database) noexcept
      : pager_(&pager), write_database_(write_database) {}

  [[nodiscard]] Status PreparePlayback(JournalPlaybackInfo info) override {
    info_ = info;
    if (pager_->cache_ != nullptr && pager_->cache_->pin_count() != 0) {
      return std::unexpected(Busy("journal playback cannot replace pinned pages"));
    }

    if (info.kind == JournalPlaybackKind::kSavepointRollback) {
      if (pager_->cache_ == nullptr || pager_->cache_->page_size() != info.page_size) {
        return std::unexpected(
            Internal("savepoint playback page size does not match the pager cache"));
      }
      return {};
    }

    if (pager_->cache_ == nullptr || pager_->cache_->page_size() != info.page_size) {
      auto replacement = PageCache::Create(PageCacheOptions{
          .page_size = info.page_size,
          .capacity_pages = pager_->options_.cache_capacity_pages,
      });
      if (!replacement.has_value()) {
        return std::unexpected(std::move(replacement.error()));
      }
      pager_->cache_ = std::move(*replacement);
      return {};
    }
    return pager_->cache_->Clear();
  }

  [[nodiscard]] Status ResizeDatabase(std::uint32_t page_count) override {
    assert(info_.has_value());
    pager_->current_page_count_ = page_count;
    if (info_->kind == JournalPlaybackKind::kSavepointRollback) {
      auto discarded = pager_->cache_->DiscardAfter(page_count);
      if (!discarded.has_value() || !write_database_) {
        return discarded;
      }

      auto target_size = DatabaseSize(page_count, info_->page_size);
      if (!target_size.has_value()) {
        return std::unexpected(std::move(target_size.error()));
      }
      auto current_size = pager_->file_->Size();
      if (!current_size.has_value()) {
        return std::unexpected(std::move(current_size.error()));
      }
      if (*current_size <= *target_size) {
        return {};
      }
      auto truncated = pager_->file_->Truncate(*target_size);
      if (!truncated.has_value()) {
        return std::unexpected(std::move(truncated.error()));
      }
      wrote_database_ = true;
      return {};
    }
    if (!write_database_) {
      return {};
    }

    auto size = DatabaseSize(page_count, info_->page_size);
    if (!size.has_value()) {
      return std::unexpected(std::move(size.error()));
    }
    auto truncated = pager_->file_->Truncate(*size);
    if (!truncated.has_value()) {
      return std::unexpected(std::move(truncated.error()));
    }
    wrote_database_ = true;
    return {};
  }

  [[nodiscard]] Status RestorePage(JournalPageImage image) override {
    assert(info_.has_value());
    if (info_->kind != JournalPlaybackKind::kSavepointRollback) {
      if (!write_database_) {
        return {};
      }
      return WriteDatabasePage(image);
    }
    if (image.page_number == PageNumber{1}) {
      restored_page_one_ = true;
    }

    auto found = pager_->cache_->LookupExclusive(image.page_number);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }

    std::optional<PageCache::Pin> pin;
    if (found->has_value()) {
      pin.emplace(std::move(found->value()));
      auto dirty = pin->MarkDirty();
      if (!dirty.has_value()) {
        return dirty;
      }
      auto bytes = pin->mutable_bytes();
      if (!bytes.has_value()) {
        return std::unexpected(std::move(bytes.error()));
      }
      std::ranges::copy(image.bytes, bytes->begin());
    }

    if (write_database_) {
      auto written = WriteDatabasePage(image);
      if (!written.has_value()) {
        return written;
      }
      if (pin.has_value()) {
        auto clean = pin->MarkClean();
        if (!clean.has_value()) {
          return clean;
        }
      }
    }
    return {};
  }

  [[nodiscard]] Status SyncDatabase() override {
    if (!wrote_database_) {
      return {};
    }
    return pager_->file_->Sync(SyncOptions{
        .mode = SyncMode::kNormal,
        .data_only = false,
    });
  }

  [[nodiscard]] Status CompletePlayback(JournalPlaybackInfo info) override {
    if (info.kind == JournalPlaybackKind::kSavepointRollback) {
      return pager_->RefreshCurrentHeader();
    }
    return {};
  }

  [[nodiscard]] bool restored_page_one() const noexcept { return restored_page_one_; }

 private:
  [[nodiscard]] Status WriteDatabasePage(JournalPageImage image) {
    if (!info_.has_value()) {
      return std::unexpected(Internal("recovery target database write requires playback metadata"));
    }
    auto offset = PageOffset(image.page_number, info_->page_size);
    if (!offset.has_value()) {
      return std::unexpected(std::move(offset.error()));
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPagesWritten, 1U);
    auto written = pager_->file_->WriteAt(image.bytes, *offset);
    if (!written.has_value()) {
      return std::unexpected(std::move(written.error()));
    }
    wrote_database_ = true;
    return {};
  }

  Pager* pager_;
  bool write_database_;
  bool wrote_database_ = false;
  bool restored_page_one_ = false;
  std::optional<JournalPlaybackInfo> info_;
};

Status Pager::BeginWrite() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (!writable()) {
    return std::unexpected(MakeError(ErrorCode::kReadOnly, "pager was not opened for writing"));
  }
  if (state_ != PagerState::kReader) {
    return std::unexpected(Misuse("write transaction requires reader state"));
  }
  if (current_header_.has_value() &&
      (current_header_->write_version() != 1U || current_header_->read_version() != 1U)) {
    return std::unexpected(Protocol("database format does not permit rollback-journal writes"));
  }

  if (storage_mode_ != StorageMode::kEphemeral) {
    auto locked = file_->Lock(DatabaseLock::kReserved);
    if (!locked.has_value()) {
      return std::unexpected(std::move(locked.error()));
    }
    database_lock_ = DatabaseLock::kReserved;
  }
  transaction_start_header_ = current_header_;
  transaction_start_change_token_ = last_change_token_;
  transaction_start_page_count_ = current_page_count_;
  transaction_modified_ = false;
  database_bytes_modified_ = false;
  image_size_changed_ = false;
  change_counter_updated_ = false;
  final_image_ = false;
  journal_finalized_ = false;
  completion_ = WriteCompletion::kNone;
  AdvanceWriteTransactionGeneration();
  write_coordinator_claimed_ = false;
  write_coordinator_failure_.reset();
  write_attempt_sealed_ = false;
  page_content_required_.reset();
  state_ = PagerState::kWriterLocked;
  return {};
}

Status Pager::ClaimWriteCoordinator() {
  try {
    if (state_ == PagerState::kError) {
      return StoredError();
    }
    if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
      return std::unexpected(
          Misuse("write coordinator claim requires a mutable write transaction"));
    }
    if (const auto failure = write_failure_code(); failure.has_value()) {
      return std::unexpected(
          MakeError(*failure, "write coordinator claim requires transaction rollback"));
    }
    if (write_coordinator_claimed_) {
      return std::unexpected(Locked("the write transaction already has a coordinator"));
    }
    if (write_attempt_sealed_) {
      return std::unexpected(Misuse("write coordinator claim requires commit or rollback cleanup"));
    }
    write_coordinator_claimed_ = true;
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<WritePagePin> Pager::WritePage(PageNumber page_number) {
  try {
    if (state_ == PagerState::kError) {
      return std::unexpected(StoredError().error());
    }
    if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
      return std::unexpected(Misuse("writable page requires an active write transaction"));
    }
    if (const auto failure = write_failure_code(); failure.has_value()) {
      return std::unexpected(MakeError(*failure, "writable page requires transaction rollback"));
    }
    if (write_attempt_sealed_) {
      return std::unexpected(Misuse("writable page requires commit or rollback cleanup"));
    }
    if (final_image_) {
      return std::unexpected(Misuse("logical truncation has finalized the transaction image"));
    }

    auto valid = ValidatePageNumber(page_number);
    if (!valid.has_value()) {
      return std::unexpected(std::move(valid.error()));
    }
    auto pressure = MaintainCachePressure();
    if (!pressure.has_value()) {
      return std::unexpected(std::move(pressure.error()));
    }
    auto pin = AcquirePage(page_number, true);
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    auto prepared = PreparePageWrite(page_number, *pin, current_page_count_);
    if (!prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    auto dirty = pin->MarkDirty();
    if (!dirty.has_value()) {
      return std::unexpected(std::move(dirty.error()));
    }

    transaction_modified_ = true;
    if (state_ == PagerState::kWriterLocked) {
      state_ = PagerState::kWriterCacheModified;
    }
    return WritePagePin{std::move(*pin)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<WritePagePin> Pager::WritePage(ReadPagePin&& read_pin) {
  try {
    if (state_ == PagerState::kError) {
      return std::unexpected(StoredError().error());
    }
    if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
      return std::unexpected(Misuse("writable page requires an active write transaction"));
    }
    if (const auto failure = write_failure_code(); failure.has_value()) {
      return std::unexpected(MakeError(*failure, "writable page requires transaction rollback"));
    }
    if (write_attempt_sealed_) {
      return std::unexpected(Misuse("writable page requires commit or rollback cleanup"));
    }
    if (final_image_) {
      return std::unexpected(Misuse("logical truncation has finalized the transaction image"));
    }

    PageCache::Pin pin = std::move(read_pin.pin_);
    if (!cache_->OwnsPinForPager(pin)) {
      return std::unexpected(Misuse("page promotion requires a pin from this pager"));
    }
    const PageNumber page_number = pin.frame().page_number();
    auto valid = ValidatePageNumber(page_number);
    if (!valid.has_value()) {
      return std::unexpected(std::move(valid.error()));
    }
    auto pressure = MaintainCachePressure();
    if (!pressure.has_value()) {
      return std::unexpected(std::move(pressure.error()));
    }

    auto promoted = cache_->PromoteExclusiveForPager(pin);
    if (!promoted.has_value()) {
      return std::unexpected(std::move(promoted.error()));
    }
    auto prepared = PreparePageWrite(page_number, pin, current_page_count_);
    if (!prepared.has_value()) {
      return std::unexpected(std::move(prepared.error()));
    }
    auto dirty = pin.MarkDirty();
    if (!dirty.has_value()) {
      return std::unexpected(std::move(dirty.error()));
    }

    transaction_modified_ = true;
    if (state_ == PagerState::kWriterLocked) {
      state_ = PagerState::kWriterCacheModified;
    }
    return WritePagePin{std::move(pin)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::uint32_t> Pager::IncrementSchemaCookie() {
  if (current_page_count_ == 0U) {
    return std::unexpected(Misuse("schema cookie increment requires an initialized database"));
  }
  auto page_one = WritePage(PageNumber{1});
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  const MutableByteView bytes = page_one->mutable_bytes();
  constexpr std::size_t kSchemaCookieOffset = 40U;
  if (bytes.size() < kSchemaCookieOffset + sizeof(std::uint32_t)) {
    return std::unexpected(
        MakeError(ErrorCode::kCorruption, "database page 1 is too small for the schema cookie"));
  }
  const auto encoded = std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + kSchemaCookieOffset, sizeof(std::uint32_t)};
  const std::uint32_t next = LoadBigEndian<std::uint32_t>(encoded) + 1U;
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + kSchemaCookieOffset,
                                                  sizeof(std::uint32_t)},
      next);
  auto parsed = ParseDatabaseHeader(bytes);
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  current_header_ = *parsed;
  return next;
}

Status Pager::PermutePageNumbers(std::span<const PageNumberRekey> pages) {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
    return std::unexpected(Misuse("page permutation requires an active write transaction"));
  }
  if (const auto failure = write_failure_code(); failure.has_value()) {
    return std::unexpected(MakeError(*failure, "page permutation requires transaction rollback"));
  }
  if (write_attempt_sealed_ || final_image_) {
    return std::unexpected(Misuse("page permutation requires a mutable transaction image"));
  }
  if (pages.size() > 5U) {
    return std::unexpected(Misuse("page permutation supports at most five pages"));
  }
  if (pages.empty()) {
    return {};
  }

  std::array<PageNumber, 5> current{};
  std::array<PageNumber, 5> target{};
  for (std::size_t index = 0; index < pages.size(); ++index) {
    if (pages[index].pin == nullptr) {
      return std::unexpected(Misuse("page permutation contains a null pin"));
    }
    const WritePagePin& pin = *pages[index].pin;
    if (!cache_->OwnsPinForPager(pin.pin_)) {
      return std::unexpected(Misuse("page permutation requires pins from this pager"));
    }
    if (!pin.pin_.exclusive() || !pin.frame().dirty()) {
      return std::unexpected(Misuse("page permutation requires dirty exclusive pins"));
    }
    current[index] = pin.frame().page_number();
    target[index] = pages[index].final_page;
    auto valid = ValidatePageNumber(target[index]);
    if (!valid.has_value()) {
      return valid;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (current[prior] == current[index] || target[prior] == target[index]) {
        return std::unexpected(Misuse("page permutation contains duplicate pages"));
      }
    }
  }
  for (std::size_t index = 0; index < pages.size(); ++index) {
    const auto targets = std::span{target}.first(pages.size());
    if (std::ranges::find(targets, current[index]) == targets.end()) {
      return std::unexpected(Misuse("page permutation targets must match its source pages"));
    }
  }
  if (pages.size() == 1U) {
    return {};
  }

  for (std::size_t index = 0; index < pages.size(); ++index) {
    auto prepared = PreparePageWrite(current[index], pages[index].pin->pin_, current_page_count_);
    if (!prepared.has_value()) {
      return prepared;
    }
  }

  const PageNumber temporary{LockingPage(page_size())};
  std::array<bool, 5> complete{};
  for (std::size_t start = 0; start < pages.size(); ++start) {
    if (complete[start] || current[start] == target[start]) {
      complete[start] = true;
      continue;
    }
    auto moved = cache_->RekeyExclusiveForPager(pages[start].pin->pin_, temporary);
    if (!moved.has_value()) {
      return EnterError(std::move(moved.error()));
    }
    PageNumber hole = current[start];
    current[start] = temporary;
    while (target[start] != hole) {
      std::size_t next = pages.size();
      for (std::size_t index = 0; index < pages.size(); ++index) {
        if (index != start && target[index] == hole) {
          next = index;
          break;
        }
      }
      if (next == pages.size()) {
        return EnterError(Internal("page permutation cycle is inconsistent"));
      }
      const PageNumber next_hole = current[next];
      moved = cache_->RekeyExclusiveForPager(pages[next].pin->pin_, hole);
      if (!moved.has_value()) {
        return EnterError(std::move(moved.error()));
      }
      current[next] = hole;
      complete[next] = true;
      hole = next_hole;
    }
    moved = cache_->RekeyExclusiveForPager(pages[start].pin->pin_, hole);
    if (!moved.has_value()) {
      return EnterError(std::move(moved.error()));
    }
    current[start] = hole;
    complete[start] = true;
  }
  return {};
}

Result<WritePagePin> Pager::AllocatePage() {
  try {
    if (state_ == PagerState::kError) {
      return std::unexpected(StoredError().error());
    }
    if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
      return std::unexpected(Misuse("page allocation requires an active write transaction"));
    }
    if (const auto failure = write_failure_code(); failure.has_value()) {
      return std::unexpected(MakeError(*failure, "page allocation requires transaction rollback"));
    }
    if (write_attempt_sealed_) {
      return std::unexpected(Misuse("page allocation requires commit or rollback cleanup"));
    }
    if (final_image_) {
      return std::unexpected(Misuse("logical truncation has finalized the transaction image"));
    }

    auto pressure = MaintainCachePressure();
    if (!pressure.has_value()) {
      return std::unexpected(std::move(pressure.error()));
    }
    if (current_page_count_ == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(TooLarge("database page-number space is exhausted"));
    }
    std::uint32_t candidate = current_page_count_ + 1U;
    if (candidate == LockingPage(page_size())) {
      if (candidate == std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(TooLarge("database page-number space is exhausted"));
      }
      ++candidate;
    }

    std::optional<Error> setup_error;
    {
      ByteBuffer bytes{page_size()};
      auto inserted = cache_->InsertExclusive(PageNumber{candidate}, std::move(bytes));
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
      PageCache::Pin pin = std::move(*inserted);

      auto prepared = PreparePageWrite(PageNumber{candidate}, pin, candidate);
      if (!prepared.has_value()) {
        setup_error.emplace(std::move(prepared.error()));
      } else {
        auto dirty = pin.MarkDirty();
        if (!dirty.has_value()) {
          setup_error.emplace(std::move(dirty.error()));
        } else {
          current_page_count_ = candidate;
          transaction_modified_ = true;
          image_size_changed_ = true;
          if (state_ == PagerState::kWriterLocked) {
            state_ = PagerState::kWriterCacheModified;
          }
          return WritePagePin{std::move(pin)};
        }
      }
    }
    auto discarded = cache_->Discard(PageNumber{candidate});
    if (!discarded.has_value()) {
      return std::unexpected(std::move(discarded.error()));
    }
    return std::unexpected(std::move(*setup_error));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status Pager::MarkPageContentRequired(PageNumber page_number) {
  auto valid = ValidatePageNumber(page_number);
  if (!valid.has_value()) {
    return valid;
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
    return std::unexpected(Misuse("page content history requires an active write transaction"));
  }
  if (!page_content_required_.has_value()) {
    auto created = PageBitvec::Create(current_page_count_);
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    page_content_required_.emplace(std::move(*created));
  }
  if (page_number.value() <= page_content_required_->size()) {
    return page_content_required_->Set(page_number);
  }
  return {};
}

bool Pager::PageContentRequired(PageNumber page_number) const noexcept {
  if (!page_content_required_.has_value() || page_number.value() == 0U) {
    return false;
  }
  if (page_number.value() > page_content_required_->size()) {
    return true;
  }
  return page_content_required_->Test(page_number);
}

Status Pager::TruncateImage(std::uint32_t page_count) {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished) {
    return std::unexpected(Misuse("logical truncation requires an active write transaction"));
  }
  if (const auto failure = write_failure_code(); failure.has_value()) {
    return std::unexpected(MakeError(*failure, "logical truncation requires transaction rollback"));
  }
  if (write_attempt_sealed_) {
    return std::unexpected(Misuse("logical truncation requires commit or rollback cleanup"));
  }
  if (final_image_) {
    return std::unexpected(Misuse("logical truncation already finalized the transaction image"));
  }
  if (page_count > current_page_count_) {
    return std::unexpected(Misuse("logical truncation cannot extend the database image"));
  }
  if (page_count == current_page_count_) {
    return {};
  }
  if (page_count == 0U) {
    return std::unexpected(Misuse("a nonempty database cannot commit a zero-page image"));
  }
  if (cache_ == nullptr || cache_->pin_count() != 0) {
    return std::unexpected(Busy("logical truncation requires all page pins to be released"));
  }

  {
    auto page_one = cache_->LookupExclusive(PageNumber{1});
    if (!page_one.has_value()) {
      return std::unexpected(std::move(page_one.error()));
    }
    if (!page_one->has_value() || !(**page_one)->dirty()) {
      return std::unexpected(Misuse("logical truncation requires a dirty cached page 1"));
    }
    const ByteView bytes = (***page_one).bytes();
    const auto encoded_page_count =
        LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
            bytes.data() + 28, sizeof(std::uint32_t)});
    if (encoded_page_count != page_count) {
      return std::unexpected(Misuse("page 1 does not contain the requested logical page count"));
    }
  }

  auto discarded = cache_->DiscardAfter(page_count);
  if (!discarded.has_value()) {
    return discarded;
  }
  current_page_count_ = page_count;
  transaction_modified_ = true;
  image_size_changed_ = true;
  final_image_ = true;
  if (state_ == PagerState::kWriterLocked) {
    state_ = PagerState::kWriterCacheModified;
  }
  return {};
}

Result<JournalSavepointId> Pager::CreateSavepoint() {
  if (state_ == PagerState::kError) {
    return std::unexpected(StoredError().error());
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished || final_image_) {
    return std::unexpected(Misuse("savepoint creation requires a mutable write transaction"));
  }
  if (storage_mode_ == StorageMode::kEphemeral) {
    return std::unexpected(Misuse("ephemeral pager does not support savepoints"));
  }
  if (const auto failure = write_failure_code(); failure.has_value()) {
    return std::unexpected(MakeError(*failure, "savepoint creation requires transaction rollback"));
  }
  if (write_attempt_sealed_) {
    return std::unexpected(Misuse("savepoint creation requires commit or rollback cleanup"));
  }
  if (cache_ != nullptr && cache_->exclusive_pin_count() != 0U) {
    return std::unexpected(Busy("savepoint creation requires all write pins to be released"));
  }
  auto journal = EnsureJournalTransaction();
  if (!journal.has_value()) {
    return std::unexpected(std::move(journal.error()));
  }
  return journal_transaction_->CreateSavepoint(current_page_count_);
}

Status Pager::ReleaseSavepoint(JournalSavepointId savepoint) {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (storage_mode_ == StorageMode::kEphemeral) {
    return std::unexpected(Misuse("ephemeral pager does not support savepoints"));
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished || final_image_ ||
      journal_transaction_ == nullptr) {
    return std::unexpected(Misuse("savepoint release requires a mutable write transaction"));
  }
  if (const auto failure = write_failure_code(); failure.has_value()) {
    return std::unexpected(MakeError(*failure, "savepoint release requires transaction rollback"));
  }
  if (write_attempt_sealed_) {
    return std::unexpected(Misuse("savepoint release requires commit or rollback cleanup"));
  }
  return journal_transaction_->ReleaseSavepoint(savepoint);
}

Status Pager::RollbackToSavepoint(JournalSavepointId savepoint) {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (storage_mode_ == StorageMode::kEphemeral) {
    return std::unexpected(Misuse("ephemeral pager does not support savepoints"));
  }
  if (!in_write_transaction() || state_ == PagerState::kWriterFinished || final_image_ ||
      journal_transaction_ == nullptr) {
    return std::unexpected(Misuse("savepoint rollback requires a mutable write transaction"));
  }
  if (cache_ != nullptr && cache_->pin_count() != 0) {
    return std::unexpected(Busy("savepoint rollback requires all page pins to be released"));
  }

  write_attempt_sealed_ = true;
  AdvanceWriteTransactionGeneration();
  RecoveryTarget target{*this, database_bytes_modified_};
  auto rolled_back = journal_transaction_->RollbackToSavepoint(savepoint, target);
  if (!rolled_back.has_value()) {
    if (journal_transaction_->state() == JournalTransactionState::kError) {
      return EnterError(std::move(rolled_back.error()));
    }
    return rolled_back;
  }
  if (target.restored_page_one()) {
    change_counter_updated_ = false;
  }
  image_size_changed_ = current_page_count_ != transaction_start_page_count_;
  const bool restored_empty_transaction =
      current_page_count_ == 0U && transaction_start_page_count_ == 0U;
  if (restored_empty_transaction) {
    transaction_modified_ = false;
    change_counter_updated_ = false;
    state_ =
        database_bytes_modified_ ? PagerState::kWriterDatabaseModified : PagerState::kWriterLocked;
  } else {
    state_ = database_bytes_modified_ ? PagerState::kWriterDatabaseModified
                                      : PagerState::kWriterCacheModified;
  }
  write_coordinator_failure_.reset();
  write_attempt_sealed_ = false;
  write_coordinator_claimed_ = false;
  return {};
}

Status Pager::Commit() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (storage_mode_ == StorageMode::kEphemeral) {
    return std::unexpected(Misuse("ephemeral pager is discarded instead of committed"));
  }
  if (state_ == PagerState::kWriterFinished) {
    if (completion_ != WriteCompletion::kCommit) {
      return std::unexpected(Misuse("rollback cleanup cannot be completed by commit"));
    }
    return CompleteWriteCleanup(WriteCompletion::kCommit);
  }
  if (!in_write_transaction()) {
    return std::unexpected(Misuse("commit requires an active write transaction"));
  }
  if (const auto failure = write_failure_code(); failure.has_value()) {
    return std::unexpected(MakeError(*failure, "commit requires transaction rollback"));
  }
  if (cache_ != nullptr && cache_->pin_count() != 0) {
    return std::unexpected(Busy("commit requires all page pins to be released"));
  }

  write_attempt_sealed_ = true;
  AdvanceWriteTransactionGeneration();
  if (!transaction_modified_) {
    if (journal_transaction_ != nullptr) {
      RecoveryTarget target{*this, false};
      auto discarded = journal_transaction_->Rollback(target);
      if (!discarded.has_value()) {
        if (journal_transaction_->state() == JournalTransactionState::kError) {
          return EnterError(std::move(discarded.error()));
        }
        return discarded;
      }
    }
    journal_finalized_ = true;
    completion_ = WriteCompletion::kCommit;
    state_ = PagerState::kWriterFinished;
    return CompleteWriteCleanup(WriteCompletion::kCommit);
  }

  auto journal = EnsureJournalTransaction();
  if (!journal.has_value()) {
    return journal;
  }
  auto counter = UpdateChangeCounter();
  if (!counter.has_value()) {
    return counter;
  }
  auto exclusive = EnsureExclusiveLock();
  if (!exclusive.has_value()) {
    return exclusive;
  }
  auto synced_journal = journal_transaction_->SyncJournal();
  if (!synced_journal.has_value()) {
    return synced_journal;
  }
  auto flushed = FlushDirtyPages();
  if (!flushed.has_value()) {
    return flushed;
  }
  auto synced_database = file_->Sync(SyncOptions{
      .mode = SyncMode::kNormal,
      .data_only = false,
  });
  if (!synced_database.has_value()) {
    journal_transaction_->ReportDatabaseFailure(synced_database.error().code());
    return synced_database;
  }
  auto marked = journal_transaction_->MarkDatabaseSynced();
  if (!marked.has_value()) {
    return marked;
  }

  completion_ = WriteCompletion::kCommit;
  state_ = PagerState::kWriterFinished;
  auto finalized = journal_transaction_->Commit();
  if (!finalized.has_value()) {
    return EnterError(std::move(finalized.error()));
  }
  journal_finalized_ = true;
  return CompleteWriteCleanup(WriteCompletion::kCommit);
}

Status Pager::Rollback() {
  if (state_ == PagerState::kError) {
    return StoredError();
  }
  if (storage_mode_ == StorageMode::kEphemeral) {
    return std::unexpected(Misuse("ephemeral pager is discarded instead of rolled back"));
  }
  if (state_ == PagerState::kWriterFinished) {
    if (completion_ != WriteCompletion::kRollback) {
      return std::unexpected(Misuse("committed transaction cannot be rolled back"));
    }
    return CompleteWriteCleanup(WriteCompletion::kRollback);
  }
  if (!in_write_transaction()) {
    return std::unexpected(Misuse("rollback requires an active write transaction"));
  }
  if (cache_ != nullptr && cache_->pin_count() != 0) {
    return std::unexpected(Busy("rollback requires all page pins to be released"));
  }

  write_attempt_sealed_ = true;
  AdvanceWriteTransactionGeneration();
  if (journal_transaction_ != nullptr) {
    RecoveryTarget target{*this, database_bytes_modified_};
    auto rolled_back = journal_transaction_->Rollback(target);
    if (!rolled_back.has_value()) {
      if (journal_transaction_->state() == JournalTransactionState::kError) {
        return EnterError(std::move(rolled_back.error()));
      }
      return rolled_back;
    }
  }

  journal_finalized_ = true;
  completion_ = WriteCompletion::kRollback;
  state_ = PagerState::kWriterFinished;
  return CompleteWriteCleanup(WriteCompletion::kRollback);
}

Status Pager::RecoverHotJournalIfNeeded() {
  auto size = file_->Size();
  if (!size.has_value()) {
    return std::unexpected(std::move(size.error()));
  }
  auto hot = CheckHotJournal(*size);
  if (!hot.has_value()) {
    return std::unexpected(std::move(hot.error()));
  }
  if (!*hot) {
    return {};
  }
  if (!writable()) {
    return std::unexpected(
        Protocol("hot rollback journal requires recovery before the database can be read"));
  }

  auto exclusive = file_->Lock(DatabaseLock::kExclusive);
  if (!exclusive.has_value()) {
    Error lock_error = std::move(exclusive.error());
    auto unlocked = file_->Unlock(DatabaseLock::kNone);
    if (unlocked.has_value()) {
      database_lock_ = DatabaseLock::kNone;
      return std::unexpected(std::move(lock_error));
    }
    return std::unexpected(std::move(unlocked.error()));
  }
  database_lock_ = DatabaseLock::kExclusive;

  RecoveryTarget target{*this, true};
  auto recovered = RecoverHotJournal(*rollback_journal_, target);
  if (!recovered.has_value()) {
    Error error = std::move(recovered.error());
    const auto unlocked = file_->Unlock(DatabaseLock::kNone);
    if (unlocked.has_value()) {
      database_lock_ = DatabaseLock::kNone;
    }
    return EnterError(std::move(error));
  }
  auto downgraded = file_->Unlock(DatabaseLock::kShared);
  if (!downgraded.has_value()) {
    return EnterError(std::move(downgraded.error()));
  }
  database_lock_ = DatabaseLock::kShared;
  return {};
}

Status Pager::MaintainCachePressure() {
  if (!in_write_transaction() || state_ == PagerState::kWriterLocked ||
      state_ == PagerState::kWriterFinished || cache_ == nullptr) {
    return {};
  }
  while (true) {
    const PageCachePressure pressure = cache_->pressure();
    if (pressure.excess_pages == 0 || !pressure.writeback.has_value()) {
      return {};
    }
    auto spilled = SpillPage(pressure.writeback->page_number);
    if (!spilled.has_value()) {
      return spilled;
    }
  }
}

Status Pager::EnsureJournalTransaction() {
  if (journal_transaction_ != nullptr) {
    if (journal_transaction_->state() == JournalTransactionState::kActive) {
      return {};
    }
    const ErrorCode code = journal_transaction_->failure_code().value_or(ErrorCode::kInternal);
    return std::unexpected(MakeError(code, "journal transaction requires full rollback"));
  }
  if (rollback_journal_ == nullptr || journal_sector_size_.value() == 0) {
    return std::unexpected(Internal("writable pager has no rollback journal"));
  }
  auto begun = JournalTransaction::Begin(*rollback_journal_,
                                         JournalTransactionInfo{
                                             .page_size = page_size(),
                                             .sector_size = journal_sector_size_,
                                             .original_page_count = transaction_start_page_count_,
                                         });
  if (!begun.has_value()) {
    return std::unexpected(std::move(begun.error()));
  }
  journal_transaction_ = std::move(*begun);
  return {};
}

Status Pager::PreparePageWrite(PageNumber page_number, const PageCache::Pin& target,
                               std::uint32_t logical_page_count) {
  if (storage_mode_ == StorageMode::kEphemeral) {
    return {};
  }
  auto journal = EnsureJournalTransaction();
  if (!journal.has_value()) {
    return journal;
  }
  return CaptureSector(page_number, target, logical_page_count);
}

std::optional<ErrorCode> Pager::write_failure_code() const noexcept {
  if (write_coordinator_failure_.has_value()) {
    return write_coordinator_failure_;
  }
  if (journal_transaction_ == nullptr) {
    return std::nullopt;
  }
  const JournalTransactionState journal_state = journal_transaction_->state();
  if (journal_state != JournalTransactionState::kFailed &&
      journal_state != JournalTransactionState::kError) {
    return std::nullopt;
  }
  return journal_transaction_->failure_code().value_or(ErrorCode::kInternal);
}

void Pager::ReportWriteCoordinatorFailure(ErrorCode code) noexcept {
  if (!write_coordinator_failure_.has_value()) {
    write_coordinator_failure_ = code;
  }
}

Status Pager::CaptureSector(PageNumber page_number, const PageCache::Pin& target,
                            std::uint32_t logical_page_count) {
  assert(journal_transaction_ != nullptr);
  const std::uint64_t page_size_value = page_size().value();
  const std::uint64_t sector_size_value = journal_sector_size_.value();
  const std::uint64_t pages_per_sector =
      sector_size_value > page_size_value ? sector_size_value / page_size_value : 1U;
  const std::uint64_t requested = page_number.value();
  const std::uint64_t first = ((requested - 1U) / pages_per_sector) * pages_per_sector + 1U;
  const std::uint64_t last = first + pages_per_sector - 1U;
  const std::uint64_t upper =
      std::min<std::uint64_t>(last, std::max(logical_page_count, transaction_start_page_count_));
  const std::uint32_t locking_page = LockingPage(page_size());

  for (std::uint64_t current = first; current <= upper; ++current) {
    if (current == locking_page) {
      continue;
    }
    const PageNumber current_page{static_cast<std::uint32_t>(current)};
    if (!journal_transaction_->NeedsCapture(current_page)) {
      continue;
    }
    if (current_page == page_number) {
      auto captured = journal_transaction_->CapturePage(JournalPageImage{
          .page_number = current_page,
          .bytes = target.frame().bytes(),
      });
      if (!captured.has_value()) {
        return captured;
      }
      continue;
    }

    auto neighbor = AcquirePage(current_page, false);
    if (!neighbor.has_value()) {
      return std::unexpected(std::move(neighbor.error()));
    }
    auto captured = journal_transaction_->CapturePage(JournalPageImage{
        .page_number = current_page,
        .bytes = neighbor->frame().bytes(),
    });
    if (!captured.has_value()) {
      return captured;
    }
  }
  return {};
}

Status Pager::EnsureExclusiveLock() {
  if (database_lock_ == DatabaseLock::kExclusive) {
    return {};
  }
  auto locked = file_->Lock(DatabaseLock::kExclusive);
  if (!locked.has_value()) {
    return std::unexpected(std::move(locked.error()));
  }
  database_lock_ = DatabaseLock::kExclusive;
  return {};
}

Status Pager::SpillPage(PageNumber page_number) {
  auto pin = cache_->LookupExclusive(page_number);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  if (!pin->has_value()) {
    return std::unexpected(Internal("dirty writeback candidate is absent from the cache"));
  }
  if (!(**pin)->dirty()) {
    return std::unexpected(Internal("writeback candidate is not dirty"));
  }

  if (storage_mode_ == StorageMode::kEphemeral) {
    auto offset = PageOffset(page_number, page_size());
    if (!offset.has_value()) {
      ReportWriteCoordinatorFailure(offset.error().code());
      return std::unexpected(std::move(offset.error()));
    }
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPagesWritten, 1U);
    auto written = file_->WriteAt((***pin).bytes(), *offset);
    if (!written.has_value()) {
      ReportWriteCoordinatorFailure(written.error().code());
      return written;
    }
    auto clean = (**pin).MarkClean();
    if (!clean.has_value()) {
      ReportWriteCoordinatorFailure(clean.error().code());
      return clean;
    }
    database_bytes_modified_ = true;
    state_ = PagerState::kWriterDatabaseModified;
    return {};
  }

  assert(journal_transaction_ != nullptr);
  auto exclusive = EnsureExclusiveLock();
  if (!exclusive.has_value()) {
    return exclusive;
  }
  auto synced = journal_transaction_->SyncJournal();
  if (!synced.has_value()) {
    return synced;
  }
  auto authorized = journal_transaction_->AuthorizeDatabaseWrite(page_number);
  if (!authorized.has_value()) {
    return authorized;
  }
  database_bytes_modified_ = true;
  auto offset = PageOffset(page_number, page_size());
  if (!offset.has_value()) {
    journal_transaction_->ReportDatabaseFailure(offset.error().code());
    return std::unexpected(std::move(offset.error()));
  }
  MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kPagesWritten, 1U);
  auto written = file_->WriteAt((***pin).bytes(), *offset);
  if (!written.has_value()) {
    journal_transaction_->ReportDatabaseFailure(written.error().code());
    return written;
  }
  auto clean = (**pin).MarkClean();
  if (!clean.has_value()) {
    return clean;
  }
  state_ = PagerState::kWriterDatabaseModified;
  return {};
}

Status Pager::UpdateChangeCounter() {
  if (change_counter_updated_) {
    return {};
  }
  if (current_page_count_ == 0U) {
    return std::unexpected(Misuse("modified transactions require database page 1"));
  }
  auto page_one = AcquirePage(PageNumber{1}, true);
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  if (!page_one->frame().dirty()) {
    if (final_image_) {
      return std::unexpected(Misuse("logical truncation requires page 1 to remain dirty"));
    }
    auto prepared = PreparePageWrite(PageNumber{1}, *page_one, current_page_count_);
    if (!prepared.has_value()) {
      return prepared;
    }
    auto dirty = page_one->MarkDirty();
    if (!dirty.has_value()) {
      return dirty;
    }
  } else if (storage_mode_ != StorageMode::kEphemeral &&
             journal_transaction_->NeedsCapture(PageNumber{1})) {
    auto captured = CaptureSector(PageNumber{1}, *page_one, current_page_count_);
    if (!captured.has_value()) {
      return captured;
    }
  }

  auto mutable_bytes = page_one->mutable_bytes();
  if (!mutable_bytes.has_value()) {
    return std::unexpected(std::move(mutable_bytes.error()));
  }
  auto parsed = ParseDatabaseHeader(*mutable_bytes);
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (parsed->write_version() != 1U || parsed->read_version() != 1U) {
    return std::unexpected(Protocol("committed database header must use rollback-journal format"));
  }
  if (parsed->page_size() != page_size()) {
    return std::unexpected(Misuse("page 1 page size does not match the active pager geometry"));
  }
  if (image_size_changed_) {
    const auto encoded_page_count =
        LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
            mutable_bytes->data() + 28, sizeof(std::uint32_t)});
    if (encoded_page_count != current_page_count_) {
      return std::unexpected(Misuse("page 1 logical page count does not match the pager image"));
    }
  }
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{mutable_bytes->data() + 28,
                                                  sizeof(std::uint32_t)},
      current_page_count_);

  const auto current_counter =
      LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
          mutable_bytes->data() + 24, sizeof(std::uint32_t)});
  const std::uint32_t next_counter = current_counter + 1U;
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{mutable_bytes->data() + 24,
                                                  sizeof(std::uint32_t)},
      next_counter);
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{mutable_bytes->data() + 92,
                                                  sizeof(std::uint32_t)},
      next_counter);
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{mutable_bytes->data() + 96,
                                                  sizeof(std::uint32_t)},
      kSqliteVersion);

  parsed = ParseDatabaseHeader(*mutable_bytes);
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  current_header_ = *parsed;
  change_counter_updated_ = true;
  return {};
}

Status Pager::FlushDirtyPages() {
  while (cache_->dirty_page_count() != 0) {
    const auto candidate = cache_->writeback_candidate();
    if (!candidate.has_value()) {
      return std::unexpected(Busy("dirty pages remain pinned during commit"));
    }
    auto spilled = SpillPage(candidate->page_number);
    if (!spilled.has_value()) {
      return spilled;
    }
  }
  return {};
}

Status Pager::CompleteWriteCleanup(WriteCompletion completion) {
  if (state_ != PagerState::kWriterFinished || completion_ != completion || !journal_finalized_) {
    return std::unexpected(Internal("write completion state is inconsistent"));
  }

  if (completion == WriteCompletion::kCommit && image_size_changed_) {
    auto target_size = DatabaseSize(current_page_count_, page_size());
    if (!target_size.has_value()) {
      return std::unexpected(std::move(target_size.error()));
    }
    auto actual_size = file_->Size();
    if (!actual_size.has_value()) {
      return std::unexpected(std::move(actual_size.error()));
    }
    if (*actual_size > *target_size) {
      auto truncated = file_->Truncate(*target_size);
      if (!truncated.has_value()) {
        return truncated;
      }
    }
  }

  auto downgraded = file_->Unlock(DatabaseLock::kShared);
  if (!downgraded.has_value()) {
    return std::unexpected(std::move(downgraded.error()));
  }
  database_lock_ = DatabaseLock::kShared;

  if (completion == WriteCompletion::kRollback) {
    current_header_ = transaction_start_header_;
    last_change_token_ = transaction_start_change_token_;
    current_page_count_ = transaction_start_page_count_;
  } else if (current_header_.has_value()) {
    last_change_token_ = current_header_->change_token_;
  }
  ResetWriteState();
  state_ = PagerState::kReader;
  return {};
}

Status Pager::RefreshCurrentHeader() {
  if (current_page_count_ == 0U) {
    current_header_.reset();
    return {};
  }
  auto page_one = AcquirePage(PageNumber{1}, false);
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  auto parsed = ParseDatabaseHeader(page_one->frame().bytes());
  if (!parsed.has_value()) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (parsed->page_size() != page_size()) {
    return std::unexpected(Misuse("page 1 page size does not match the active pager geometry"));
  }
  current_header_ = *parsed;
  return {};
}

Status Pager::EnterError(Error error) {
  persistent_error_ = error.code();
  state_ = PagerState::kError;
  return std::unexpected(std::move(error));
}

void Pager::ResetWriteState() noexcept {
  journal_transaction_.reset();
  transaction_start_header_.reset();
  transaction_start_change_token_.reset();
  transaction_start_page_count_ = 0;
  completion_ = WriteCompletion::kNone;
  transaction_modified_ = false;
  database_bytes_modified_ = false;
  image_size_changed_ = false;
  change_counter_updated_ = false;
  final_image_ = false;
  journal_finalized_ = false;
  write_coordinator_failure_.reset();
  write_attempt_sealed_ = false;
  page_content_required_.reset();
}

void Pager::AdvanceWriteTransactionGeneration() noexcept { ++write_transaction_generation_; }

}  // namespace modern_sqlite
