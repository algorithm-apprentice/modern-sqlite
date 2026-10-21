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
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "cursor_state.hpp"
#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

using btree_internal::BtreeCursorPhase;
using btree_internal::CursorAccess;

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

[[nodiscard]] Error OutOfRange(std::string_view message) noexcept {
  return MakeError(ErrorCode::kOutOfRange, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) noexcept {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] Error Protocol(std::string_view message) noexcept {
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

enum class PayloadReadExtent : std::uint8_t {
  kRequestedRange,
  kCompletePayload,
};

class CursorCore final : public btree_internal::CursorState {
 private:
  struct Frame {
    explicit Frame(ReadPagePin page_pin) noexcept : pin(std::move(page_pin)) {}

    ReadPagePin pin;
    std::size_t index = 0;
  };

  struct RootPage {
    ReadPagePin pin;
    BtreePageView view;
  };

  [[nodiscard]] static Result<RootPage> LoadRoot(Pager& pager, PageNumber root_page,
                                                 BtreePageGeometry geometry, bool table) {
    auto pin = pager.ReadPage(root_page);
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    auto page = BtreePageView::Parse(pin->frame().bytes(), root_page, geometry);
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    if (page->is_table() != table) {
      return std::unexpected(Corruption("B-tree root has the wrong tree kind"));
    }
    if (page->cell_count() == 0 && !page->is_leaf() && root_page != PageNumber{1}) {
      return std::unexpected(Corruption("only page one may contain an empty interior B-tree root"));
    }
    return RootPage{.pin = std::move(*pin), .view = *page};
  }

 public:
  CursorCore(const CursorCore&) = delete;
  CursorCore& operator=(const CursorCore&) = delete;
  CursorCore(CursorCore&&) noexcept = default;
  CursorCore& operator=(CursorCore&&) = delete;
  ~CursorCore() override { Detach(); }

  [[nodiscard]] static Result<CursorCore> Open(BtreeDatabase& database, PageNumber root_page,
                                               bool table) {
    Pager& pager = database.pager();
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
      return CursorCore{database, root_page, table, data_version, std::nullopt, true};
    }

    const DatabaseHeader* header = pager.header();
    if (header == nullptr) {
      return std::unexpected(Corruption("nonempty database is missing its header"));
    }
    auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
    if (!geometry.has_value()) {
      return std::unexpected(std::move(geometry.error()));
    }

    auto root = LoadRoot(pager, root_page, *geometry, table);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    CursorCore core{database, root_page, table, data_version, *geometry, false};
    core.frames_[0].emplace<Frame>(std::move(root->pin));
    core.frame_count_ = 1;
    core.page_view_.emplace(root->view);
    return core;
  }

  [[nodiscard]] bool valid() const noexcept { return state_ == BtreeCursorPhase::Valid; }

  void ClearPosition() noexcept {
    DiscardSavedPosition();
    InvalidatePosition();
  }

  [[nodiscard]] Result<std::uint64_t> CountEntries() {
    auto counted = CountEntriesImpl();
    InvalidatePosition();
    return counted;
  }

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
    state_ = BtreeCursorPhase::Valid;
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
    state_ = BtreeCursorPhase::Valid;
    return true;
  }

  [[nodiscard]] Result<bool> Next() {
    cell_view_.reset();
    auto prepared = PrepareTableStep(true);
    if (!prepared.has_value()) {
      return FailMovement(std::move(prepared.error()));
    }
    if (prepared->has_value()) {
      return **prepared;
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
    cell_view_.reset();
    auto prepared = PrepareTableStep(false);
    if (!prepared.has_value()) {
      return FailMovement(std::move(prepared.error()));
    }
    if (prepared->has_value()) {
      return **prepared;
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
    auto nearest = MoveToTableKey(target);
    if (!nearest.has_value()) {
      return FailMovement(std::move(nearest.error()));
    }
    if (state_ != BtreeCursorPhase::Valid) {
      return false;
    }
    Result<bool> positioned{true};
    switch (mode) {
      case BtreeSeekMode::kEqual:
        positioned = *nearest == 0;
        break;
      case BtreeSeekMode::kGreaterOrEqual:
        if (*nearest < 0) {
          positioned = NextImpl();
        }
        break;
      case BtreeSeekMode::kGreater:
        if (*nearest <= 0) {
          positioned = NextImpl();
        }
        break;
      case BtreeSeekMode::kLessOrEqual:
        if (*nearest > 0) {
          positioned = PreviousImpl();
        }
        break;
      case BtreeSeekMode::kLess:
        if (*nearest >= 0) {
          positioned = PreviousImpl();
        }
        break;
    }
    if (!positioned.has_value()) {
      return FailMovement(std::move(positioned.error()));
    }
    if (!*positioned) {
      InvalidatePosition();
    }
    return *positioned;
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
      auto current_page = CurrentPage();
      if (!current_page.has_value()) {
        return FailMovement(std::move(current_page.error()));
      }
      const BtreePageView& page = **current_page;
      std::size_t lower = 0;
      std::size_t upper = page.cell_count();
      while (lower < upper) {
        const std::size_t middle = lower + (upper - lower) / 2U;
        auto cell = page.cell(middle);
        if (!cell.has_value()) {
          return FailMovement(std::move(cell.error()));
        }
        auto ordering = compare_cell(*cell);
        if (!ordering.has_value()) {
          return FailMovement(std::move(ordering.error()));
        }
        if (*ordering == std::weak_ordering::equivalent) {
          SetCurrentIndex(middle);
          state_ = BtreeCursorPhase::Valid;
          return true;
        }
        if (*ordering == std::weak_ordering::less) {
          lower = middle + 1U;
        } else {
          upper = middle;
        }
      }

      if (!page.is_leaf()) {
        auto descended = DescendChild(lower, page);
        if (!descended.has_value()) {
          return FailMovement(std::move(descended.error()));
        }
        continue;
      }

      Result<bool> positioned =
          mode == BtreeSeekMode::kGreaterOrEqual || mode == BtreeSeekMode::kGreater
              ? PositionForwardCandidate(lower, page.cell_count())
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

  [[nodiscard]] Result<const BtreeCellView*> CurrentCell() {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    if (state_ == BtreeCursorPhase::RequireSeek || state_ == BtreeCursorPhase::SkipNext) {
      auto restored = RestoreTablePosition();
      if (!restored.has_value()) {
        return std::unexpected(std::move(restored.error()));
      }
      if (!*restored) {
        return std::unexpected(MakeError(ErrorCode::kNotFound, "original table row was deleted"));
      }
    }
    if (state_ != BtreeCursorPhase::Valid) {
      return std::unexpected(Misuse("B-tree cursor is not positioned"));
    }
    if (!cell_view_.has_value()) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return std::unexpected(std::move(page.error()));
      }
      auto cell = (*page)->cell(CurrentFrame().index);
      if (!cell.has_value()) {
        return std::unexpected(std::move(cell.error()));
      }
      cell_view_.emplace(*cell);
    }
    return &*cell_view_;
  }

  [[nodiscard]] Status ReadCellPayload(
      const BtreeCellView& cell, ByteOffset offset, MutableByteView destination,
      PayloadReadExtent extent = PayloadReadExtent::kRequestedRange) {
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    const std::size_t payload_size = cell.payload_size().value();
    const std::size_t payload_offset = offset.value();
    if (payload_offset > payload_size || destination.size() > payload_size - payload_offset) {
      return std::unexpected(OutOfRange("B-tree payload range is out of bounds"));
    }
    if (destination.empty() && extent == PayloadReadExtent::kRequestedRange) {
      return {};
    }
    if (!geometry_.has_value()) {
      return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
    }
    const BtreePageGeometry geometry = geometry_.value();

    const std::size_t local_size = cell.local_payload().size();
    const std::size_t range_end = payload_offset + destination.size();
    const std::size_t walk_end =
        extent == PayloadReadExtent::kCompletePayload ? payload_size : range_end;
    if (walk_end > local_size) {
      const std::size_t overflow_end = walk_end - local_size;
      const std::size_t capacity = geometry.overflow_payload_capacity().value();
      const std::size_t pages_to_reach = 1U + (overflow_end - 1U) / capacity;
      if (pages_to_reach > pager_->page_count()) {
        return std::unexpected(
            Corruption("B-tree payload range cannot fit in the current snapshot"));
      }
    }

    const auto copy_overlap = [payload_offset, range_end, destination](ByteView source,
                                                                       std::size_t source_offset) {
      const std::size_t first = std::max(payload_offset, source_offset);
      const std::size_t last = std::min(range_end, source_offset + source.size());
      if (first >= last) {
        return;
      }
      const std::size_t count = last - first;
      std::memcpy(destination.data() + (first - payload_offset),
                  source.data() + (first - source_offset), count);
      MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBytesCopied, count);
    };
    copy_overlap(cell.local_payload(), 0);

    std::optional<PageNumber> next_page = cell.first_overflow_page();
    const std::size_t capacity = geometry.overflow_payload_capacity().value();
    std::size_t source_offset = local_size;
    while (source_offset < walk_end) {
      if (!next_page.has_value()) {
        return std::unexpected(Corruption("B-tree overflow chain ends prematurely"));
      }
      auto pin = pager_->ReadPage(*next_page);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      const ByteView page = pin->frame().bytes();
      const std::size_t count = std::min(capacity, walk_end - source_offset);
      copy_overlap(page.subspan(4U, count), source_offset);
      source_offset += count;
      if (source_offset == walk_end) {
        return {};
      }

      auto overflow = OverflowPageView::Parse(page, geometry);
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      next_page = overflow->next_page();
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

  [[nodiscard]] Result<ByteBuffer> CopyPayload() try {
    auto cell = CurrentCell();
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    auto possible = EnsureCompletePayloadPossible(**cell);
    if (!possible.has_value()) {
      return std::unexpected(std::move(possible.error()));
    }
    ByteBuffer copied{(*cell)->payload_size()};
    auto read = ReadCellPayload(**cell, ByteOffset{0}, copied.mutable_view());
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    return copied;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }

 private:
  [[nodiscard]] Result<int> MoveToTableKey(std::int64_t target) override {
    if (!table_) {
      return std::unexpected(Misuse("rowid positioning requires a table cursor"));
    }
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (!*ready) {
      return -1;
    }
    while (true) {
      auto current_page = CurrentPage();
      if (!current_page.has_value()) {
        return std::unexpected(std::move(current_page.error()));
      }
      const BtreePageView& page = **current_page;
      auto search = page.SearchRowid(target);
      if (!search.has_value()) {
        return std::unexpected(std::move(search.error()));
      }
      if (!page.is_leaf()) {
        auto descended = DescendChild(search->insertion_index, page);
        if (!descended.has_value()) {
          return std::unexpected(std::move(descended.error()));
        }
        continue;
      }
      SetCurrentIndex(search->cell_index);
      state_ = BtreeCursorPhase::Valid;
      return search->comparison;
    }
  }

  CursorCore(BtreeDatabase& database, PageNumber root_page, bool table, std::uint64_t data_version,
             std::optional<BtreePageGeometry> geometry, bool empty_database) noexcept
      : CursorState(database, empty_database ? PageNumber{} : root_page, CursorAccess::Read),
        pager_(&database.pager()),
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

  void SetCurrentIndex(std::size_t index) noexcept {
    cell_view_.reset();
    CurrentFrame().index = index;
  }

  [[nodiscard]] Result<const BtreePageView*> CurrentPage() const {
    if (!page_view_.has_value()) {
      if (!geometry_.has_value()) {
        return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
      }
      const Frame& frame = CurrentFrame();
      auto page = BtreePageView::Parse(frame.pin.frame().bytes(), frame.pin.frame().page_number(),
                                       geometry_.value());
      if (!page.has_value()) {
        return std::unexpected(std::move(page.error()));
      }
      page_view_.emplace(*page);
    }
    return &*page_view_;
  }

  [[nodiscard]] Status CheckSnapshot() {
    if (state_ == BtreeCursorPhase::Fault) {
      return std::unexpected(SchemaChanged("B-tree cursor belongs to an obsolete pager snapshot"));
    }
    if (!pager_->in_read_transaction()) {
      return std::unexpected(Misuse("B-tree cursor requires an active read transaction"));
    }
    if (pager_->data_version() != data_version_) {
      state_ = BtreeCursorPhase::Fault;
      InvalidatePosition();
      return std::unexpected(SchemaChanged("B-tree cursor belongs to an obsolete pager snapshot"));
    }
    return {};
  }

  [[nodiscard]] Status CheckPositionActive() override { return CheckSnapshot(); }

  [[nodiscard]] Result<std::int64_t> CaptureTableKey() override {
    if (!table_) {
      return std::unexpected(Misuse("rowid capture requires a table cursor"));
    }
    auto cell = CurrentCell();
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    const auto rowid = (*cell)->rowid();
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
    }
    return *rowid;
  }

  void InvalidatePositionMetadata() noexcept override {
    cell_view_.reset();
    page_view_.reset();
  }

  void ReleasePathPages() noexcept override {
    InvalidatePositionMetadata();
    for (std::size_t index = 0; index < frame_count_; ++index) {
      frames_[index].emplace<std::monostate>();
    }
    frame_count_ = 0;
  }

  void InvalidatePosition() noexcept {
    if (state_ != BtreeCursorPhase::Fault) {
      state_ = BtreeCursorPhase::Invalid;
    }
    cell_view_.reset();
    if (frame_count_ != 1U) {
      page_view_.reset();
    }
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
    DiscardSavedPosition();
    cell_view_.reset();
    auto snapshot = CheckSnapshot();
    if (!snapshot.has_value()) {
      return std::unexpected(std::move(snapshot.error()));
    }
    InvalidatePosition();
    if (empty_database_) {
      return false;
    }
    if (frame_count_ == 0U) {
      if (!geometry_.has_value()) {
        return std::unexpected(Corruption("B-tree cursor is missing page geometry"));
      }
      auto root = LoadRoot(*pager_, root_page_, *geometry_, table_);
      if (!root.has_value()) {
        return std::unexpected(std::move(root.error()));
      }
      frames_[0].emplace<Frame>(std::move(root->pin));
      frame_count_ = 1;
      page_view_.emplace(root->view);
    }

    auto current_page = CurrentPage();
    if (!current_page.has_value()) {
      return std::unexpected(std::move(current_page.error()));
    }
    const BtreePageView& root = **current_page;
    if (root.cell_count() > 0) {
      return true;
    }
    if (root.is_leaf()) {
      return false;
    }
    if (root_page_ != PageNumber{1}) {
      return std::unexpected(Corruption("only page one may contain an empty interior B-tree root"));
    }
    auto descended = DescendChild(0, root);
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
    SetCurrentIndex(child_slot);

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

    cell_view_.reset();
    page_view_.reset();
    frames_[frame_count_].emplace<Frame>(std::move(*child_pin));
    ++frame_count_;
    page_view_.emplace(*child);
    return {};
  }

  [[nodiscard]] Status DescendLeftmost() {
    while (true) {
      auto current_page = CurrentPage();
      if (!current_page.has_value()) {
        return std::unexpected(std::move(current_page.error()));
      }
      const BtreePageView& page = **current_page;
      if (page.is_leaf()) {
        SetCurrentIndex(0);
        return {};
      }
      auto descended = DescendChild(0, page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
    }
  }

  [[nodiscard]] Status DescendRightmost() {
    while (true) {
      auto current_page = CurrentPage();
      if (!current_page.has_value()) {
        return std::unexpected(std::move(current_page.error()));
      }
      const BtreePageView& page = **current_page;
      if (page.is_leaf()) {
        SetCurrentIndex(page.cell_count() - 1U);
        return {};
      }
      auto descended = DescendChild(page.cell_count(), page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
    }
  }

  void PopFrame() noexcept {
    assert(frame_count_ > 1);
    cell_view_.reset();
    page_view_.reset();
    frames_[frame_count_ - 1U].emplace<std::monostate>();
    --frame_count_;
  }

  [[nodiscard]] Result<std::uint64_t> CountEntriesImpl() {
    auto ready = ResetToRoot();
    if (!ready.has_value()) {
      return std::unexpected(std::move(ready.error()));
    }
    if (!*ready) {
      return 0;
    }
    std::uint64_t entries = 0;
    while (true) {
      auto page = CurrentPage();
      if (!page.has_value()) {
        return std::unexpected(std::move(page.error()));
      }
      if ((*page)->is_leaf() || !table_) {
        const auto cells = static_cast<std::uint64_t>((*page)->cell_count());
        if (cells > std::numeric_limits<std::uint64_t>::max() - entries) {
          return std::unexpected(Corruption("B-tree entry count is out of range"));
        }
        entries += cells;
      }
      if ((*page)->is_leaf()) {
        while (true) {
          if (frame_count_ == 1U) {
            return entries;
          }
          PopFrame();
          page = CurrentPage();
          if (!page.has_value()) {
            return std::unexpected(std::move(page.error()));
          }
          if (CurrentFrame().index < (*page)->cell_count()) {
            SetCurrentIndex(CurrentFrame().index + 1U);
            break;
          }
        }
      }
      auto descended = DescendChild(CurrentFrame().index, **page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
    }
  }

  [[nodiscard]] Result<bool> NextImpl() {
    auto current_page = CurrentPage();
    if (!current_page.has_value()) {
      return std::unexpected(std::move(current_page.error()));
    }
    const BtreePageView& page = **current_page;

    const std::size_t next_index = CurrentFrame().index + 1U;
    if (next_index < page.cell_count()) {
      SetCurrentIndex(next_index);
      if (page.is_leaf()) {
        return true;
      }
      auto descended = DescendChild(next_index, page);
      if (!descended.has_value()) {
        return std::unexpected(std::move(descended.error()));
      }
      auto leftmost = DescendLeftmost();
      if (!leftmost.has_value()) {
        return std::unexpected(std::move(leftmost.error()));
      }
      return true;
    }

    if (!page.is_leaf()) {
      auto descended = DescendChild(page.cell_count(), page);
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
      auto parent_page = CurrentPage();
      if (!parent_page.has_value()) {
        return std::unexpected(std::move(parent_page.error()));
      }
      const BtreePageView& parent = **parent_page;
      const std::size_t child_slot = CurrentFrame().index;
      if (child_slot >= parent.cell_count()) {
        continue;
      }
      if (!table_) {
        return true;
      }

      auto descended = DescendChild(child_slot + 1U, parent);
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
    auto current_page = CurrentPage();
    if (!current_page.has_value()) {
      return std::unexpected(std::move(current_page.error()));
    }
    const BtreePageView& page = **current_page;

    if (!page.is_leaf()) {
      auto descended = DescendChild(CurrentFrame().index, page);
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
      SetCurrentIndex(CurrentFrame().index - 1U);
      return true;
    }

    while (frame_count_ > 1U) {
      PopFrame();
      auto parent_page = CurrentPage();
      if (!parent_page.has_value()) {
        return std::unexpected(std::move(parent_page.error()));
      }
      const BtreePageView& parent = **parent_page;
      const std::size_t child_slot = CurrentFrame().index;
      if (child_slot == 0) {
        continue;
      }
      SetCurrentIndex(child_slot - 1U);
      if (!table_) {
        return true;
      }

      auto descended = DescendChild(child_slot - 1U, parent);
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
      SetCurrentIndex(index);
      state_ = BtreeCursorPhase::Valid;
      return true;
    }
    SetCurrentIndex(cell_count - 1U);
    state_ = BtreeCursorPhase::Valid;
    return NextImpl();
  }

  [[nodiscard]] Result<bool> PositionReverseCandidate(std::size_t index) {
    if (index > 0) {
      SetCurrentIndex(index - 1U);
      state_ = BtreeCursorPhase::Valid;
      return true;
    }
    SetCurrentIndex(0);
    state_ = BtreeCursorPhase::Valid;
    return PreviousImpl();
  }

  Pager* pager_;
  bool table_;
  std::uint64_t data_version_;
  std::optional<BtreePageGeometry> geometry_;
  bool empty_database_;
  std::array<std::variant<std::monostate, Frame>, kMaximumBtreeDepth> frames_;
  // End the current-frame borrow before releasing its owning pins.
  mutable std::optional<BtreePageView> page_view_;
  std::optional<BtreeCellView> cell_view_;
  std::size_t frame_count_ = 0;
};

[[nodiscard]] Error MovedFromCursor() noexcept {
  return Misuse("operation on a moved-from B-tree cursor");
}

}  // namespace

struct TableBtreeCursor::Impl {
  explicit Impl(CursorCore cursor_core) noexcept : core(std::move(cursor_core)) {}

  CursorCore core;
};

struct IndexBtreeCursor::Impl {
  Impl(CursorCore cursor_core, std::vector<IndexColumnOrder> column_orders,
       RecordCodecOptions codec_options) noexcept
      : core(std::move(cursor_core)), columns(std::move(column_orders)), options(codec_options) {}

  template <typename Compare>
  [[nodiscard]] Result<std::weak_ordering> CompareCellPrefix(const BtreeCellView& cell,
                                                             std::size_t requested_fields,
                                                             Compare&& compare,
                                                             ByteBuffer& scratch) {
    MODERN_SQLITE_RECORD_COUNTER(instrumentation::Counter::kBtreeComparisons, 1);
    if (cell.local_payload().size() == cell.payload_size().value()) {
      auto record = RecordView::Parse(cell.local_payload(), options);
      if (!record.has_value()) {
        return std::unexpected(std::move(record.error()));
      }
      return compare(*record);
    }
    auto record = ReadCellRecordPrefix(cell, requested_fields, scratch,
                                       ByteCount{std::numeric_limits<std::size_t>::max()});
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    return compare(*record);
  }

  [[nodiscard]] Result<RecordPrefixView> ReadCellRecordPrefix(const BtreeCellView& cell,
                                                              std::size_t requested_fields,
                                                              ByteBuffer& scratch,
                                                              ByteCount maximum_record_bytes) {
    auto possible = core.EnsureCompletePayloadPossible(cell);
    if (!possible.has_value()) {
      return std::unexpected(std::move(possible.error()));
    }
    if (cell.payload_size() > maximum_record_bytes) {
      return std::unexpected(
          MakeError(ErrorCode::kTooLarge, "B-tree record exceeds the requested size limit"));
    }
    if (cell.local_payload().size() == cell.payload_size().value()) {
      return RecordPrefixView::Parse(cell.local_payload(), cell.payload_size(), requested_fields,
                                     options);
    }
    bool complete_chain_read = false;
    const auto fail_after_chain_validation =
        [this, &cell, &complete_chain_read](Error error) -> Result<RecordPrefixView> {
      if (!complete_chain_read) {
        auto read =
            core.ReadCellPayload(cell, ByteOffset{0}, {}, PayloadReadExtent::kCompletePayload);
        if (!read.has_value()) {
          return std::unexpected(std::move(read.error()));
        }
      }
      return std::unexpected(std::move(error));
    };
    auto header_size =
        RecordPrefixView::RequiredHeaderSize(cell.local_payload(), cell.payload_size(), options);
    if (!header_size.has_value()) {
      return fail_after_chain_validation(std::move(header_size.error()));
    }
    ByteView header = cell.local_payload();
    if (header_size->value() > header.size()) {
      auto read = ReadComparisonPrefix(cell, *header_size, scratch);
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      complete_chain_read = true;
      header = scratch.view().first(header_size->value());
    }
    auto prefix_size = RecordPrefixView::RequiredPrefixSize(header, cell.payload_size(),
                                                            requested_fields, options);
    header = {};
    if (!prefix_size.has_value()) {
      return fail_after_chain_validation(std::move(prefix_size.error()));
    }

    ByteView encoded;
    if (prefix_size->value() <= cell.local_payload().size()) {
      auto read =
          core.ReadCellPayload(cell, ByteOffset{0}, {}, PayloadReadExtent::kCompletePayload);
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      encoded = cell.local_payload().first(prefix_size->value());
    } else if (complete_chain_read && *prefix_size == *header_size) {
      encoded = scratch.view().first(prefix_size->value());
    } else {
      auto read = ReadComparisonPrefix(cell, *prefix_size, scratch);
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      encoded = scratch.view().first(prefix_size->value());
    }
    auto record = RecordPrefixView::Parse(encoded, cell.payload_size(), requested_fields, options);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    return *record;
  }

  [[nodiscard]] Result<std::weak_ordering> CompareCell(const BtreeCellView& cell,
                                                       std::span<const SqlValue> key,
                                                       EqualPrefixResult equal_prefix_result,
                                                       ByteBuffer& scratch) {
    const auto compare = [this, key,
                          equal_prefix_result](const auto& record) -> Result<std::weak_ordering> {
      auto comparison = CompareIndexRecord(record, key, columns, equal_prefix_result);
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      return comparison->ordering;
    };
    return CompareCellPrefix(cell, key.size(), compare, scratch);
  }

  [[nodiscard]] Result<std::weak_ordering> CompareCellRecordPrefix(const BtreeCellView& cell,
                                                                   const RecordView& key,
                                                                   std::size_t key_field_count,
                                                                   ByteBuffer& scratch) {
    const auto compare = [this, &key,
                          key_field_count](const auto& record) -> Result<std::weak_ordering> {
      return CompareRecordPrefixes(
          record, key, std::span<const IndexColumnOrder>{columns}.first(key_field_count));
    };
    return CompareCellPrefix(cell, key_field_count, compare, scratch);
  }

  [[nodiscard]] Status ReadComparisonPrefix(const BtreeCellView& cell, ByteCount size,
                                            ByteBuffer& scratch) {
    if (scratch.size() < size) {
      scratch = ByteBuffer{};
      try {
        scratch = ByteBuffer{size};
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      } catch (const std::length_error&) {
        return std::unexpected(Error::OutOfMemory());
      }
    }
    return core.ReadCellPayload(cell, ByteOffset{0}, scratch.mutable_view().first(size.value()),
                                PayloadReadExtent::kCompletePayload);
  }

  CursorCore core;
  std::vector<IndexColumnOrder> columns;
  RecordCodecOptions options;
};

TableBtreeCursor::TableBtreeCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TableBtreeCursor::TableBtreeCursor(TableBtreeCursor&&) noexcept = default;

TableBtreeCursor& TableBtreeCursor::operator=(TableBtreeCursor&&) noexcept = default;

TableBtreeCursor::~TableBtreeCursor() = default;

Result<TableBtreeCursor> TableBtreeCursor::Open(BtreeDatabase& database, PageNumber root_page) try {
  auto core = CursorCore::Open(database, root_page, true);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }
  return TableBtreeCursor{std::make_unique<Impl>(std::move(*core))};
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
} catch (const std::length_error&) {
  return std::unexpected(Error::OutOfMemory());
}

bool TableBtreeCursor::valid() const noexcept { return impl_ != nullptr && impl_->core.valid(); }

bool TableBtreeCursor::HasMoved() const noexcept {
  return impl_ != nullptr && impl_->core.HasMoved();
}

bool TableBtreeCursor::HasPosition() const noexcept {
  return impl_ != nullptr && impl_->core.HasPosition();
}

Result<bool> TableBtreeCursor::RestorePosition() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.RestoreTablePosition();
}

Status TableBtreeCursor::ClearPosition() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  impl_->core.ClearPosition();
  return {};
}

Result<std::uint64_t> TableBtreeCursor::CountEntries() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.CountEntries();
}

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
  const std::optional<std::int64_t> rowid = (*cell)->rowid();
  if (!rowid.has_value()) {
    return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
  }
  return *rowid;
}

Result<BtreePayloadView> TableBtreeCursor::payload() const {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return BtreePayloadView{(*cell)->payload_size(), (*cell)->local_payload()};
}

Status TableBtreeCursor::ReadPayload(ByteOffset offset, MutableByteView destination) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return impl_->core.ReadCellPayload(**cell, offset, destination);
}

Result<ByteBuffer> TableBtreeCursor::CopyPayload() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.CopyPayload();
}

IndexBtreeCursor::IndexBtreeCursor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

IndexBtreeCursor::IndexBtreeCursor(IndexBtreeCursor&&) noexcept = default;

IndexBtreeCursor& IndexBtreeCursor::operator=(IndexBtreeCursor&&) noexcept = default;

IndexBtreeCursor::~IndexBtreeCursor() = default;

Result<IndexBtreeCursor> IndexBtreeCursor::Open(BtreeDatabase& database, PageNumber root_page,
                                                std::span<const IndexColumnOrder> columns) try {
  const Pager& pager = database.pager();
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
  auto core = CursorCore::Open(database, root_page, false);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }

  std::vector<IndexColumnOrder> copied_columns;
  copied_columns.reserve(columns.size());
  copied_columns.insert(copied_columns.end(), columns.begin(), columns.end());
  return IndexBtreeCursor{
      std::make_unique<Impl>(std::move(*core), std::move(copied_columns), *options)};
} catch (const std::bad_alloc&) {
  return std::unexpected(Error::OutOfMemory());
} catch (const std::length_error&) {
  return std::unexpected(Error::OutOfMemory());
}

bool IndexBtreeCursor::valid() const noexcept { return impl_ != nullptr && impl_->core.valid(); }

Status IndexBtreeCursor::ClearPosition() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  impl_->core.ClearPosition();
  return {};
}

Result<std::uint64_t> IndexBtreeCursor::CountEntries() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.CountEntries();
}

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
    impl_->core.ClearPosition();
    return std::unexpected(Misuse("index seek key must not be empty"));
  }
  if (key.size() > impl_->columns.size()) {
    impl_->core.ClearPosition();
    return std::unexpected(Misuse("index seek key is longer than the comparison metadata"));
  }
  const EqualPrefixResult prefix_result = PrefixResultFor(mode);
  ByteBuffer scratch;
  return impl_->core.SeekIndex(mode,
                               [this, key, prefix_result, &scratch](const BtreeCellView& cell) {
                                 return impl_->CompareCell(cell, key, prefix_result, scratch);
                               });
}

Result<std::weak_ordering> IndexBtreeCursor::CompareCurrent(std::span<const SqlValue> key) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  if (key.empty()) {
    return std::unexpected(Misuse("index comparison key must not be empty"));
  }
  if (key.size() > impl_->columns.size()) {
    return std::unexpected(Misuse("index comparison key is longer than the comparison metadata"));
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  ByteBuffer scratch;
  return impl_->CompareCell(**cell, key, EqualPrefixResult::kEquivalent, scratch);
}

Result<std::weak_ordering> IndexBtreeCursor::CompareCurrentRecordPrefix(
    const RecordView& key, std::size_t key_field_count) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  if (key_field_count == 0U || key_field_count > key.field_count() ||
      key_field_count > impl_->columns.size()) {
    return std::unexpected(Misuse("encoded index comparison prefix is invalid"));
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  ByteBuffer scratch;
  return impl_->CompareCellRecordPrefix(**cell, key, key_field_count, scratch);
}

Result<RecordPrefixView> IndexBtreeCursor::ReadRecordPrefix(std::size_t requested_fields,
                                                            ByteBuffer& scratch,
                                                            ByteCount maximum_record_bytes) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  if (requested_fields == 0U) {
    return std::unexpected(Misuse("index record prefix requires at least one requested field"));
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return impl_->ReadCellRecordPrefix(**cell, requested_fields, scratch, maximum_record_bytes);
}

Result<BtreePayloadView> IndexBtreeCursor::payload() const {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return BtreePayloadView{(*cell)->payload_size(), (*cell)->local_payload()};
}

Status IndexBtreeCursor::ReadPayload(ByteOffset offset, MutableByteView destination) {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  auto cell = impl_->core.CurrentCell();
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return impl_->core.ReadCellPayload(**cell, offset, destination);
}

Result<ByteBuffer> IndexBtreeCursor::CopyPayload() {
  if (impl_ == nullptr) {
    return std::unexpected(MovedFromCursor());
  }
  return impl_->core.CopyPayload();
}

}  // namespace modern_sqlite
