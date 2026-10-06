#include "writer_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace modern_sqlite::btree_internal {
namespace {

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

[[nodiscard]] Error Corruption(std::string_view message) noexcept {
  return MakeError(ErrorCode::kCorruption, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) noexcept {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

}  // namespace

MutationPageOwner::MutationPageOwner(Pager& pager) noexcept
    : pager_(&pager), generation_(pager.write_transaction_generation()) {}

Status MutationPageOwner::CheckActive() const {
  if (pager_ == nullptr || !pager_->in_write_transaction() ||
      pager_->state() == PagerState::kWriterFinished ||
      pager_->write_transaction_generation() != generation_) {
    return std::unexpected(
        SchemaChanged("mutation page owner belongs to an obsolete write transaction"));
  }
  if (const auto failure = pager_->write_failure_code(); failure.has_value()) {
    return std::unexpected(
        MakeError(*failure, "mutation page owner requires transaction rollback"));
  }
  return {};
}

Result<std::size_t> MutationPageOwner::AcquireRead(PageNumber page_number) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (Find(page_number).has_value()) {
    return std::unexpected(Corruption("mutation operation acquired one page more than once"));
  }
  auto slot = EmptySlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  auto pin = pager_->ReadPage(page_number);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  OwnedPage& page = pages_[*slot];
  page.page_number = page_number;
  page.pin.emplace<ReadPagePin>(std::move(*pin));
  ++size_;
  return *slot;
}

Result<std::size_t> MutationPageOwner::AcquireWrite(PageNumber page_number) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (Find(page_number).has_value()) {
    return std::unexpected(Corruption("mutation operation acquired one page more than once"));
  }
  auto slot = EmptySlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  auto pin = pager_->WritePage(page_number);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  OwnedPage& page = pages_[*slot];
  page.page_number = page_number;
  page.pin.emplace<WritePagePin>(std::move(*pin));
  ++size_;
  return *slot;
}

Result<std::size_t> MutationPageOwner::Borrow(PageNumber page_number) const {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  const std::optional<std::size_t> slot = Find(page_number);
  if (!slot.has_value()) {
    return std::unexpected(Misuse("mutation operation does not own the borrowed page"));
  }
  return *slot;
}

Status MutationPageOwner::Promote(std::size_t slot) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (slot >= pages_.size()) {
    return std::unexpected(Misuse("mutation page slot is out of range"));
  }
  OwnedPage& page = pages_[slot];
  if (std::holds_alternative<WritePagePin>(page.pin)) {
    return {};
  }
  auto* read = std::get_if<ReadPagePin>(&page.pin);
  if (read == nullptr) {
    return std::unexpected(Misuse("mutation page slot is empty"));
  }

  const PageNumber page_number = page.page_number;
  ReadPagePin read_pin = std::move(*read);
  page.pin.emplace<std::monostate>();
  page.page_number = PageNumber{};
  --size_;

  auto promoted = pager_->WritePage(std::move(read_pin));
  if (!promoted.has_value()) {
    return std::unexpected(std::move(promoted.error()));
  }
  page.page_number = page_number;
  page.pin.emplace<WritePagePin>(std::move(*promoted));
  ++size_;
  return {};
}

void MutationPageOwner::Release(std::size_t slot) noexcept {
  assert(slot < pages_.size());
  OwnedPage& page = pages_[slot];
  assert(!std::holds_alternative<std::monostate>(page.pin));
  assert(size_ > 0U);
  page.pin.emplace<std::monostate>();
  page.page_number = PageNumber{};
  --size_;
}

Result<std::reference_wrapper<const PageFrame>> MutationPageOwner::Frame(std::size_t slot) const {
  if (slot >= pages_.size()) {
    return std::unexpected(Misuse("mutation page slot is out of range"));
  }
  const OwnedPage& page = pages_[slot];
  if (const auto* read = std::get_if<ReadPagePin>(&page.pin); read != nullptr) {
    return std::cref(read->frame());
  }
  if (const auto* write = std::get_if<WritePagePin>(&page.pin); write != nullptr) {
    return std::cref(write->frame());
  }
  return std::unexpected(Misuse("mutation page slot is empty"));
}

Result<MutableByteView> MutationPageOwner::MutableBytes(std::size_t slot) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (slot >= pages_.size()) {
    return std::unexpected(Misuse("mutation page slot is out of range"));
  }
  auto* write = std::get_if<WritePagePin>(&pages_[slot].pin);
  if (write == nullptr) {
    return std::unexpected(Misuse("mutation page is not writable"));
  }
  return write->mutable_bytes();
}

std::optional<std::size_t> MutationPageOwner::Find(PageNumber page_number) const noexcept {
  for (std::size_t slot = 0; slot < pages_.size(); ++slot) {
    if (!std::holds_alternative<std::monostate>(pages_[slot].pin) &&
        pages_[slot].page_number == page_number) {
      return slot;
    }
  }
  return std::nullopt;
}

bool MutationPageOwner::IsWritable(std::size_t slot) const noexcept {
  return slot < pages_.size() && std::holds_alternative<WritePagePin>(pages_[slot].pin);
}

Result<std::size_t> MutationPageOwner::EmptySlot() const {
  for (std::size_t slot = 0; slot < pages_.size(); ++slot) {
    if (std::holds_alternative<std::monostate>(pages_[slot].pin)) {
      return slot;
    }
  }
  return std::unexpected(TooLarge("mutation operation exceeded its bounded page ownership"));
}

Result<WritableCursor> WritableCursor::Open(MutationPageOwner& owner, PageNumber root_page,
                                            bool table) {
  auto active = owner.CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (root_page.value() == 0U) {
    return std::unexpected(Misuse("writable cursor root page zero is invalid"));
  }
  const DatabaseHeader* header = owner.pager().header();
  if (header == nullptr) {
    return std::unexpected(Misuse("writable cursor requires an initialized database"));
  }
  auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const bool root_already_owned = owner.Find(root_page).has_value();
  auto root_slot = root_already_owned ? owner.Borrow(root_page) : owner.AcquireRead(root_page);
  if (!root_slot.has_value()) {
    return std::unexpected(std::move(root_slot.error()));
  }
  auto frame = owner.Frame(*root_slot);
  if (!frame.has_value()) {
    if (!root_already_owned) {
      owner.Release(*root_slot);
    }
    return std::unexpected(std::move(frame.error()));
  }
  auto root = BtreePageView::Parse(frame->get().bytes(), root_page, *geometry);
  if (!root.has_value()) {
    if (!root_already_owned) {
      owner.Release(*root_slot);
    }
    return std::unexpected(std::move(root.error()));
  }
  if (root->is_table() != table) {
    if (!root_already_owned) {
      owner.Release(*root_slot);
    }
    return std::unexpected(Corruption("writable cursor root has the wrong tree kind"));
  }
  if (!root->is_leaf() && root->cell_count() == 0U && root_page != PageNumber{1}) {
    if (!root_already_owned) {
      owner.Release(*root_slot);
    }
    return std::unexpected(Corruption("only page one may contain an empty interior B-tree root"));
  }
  return WritableCursor{owner, root_page, table, *geometry, *root_slot};
}

WritableCursor::WritableCursor(WritableCursor&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      root_page_(other.root_page_),
      table_(other.table_),
      geometry_(other.geometry_),
      frames_(other.frames_),
      frame_count_(std::exchange(other.frame_count_, 0)),
      current_index_(other.current_index_),
      state_(std::exchange(other.state_, WritableCursorState::kInvalid)) {}

Status WritableCursor::CheckActive() const {
  if (owner_ == nullptr) {
    return std::unexpected(Misuse("writable cursor is moved from"));
  }
  if (state_ == WritableCursorState::kFault) {
    return std::unexpected(Corruption("writable cursor is faulted"));
  }
  return owner_->CheckActive();
}

Result<TableSeekResult> WritableCursor::SeekTable(std::int64_t rowid) {
  if (!table_) {
    return std::unexpected(Misuse("table seek requires a table B-tree cursor"));
  }
  auto reset = ResetToRoot();
  if (!reset.has_value()) {
    EnterFault();
    return std::unexpected(std::move(reset.error()));
  }

  while (true) {
    auto page = CurrentPage();
    if (!page.has_value()) {
      EnterFault();
      return std::unexpected(std::move(page.error()));
    }

    std::size_t lower = 0;
    std::size_t upper = page->cell_count();
    while (lower < upper) {
      const std::size_t middle = lower + (upper - lower) / 2U;
      auto cell = page->cell(middle);
      if (!cell.has_value()) {
        EnterFault();
        return std::unexpected(std::move(cell.error()));
      }
      if (!cell->rowid().has_value()) {
        EnterFault();
        return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
      }
      if (*cell->rowid() < rowid) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }

    bool exact = false;
    if (lower < page->cell_count()) {
      auto cell = page->cell(lower);
      if (!cell.has_value()) {
        EnterFault();
        return std::unexpected(std::move(cell.error()));
      }
      if (!cell->rowid().has_value()) {
        EnterFault();
        return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
      }
      exact = *cell->rowid() == rowid;
    }

    if (!page->is_leaf()) {
      auto descended = Descend(lower, *page);
      if (!descended.has_value()) {
        EnterFault();
        return std::unexpected(std::move(descended.error()));
      }
      continue;
    }

    const std::size_t count = page->cell_count();
    if (count == 0U) {
      current_index_ = 0U;
      state_ = WritableCursorState::kInvalid;
      return TableSeekResult{
          .insertion_index = 0U,
          .comparison = -1,
          .exact = false,
          .tree_depth = frame_count_,
      };
    }

    current_index_ = lower < count ? lower : count - 1U;
    frames_[frame_count_ - 1U].child_index = current_index_;
    state_ = WritableCursorState::kValid;
    return TableSeekResult{
        .insertion_index = lower,
        .comparison = exact ? 0 : (lower == count ? -1 : 1),
        .exact = exact,
        .tree_depth = frame_count_,
    };
  }
}

Result<IndexSeekResult> WritableCursor::SeekIndex(std::span<const SqlValue> key,
                                                  std::span<const IndexColumnOrder> columns,
                                                  RecordCodecOptions options,
                                                  std::vector<std::byte>& scratch) {
  if (table_) {
    return std::unexpected(Misuse("index seek requires an index B-tree cursor"));
  }
  if (key.empty() || key.size() > columns.size()) {
    return std::unexpected(Misuse("index seek key does not match its comparison metadata"));
  }
  auto reset = ResetToRoot();
  if (!reset.has_value()) {
    EnterFault();
    return std::unexpected(std::move(reset.error()));
  }

  while (true) {
    auto page = CurrentPage();
    if (!page.has_value()) {
      EnterFault();
      return std::unexpected(std::move(page.error()));
    }

    std::size_t lower = 0;
    std::size_t upper = page->cell_count();
    while (lower < upper) {
      const std::size_t middle = lower + (upper - lower) / 2U;
      auto cell = page->cell(middle);
      if (!cell.has_value()) {
        EnterFault();
        return std::unexpected(std::move(cell.error()));
      }
      auto encoded = ReadCellPayload(*cell, scratch);
      if (!encoded.has_value()) {
        EnterFault();
        return std::unexpected(std::move(encoded.error()));
      }
      auto record = RecordView::Parse(*encoded, options);
      if (!record.has_value()) {
        EnterFault();
        return std::unexpected(std::move(record.error()));
      }
      auto comparison = CompareIndexRecord(*record, key, columns);
      if (!comparison.has_value()) {
        EnterFault();
        return std::unexpected(std::move(comparison.error()));
      }
      if (comparison->ordering == std::weak_ordering::equivalent) {
        current_index_ = middle;
        frames_[frame_count_ - 1U].child_index = middle;
        state_ = WritableCursorState::kValid;
        return IndexSeekResult{
            .insertion_index = middle,
            .comparison = 0,
            .exact = true,
            .tree_depth = frame_count_,
        };
      }
      if (comparison->ordering == std::weak_ordering::less) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }

    if (!page->is_leaf()) {
      auto descended = Descend(lower, *page);
      if (!descended.has_value()) {
        EnterFault();
        return std::unexpected(std::move(descended.error()));
      }
      continue;
    }

    const std::size_t count = page->cell_count();
    if (count == 0U) {
      current_index_ = 0U;
      state_ = WritableCursorState::kInvalid;
      return IndexSeekResult{
          .insertion_index = 0U,
          .comparison = -1,
          .exact = false,
          .tree_depth = frame_count_,
      };
    }
    current_index_ = lower < count ? lower : count - 1U;
    frames_[frame_count_ - 1U].child_index = current_index_;
    state_ = WritableCursorState::kValid;
    return IndexSeekResult{
        .insertion_index = lower,
        .comparison = lower == count ? -1 : 1,
        .exact = false,
        .tree_depth = frame_count_,
    };
  }
}

Result<BtreePageView> WritableCursor::CurrentPage() const {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (frame_count_ == 0U) {
    return std::unexpected(Misuse("writable cursor has no page"));
  }
  auto frame = owner_->Frame(frames_[frame_count_ - 1U].owner_slot);
  if (!frame.has_value()) {
    return std::unexpected(std::move(frame.error()));
  }
  return BtreePageView::Parse(frame->get().bytes(), frame->get().page_number(), geometry_);
}

Status WritableCursor::PromoteCurrent() {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (frame_count_ == 0U) {
    return std::unexpected(Misuse("writable cursor has no current page"));
  }
  auto promoted = owner_->Promote(frames_[frame_count_ - 1U].owner_slot);
  if (!promoted.has_value()) {
    EnterFault();
    return promoted;
  }
  return {};
}

Status WritableCursor::MoveToParent() {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (frame_count_ <= 1U) {
    return std::unexpected(Misuse("writable cursor is already at its root"));
  }
  owner_->Release(frames_[frame_count_ - 1U].owner_slot);
  --frame_count_;
  current_index_ = frames_[frame_count_ - 1U].child_index;
  state_ = WritableCursorState::kValid;
  return {};
}

Status WritableCursor::ResetToRoot() {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  ReleaseDescendants();
  current_index_ = 0U;
  state_ = WritableCursorState::kInvalid;

  auto root = CurrentPage();
  if (!root.has_value()) {
    return std::unexpected(std::move(root.error()));
  }
  if (root->cell_count() != 0U) {
    state_ = WritableCursorState::kValid;
    return {};
  }
  if (root->is_leaf()) {
    return {};
  }
  if (root_page_ != PageNumber{1}) {
    return std::unexpected(Corruption("only page one may contain an empty interior root"));
  }
  auto descended = Descend(0U, *root);
  if (!descended.has_value()) {
    return descended;
  }
  state_ = WritableCursorState::kValid;
  return {};
}

std::size_t WritableCursor::current_index() const noexcept { return current_index_; }

std::size_t WritableCursor::current_owner_slot() const noexcept {
  return frame_count_ == 0 ? 0 : frames_[frame_count_ - 1U].owner_slot;
}

WritableCursor::WritableCursor(MutationPageOwner& owner, PageNumber root_page, bool table,
                               BtreePageGeometry geometry, std::size_t root_slot) noexcept
    : owner_(&owner), root_page_(root_page), table_(table), geometry_(geometry), frame_count_(1) {
  frames_[0].owner_slot = root_slot;
}

Result<PageNumber> WritableCursor::ChildAt(const BtreePageView& page,
                                           std::size_t child_index) const {
  if (page.is_leaf() || child_index > page.cell_count()) {
    return std::unexpected(Corruption("writable cursor child slot is invalid"));
  }
  if (child_index == page.cell_count()) {
    if (!page.rightmost_child().has_value()) {
      return std::unexpected(Corruption("interior B-tree page has no rightmost child"));
    }
    return *page.rightmost_child();
  }
  auto cell = page.cell(child_index);
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  if (!cell->left_child().has_value()) {
    return std::unexpected(Corruption("interior B-tree cell has no left child"));
  }
  return *cell->left_child();
}

Result<ByteView> WritableCursor::ReadCellPayload(const BtreeCellView& cell,
                                                 std::vector<std::byte>& scratch) {
  if (cell.first_overflow_page().has_value() == false) {
    if (cell.local_payload().size() != cell.payload_size().value()) {
      return std::unexpected(Corruption("local B-tree payload has the wrong size"));
    }
    return cell.local_payload();
  }

  const std::size_t payload_size = cell.payload_size().value();
  const std::size_t local_size = cell.local_payload().size();
  if (local_size >= payload_size || geometry_.overflow_payload_capacity().value() == 0U) {
    return std::unexpected(Corruption("overflow-backed B-tree payload is malformed"));
  }
  const std::size_t remaining = payload_size - local_size;
  const std::size_t overflow_capacity = geometry_.overflow_payload_capacity().value();
  const std::size_t required_pages = 1U + (remaining - 1U) / overflow_capacity;
  if (required_pages > owner_->pager().page_count()) {
    return std::unexpected(Corruption("B-tree payload requires too many overflow pages"));
  }

  try {
    scratch.resize(payload_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
  std::ranges::copy(cell.local_payload(), scratch.begin());

  std::size_t offset = local_size;
  std::optional<PageNumber> overflow_page = cell.first_overflow_page();
  while (offset < payload_size) {
    if (!overflow_page.has_value()) {
      return std::unexpected(Corruption("B-tree overflow chain ended prematurely"));
    }
    auto slot = owner_->AcquireRead(*overflow_page);
    if (!slot.has_value()) {
      return std::unexpected(std::move(slot.error()));
    }
    auto frame = owner_->Frame(*slot);
    if (!frame.has_value()) {
      owner_->Release(*slot);
      return std::unexpected(std::move(frame.error()));
    }
    auto overflow = OverflowPageView::Parse(frame->get().bytes(), geometry_);
    if (!overflow.has_value()) {
      owner_->Release(*slot);
      return std::unexpected(std::move(overflow.error()));
    }

    const std::size_t count = std::min(overflow->payload().size(), payload_size - offset);
    std::ranges::copy(overflow->payload().first(count),
                      scratch.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += count;
    overflow_page = overflow->next_page();
    owner_->Release(*slot);
  }
  return ByteView{scratch};
}

Status WritableCursor::Descend(std::size_t child_index, const BtreePageView& parent) {
  if (frame_count_ >= kMaximumBtreeDepth) {
    return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
  }
  auto child_page = ChildAt(parent, child_index);
  if (!child_page.has_value()) {
    return std::unexpected(std::move(child_page.error()));
  }
  auto child_slot = owner_->AcquireRead(*child_page);
  if (!child_slot.has_value()) {
    return std::unexpected(std::move(child_slot.error()));
  }
  auto frame = owner_->Frame(*child_slot);
  if (!frame.has_value()) {
    owner_->Release(*child_slot);
    return std::unexpected(std::move(frame.error()));
  }
  auto child = BtreePageView::Parse(frame->get().bytes(), *child_page, geometry_);
  if (!child.has_value()) {
    owner_->Release(*child_slot);
    return std::unexpected(std::move(child.error()));
  }
  if (child->cell_count() == 0U) {
    owner_->Release(*child_slot);
    return std::unexpected(Corruption("non-root B-tree page is empty"));
  }
  if (child->is_table() != table_) {
    owner_->Release(*child_slot);
    return std::unexpected(Corruption("B-tree child has the wrong tree kind"));
  }

  frames_[frame_count_ - 1U].child_index = child_index;
  frames_[frame_count_] = Frame{
      .owner_slot = *child_slot,
      .child_index = 0U,
  };
  ++frame_count_;
  current_index_ = 0U;
  state_ = WritableCursorState::kValid;
  return {};
}

void WritableCursor::ReleaseDescendants() noexcept {
  while (frame_count_ > 1U) {
    owner_->Release(frames_[frame_count_ - 1U].owner_slot);
    --frame_count_;
  }
}

void WritableCursor::EnterFault() noexcept { state_ = WritableCursorState::kFault; }

}  // namespace modern_sqlite::btree_internal
