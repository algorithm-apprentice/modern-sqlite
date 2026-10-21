#include "modern_sqlite/storage/btree/database.hpp"

#include <cassert>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include "cursor_state.hpp"

namespace modern_sqlite {

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
      root_page_(other.root_page_),
      state_(std::exchange(other.state_, BtreeCursorPhase::Invalid)) {
  if (database_ != nullptr) {
    database_->RelocateCursor(&other, *this);
  }
}

CursorState::~CursorState() { Detach(); }

void CursorState::Detach() noexcept {
  if (database_ != nullptr) {
    database_->UnregisterCursor(*this);
    database_ = nullptr;
    next_ = nullptr;
    multiple_ = false;
    state_ = BtreeCursorPhase::Invalid;
  }
}

}  // namespace btree_internal

}  // namespace modern_sqlite
