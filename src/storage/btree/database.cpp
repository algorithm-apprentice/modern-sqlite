#include "modern_sqlite/storage/btree/database.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "cursor_state.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/storage/btree/page.hpp"

namespace modern_sqlite {

namespace {
constexpr int kAbortRollbackSqliteCode = 516;
constexpr std::size_t kOverflowPageListGrowthFactor = 2U;

[[nodiscard]] Error CursorMisuse(std::string_view message) noexcept {
  try {
    return Error::Create(ErrorCode::kMisuse, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error CursorTripError(int sqlite_code) noexcept {
  try {
    auto error = Error::FromSqliteCode(sqlite_code, "B-tree cursor was invalidated by rollback");
    if (!error.has_value()) {
      std::terminate();
    }
    return std::move(*error);
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}
}  // namespace

template <typename OpenPager>
Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenWith(OpenPager&& open_pager) {
  try {
    auto database = std::make_unique<BtreeDatabase>(ConstructionKey{});
    auto pager = std::forward<OpenPager>(open_pager)();
    if (!pager.has_value()) {
      return std::unexpected(std::move(pager.error()));
    }
    database->pager_ = std::move(*pager);
    return database;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::Open(Vfs& vfs, std::string_view path,
                                                           PagerOptions options) {
  return OpenWith([&] { return Pager::Open(vfs, path, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenWritable(Vfs& vfs, std::string_view path,
                                                                   WritablePagerOptions options) {
  return OpenWith([&] { return Pager::OpenWritable(vfs, path, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenEphemeral(Vfs& services,
                                                                    PagerOptions options) {
  return OpenWith([&] { return Pager::OpenEphemeral(services, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenEphemeralMemory(Vfs& services,
                                                                          PagerOptions options) {
  return OpenWith([&] { return Pager::OpenEphemeralMemory(services, options); });
}

BtreeDatabase::~BtreeDatabase() { assert(cursors_ == nullptr); }

void BtreeDatabase::RegisterCursor(btree_internal::CursorState& cursor) noexcept {
  for (auto* peer = cursors_; peer != nullptr; peer = peer->next_) {
    if (peer->root_page_ == cursor.root_page_) {
      peer->multiple_ = true;
      cursor.multiple_ = true;
    }
  }
  cursor.next_ = cursors_;
  cursors_ = &cursor;
}

void BtreeDatabase::UnregisterCursor(btree_internal::CursorState& cursor) noexcept {
  auto** link = &cursors_;
  while (*link != nullptr && *link != &cursor) {
    link = &(*link)->next_;
  }
  if (*link == nullptr) {
    std::terminate();
  }
  *link = cursor.next_;
}

void BtreeDatabase::RelocateCursor(const btree_internal::CursorState* previous_identity,
                                   btree_internal::CursorState& replacement) noexcept {
  auto** link = &cursors_;
  while (*link != nullptr && *link != previous_identity) {
    link = &(*link)->next_;
  }
  if (*link == nullptr) {
    std::terminate();
  }
  *link = &replacement;
}

Status BtreeDatabase::SaveCursors(PageNumber root, btree_internal::CursorState* except) {
  bool found = false;
  for (auto* cursor = cursors_; cursor != nullptr; cursor = cursor->next_) {
    if (cursor == except || (root.value() != 0U && cursor->root_page_ != root)) {
      continue;
    }
    found = true;
    auto saved = cursor->SavePosition();
    if (!saved.has_value()) {
      return std::unexpected(std::move(saved.error()));
    }
  }
  if (!found && except != nullptr) {
    except->multiple_ = false;
  }
  return {};
}

bool BtreeDatabase::HasCursors(PageNumber root) const noexcept {
  for (const auto* cursor = cursors_; cursor != nullptr; cursor = cursor->next_) {
    if (cursor->root_page_ == root) {
      return true;
    }
  }
  return false;
}

Status BtreeDatabase::RollbackToSavepoint(JournalSavepointId savepoint) {
  if (pager().in_write_transaction()) {
    auto saved = SaveCursors(PageNumber{}, nullptr);
    if (!saved.has_value()) {
      return saved;
    }
  }
  return pager().RollbackToSavepoint(savepoint);
}

void BtreeDatabase::TripAllCursors(int sqlite_code) noexcept {
  for (auto* cursor = cursors_; cursor != nullptr; cursor = cursor->next_) {
    cursor->Trip(sqlite_code);
  }
}

Status BtreeDatabase::TripCursors(BtreeCursorTripMode mode) {
  switch (mode) {
    case BtreeCursorTripMode::All:
      TripAllCursors(kAbortRollbackSqliteCode);
      return {};
    case BtreeCursorTripMode::WriteOnly:
      break;
    default:
      return std::unexpected(CursorMisuse("B-tree cursor trip mode is invalid"));
  }
  for (auto* cursor = cursors_; cursor != nullptr; cursor = cursor->next_) {
    if (cursor->access_ == btree_internal::CursorAccess::Read) {
      auto saved = cursor->SavePosition();
      if (!saved.has_value()) {
        TripAllCursors(saved.error().sqlite_code());
        return saved;
      }
    } else {
      cursor->Trip(kAbortRollbackSqliteCode);
    }
  }
  return {};
}

Status BtreeDatabase::PrepareRollbackCursors(std::optional<BtreeCursorTripMode> trip_mode) {
  if (trip_mode.has_value()) {
    return TripCursors(*trip_mode);
  }
  auto saved = SaveCursors(PageNumber{}, nullptr);
  if (!saved.has_value()) {
    TripAllCursors(saved.error().sqlite_code());
  }
  return saved;
}

Pager& BtreeDatabase::pager() noexcept {
  if (pager_ == nullptr) {
    std::terminate();
  }
  return *pager_;
}

const Pager& BtreeDatabase::pager() const noexcept {
  if (pager_ == nullptr) {
    std::terminate();
  }
  return *pager_;
}

namespace btree_internal {

FileOffset PayloadFileOffset(PageNumber page_number, BtreePageGeometry geometry, ByteView page,
                             const BtreeCellView& cell) noexcept {
  assert(page_number != PageNumber{});
  assert(cell.local_payload().data() != nullptr);
  const auto offset = cell.local_payload().data() - page.data();
  assert(offset >= 0 && static_cast<std::size_t>(offset) <= page.size());
  return FileOffset{static_cast<std::uint64_t>(page_number.value() - 1U) *
                        geometry.page_size().value() +
                    static_cast<std::uint64_t>(offset)};
}

Result<std::optional<PageNumber>> OverflowPageCandidate(PageNumber current,
                                                        BtreePageGeometry geometry,
                                                        std::uint32_t page_count) try {
  if (current == PageNumber{}) {
    return std::unexpected(CursorMisuse("overflow candidate requires a nonzero current page"));
  }
  if (current.value() > page_count) {
    return std::unexpected(
        Error::Create(ErrorCode::kCorruption, "current overflow page is outside the snapshot"));
  }
  std::uint64_t candidate = static_cast<std::uint64_t>(current.value()) + 1U;
  while (candidate <= page_count) {
    const PageNumber page{static_cast<std::uint32_t>(candidate)};
    if (page != geometry.locking_page()) {
      auto is_map = IsPointerMapPage(page, geometry);
      if (!is_map.has_value()) {
        return std::unexpected(std::move(is_map.error()));
      }
      if (!*is_map) {
        return std::optional{page};
      }
    }
    ++candidate;
  }
  return std::optional<PageNumber>{};
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
} catch (const std::length_error&) {
  return std::unexpected(Error::OutOfMemory());
}

CursorState::CursorState(BtreeDatabase& database, PageNumber root_page,
                         CursorAccess access) noexcept
    : database_(&database), access_(access), root_page_(root_page) {
  database.RegisterCursor(*this);
}

CursorState::CursorState(CursorState&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      next_(std::exchange(other.next_, nullptr)),
      access_(other.access_),
      multiple_(std::exchange(other.multiple_, false)),
      position_data_(std::move(other.position_data_)),
      skip_direction_(std::exchange(other.skip_direction_, 0)),
      overflow_pages_(std::move(other.overflow_pages_)),
      overflow_pages_valid_(std::exchange(other.overflow_pages_valid_, false)),
      root_page_(other.root_page_),
      state_(std::exchange(other.state_, BtreeCursorPhase::Invalid)) {
  other.position_data_ = std::int64_t{0};
  other.overflow_pages_.clear();
  if (database_ != nullptr) {
    database_->RelocateCursor(&other, *this);
  }
}

CursorState::~CursorState() { Detach(); }

Result<std::optional<PageNumber>> CursorState::ReadOverflowLink(PageNumber current,
                                                                BtreePageGeometry geometry) {
  Pager& pager = database_->pager();
  const auto* header = pager.header();
  if (header != nullptr && header->largest_root_page() != PageNumber{}) {
    auto candidate = OverflowPageCandidate(current, geometry, pager.page_count());
    if (!candidate.has_value()) {
      return std::unexpected(std::move(candidate.error()));
    }
    if (candidate->has_value()) {
      auto map_page = PointerMapPageFor(**candidate, geometry);
      if (!map_page.has_value()) {
        return std::unexpected(std::move(map_page.error()));
      }
      auto pin = pager.ReadPage(*map_page);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      auto map = PointerMapView::Parse(pin->frame().bytes(), *map_page, geometry);
      if (!map.has_value()) {
        return std::unexpected(std::move(map.error()));
      }
      auto entry = map->raw_entry(**candidate);
      if (!entry.has_value()) {
        return std::unexpected(std::move(entry.error()));
      }
      if (entry->type == PointerMapType::kLaterOverflow && entry->parent == current) {
        return *candidate;
      }
    }
  }
  auto pin = pager.ReadPage(current);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  auto page = OverflowPageView::Parse(pin->frame().bytes(), geometry);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  return page->next_page();
}

Status CursorState::ReadPayloadRange(const BtreeCellView& cell, BtreePageGeometry geometry,
                                     ByteOffset offset, MutableByteView destination,
                                     PayloadReadExtent extent) try {
  const std::size_t payload_size = cell.payload_size().value();
  const std::size_t payload_offset = offset.value();
  if (payload_offset > payload_size || destination.size() > payload_size - payload_offset) {
    return std::unexpected(
        Error::Create(ErrorCode::kOutOfRange, "B-tree payload range is out of bounds"));
  }
  if (destination.empty() && extent == PayloadReadExtent::RequestedRange) {
    return {};
  }

  Pager& pager = database_->pager();
  const std::size_t local_size = cell.local_payload().size();
  const std::size_t range_end = payload_offset + destination.size();
  const std::size_t walk_end =
      extent == PayloadReadExtent::CompletePayload ? payload_size : range_end;
  const std::size_t capacity = geometry.overflow_payload_capacity().value();
  if (walk_end > local_size && 1U + (walk_end - local_size - 1U) / capacity > pager.page_count()) {
    return std::unexpected(Error::Create(
        ErrorCode::kCorruption, "B-tree payload range cannot fit in the current snapshot"));
  }

  const auto copy_overlap = [this, payload_offset, range_end, destination](
                                ByteView source, std::size_t source_offset) {
    const std::size_t first = std::max(payload_offset, source_offset);
    const std::size_t last = std::min(range_end, source_offset + source.size());
    if (first >= last) {
      return;
    }
    const std::size_t count = last - first;
    std::memcpy(destination.data() + (first - payload_offset),
                source.data() + (first - source_offset), count);
    if (access_ == CursorAccess::Read) {
      MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBytesCopied, count);
    }
  };
  copy_overlap(cell.local_payload(), 0U);
  if (walk_end <= local_size) {
    return {};
  }

  const std::size_t required = 1U + (payload_size - local_size - 1U) / capacity;
  if (!overflow_pages_valid_) {
    if (required > overflow_pages_.capacity()) {
      if (required > overflow_pages_.max_size() / kOverflowPageListGrowthFactor) {
        return std::unexpected(Error::OutOfMemory());
      }
      overflow_pages_.reserve(required * kOverflowPageListGrowthFactor);
    }
    overflow_pages_.resize(required);
    std::ranges::fill(overflow_pages_, PageNumber{});
    overflow_pages_valid_ = true;
  }
  assert(overflow_pages_.size() == required);

  std::size_t index = 0U;
  std::optional<PageNumber> next_page = cell.first_overflow_page();
  if (extent == PayloadReadExtent::RequestedRange && payload_offset > local_size) {
    const std::size_t first = (payload_offset - local_size) / capacity;
    if (overflow_pages_[first] != PageNumber{}) {
      index = first;
      next_page = overflow_pages_[first];
    }
  }
  std::size_t source_offset = local_size + index * capacity;
  while (source_offset < walk_end) {
    if (!next_page.has_value()) {
      return std::unexpected(
          Error::Create(ErrorCode::kCorruption, "B-tree overflow chain ends prematurely"));
    }
    if (next_page->value() > pager.page_count()) {
      return std::unexpected(Error::Create(ErrorCode::kCorruption,
                                           "B-tree overflow page is outside the current snapshot"));
    }
    overflow_pages_[index] = *next_page;
    const std::size_t count = std::min(capacity, walk_end - source_offset);
    if (extent == PayloadReadExtent::RequestedRange && source_offset + count <= payload_offset) {
      if (index + 1U < required && overflow_pages_[index + 1U] != PageNumber{}) {
        next_page = overflow_pages_[index + 1U];
      } else {
        auto next = ReadOverflowLink(*next_page, geometry);
        if (!next.has_value()) {
          return std::unexpected(std::move(next.error()));
        }
        next_page = *next;
      }
      ++index;
      source_offset += count;
      continue;
    }

    std::array<std::byte, sizeof(std::uint32_t)> link{};
    bool direct = false;
    if (source_offset >= payload_offset) {
      const auto emitted = source_offset - payload_offset;
      if (emitted >= link.size() && emitted <= destination.size() &&
          count <= destination.size() - emitted) {
        std::array<std::byte, sizeof(std::uint32_t)> saved{};
        const auto output = destination.subspan(emitted - link.size(), link.size() + count);
        std::ranges::copy(output.first(link.size()), saved.begin());
        auto read = pager.ReadOverflowDirect(*next_page, output);
        if (read.has_value() && *read) {
          std::ranges::copy(output.first(link.size()), link.begin());
          direct = true;
        }
        std::ranges::copy(saved, output.begin());
        if (!read.has_value()) {
          return std::unexpected(std::move(read.error()));
        }
        if (direct && access_ == CursorAccess::Read) {
          MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBytesCopied, saved.size() * 2U);
        }
      }
    }
    if (!direct) {
      auto pin = pager.ReadPage(*next_page);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      const ByteView page = pin->frame().bytes();
      copy_overlap(page.subspan(link.size(), count), source_offset);
      if (source_offset + count < walk_end) {
        std::ranges::copy(page.first(link.size()), link.begin());
      }
    }
    source_offset += count;
    if (source_offset == walk_end) {
      return {};
    }
    auto next = OverflowPageView::DecodeNextPage(link, geometry);
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    next_page = *next;
    ++index;
  }
  return {};
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
} catch (const std::length_error&) {
  return std::unexpected(Error::OutOfMemory());
}

Status CursorState::SavePosition() {
  if (state_ != BtreeCursorPhase::Valid && state_ != BtreeCursorPhase::SkipNext) {
    InvalidatePositionMetadata();
    ReleasePathPages();
    return {};
  }
  if (state_ == BtreeCursorPhase::SkipNext) {
    state_ = BtreeCursorPhase::Valid;
  } else {
    skip_direction_ = 0;
  }
  auto key = CapturePositionKey();
  InvalidatePositionMetadata();
  if (!key.has_value()) {
    return std::unexpected(std::move(key.error()));
  }
  position_data_ = std::move(*key);
  ReleasePathPages();
  state_ = BtreeCursorPhase::RequireSeek;
  return {};
}

Status CursorState::SavePeers() {
  InvalidateOverflowPages();
  if (!multiple_) {
    return {};
  }
  return database_->SaveCursors(root_page_, this);
}

Result<bool> CursorState::RestorePosition() {
  auto active = CheckPositionActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (state_ == BtreeCursorPhase::RequireSeek) {
    const int previous_skip = skip_direction_;
    state_ = BtreeCursorPhase::Invalid;
    auto moved = MoveToSavedKey(position_data_);
    if (!moved.has_value()) {
      return std::unexpected(std::move(moved.error()));
    }
    position_data_ = std::int64_t{0};
    skip_direction_ = *moved != 0 ? *moved : previous_skip;
    if (skip_direction_ != 0 && state_ == BtreeCursorPhase::Valid) {
      state_ = BtreeCursorPhase::SkipNext;
    }
  }
  return state_ == BtreeCursorPhase::Valid;
}

Result<std::optional<bool>> CursorState::PrepareStep(bool forward) {
  auto active = CheckPositionActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (state_ == BtreeCursorPhase::RequireSeek || state_ == BtreeCursorPhase::SkipNext) {
    auto restored = RestorePosition();
    if (!restored.has_value()) {
      return std::unexpected(std::move(restored.error()));
    }
    if (state_ == BtreeCursorPhase::Invalid) {
      return std::optional<bool>{false};
    }
    if (state_ == BtreeCursorPhase::SkipNext) {
      state_ = BtreeCursorPhase::Valid;
      if ((forward && skip_direction_ > 0) || (!forward && skip_direction_ < 0)) {
        return std::optional<bool>{true};
      }
    }
  }
  if (state_ != BtreeCursorPhase::Valid) {
    return std::unexpected(CursorMisuse("movement requires a valid B-tree cursor position"));
  }
  return std::optional<bool>{};
}

void CursorState::DiscardSavedPosition() noexcept {
  if (std::holds_alternative<CursorTripCode>(position_data_)) {
    return;
  }
  position_data_ = std::int64_t{0};
  skip_direction_ = 0;
  if (state_ == BtreeCursorPhase::RequireSeek || state_ == BtreeCursorPhase::SkipNext) {
    state_ = BtreeCursorPhase::Invalid;
  }
}

Status CursorState::CheckTripped() const noexcept {
  if (const auto* code = std::get_if<CursorTripCode>(&position_data_); code != nullptr) {
    assert(state_ == BtreeCursorPhase::Fault);
    return std::unexpected(CursorTripError(code->sqlite_code));
  }
  return {};
}

void CursorState::Trip(int sqlite_code) noexcept {
  position_data_ = CursorTripCode{sqlite_code};
  skip_direction_ = 0;
  state_ = BtreeCursorPhase::Fault;
  InvalidatePositionMetadata();
  ReleasePathPages();
}

void CursorState::Detach() noexcept {
  InvalidateOverflowPages();
  if (database_ != nullptr) {
    database_->UnregisterCursor(*this);
    database_ = nullptr;
    next_ = nullptr;
    multiple_ = false;
    state_ = BtreeCursorPhase::Invalid;
    position_data_ = std::int64_t{0};
    skip_direction_ = 0;
  }
}

}  // namespace btree_internal

}  // namespace modern_sqlite
