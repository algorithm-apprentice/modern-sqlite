#include "modern_sqlite/storage/btree/cursor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/read_pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] Error MakeError(ErrorCode code, std::string_view message) {
  return Error::Create(code, std::string{message});
}

[[nodiscard]] Error Misuse(std::string_view message) {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] Error Corruption(std::string_view message) {
  return MakeError(ErrorCode::kCorruption, message);
}

[[nodiscard]] Error OutOfRange(std::string_view message) {
  return MakeError(ErrorCode::kOutOfRange, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] Error Protocol(std::string_view message) {
  return MakeError(ErrorCode::kProtocol, message);
}

[[nodiscard]] bool IsValidSeekMode(BtreeSeekMode mode) noexcept {
  switch (mode) {
    case BtreeSeekMode::kEqual:
    case BtreeSeekMode::kGreaterOrEqual:
    case BtreeSeekMode::kGreater:
    case BtreeSeekMode::kLessOrEqual:
    case BtreeSeekMode::kLess:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValidDirection(IndexSortDirection direction) noexcept {
  return direction == IndexSortDirection::kAscending ||
         direction == IndexSortDirection::kDescending;
}

[[nodiscard]] bool IsValidNullPlacement(IndexNullPlacement placement) noexcept {
  return placement == IndexNullPlacement::kFirst || placement == IndexNullPlacement::kLast;
}

[[nodiscard]] Result<RecordCodecOptions> RecordOptionsFor(const DatabaseHeader& header) {
  auto schema_format = NormalizeSchemaFormat(header.schema_format());
  if (!schema_format.has_value()) {
    return std::unexpected(std::move(schema_format.error()));
  }

  auto text_encoding = NormalizeTextEncoding(header.text_encoding());
  if (!text_encoding.has_value()) {
    return std::unexpected(std::move(text_encoding.error()));
  }
  if (*text_encoding != DatabaseTextEncoding::kUtf8) {
    return std::unexpected(Protocol("UTF-16 index comparison is not implemented"));
  }

  return RecordCodecOptions{.schema_format = *schema_format};
}

[[nodiscard]] EqualPrefixResult PrefixResultFor(BtreeSeekMode mode) {
  switch (mode) {
    case BtreeSeekMode::kEqual:
      return EqualPrefixResult::kEquivalent;
    case BtreeSeekMode::kGreaterOrEqual:
    case BtreeSeekMode::kLess:
      return EqualPrefixResult::kGreater;
    case BtreeSeekMode::kGreater:
    case BtreeSeekMode::kLessOrEqual:
      return EqualPrefixResult::kLess;
  }
  return EqualPrefixResult::kEquivalent;
}

class CursorCore final {
 private:
  struct Frame {
    explicit Frame(ReadPagePin page_pin) noexcept : pin(std::move(page_pin)) {}

    ReadPagePin pin;
    std::size_t index = 0;
  };

 public:
  CursorCore(const CursorCore&) = delete;
  CursorCore& operator=(const CursorCore&) = delete;
  CursorCore(CursorCore&&) noexcept = default;
  CursorCore& operator=(CursorCore&&) noexcept = default;
  ~CursorCore() = default;

  [[nodiscard]] static Result<CursorCore> Open(ReadPager& pager, PageNumber root_page, bool table) {
    if (!pager.in_read_transaction()) {
      return std::unexpected(Misuse("opening a B-tree cursor requires a read transaction"));
    }
    if (root_page.value() == 0) {
      return std::unexpected(Misuse("B-tree root page zero is invalid"));
    }

    const std::uint64_t data_version = pager.data_version();
    if (pager.page_count() == 0) {
      if (!table || root_page != PageNumber{1}) {
        return std::unexpected(
            Corruption("an empty database contains only an empty table root at page one"));
      }
      return CursorCore{pager, root_page, table, data_version, std::nullopt, true};
    }

    const DatabaseHeader* header = pager.header();
    if (header == nullptr) {
      return std::unexpected(Corruption("nonempty database is missing its header"));
    }
    auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
    if (!geometry.has_value()) {
      return std::unexpected(std::move(geometry.error()));
    }

    auto root_pin = pager.ReadPage(root_page);
    if (!root_pin.has_value()) {
      return std::unexpected(std::move(root_pin.error()));
    }
    auto root = BtreePageView::Parse(root_pin->frame().bytes(), root_page, *geometry);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    if (root->is_table() != table) {
      return std::unexpected(Corruption("B-tree root has the wrong tree kind"));
    }
    if (root->cell_count() == 0 && !root->is_leaf() && root_page != PageNumber{1}) {
      return std::unexpected(Corruption("only page one may contain an empty interior B-tree root"));
    }

    CursorCore core{pager, root_page, table, data_version, *geometry, false};
    core.frames_[0].emplace<Frame>(std::move(*root_pin));
    core.frame_count_ = 1;
    return core;
  }

  [[nodiscard]] bool valid() const noexcept { return valid_ && !terminal_; }

  void InvalidateForFailedSeek() noexcept { InvalidatePosition(); }

  [[nodiscard]] Result<bool> First() {
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return FailMovement(std::move(ready.error()));
    }
    if (!*ready) {
      InvalidatePosition();
      return false;
    }
    auto descended = DescendLeftmost();
    if (!descended.has_value()) {
      return FailMovement(std::move(descended.error()));
    }
    valid_ = true;
    return true;
  }

  [[nodiscard]] Result<bool> Last() {
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return FailMovement(std::move(ready.error()));
    }
    if (!*ready) {
      InvalidatePosition();
      return false;
    }
    auto descended = DescendRightmost();
    if (!descended.has_value()) {
      return FailMovement(std::move(descended.error()));
    }
    valid_ = true;
    return true;
  }

  [[nodiscard]] Result<bool> Next() {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return FailMovement(std::move(snapshot.error()));
    }
    if (!valid_) {
      return std::unexpected(Misuse("next requires a valid B-tree cursor position"));
    }
    auto advanced = NextImpl();
    if (!advanced.has_value()) {
      return FailMovement(std::move(advanced.error()));
    }
    if (!*advanced) {
      InvalidatePosition();
    }
    return *advanced;
  }

  [[nodiscard]] Result<bool> Previous() {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return FailMovement(std::move(snapshot.error()));
    }
    if (!valid_) {
      return std::unexpected(Misuse("previous requires a valid B-tree cursor position"));
    }
    auto retreated = PreviousImpl();
    if (!retreated.has_value()) {
      return FailMovement(std::move(retreated.error()));
    }
    if (!*retreated) {
      InvalidatePosition();
    }
    return *retreated;
  }

  [[nodiscard]] Result<bool> SeekTable(std::int64_t target, BtreeSeekMode mode) {
    if (!IsValidSeekMode(mode)) {
      InvalidatePosition();
      return std::unexpected(Misuse("invalid B-tree seek mode"));
    }
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return FailMovement(std::move(ready.error()));
    }
    if (!*ready) {
      InvalidatePosition();
      return false;
    }

    while (true) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return FailMovement(std::move(page.error()));
      }
      std::size_t lower = 0;
      std::size_t upper = page->cell_count();
      while (lower < upper) {
        const std::size_t middle = lower + (upper - lower) / 2U;
        auto cell = page->cell(middle);
        if (!cell.has_value()) {
          return FailMovement(std::move(cell.error()));
        }
        if (!cell->rowid().has_value()) {
          return FailMovement(Corruption("table B-tree cell is missing its rowid"));
        }
        MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBtreeComparisons, 1);
        if (*cell->rowid() < target) {
          lower = middle + 1U;
        } else {
          upper = middle;
        }
      }

      bool exact = false;
      if (lower < page->cell_count()) {
        auto candidate = page->cell(lower);
        if (!candidate.has_value()) {
          return FailMovement(std::move(candidate.error()));
        }
        if (!candidate->rowid().has_value()) {
          return FailMovement(Corruption("table B-tree cell is missing its rowid"));
        }
        MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBtreeComparisons, 1);
        exact = *candidate->rowid() == target;
      }

      if (!page->is_leaf()) {
        auto descended = DescendChild(lower, *page);
        if (!descended.has_value()) {
          return FailMovement(std::move(descended.error()));
        }
        continue;
      }

      auto positioned = PositionTableLeaf(lower, exact, mode, page->cell_count());
      if (!positioned.has_value()) {
        return FailMovement(std::move(positioned.error()));
      }
      if (!*positioned) {
        InvalidatePosition();
      }
      return *positioned;
    }
  }

  template <typename CompareCell>
  [[nodiscard]] Result<bool> SeekIndex(BtreeSeekMode mode, CompareCell&& compare_cell) {
    if (!IsValidSeekMode(mode)) {
      InvalidatePosition();
      return std::unexpected(Misuse("invalid B-tree seek mode"));
    }
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return FailMovement(std::move(ready.error()));
    }
    if (!*ready) {
      InvalidatePosition();
      return false;
    }

    while (true) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return FailMovement(std::move(page.error()));
      }
      std::size_t lower = 0;
      std::size_t upper = page->cell_count();
      while (lower < upper) {
        const std::size_t middle = lower + (upper - lower) / 2U;
        auto cell = page->cell(middle);
        if (!cell.has_value()) {
          return FailMovement(std::move(cell.error()));
        }
        auto ordering = compare_cell(*cell);
        if (!ordering.has_value()) {
          return FailMovement(std::move(ordering.error()));
        }
        if (*ordering == std::weak_ordering::equivalent) {
          CurrentFrame().index = middle;
          valid_ = true;
          return true;
        }
        if (*ordering == std::weak_ordering::less) {
          lower = middle + 1U;
        } else {
          upper = middle;
        }
      }

      if (!page->is_leaf()) {
        auto descended = DescendChild(lower, *page);
        if (!descended.has_value()) {
          return FailMovement(std::move(descended.error()));
        }
        continue;
      }

      Result<bool> positioned =
          mode == BtreeSeekMode::kGreaterOrEqual || mode == BtreeSeekMode::kGreater
              ? PositionForwardCandidate(lower, page->cell_count())
          : mode == BtreeSeekMode::kLessOrEqual || mode == BtreeSeekMode::kLess
              ? PositionReverseCandidate(lower)
              : Result<bool>{false};
      if (!positioned.has_value()) {
        return FailMovement(std::move(positioned.error()));
      }
      if (!*positioned) {
        InvalidatePosition();
      }
      return *positioned;
    }
  }

  [[nodiscard]] Result<BtreeCellView> CurrentCell() {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    if (!valid_) {
      return std::unexpected(Misuse("B-tree cursor is not positioned"));
    }
    auto page = CurrentPage();
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    return page->cell(CurrentFrame().index);
  }

  [[nodiscard]] Status ReadCellPayload(const BtreeCellView& cell, ByteOffset offset,
                                       MutableByteView destination) {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    const std::size_t payload_size = cell.payload_size().value();
    const std::size_t payload_offset = offset.value();
    if (payload_offset > payload_size || destination.size() > payload_size - payload_offset) {
      return std::unexpected(OutOfRange("B-tree payload range is out of bounds"));
    }
    if (destination.empty()) {
      return {};
    }
    if (!geometry_.has_value()) {
      return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
    }
    const BtreePageGeometry geometry = geometry_.value();

    const std::size_t local_size = cell.local_payload().size();
    const std::size_t range_end = payload_offset + destination.size();
    if (range_end > local_size) {
      const std::size_t overflow_end = range_end - local_size;
      const std::size_t capacity = geometry.overflow_payload_capacity().value();
      const std::size_t pages_to_reach = 1U + (overflow_end - 1U) / capacity;
      if (pages_to_reach > pager_->page_count()) {
        return std::unexpected(
            Corruption("B-tree payload range cannot fit in the current snapshot"));
      }
    }

    std::size_t source_offset = payload_offset;
    std::size_t output_offset = 0;
    std::size_t remaining = destination.size();
    if (source_offset < local_size) {
      const std::size_t count = std::min(remaining, local_size - source_offset);
      std::memcpy(destination.data(), cell.local_payload().data() + source_offset, count);
      MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBytesCopied, count);
      output_offset += count;
      remaining -= count;
      source_offset = 0;
    } else {
      source_offset -= local_size;
    }
    if (remaining == 0) {
      return {};
    }

    std::optional<PageNumber> next_page = cell.first_overflow_page();
    const std::size_t capacity = geometry.overflow_payload_capacity().value();
    while (source_offset >= capacity) {
      if (!next_page.has_value()) {
        return std::unexpected(Corruption("B-tree overflow chain ends prematurely"));
      }
      auto pin = pager_->ReadPage(*next_page);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      auto overflow = OverflowPageView::Parse(pin->frame().bytes(), geometry);
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      next_page = overflow->next_page();
      source_offset -= capacity;
    }

    while (remaining > 0) {
      if (!next_page.has_value()) {
        return std::unexpected(Corruption("B-tree overflow chain ends prematurely"));
      }
      auto pin = pager_->ReadPage(*next_page);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      const ByteView page = pin->frame().bytes();
      const ByteView overflow_payload = page.subspan(4U, capacity);
      const std::size_t count = std::min(remaining, capacity - source_offset);
      std::memcpy(destination.data() + output_offset, overflow_payload.data() + source_offset,
                  count);
      MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBytesCopied, count);
      output_offset += count;
      remaining -= count;
      if (remaining == 0) {
        return {};
      }

      auto overflow = OverflowPageView::Parse(page, geometry);
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      next_page = overflow->next_page();
      source_offset = 0;
    }
    return {};
  }

  [[nodiscard]] Status EnsureCompletePayloadPossible(const BtreeCellView& cell) {
    const std::size_t payload_size = cell.payload_size().value();
    const std::size_t local_size = cell.local_payload().size();
    if (payload_size <= local_size) {
      return {};
    }
    if (!geometry_.has_value()) {
      return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
    }
    const BtreePageGeometry geometry = geometry_.value();
    const std::size_t remaining = payload_size - local_size;
    const std::size_t capacity = geometry.overflow_payload_capacity().value();
    const std::size_t required = 1U + (remaining - 1U) / capacity;
    if (required > pager_->page_count()) {
      return std::unexpected(Corruption("B-tree payload cannot fit in the current snapshot"));
    }
    return {};
  }

 private:
  CursorCore(ReadPager& pager, PageNumber root_page, bool table, std::uint64_t data_version,
             std::optional<BtreePageGeometry> geometry, bool empty_database) noexcept
      : pager_(&pager),
        root_page_(root_page),
        table_(table),
        data_version_(data_version),
        geometry_(geometry),
        empty_database_(empty_database) {}

  [[nodiscard]] Frame& CurrentFrame() {
    if (frame_count_ == 0) {
      std::terminate();
    }
    const std::size_t index = frame_count_ - 1U;
    Frame* frame = std::get_if<Frame>(&frames_[index]);
    if (frame == nullptr) {
      std::terminate();
    }
    return *frame;
  }

  [[nodiscard]] const Frame& CurrentFrame() const {
    if (frame_count_ == 0) {
      std::terminate();
    }
    const std::size_t index = frame_count_ - 1U;
    const Frame* frame = std::get_if<Frame>(&frames_[index]);
    if (frame == nullptr) {
      std::terminate();
    }
    return *frame;
  }

  [[nodiscard]] Result<BtreePageView> CurrentPage() const {
    if (!geometry_.has_value()) {
      return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
    }
    const Frame& frame = CurrentFrame();
    return BtreePageView::Parse(frame.pin.frame().bytes(), frame.pin.frame().page_number(),
                                geometry_.value());
  }

  [[nodiscard]] Status CheckSnapshot() {
    if (terminal_) {
      return std::unexpected(SchemaChanged("B-tree cursor belongs to an obsolete pager snapshot"));
    }
    if (!pager_->in_read_transaction()) {
      return std::unexpected(Misuse("B-tree cursor requires an active read transaction"));
    }
    if (pager_->data_version() != data_version_) {
      terminal_ = true;
      InvalidatePosition();
      return std::unexpected(SchemaChanged("B-tree cursor belongs to an obsolete pager snapshot"));
    }
    return {};
  }

  void InvalidatePosition() noexcept {
    valid_ = false;
    for (std::size_t index = 1; index < frame_count_; ++index) {
      frames_[index].emplace<std::monostate>();
    }
    Frame* root = std::get_if<Frame>(&frames_[0]);
    if (root != nullptr) {
      frame_count_ = 1;
      root->index = 0;
    } else {
      frame_count_ = 0;
    }
  }

  [[nodiscard]] Result<bool> FailMovement(Error error) {
    InvalidatePosition();
    return std::unexpected(std::move(error));
  }

  [[nodiscard]] Result<bool> ResetToRoot() {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    InvalidatePosition();
    if (empty_database_) {
      return false;
    }

    auto root = CurrentPage();
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    if (root->cell_count() > 0) {
      return true;
    }
    if (root->is_leaf()) {
      return false;
    }
    if (root_page_ != PageNumber{1}) {
      return std::unexpected(Corruption("only page one may contain an empty interior B-tree root"));
    }
    auto descended = DescendChild(0, *root);
    if (!descended.has_value()) {
      return std::unexpected(std::move(descended.error()));
    }
    return true;
  }

  [[nodiscard]] Result<PageNumber> ChildAt(const BtreePageView& page,
                                           std::size_t child_slot) const {
    if (page.is_leaf() || child_slot > page.cell_count()) {
      return std::unexpected(Corruption("B-tree child slot is invalid"));
    }
    if (child_slot == page.cell_count()) {
      if (!page.rightmost_child().has_value()) {
        return std::unexpected(Corruption("B-tree interior page is missing its rightmost child"));
      }
      return *page.rightmost_child();
    }
    auto cell = page.cell(child_slot);
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    if (!cell->left_child().has_value()) {
      return std::unexpected(Corruption("B-tree interior cell is missing its left child"));
    }
    return *cell->left_child();
  }

  [[nodiscard]] Status DescendChild(std::size_t child_slot, const BtreePageView& parent) {
    if (frame_count_ >= kMaximumBtreeDepth) {
      return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
    }
    auto child_page_number = ChildAt(parent, child_slot);
    if (!child_page_number.has_value()) {
      return std::unexpected(std::move(child_page_number.error()));
    }
    CurrentFrame().index = child_slot;

    auto child_pin = pager_->ReadPage(*child_page_number);
    if (!child_pin.has_value()) {
      return std::unexpected(std::move(child_pin.error()));
    }
    if (!geometry_.has_value()) {
      return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
    }
    auto child = BtreePageView::Parse(child_pin->frame().bytes(), child_page_number.value(),
                                      geometry_.value());
    if (!child.has_value()) {
      return std::unexpected(std::move(child.error()));
    }
    if (child->cell_count() == 0) {
      return std::unexpected(Corruption("non-root B-tree page is empty"));
    }
    if (child->is_table() != table_) {
      return std::unexpected(Corruption("B-tree child has the wrong tree kind"));
    }

    frames_[frame_count_].emplace<Frame>(std::move(*child_pin));
    ++frame_count_;
    return {};
  }

  [[nodiscard]] Status DescendLeftmost() {
    while (true) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return std::unexpected(std::move(page.error()));
      }
      if (page->is_leaf()) {
        CurrentFrame().index = 0;
        return {};
      }
      auto descended = DescendChild(0, *page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
    }
  }

  [[nodiscard]] Status DescendRightmost() {
    while (true) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return std::unexpected(std::move(page.error()));
      }
      if (page->is_leaf()) {
        CurrentFrame().index = page->cell_count() - 1U;
        return {};
      }
      auto descended = DescendChild(page->cell_count(), *page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
    }
  }

  void PopFrame() noexcept {
    assert(frame_count_ > 1);
    frames_[frame_count_ - 1U].emplace<std::monostate>();
    --frame_count_;
  }

  [[nodiscard]] Result<bool> NextImpl() {
    auto page = CurrentPage();
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }

    const std::size_t next_index = CurrentFrame().index + 1U;
    if (next_index < page->cell_count()) {
      CurrentFrame().index = next_index;
      if (page->is_leaf()) {
        return true;
      }
      auto descended = DescendChild(next_index, *page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto leftmost = DescendLeftmost();
      if (!leftmost.has_value()) {
        return std::unexpected(std::move(leftmost.error()));
      }
      return true;
    }

    if (!page->is_leaf()) {
      auto descended = DescendChild(page->cell_count(), *page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto leftmost = DescendLeftmost();
      if (!leftmost.has_value()) {
        return std::unexpected(std::move(leftmost.error()));
      }
      return true;
    }

    while (frame_count_ > 1U) {
      PopFrame();
      auto parent = CurrentPage();
      if (!parent.has_value()) {
        return std::unexpected(std::move(parent.error()));
      }
      const std::size_t child_slot = CurrentFrame().index;
      if (child_slot >= parent->cell_count()) {
        continue;
      }
      if (!table_) {
        return true;
      }

      auto descended = DescendChild(child_slot + 1U, *parent);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto leftmost = DescendLeftmost();
      if (!leftmost.has_value()) {
        return std::unexpected(std::move(leftmost.error()));
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] Result<bool> PreviousImpl() {
    auto page = CurrentPage();
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }

    if (!page->is_leaf()) {
      auto descended = DescendChild(CurrentFrame().index, *page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto rightmost = DescendRightmost();
      if (!rightmost.has_value()) {
        return std::unexpected(std::move(rightmost.error()));
      }
      return true;
    }

    if (CurrentFrame().index > 0) {
      --CurrentFrame().index;
      return true;
    }

    while (frame_count_ > 1U) {
      PopFrame();
      auto parent = CurrentPage();
      if (!parent.has_value()) {
        return std::unexpected(std::move(parent.error()));
      }
      const std::size_t child_slot = CurrentFrame().index;
      if (child_slot == 0) {
        continue;
      }
      CurrentFrame().index = child_slot - 1U;
      if (!table_) {
        return true;
      }

      auto descended = DescendChild(child_slot - 1U, *parent);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto rightmost = DescendRightmost();
      if (!rightmost.has_value()) {
        return std::unexpected(std::move(rightmost.error()));
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] Result<bool> PositionForwardCandidate(std::size_t index, std::size_t cell_count) {
    if (index < cell_count) {
      CurrentFrame().index = index;
      valid_ = true;
      return true;
    }
    CurrentFrame().index = cell_count - 1U;
    valid_ = true;
    return NextImpl();
  }

  [[nodiscard]] Result<bool> PositionReverseCandidate(std::size_t index) {
    if (index > 0) {
      CurrentFrame().index = index - 1U;
      valid_ = true;
      return true;
    }
    CurrentFrame().index = 0;
    valid_ = true;
    return PreviousImpl();
  }

  [[nodiscard]] Result<bool> PositionTableLeaf(std::size_t lower, bool exact, BtreeSeekMode mode,
                                               std::size_t cell_count) {
    switch (mode) {
      case BtreeSeekMode::kEqual:
        if (!exact) {
          return false;
        }
        CurrentFrame().index = lower;
        valid_ = true;
        return true;
      case BtreeSeekMode::kGreaterOrEqual:
        return PositionForwardCandidate(lower, cell_count);
      case BtreeSeekMode::kGreater:
        return PositionForwardCandidate(lower + (exact ? 1U : 0U), cell_count);
      case BtreeSeekMode::kLessOrEqual:
        if (exact) {
          CurrentFrame().index = lower;
          valid_ = true;
          return true;
        }
        return PositionReverseCandidate(lower);
      case BtreeSeekMode::kLess:
        return PositionReverseCandidate(lower);
    }
    return std::unexpected(Misuse("invalid B-tree seek mode"));
  }

  ReadPager* pager_;
  PageNumber root_page_;
  bool table_;
  std::uint64_t data_version_;
  std::optional<BtreePageGeometry> geometry_;
  bool empty_database_;
  std::array<std::variant<std::monostate, Frame>, kMaximumBtreeDepth> frames_;
  std::size_t frame_count_ = 0;
  bool valid_ = false;
  bool terminal_ = false;
};

[[nodiscard]] Error MovedFromCursor() { return Misuse("operation on a moved-from B-tree cursor"); }

}  // namespace

struct TableBtreeCursor::Impl {
  explicit Impl(CursorCore cursor_core) noexcept : core(std::move(cursor_core)) {}

  CursorCore core;
};

struct IndexBtreeCursor::Impl {
  Impl(CursorCore cursor_core, std::vector<IndexColumnOrder> column_orders,
       RecordCodecOptions codec_options) noexcept
      : core(std::move(cursor_core)), columns(std::move(column_orders)), options(codec_options) {}

  [[nodiscard]] Result<std::weak_ordering> CompareCell(const BtreeCellView& cell,
                                                       std::span<const SqlValue> key,
                                                       EqualPrefixResult equal_prefix_result) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBtreeComparisons, 1);
    ByteView encoded;
    if (cell.local_payload().size() == cell.payload_size().value()) {
      encoded = cell.local_payload();
    } else {
      auto possible = core.EnsureCompletePayloadPossible(cell);
      if (!possible.has_value()) {
        return std::unexpected(std::move(possible.error()));
      }
      scratch.resize(cell.payload_size().value());
      auto read = core.ReadCellPayload(cell, ByteOffset{0}, MutableByteView{scratch});
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      encoded = ByteView{scratch};
    }

    auto record = RecordView::Parse(encoded, options);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    auto comparison = CompareIndexRecord(*record, key, columns, equal_prefix_result);
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    return comparison->ordering;
  }

  CursorCore core;
  std::vector<IndexColumnOrder> columns;
  RecordCodecOptions options;
  std::vector<std::byte> scratch;
};

TableBtreeCursor::TableBtreeCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TableBtreeCursor::TableBtreeCursor(TableBtreeCursor&&) noexcept = default;

TableBtreeCursor& TableBtreeCursor::operator=(TableBtreeCursor&&) noexcept = default;

TableBtreeCursor::~TableBtreeCursor() = default;

Result<TableBtreeCursor> TableBtreeCursor::Open(ReadPager& pager, PageNumber root_page) {
  auto core = CursorCore::Open(pager, root_page, true);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }
  return TableBtreeCursor{std::make_unique<Impl>(std::move(*core))};
}

bool TableBtreeCursor::valid() const noexcept { return impl_ != nullptr && impl_->core.valid(); }

Result<bool> TableBtreeCursor::First() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.First();
}

Result<bool> TableBtreeCursor::Last() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Last();
}

Result<bool> TableBtreeCursor::Next() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Next();
}

Result<bool> TableBtreeCursor::Previous() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Previous();
}

Result<bool> TableBtreeCursor::Seek(std::int64_t rowid, BtreeSeekMode mode) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.SeekTable(rowid, mode);
}

Result<std::int64_t> TableBtreeCursor::rowid() const {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  if (!cell->rowid().has_value()) {
    return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
  }
  return *cell->rowid();
}

Result<BtreePayloadView> TableBtreeCursor::payload() const {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return BtreePayloadView{cell->payload_size(), cell->local_payload()};
}

Status TableBtreeCursor::ReadPayload(ByteOffset offset, MutableByteView destination) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return impl_->core.ReadCellPayload(*cell, offset, destination);
}

Result<ByteBuffer> TableBtreeCursor::CopyPayload() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  auto possible = impl_->core.EnsureCompletePayloadPossible(*cell);
  if (!possible.has_value()) {
    return std::unexpected(std::move(possible.error()));
  }
  ByteBuffer copied{cell->payload_size()};
  auto read = impl_->core.ReadCellPayload(*cell, ByteOffset{0}, copied.mutable_view());
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  return copied;
}

IndexBtreeCursor::IndexBtreeCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

IndexBtreeCursor::IndexBtreeCursor(IndexBtreeCursor&&) noexcept = default;

IndexBtreeCursor& IndexBtreeCursor::operator=(IndexBtreeCursor&&) noexcept = default;

IndexBtreeCursor::~IndexBtreeCursor() = default;

Result<IndexBtreeCursor> IndexBtreeCursor::Open(ReadPager& pager, PageNumber root_page,
                                                std::span<const IndexColumnOrder> columns) {
  if (!pager.in_read_transaction()) {
    return std::unexpected(Misuse("opening an index cursor requires a read transaction"));
  }
  if (root_page.value() == 0) {
    return std::unexpected(Misuse("B-tree root page zero is invalid"));
  }
  if (columns.empty()) {
    return std::unexpected(Misuse("index cursor requires comparison metadata"));
  }
  for (const IndexColumnOrder& column : columns) {
    if (!IsValidDirection(column.direction()) || !IsValidNullPlacement(column.null_placement())) {
      return std::unexpected(Misuse("index cursor has invalid comparison metadata"));
    }
  }
  if (pager.page_count() == 0) {
    return std::unexpected(Corruption("an empty database contains no index B-tree"));
  }
  const DatabaseHeader* header = pager.header();
  if (header == nullptr) {
    return std::unexpected(Corruption("nonempty database is missing its header"));
  }
  auto options = RecordOptionsFor(*header);
  if (!options.has_value()) {
    return std::unexpected(std::move(options.error()));
  }
  auto core = CursorCore::Open(pager, root_page, false);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }

  std::vector<IndexColumnOrder> copied_columns;
  copied_columns.reserve(columns.size());
  copied_columns.insert(copied_columns.end(), columns.begin(), columns.end());
  return IndexBtreeCursor{
      std::make_unique<Impl>(std::move(*core), std::move(copied_columns), *options)};
}

bool IndexBtreeCursor::valid() const noexcept { return impl_ != nullptr && impl_->core.valid(); }

Result<bool> IndexBtreeCursor::First() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.First();
}

Result<bool> IndexBtreeCursor::Last() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Last();
}

Result<bool> IndexBtreeCursor::Next() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Next();
}

Result<bool> IndexBtreeCursor::Previous() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.Previous();
}

Result<bool> IndexBtreeCursor::Seek(std::span<const SqlValue> key, BtreeSeekMode mode) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  if (key.empty()) {
    impl_->core.InvalidateForFailedSeek();
    return std::unexpected(Misuse("index seek key must not be empty"));
  }
  if (key.size() > impl_->columns.size()) {
    impl_->core.InvalidateForFailedSeek();
    return std::unexpected(Misuse("index seek key is longer than the comparison metadata"));
  }
  const EqualPrefixResult prefix_result = PrefixResultFor(mode);
  return impl_->core.SeekIndex(mode, [this, key, prefix_result](const BtreeCellView& cell) {
    return impl_->CompareCell(cell, key, prefix_result);
  });
}

Result<BtreePayloadView> IndexBtreeCursor::payload() const {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return BtreePayloadView{cell->payload_size(), cell->local_payload()};
}

Status IndexBtreeCursor::ReadPayload(ByteOffset offset, MutableByteView destination) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return impl_->core.ReadCellPayload(*cell, offset, destination);
}

Result<ByteBuffer> IndexBtreeCursor::CopyPayload() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  auto possible = impl_->core.EnsureCompletePayloadPossible(*cell);
  if (!possible.has_value()) {
    return std::unexpected(std::move(possible.error()));
  }
  ByteBuffer copied{cell->payload_size()};
  auto read = impl_->core.ReadCellPayload(*cell, ByteOffset{0}, copied.mutable_view());
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  return copied;
}

}  // namespace modern_sqlite
