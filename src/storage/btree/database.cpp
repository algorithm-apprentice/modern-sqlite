#include "modern_sqlite/storage/btree/database.hpp"

#include <cassert>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "cursor_state.hpp"

namespace modern_sqlite {

namespace {
constexpr int kAbortRollbackSqliteCode = 516;

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
      root_page_(other.root_page_),
      state_(std::exchange(other.state_, BtreeCursorPhase::Invalid)) {
  other.position_data_ = std::int64_t{0};
  if (database_ != nullptr) {
    database_->RelocateCursor(&other, *this);
  }
}

CursorState::~CursorState() { Detach(); }

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
