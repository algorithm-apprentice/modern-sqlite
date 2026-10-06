#include "modern_sqlite/storage/btree/writer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint32_t kMaximumPageNumber = 0xfffffffeU;
constexpr std::uint64_t kMaximumPayloadSize = 0x7fffffffULL;
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

[[nodiscard]] Error Misuse(std::string_view message) {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] Error Corruption(std::string_view message) {
  return MakeError(ErrorCode::kCorruption, message);
}

[[nodiscard]] Error Protocol(std::string_view message) {
  return MakeError(ErrorCode::kProtocol, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Error Constraint(std::string_view message) {
  return MakeError(ErrorCode::kConstraint, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] bool IsValidDirection(IndexSortDirection direction) noexcept {
  return direction == IndexSortDirection::kAscending ||
         direction == IndexSortDirection::kDescending;
}

[[nodiscard]] bool IsValidNullPlacement(IndexNullPlacement placement) noexcept {
  return placement == IndexNullPlacement::kFirst || placement == IndexNullPlacement::kLast;
}

void Store16(MutableByteView bytes, std::size_t offset, std::uint16_t value) noexcept {
  StoreBigEndian<std::uint16_t>(
      std::span<std::byte, sizeof(std::uint16_t)>{bytes.data() + offset, sizeof(std::uint16_t)},
      value);
}

void Store32(MutableByteView bytes, std::size_t offset, std::uint32_t value) noexcept {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{bytes.data() + offset, sizeof(std::uint32_t)},
      value);
}

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + offset, sizeof(std::uint32_t)});
}

[[nodiscard]] std::uint32_t SchemaFormatValue(DatabaseSchemaFormat format) noexcept {
  return static_cast<std::uint32_t>(format);
}

[[nodiscard]] bool IsValidSchemaFormat(DatabaseSchemaFormat format) noexcept {
  switch (format) {
    case DatabaseSchemaFormat::kOne:
    case DatabaseSchemaFormat::kTwo:
    case DatabaseSchemaFormat::kThree:
    case DatabaseSchemaFormat::kFour:
      return true;
  }
  return false;
}

// libc++ models polymorphic-vector move assignment as conditionally allocating.
// NOLINTNEXTLINE(bugprone-exception-escape)
struct CellImage {
  std::pmr::vector<std::byte> encoded;
  std::optional<PageNumber> left_child;
  std::optional<std::int64_t> rowid;
  std::size_t payload_size = 0;
  std::optional<PageNumber> first_overflow_page;
};

// Node images share one session pool, so their vector moves do not allocate.
// NOLINTNEXTLINE(bugprone-exception-escape)
struct NodeImage {
  PageNumber page_number;
  BtreePageType type;
  std::optional<PageNumber> rightmost_child;
  std::pmr::vector<CellImage> cells;

  [[nodiscard]] bool is_leaf() const noexcept {
    return type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  }

  [[nodiscard]] bool is_table() const noexcept {
    return type == BtreePageType::kInteriorTable || type == BtreePageType::kLeafTable;
  }
};

struct TablePathEntry {
  NodeImage parent;
  std::size_t child_slot;
};

struct TableSearchResult {
  std::pmr::vector<TablePathEntry> path;
  NodeImage leaf;
  std::size_t index;
  bool exact;
  std::size_t tree_depth;
};

struct TableSplitPlan {
  NodeImage left;
  NodeImage right;
  CellImage divider;
};

struct MultiSplitPlan {
  std::vector<NodeImage> pages;
  std::vector<CellImage> dividers;
};

struct PromotedSplits {
  std::vector<CellImage> dividers;
  std::vector<PageNumber> right_pages;
};

struct IndexPathEntry {
  NodeImage parent;
  std::size_t child_slot;
};

struct IndexSearchResult {
  std::pmr::vector<IndexPathEntry> path;
  NodeImage node;
  std::size_t index;
  bool exact;
  std::optional<std::size_t> tree_depth;
};

[[nodiscard]] Result<NodeImage> CloneNodeImage(const NodeImage& node) {
  try {
    std::pmr::memory_resource* resource = node.cells.get_allocator().resource();
    NodeImage clone{
        .page_number = node.page_number,
        .type = node.type,
        .rightmost_child = node.rightmost_child,
        .cells = std::pmr::vector<CellImage>{resource},
    };
    clone.cells.reserve(node.cells.size());
    for (const CellImage& cell : node.cells) {
      clone.cells.push_back(CellImage{
          .encoded =
              std::pmr::vector<std::byte>{cell.encoded.begin(), cell.encoded.end(), resource},
          .left_child = cell.left_child,
          .rowid = cell.rowid,
          .payload_size = cell.payload_size,
          .first_overflow_page = cell.first_overflow_page,
      });
    }
    return clone;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

struct IndexSplitPlan {
  NodeImage left;
  NodeImage right;
  CellImage divider;
};

using PageOwnershipSet = std::pmr::unordered_set<std::uint32_t>;

void SetLeftChild(CellImage& cell, PageNumber child) noexcept;

class BtreeMutationCore final {
 public:
  class FreelistAddition final {
   public:
    FreelistAddition(const FreelistAddition&) = delete;
    FreelistAddition& operator=(const FreelistAddition&) = delete;

    FreelistAddition(FreelistAddition&& other) noexcept
        : core_(std::exchange(other.core_, nullptr)), page_(other.page_) {}
    FreelistAddition& operator=(FreelistAddition&&) = delete;

    ~FreelistAddition() {
      if (core_ != nullptr) {
        core_->CancelFreelistAddition(page_);
      }
    }

    void Commit() noexcept { core_ = nullptr; }

   private:
    friend class BtreeMutationCore;

    FreelistAddition(BtreeMutationCore& core, PageNumber page) noexcept
        : core_(&core), page_(page) {}

    BtreeMutationCore* core_;
    PageNumber page_;
  };

  BtreeMutationCore(Pager& pager, std::uint64_t generation)
      : pager_(&pager),
        generation_(generation),
        index_payload_scratch_(&scratch_pool_),
        encoded_record_scratch_(&scratch_pool_),
        root_incarnations_(&scratch_pool_) {}

  [[nodiscard]] Pager& pager() const noexcept { return *pager_; }
  [[nodiscard]] std::pmr::memory_resource* scratch_resource() noexcept { return &scratch_pool_; }

  [[nodiscard]] bool requires_rollback() const noexcept {
    return rollback_required_.has_value() || pager_->write_failure_code().has_value();
  }

  [[nodiscard]] Status CheckActive() const {
    if (pager_->write_transaction_generation() != generation_ || !pager_->in_write_transaction() ||
        pager_->state() == PagerState::kWriterFinished) {
      return std::unexpected(
          SchemaChanged("B-tree writer belongs to an obsolete write transaction"));
    }
    if (rollback_required_.has_value()) {
      return std::unexpected(
          MakeError(*rollback_required_, "B-tree mutation requires transaction rollback"));
    }
    if (const auto failure = pager_->write_failure_code(); failure.has_value()) {
      return std::unexpected(
          MakeError(*failure, "pager write failure requires transaction rollback"));
    }
    return {};
  }

  [[nodiscard]] Status CheckHandle(PageNumber root_page, std::uint64_t incarnation) const {
    auto active = CheckActive();
    if (!active.has_value()) {
      return active;
    }
    if (RootIncarnation(root_page) != incarnation) {
      return std::unexpected(SchemaChanged("B-tree root handle is obsolete"));
    }
    return {};
  }

  [[nodiscard]] Result<BtreePageGeometry> Geometry() const {
    auto active = CheckActive();
    if (!active.has_value()) {
      return std::unexpected(std::move(active.error()));
    }
    if (geometry_.has_value()) {
      return *geometry_;
    }
    const DatabaseHeader* header = pager_->header();
    if (header == nullptr) {
      return std::unexpected(Misuse("B-tree database must be initialized before mutation"));
    }
    return BtreePageGeometry::Create(header->page_size(), header->usable_size());
  }

  void SetGeometry(BtreePageGeometry geometry) noexcept { geometry_ = geometry; }

  void SetRecordOptions(RecordCodecOptions options) noexcept { record_options_ = options; }

  [[nodiscard]] Result<MutableByteView> ResizeIndexPayloadScratch(std::size_t size) {
    try {
      index_payload_scratch_.resize(size);
      return MutableByteView{index_payload_scratch_};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<MutableByteView> ResizeEncodedRecordScratch(std::size_t size) {
    try {
      encoded_record_scratch_.resize(size);
      return MutableByteView{encoded_record_scratch_};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<RecordCodecOptions> RecordOptions() const {
    auto active = CheckActive();
    if (!active.has_value()) {
      return std::unexpected(std::move(active.error()));
    }
    if (record_options_.has_value()) {
      return *record_options_;
    }
    const DatabaseHeader* header = pager_->header();
    if (header == nullptr) {
      return std::unexpected(Misuse("B-tree database must be initialized before mutation"));
    }
    auto schema_format = NormalizeSchemaFormat(header->schema_format());
    if (!schema_format.has_value()) {
      return std::unexpected(std::move(schema_format.error()));
    }
    auto text_encoding = NormalizeTextEncoding(header->text_encoding());
    if (!text_encoding.has_value()) {
      return std::unexpected(std::move(text_encoding.error()));
    }
    if (*text_encoding != DatabaseTextEncoding::kUtf8) {
      return std::unexpected(Protocol("UTF-16 B-tree mutation is not implemented"));
    }
    return RecordCodecOptions{.schema_format = *schema_format};
  }

  [[nodiscard]] std::uint64_t RootIncarnation(PageNumber root_page) const noexcept {
    const auto found = root_incarnations_.find(root_page.value());
    return found == root_incarnations_.end() ? 0U : found->second;
  }

  [[nodiscard]] const PageOwnershipSet* CachedFreelistPages() const noexcept {
    return freelist_pages_.has_value() ? &*freelist_pages_ : nullptr;
  }

  [[nodiscard]] Status CacheFreelistPages(PageOwnershipSet pages) {
    try {
      freelist_pages_.emplace(std::move(pages));
      return {};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<FreelistAddition> ReserveFreelistAddition(PageNumber page_number) {
    if (!freelist_pages_.has_value()) {
      return std::unexpected(Misuse("freelist ownership is not initialized"));
    }
    try {
      const bool inserted = freelist_pages_->insert(page_number.value()).second;
      if (!inserted) {
        return std::unexpected(Corruption("database page is already owned by the freelist"));
      }
      return FreelistAddition{*this, page_number};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Status RemoveFreelistPage(PageNumber page_number) {
    if (!freelist_pages_.has_value()) {
      return std::unexpected(Misuse("freelist ownership is not initialized"));
    }
    if (freelist_pages_->erase(page_number.value()) != 1U) {
      return std::unexpected(Corruption("allocated page is absent from the freelist"));
    }
    return {};
  }

  void MarkRollbackRequired(ErrorCode code) noexcept {
    if (!rollback_required_.has_value()) {
      rollback_required_ = code;
    }
    pager_->ReportWriteCoordinatorFailure(code);
  }

  [[nodiscard]] std::uint64_t mutation_sequence() const noexcept { return mutation_sequence_; }

  void NoteMutation() noexcept { ++mutation_sequence_; }

  void MarkRollbackRequiredAfter(ErrorCode code, std::uint64_t sequence) noexcept {
    if (mutation_sequence_ != sequence) {
      MarkRollbackRequired(code);
    }
  }

  void AdvanceRootIncarnation(PageNumber root_page) {
    const auto found = root_incarnations_.find(root_page.value());
    if (found != root_incarnations_.end()) {
      ++found->second;
    }
  }

  [[nodiscard]] Status EnsureRootIncarnationTracked(PageNumber root_page) {
    if (root_incarnations_.contains(root_page.value())) {
      return {};
    }
    try {
      static_cast<void>(root_incarnations_.try_emplace(root_page.value(), 0U));
      return {};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

 private:
  void CancelFreelistAddition(PageNumber page_number) noexcept {
    if (freelist_pages_.has_value()) {
      static_cast<void>(freelist_pages_->erase(page_number.value()));
    }
  }

  Pager* pager_;
  std::uint64_t generation_;
  std::pmr::unsynchronized_pool_resource scratch_pool_;
  std::pmr::vector<std::byte> index_payload_scratch_;
  std::pmr::vector<std::byte> encoded_record_scratch_;
  std::optional<BtreePageGeometry> geometry_;
  std::optional<RecordCodecOptions> record_options_;
  std::optional<ErrorCode> rollback_required_;
  std::pmr::unordered_map<std::uint32_t, std::uint64_t> root_incarnations_;
  std::optional<PageOwnershipSet> freelist_pages_;
  std::uint64_t mutation_sequence_ = 0;
};

struct PageOneMetadata {
  std::uint32_t page_count;
  PageNumber first_freelist_trunk;
  std::uint32_t freelist_page_count;
};

[[nodiscard]] Result<PageOneMetadata> ReadPageOneMetadata(const BtreeMutationCore& core) {
  auto pin = core.pager().ReadPage(PageNumber{1});
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  const ByteView bytes = pin->frame().bytes();
  const std::uint32_t page_count = core.pager().page_count();
  const std::uint32_t first_trunk = Load32(bytes, 32U);
  const std::uint32_t freelist_count = Load32(bytes, 36U);
  if ((first_trunk == 0U) != (freelist_count == 0U)) {
    return std::unexpected(Corruption("database freelist header fields are inconsistent"));
  }
  if (freelist_count > page_count) {
    return std::unexpected(Corruption("database freelist count exceeds the page count"));
  }
  return PageOneMetadata{
      .page_count = page_count,
      .first_freelist_trunk = PageNumber{first_trunk},
      .freelist_page_count = freelist_count,
  };
}

[[nodiscard]] Result<const PageOwnershipSet*> FreelistPages(BtreeMutationCore& core) {
  if (const PageOwnershipSet* cached = core.CachedFreelistPages(); cached != nullptr) {
    return cached;
  }
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto metadata = ReadPageOneMetadata(core);
  if (!metadata.has_value()) {
    return std::unexpected(std::move(metadata.error()));
  }

  try {
    PageOwnershipSet pages{core.scratch_resource()};
    pages.reserve(metadata->freelist_page_count);
    PageNumber trunk_page = metadata->first_freelist_trunk;
    while (trunk_page.value() != 0U) {
      if (trunk_page == PageNumber{1} || trunk_page == geometry->locking_page() ||
          trunk_page.value() > metadata->page_count ||
          pages.size() >= metadata->freelist_page_count) {
        return std::unexpected(Corruption("database freelist trunk reference is invalid"));
      }
      if (!pages.insert(trunk_page.value()).second) {
        return std::unexpected(Corruption("database freelist contains a duplicate page"));
      }

      auto trunk_pin = core.pager().ReadPage(trunk_page);
      if (!trunk_pin.has_value()) {
        return std::unexpected(std::move(trunk_pin.error()));
      }
      auto trunk = FreelistTrunkView::Parse(trunk_pin->frame().bytes(), *geometry);
      if (!trunk.has_value()) {
        return std::unexpected(std::move(trunk.error()));
      }
      for (std::size_t index = 0; index < trunk->leaf_count(); ++index) {
        if (pages.size() >= metadata->freelist_page_count) {
          return std::unexpected(Corruption("database freelist count is inconsistent"));
        }
        auto leaf_page = trunk->leaf_page(index);
        if (!leaf_page.has_value()) {
          return std::unexpected(std::move(leaf_page.error()));
        }
        if (*leaf_page == PageNumber{1} || *leaf_page == geometry->locking_page() ||
            leaf_page->value() > metadata->page_count) {
          return std::unexpected(Corruption("database freelist leaf reference is invalid"));
        }
        if (!pages.insert(leaf_page->value()).second) {
          return std::unexpected(Corruption("database freelist contains a duplicate page"));
        }
      }
      trunk_page = trunk->next_trunk().value_or(PageNumber{});
    }
    if (pages.size() != metadata->freelist_page_count) {
      return std::unexpected(Corruption("database freelist count is inconsistent"));
    }
    auto cached = core.CacheFreelistPages(std::move(pages));
    if (!cached.has_value()) {
      return std::unexpected(std::move(cached.error()));
    }
    const PageOwnershipSet* cached_pages = core.CachedFreelistPages();
    if (cached_pages == nullptr) {
      return std::unexpected(Misuse("freelist ownership cache was not published"));
    }
    return cached_pages;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<bool> FreelistContainsPage(BtreeMutationCore& core, PageNumber page_number) {
  auto pages = FreelistPages(core);
  if (!pages.has_value()) {
    return std::unexpected(std::move(pages.error()));
  }
  return (*pages)->contains(page_number.value());
}

[[nodiscard]] Status PublishPageCount(BtreeMutationCore& core) {
  auto page_one = core.pager().WritePage(PageNumber{1});
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  core.NoteMutation();
  Store32(page_one->mutable_bytes(), 28U, core.pager().page_count());
  return {};
}

[[nodiscard]] Result<PageNumber> AllocateBtreePage(BtreeMutationCore& core) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto freelist_pages = FreelistPages(core);
  if (!freelist_pages.has_value()) {
    return std::unexpected(std::move(freelist_pages.error()));
  }
  auto metadata = ReadPageOneMetadata(core);
  if (!metadata.has_value()) {
    return std::unexpected(std::move(metadata.error()));
  }

  if (metadata->freelist_page_count != 0U) {
    const PageNumber trunk_page = metadata->first_freelist_trunk;
    if (trunk_page == PageNumber{1} || trunk_page == geometry->locking_page() ||
        trunk_page.value() > metadata->page_count) {
      return std::unexpected(Corruption("database freelist trunk reference is invalid"));
    }

    std::optional<PageNumber> next_trunk;
    std::size_t leaf_count = 0;
    PageNumber allocated;
    {
      auto trunk_pin = core.pager().ReadPage(trunk_page);
      if (!trunk_pin.has_value()) {
        return std::unexpected(std::move(trunk_pin.error()));
      }
      auto trunk = FreelistTrunkView::Parse(trunk_pin->frame().bytes(), *geometry);
      if (!trunk.has_value()) {
        return std::unexpected(std::move(trunk.error()));
      }
      next_trunk = trunk->next_trunk();
      leaf_count = trunk->leaf_count();
      if (leaf_count == 0U) {
        allocated = trunk_page;
      } else {
        auto leaf = trunk->leaf_page(leaf_count - 1U);
        if (!leaf.has_value()) {
          return std::unexpected(std::move(leaf.error()));
        }
        allocated = *leaf;
        if (allocated == PageNumber{1} || allocated == geometry->locking_page() ||
            allocated.value() > metadata->page_count || allocated == trunk_page) {
          return std::unexpected(Corruption("database freelist leaf reference is invalid"));
        }
      }
    }

    if (leaf_count != 0U) {
      auto trunk_pin = core.pager().WritePage(trunk_page);
      if (!trunk_pin.has_value()) {
        return std::unexpected(std::move(trunk_pin.error()));
      }
      core.NoteMutation();
      Store32(trunk_pin->mutable_bytes(), 4U, static_cast<std::uint32_t>(leaf_count - 1U));
    }
    auto page_one = core.pager().WritePage(PageNumber{1});
    if (!page_one.has_value()) {
      core.MarkRollbackRequired(page_one.error().code());
      return std::unexpected(std::move(page_one.error()));
    }
    core.NoteMutation();
    const MutableByteView header = page_one->mutable_bytes();
    if (leaf_count == 0U) {
      Store32(header, 32U, next_trunk.has_value() ? next_trunk->value() : 0U);
    }
    Store32(header, 36U, metadata->freelist_page_count - 1U);
    auto removed = core.RemoveFreelistPage(allocated);
    if (!removed.has_value()) {
      core.MarkRollbackRequired(removed.error().code());
      return std::unexpected(std::move(removed.error()));
    }
    return allocated;
  }

  if (metadata->page_count >= kMaximumPageNumber) {
    return std::unexpected(TooLarge("database page-number space is exhausted"));
  }
  PageNumber allocated;
  {
    auto pin = core.pager().AllocatePage();
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    core.NoteMutation();
    allocated = pin->frame().page_number();
  }
  if (allocated.value() > kMaximumPageNumber) {
    core.MarkRollbackRequired(ErrorCode::kTooLarge);
    return std::unexpected(TooLarge("database page-number space is exhausted"));
  }
  auto published = PublishPageCount(core);
  if (!published.has_value()) {
    core.MarkRollbackRequired(published.error().code());
    return std::unexpected(std::move(published.error()));
  }
  return allocated;
}

[[nodiscard]] Status FreeBtreePage(BtreeMutationCore& core, PageNumber page_number) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto freelist_pages = FreelistPages(core);
  if (!freelist_pages.has_value()) {
    return std::unexpected(std::move(freelist_pages.error()));
  }
  auto metadata = ReadPageOneMetadata(core);
  if (!metadata.has_value()) {
    return std::unexpected(std::move(metadata.error()));
  }
  if (page_number == PageNumber{1} || page_number == geometry->locking_page() ||
      page_number.value() == 0U || page_number.value() > metadata->page_count) {
    return std::unexpected(Corruption("attempted to free an invalid B-tree page"));
  }
  if (metadata->freelist_page_count == (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(Corruption("database freelist count overflows"));
  }
  auto reservation = core.ReserveFreelistAddition(page_number);
  if (!reservation.has_value()) {
    return std::unexpected(std::move(reservation.error()));
  }

  std::size_t first_trunk_leaf_count = 0;
  if (metadata->first_freelist_trunk.value() != 0U) {
    auto trunk_pin = core.pager().ReadPage(metadata->first_freelist_trunk);
    if (!trunk_pin.has_value()) {
      return std::unexpected(std::move(trunk_pin.error()));
    }
    auto trunk = FreelistTrunkView::Parse(trunk_pin->frame().bytes(), *geometry);
    if (!trunk.has_value()) {
      return std::unexpected(std::move(trunk.error()));
    }
    first_trunk_leaf_count = trunk->leaf_count();
  }

  const std::size_t compatible_leaf_capacity = geometry->usable_size().value() / 4U - 8U;
  if (metadata->first_freelist_trunk.value() != 0U &&
      first_trunk_leaf_count < compatible_leaf_capacity) {
    auto trunk_pin = core.pager().WritePage(metadata->first_freelist_trunk);
    if (!trunk_pin.has_value()) {
      return std::unexpected(std::move(trunk_pin.error()));
    }
    core.NoteMutation();
    const MutableByteView trunk = trunk_pin->mutable_bytes();
    Store32(trunk, 8U + first_trunk_leaf_count * 4U, page_number.value());
    Store32(trunk, 4U, static_cast<std::uint32_t>(first_trunk_leaf_count + 1U));
  } else {
    auto freed = core.pager().WritePage(page_number);
    if (!freed.has_value()) {
      return std::unexpected(std::move(freed.error()));
    }
    core.NoteMutation();
    const MutableByteView trunk = freed->mutable_bytes();
    Store32(trunk, 0U, metadata->first_freelist_trunk.value());
    Store32(trunk, 4U, 0U);
  }

  auto page_one = core.pager().WritePage(PageNumber{1});
  if (!page_one.has_value()) {
    core.MarkRollbackRequired(page_one.error().code());
    return std::unexpected(std::move(page_one.error()));
  }
  core.NoteMutation();
  const MutableByteView header = page_one->mutable_bytes();
  if (metadata->first_freelist_trunk.value() == 0U ||
      first_trunk_leaf_count >= compatible_leaf_capacity) {
    Store32(header, 32U, page_number.value());
  }
  Store32(header, 36U, metadata->freelist_page_count + 1U);
  reservation->Commit();
  return {};
}

[[nodiscard]] Status ValidateExistingDatabase(Pager& pager) {
  if (pager.page_count() == 0U) {
    return {};
  }
  const DatabaseHeader* header = pager.header();
  if (header == nullptr) {
    return std::unexpected(Corruption("nonempty B-tree database is missing its header"));
  }
  if (header->largest_root_page().value() != 0U || header->incremental_vacuum() != 0U) {
    return std::unexpected(Protocol("auto-vacuum B-tree mutation is not implemented"));
  }
  auto text_encoding = NormalizeTextEncoding(header->text_encoding());
  if (!text_encoding.has_value()) {
    return std::unexpected(std::move(text_encoding.error()));
  }
  if (*text_encoding != DatabaseTextEncoding::kUtf8) {
    return std::unexpected(Protocol("UTF-16 B-tree mutation is not implemented"));
  }
  auto schema_format = NormalizeSchemaFormat(header->schema_format());
  if (!schema_format.has_value()) {
    return std::unexpected(std::move(schema_format.error()));
  }
  auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto page_one = pager.ReadPage(PageNumber{1});
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  auto root = BtreePageView::Parse(page_one->frame().bytes(), PageNumber{1}, *geometry);
  if (!root.has_value()) {
    return std::unexpected(std::move(root.error()));
  }
  if (!root->is_table()) {
    return std::unexpected(Corruption("database page one is not a table B-tree root"));
  }
  return {};
}

[[nodiscard]] Result<NodeImage> ReadNode(BtreeMutationCore& core, PageNumber page_number) {
  auto freelist_owned = FreelistContainsPage(core, page_number);
  if (!freelist_owned.has_value()) {
    return std::unexpected(std::move(freelist_owned.error()));
  }
  if (*freelist_owned) {
    return std::unexpected(Corruption("B-tree page is also owned by the freelist"));
  }
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto pin = core.pager().ReadPage(page_number);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  auto page = BtreePageView::Parse(pin->frame().bytes(), page_number, *geometry);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }

  try {
    NodeImage node{
        .page_number = page_number,
        .type = page->type(),
        .rightmost_child = page->rightmost_child(),
        .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
    };
    node.cells.reserve(page->cell_count());
    for (std::size_t index = 0; index < page->cell_count(); ++index) {
      auto cell = page->cell(index);
      if (!cell.has_value()) {
        return std::unexpected(std::move(cell.error()));
      }
      auto offset = page->cell_offset(index);
      if (!offset.has_value()) {
        return std::unexpected(std::move(offset.error()));
      }
      const std::size_t start = offset->value();
      const std::size_t size = cell->encoded_size().value();
      if (start > geometry->usable_size().value() ||
          size > geometry->usable_size().value() - start) {
        return std::unexpected(Corruption("B-tree cell encoding exceeds the usable page"));
      }
      const ByteView encoded = pin->frame().bytes().subspan(start, size);
      node.cells.push_back(CellImage{
          .encoded =
              std::pmr::vector<std::byte>{encoded.begin(), encoded.end(), core.scratch_resource()},
          .left_child = cell->left_child(),
          .rowid = cell->rowid(),
          .payload_size = cell->payload_size().value(),
          .first_overflow_page = cell->first_overflow_page(),
      });
    }
    return node;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] bool NodeFits(const NodeImage& node, BtreePageGeometry geometry) noexcept {
  const std::size_t header_offset = node.page_number == PageNumber{1} ? 100U : 0U;
  const std::size_t header_size = node.is_leaf() ? 8U : 12U;
  std::size_t required = header_offset + header_size;
  if (node.cells.size() > (std::numeric_limits<std::uint16_t>::max)()) {
    return false;
  }
  if (node.cells.size() > (geometry.usable_size().value() - required) / sizeof(std::uint16_t)) {
    return false;
  }
  required += node.cells.size() * sizeof(std::uint16_t);
  for (const CellImage& cell : node.cells) {
    if (cell.encoded.size() > geometry.usable_size().value() - required) {
      return false;
    }
    required += cell.encoded.size();
  }
  return required <= geometry.usable_size().value();
}

[[nodiscard]] std::size_t NodeRangeBytes(const NodeImage& node, std::size_t begin, std::size_t end,
                                         bool page_one) noexcept {
  const std::size_t header_offset = page_one ? 100U : 0U;
  const std::size_t header_size = node.is_leaf() ? 8U : 12U;
  std::size_t used = header_offset + header_size + (end - begin) * 2U;
  for (std::size_t index = begin; index < end; ++index) {
    used += node.cells[index].encoded.size();
  }
  return used;
}

[[nodiscard]] bool NodeRangeFits(const NodeImage& node, std::size_t begin, std::size_t end,
                                 BtreePageGeometry geometry, bool page_one = false) noexcept {
  return NodeRangeBytes(node, begin, end, page_one) <= geometry.usable_size().value();
}

[[nodiscard]] Result<std::pmr::vector<std::size_t>> BuildNodeSizePrefix(const NodeImage& node) {
  try {
    std::pmr::vector<std::size_t> prefix{node.cells.get_allocator().resource()};
    prefix.reserve(node.cells.size() + 1U);
    prefix.push_back(0U);
    for (const CellImage& cell : node.cells) {
      if (cell.encoded.size() >
          (std::numeric_limits<std::size_t>::max)() - prefix.back()) {
        return std::unexpected(TooLarge("B-tree cell sizes exceed addressable memory"));
      }
      prefix.push_back(prefix.back() + cell.encoded.size());
    }
    return prefix;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] std::size_t NodeRangeBytes(
    const NodeImage& node, std::span<const std::size_t> prefix, std::size_t begin,
    std::size_t end, bool page_one = false) noexcept {
  const std::size_t header_offset = page_one ? 100U : 0U;
  const std::size_t header_size = node.is_leaf() ? 8U : 12U;
  return header_offset + header_size + (end - begin) * 2U + prefix[end] - prefix[begin];
}

[[nodiscard]] bool NodeRangeFits(const NodeImage& node, std::span<const std::size_t> prefix,
                                 std::size_t begin, std::size_t end,
                                 BtreePageGeometry geometry,
                                 bool page_one = false) noexcept {
  return NodeRangeBytes(node, prefix, begin, end, page_one) <=
         geometry.usable_size().value();
}

[[nodiscard]] Status WriteNode(BtreeMutationCore& core, const NodeImage& node) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  if (!NodeFits(node, *geometry)) {
    return std::unexpected(TooLarge("B-tree page image does not fit"));
  }
  if (node.page_number == PageNumber{1} && !node.is_table()) {
    return std::unexpected(Corruption("database page one must remain a table B-tree"));
  }
  if (node.is_leaf() != !node.rightmost_child.has_value()) {
    return std::unexpected(Corruption("B-tree node has inconsistent child metadata"));
  }

  auto pin = core.pager().WritePage(node.page_number);
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  core.NoteMutation();
  const MutableByteView page = pin->mutable_bytes();
  const std::size_t usable_size = geometry->usable_size().value();
  const std::size_t header_offset = node.page_number == PageNumber{1} ? 100U : 0U;
  const std::size_t header_size = node.is_leaf() ? 8U : 12U;
  std::fill(page.begin() + static_cast<std::ptrdiff_t>(header_offset),
            page.begin() + static_cast<std::ptrdiff_t>(usable_size), std::byte{0});
  page[header_offset] = static_cast<std::byte>(node.type);
  Store16(page, header_offset + 1U, 0U);
  Store16(page, header_offset + 3U, static_cast<std::uint16_t>(node.cells.size()));
  page[header_offset + 7U] = std::byte{0};
  if (!node.is_leaf()) {
    Store32(page, header_offset + 8U, node.rightmost_child->value());
  }

  std::size_t content_offset = usable_size;
  for (std::size_t index = 0; index < node.cells.size(); ++index) {
    const std::pmr::vector<std::byte>& encoded = node.cells[index].encoded;
    content_offset -= encoded.size();
    std::ranges::copy(encoded, page.begin() + static_cast<std::ptrdiff_t>(content_offset));
    Store16(page, header_offset + header_size + index * 2U,
            content_offset == 65536U ? 0U : static_cast<std::uint16_t>(content_offset));
  }
  Store16(page, header_offset + 5U,
          content_offset == 65536U ? 0U : static_cast<std::uint16_t>(content_offset));
  return {};
}

[[nodiscard]] std::size_t LocalPayloadSize(BtreePageGeometry geometry, BtreePageType type,
                                           std::size_t payload_size) noexcept {
  const std::size_t maximum = type == BtreePageType::kLeafTable
                                  ? geometry.maximum_table_leaf_local_payload().value()
                                  : geometry.maximum_index_local_payload().value();
  if (payload_size <= maximum) {
    return payload_size;
  }
  const std::size_t minimum = geometry.minimum_local_payload().value();
  const std::size_t candidate =
      minimum + ((payload_size - minimum) % geometry.overflow_payload_capacity().value());
  return candidate <= maximum ? candidate : minimum;
}

[[nodiscard]] std::size_t MaximumOverflowPageCount(const BtreeMutationCore& core,
                                                   BtreePageGeometry geometry) noexcept {
  const std::uint32_t page_count = core.pager().page_count();
  if (page_count == 0U) {
    return 0U;
  }
  auto eligible_pages = static_cast<std::size_t>(page_count - 1U);
  const PageNumber locking_page = geometry.locking_page();
  if (locking_page.value() > 1U && locking_page.value() <= page_count) {
    --eligible_pages;
  }
  return eligible_pages;
}

[[nodiscard]] Result<std::optional<PageNumber>> WriteOverflowChain(BtreeMutationCore& core,
                                                                   ByteView payload) {
  if (payload.empty()) {
    return std::optional<PageNumber>{};
  }
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const std::size_t capacity = geometry->overflow_payload_capacity().value();
  const std::size_t page_count = 1U + (payload.size() - 1U) / capacity;

  try {
    std::vector<PageNumber> pages;
    pages.reserve(page_count);
    for (std::size_t index = 0; index < page_count; ++index) {
      auto page = AllocateBtreePage(core);
      if (!page.has_value()) {
        if (!pages.empty()) {
          core.MarkRollbackRequired(page.error().code());
        }
        return std::unexpected(std::move(page.error()));
      }
      pages.push_back(*page);
    }

    std::size_t payload_offset = 0;
    for (std::size_t index = 0; index < pages.size(); ++index) {
      auto pin = core.pager().WritePage(pages[index]);
      if (!pin.has_value()) {
        core.MarkRollbackRequired(pin.error().code());
        return std::unexpected(std::move(pin.error()));
      }
      core.NoteMutation();
      const MutableByteView bytes = pin->mutable_bytes();
      std::fill(bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(geometry->usable_size().value()),
                std::byte{0});
      Store32(bytes, 0U, index + 1U < pages.size() ? pages[index + 1U].value() : 0U);
      const std::size_t count = std::min(capacity, payload.size() - payload_offset);
      std::ranges::copy(payload.subspan(payload_offset, count),
                        bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
      payload_offset += count;
    }
    return std::optional<PageNumber>{pages.front()};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<CellImage> EncodeTableLeafCell(BtreeMutationCore& core, std::int64_t rowid,
                                                    ByteView payload, BtreePageGeometry geometry) {
  if (payload.size() > kMaximumPayloadSize) {
    return std::unexpected(TooLarge("table payload exceeds SQLite's limit"));
  }

  std::array<std::byte, 9> payload_size_varint{};
  auto payload_size_bytes =
      EncodeSqliteVarint(payload.size(), MutableByteView{payload_size_varint});
  if (!payload_size_bytes.has_value()) {
    return std::unexpected(Misuse("table payload size cannot be encoded"));
  }
  std::array<std::byte, 9> rowid_varint{};
  auto rowid_bytes =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{rowid_varint});
  if (!rowid_bytes.has_value()) {
    return std::unexpected(Misuse("table rowid cannot be encoded"));
  }

  try {
    const std::size_t local_payload_size =
        LocalPayloadSize(geometry, BtreePageType::kLeafTable, payload.size());
    const bool has_overflow = local_payload_size < payload.size();
    const std::size_t unpadded_size = payload_size_bytes->value() + rowid_bytes->value() +
                                      local_payload_size + (has_overflow ? 4U : 0U);
    const std::size_t encoded_size = std::max<std::size_t>(4U, unpadded_size);
    std::pmr::vector<std::byte> encoded(encoded_size, core.scratch_resource());
    std::size_t offset = 0;
    std::ranges::copy(std::span{payload_size_varint}.first(payload_size_bytes->value()),
                      encoded.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += payload_size_bytes->value();
    std::ranges::copy(std::span{rowid_varint}.first(rowid_bytes->value()),
                      encoded.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += rowid_bytes->value();
    std::ranges::copy(payload.first(local_payload_size),
                      encoded.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += local_payload_size;

    std::optional<PageNumber> first_overflow_page;
    if (has_overflow) {
      auto overflow = WriteOverflowChain(core, payload.subspan(local_payload_size));
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      if (!overflow->has_value()) {
        return std::unexpected(Corruption("overflow payload did not allocate its first page"));
      }
      first_overflow_page = *overflow;
      Store32(MutableByteView{encoded}, offset, first_overflow_page.value().value());
    }
    return CellImage{
        .encoded = std::move(encoded),
        .left_child = std::nullopt,
        .rowid = rowid,
        .payload_size = payload.size(),
        .first_overflow_page = first_overflow_page,
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<ByteView> ReadCellPayload(BtreeMutationCore& core, const CellImage& cell,
                                               BtreePageType type) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const bool interior =
      type == BtreePageType::kInteriorIndex || type == BtreePageType::kInteriorTable;
  std::size_t cursor = interior ? sizeof(std::uint32_t) : 0U;
  if (cursor >= cell.encoded.size()) {
    return std::unexpected(Corruption("B-tree cell payload header is truncated"));
  }
  auto decoded_size = DecodeSqliteVarint(ByteView{cell.encoded}.subspan(cursor));
  if (!decoded_size.has_value() || decoded_size->value != cell.payload_size) {
    return std::unexpected(Corruption("B-tree cell payload size metadata is inconsistent"));
  }
  cursor += decoded_size->bytes_consumed.value();
  const std::size_t local_size = LocalPayloadSize(*geometry, type, cell.payload_size);
  if (cursor > cell.encoded.size() || local_size > cell.encoded.size() - cursor) {
    return std::unexpected(Corruption("B-tree local payload is truncated"));
  }
  std::size_t expected_pages = 0;
  if (local_size == cell.payload_size) {
    if (cell.first_overflow_page.has_value()) {
      return std::unexpected(Corruption("local B-tree payload has an overflow reference"));
    }
    return ByteView{cell.encoded}.subspan(cursor, local_size);
  } else {
    if (!cell.first_overflow_page.has_value()) {
      return std::unexpected(Corruption("B-tree payload is missing its overflow chain"));
    }
    const std::size_t remaining = cell.payload_size - local_size;
    const std::size_t capacity = geometry->overflow_payload_capacity().value();
    expected_pages = 1U + (remaining - 1U) / capacity;
    if (expected_pages > MaximumOverflowPageCount(core, *geometry)) {
      return std::unexpected(Corruption("B-tree overflow chain cannot fit in the database"));
    }
    const PageNumber first_overflow = *cell.first_overflow_page;
    if (first_overflow == PageNumber{1} || first_overflow == geometry->locking_page() ||
        first_overflow.value() == 0U || first_overflow.value() > core.pager().page_count()) {
      return std::unexpected(Corruption("B-tree overflow page reference is invalid"));
    }
  }

  auto freelist_pages = FreelistPages(core);
  if (!freelist_pages.has_value()) {
    return std::unexpected(std::move(freelist_pages.error()));
  }
  PageOwnershipSet seen{core.scratch_resource()};
  try {
    seen.reserve(expected_pages);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  auto scratch = core.ResizeIndexPayloadScratch(cell.payload_size);
  if (!scratch.has_value()) {
    return std::unexpected(std::move(scratch.error()));
  }
  const MutableByteView output = *scratch;
  std::ranges::copy(ByteView{cell.encoded}.subspan(cursor, local_size), output.begin());

  const std::size_t capacity = geometry->overflow_payload_capacity().value();
  std::size_t output_offset = local_size;
  PageNumber current = cell.first_overflow_page.value_or(PageNumber{});
  while (output_offset < cell.payload_size) {
    if (current == PageNumber{1} || current == geometry->locking_page() ||
        current.value() == 0U || current.value() > core.pager().page_count()) {
      return std::unexpected(Corruption("B-tree overflow page reference is invalid"));
    }
    if ((*freelist_pages)->contains(current.value())) {
      return std::unexpected(Corruption("overflow page is also owned by the freelist"));
    }
    if (!seen.insert(current.value()).second) {
      return std::unexpected(Corruption("B-tree overflow chain contains a cycle"));
    }
    auto pin = core.pager().ReadPage(current);
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    auto overflow = OverflowPageView::Parse(pin->frame().bytes(), *geometry);
    if (!overflow.has_value()) {
      return std::unexpected(std::move(overflow.error()));
    }
    const std::size_t count = std::min(capacity, cell.payload_size - output_offset);
    std::ranges::copy(overflow->payload().first(count),
                      output.begin() + static_cast<std::ptrdiff_t>(output_offset));
    output_offset += count;
    if (output_offset < cell.payload_size) {
      if (!overflow->next_page().has_value()) {
        return std::unexpected(Corruption("B-tree overflow chain ends prematurely"));
      }
      current = *overflow->next_page();
    } else if (overflow->next_page().has_value()) {
      return std::unexpected(Corruption("B-tree overflow chain is longer than its payload"));
    }
  }
  return ByteView{output};
}

[[nodiscard]] Result<CellImage> EncodeIndexCell(BtreeMutationCore& core, ByteView payload,
                                                BtreePageType type,
                                                std::optional<PageNumber> left_child) {
  if (payload.size() > kMaximumPayloadSize) {
    return std::unexpected(TooLarge("index payload exceeds SQLite's limit"));
  }
  const bool interior = type == BtreePageType::kInteriorIndex;
  if (interior != left_child.has_value() ||
      (type != BtreePageType::kLeafIndex && type != BtreePageType::kInteriorIndex)) {
    return std::unexpected(Misuse("index cell kind and child metadata do not match"));
  }
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  std::array<std::byte, 9> payload_size_varint{};
  auto payload_size_bytes =
      EncodeSqliteVarint(payload.size(), MutableByteView{payload_size_varint});
  if (!payload_size_bytes.has_value()) {
    return std::unexpected(Misuse("index payload size cannot be encoded"));
  }

  try {
    const std::size_t local_size = LocalPayloadSize(*geometry, type, payload.size());
    const bool has_overflow = local_size < payload.size();
    const std::size_t prefix_size = interior ? sizeof(std::uint32_t) : 0U;
    const std::size_t unpadded_size = prefix_size + payload_size_bytes->value() + local_size +
                                      (has_overflow ? sizeof(std::uint32_t) : 0U);
    const std::size_t encoded_size =
        interior ? unpadded_size : std::max<std::size_t>(4U, unpadded_size);
    std::pmr::vector<std::byte> encoded(encoded_size, core.scratch_resource());
    std::size_t offset = 0;
    if (left_child.has_value()) {
      Store32(MutableByteView{encoded}, 0U, left_child->value());
      offset += sizeof(std::uint32_t);
    }
    std::ranges::copy(std::span{payload_size_varint}.first(payload_size_bytes->value()),
                      encoded.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += payload_size_bytes->value();
    std::ranges::copy(payload.first(local_size),
                      encoded.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += local_size;

    std::optional<PageNumber> first_overflow_page;
    if (has_overflow) {
      auto overflow = WriteOverflowChain(core, payload.subspan(local_size));
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      if (!overflow->has_value()) {
        return std::unexpected(Corruption("overflow payload did not allocate its first page"));
      }
      first_overflow_page = *overflow;
      Store32(MutableByteView{encoded}, offset, first_overflow_page.value().value());
    }
    return CellImage{
        .encoded = std::move(encoded),
        .left_child = left_child,
        .rowid = std::nullopt,
        .payload_size = payload.size(),
        .first_overflow_page = first_overflow_page,
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<CellImage> IndexCellAsInterior(CellImage cell, PageNumber left_child,
                                                    BtreePageGeometry geometry) {
  if (cell.left_child.has_value()) {
    SetLeftChild(cell, left_child);
    return cell;
  }
  auto decoded_size = DecodeSqliteVarint(ByteView{cell.encoded});
  if (!decoded_size.has_value() || decoded_size->value != cell.payload_size) {
    return std::unexpected(Corruption("index leaf cell payload size metadata is inconsistent"));
  }
  const std::size_t local_size =
      LocalPayloadSize(geometry, BtreePageType::kLeafIndex, cell.payload_size);
  const std::size_t unpadded_size =
      decoded_size->bytes_consumed.value() + local_size +
      (cell.first_overflow_page.has_value() ? sizeof(std::uint32_t) : 0U);
  if (cell.encoded.size() != std::max<std::size_t>(4U, unpadded_size)) {
    return std::unexpected(Corruption("index leaf cell encoding size is inconsistent"));
  }
  try {
    std::pmr::vector<std::byte> encoded(sizeof(std::uint32_t) + unpadded_size,
                                        cell.encoded.get_allocator().resource());
    Store32(MutableByteView{encoded}, 0U, left_child.value());
    std::ranges::copy(ByteView{cell.encoded}.first(unpadded_size),
                      encoded.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
    cell.encoded = std::move(encoded);
    cell.left_child = left_child;
    return cell;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<CellImage> IndexCellAsLeaf(CellImage cell) {
  if (!cell.left_child.has_value()) {
    return cell;
  }
  if (cell.encoded.size() < sizeof(std::uint32_t)) {
    return std::unexpected(Corruption("index interior cell is truncated"));
  }
  try {
    std::pmr::vector<std::byte> encoded(
        cell.encoded.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)),
        cell.encoded.end(), cell.encoded.get_allocator().resource());
    cell.encoded = std::move(encoded);
    cell.left_child.reset();
    if (cell.encoded.size() < 4U) {
      cell.encoded.resize(4U);
    }
    return cell;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<std::pmr::vector<PageNumber>> CollectOverflowChain(BtreeMutationCore& core,
                                                                        const CellImage& cell,
                                                                        BtreePageType type) {
  std::pmr::vector<PageNumber> pages{core.scratch_resource()};
  if (!cell.first_overflow_page.has_value()) {
    return pages;
  }
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const std::size_t local_size = LocalPayloadSize(*geometry, type, cell.payload_size);
  if (local_size >= cell.payload_size) {
    return std::unexpected(Corruption("B-tree cell has an unexpected overflow reference"));
  }
  const std::size_t remaining = cell.payload_size - local_size;
  const std::size_t capacity = geometry->overflow_payload_capacity().value();
  const std::size_t expected_pages = 1U + (remaining - 1U) / capacity;
  if (expected_pages > MaximumOverflowPageCount(core, *geometry)) {
    return std::unexpected(Corruption("B-tree overflow chain cannot fit in the database"));
  }
  auto freelist_pages = FreelistPages(core);
  if (!freelist_pages.has_value()) {
    return std::unexpected(std::move(freelist_pages.error()));
  }

  try {
    pages.reserve(expected_pages);
    PageOwnershipSet seen{core.scratch_resource()};
    seen.reserve(expected_pages);
    PageNumber current = *cell.first_overflow_page;
    for (std::size_t index = 0; index < expected_pages; ++index) {
      if (current == PageNumber{1} || current == geometry->locking_page() ||
          current.value() == 0U || current.value() > core.pager().page_count()) {
        return std::unexpected(Corruption("B-tree overflow page reference is invalid"));
      }
      if ((*freelist_pages)->contains(current.value())) {
        return std::unexpected(Corruption("overflow page is also owned by the freelist"));
      }
      if (!seen.insert(current.value()).second) {
        return std::unexpected(Corruption("B-tree overflow chain contains a cycle"));
      }
      pages.push_back(current);

      auto pin = core.pager().ReadPage(current);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      auto overflow = OverflowPageView::Parse(pin->frame().bytes(), *geometry);
      if (!overflow.has_value()) {
        return std::unexpected(std::move(overflow.error()));
      }
      const std::optional<PageNumber> next = overflow->next_page();
      if (index + 1U < expected_pages) {
        if (!next.has_value()) {
          return std::unexpected(Corruption("B-tree overflow chain ends prematurely"));
        }
        current = *next;
      } else if (next.has_value()) {
        return std::unexpected(Corruption("B-tree overflow chain is longer than its payload"));
      }
    }
    return pages;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Status FreeOverflowPages(BtreeMutationCore& core,
                                       std::span<const PageNumber> pages) {
  for (std::size_t index = 0; index < pages.size(); ++index) {
    auto freed = FreeBtreePage(core, pages[index]);
    if (!freed.has_value()) {
      if (index != 0U) {
        core.MarkRollbackRequired(freed.error().code());
      }
      return freed;
    }
  }
  return {};
}

[[nodiscard]] Result<PageNumber> ChildAt(const NodeImage& node, std::size_t child_slot) {
  if (node.is_leaf() || child_slot > node.cells.size()) {
    return std::unexpected(Corruption("B-tree child slot is invalid"));
  }
  if (child_slot == node.cells.size()) {
    if (!node.rightmost_child.has_value()) {
      return std::unexpected(Corruption("B-tree interior node is missing its rightmost child"));
    }
    return node.rightmost_child.value_or(PageNumber{});
  }
  if (!node.cells[child_slot].left_child.has_value()) {
    return std::unexpected(Corruption("B-tree interior cell is missing its left child"));
  }
  return node.cells[child_slot].left_child.value_or(PageNumber{});
}

void SetLeftChild(CellImage& cell, PageNumber child) noexcept {
  cell.left_child = child;
  Store32(MutableByteView{cell.encoded}, 0U, child.value());
}

[[nodiscard]] Result<CellImage> EncodeTableInteriorCell(PageNumber left_child, std::int64_t rowid,
                                                        std::pmr::memory_resource* resource) {
  std::array<std::byte, 9> rowid_varint{};
  auto rowid_bytes =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{rowid_varint});
  if (!rowid_bytes.has_value()) {
    return std::unexpected(Misuse("table rowid cannot be encoded"));
  }
  try {
    std::array<std::byte, sizeof(std::uint32_t) + 9U> bytes{};
    Store32(MutableByteView{bytes}, 0U, left_child.value());
    std::ranges::copy(std::span{rowid_varint}.first(rowid_bytes->value()),
                      bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
    std::pmr::vector<std::byte> encoded(
        bytes.begin(),
        bytes.begin() +
            static_cast<std::ptrdiff_t>(sizeof(std::uint32_t) + rowid_bytes->value()),
        resource);
    return CellImage{
        .encoded = std::move(encoded),
        .left_child = left_child,
        .rowid = rowid,
        .payload_size = 0U,
        .first_overflow_page = std::nullopt,
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<TableSplitPlan> PlanTableSplit(NodeImage node, BtreePageGeometry geometry) {
  if (!node.is_table()) {
    return std::unexpected(Corruption("table split received an index page"));
  }
  const std::size_t count = node.cells.size();
  auto prefix = BuildNodeSizePrefix(node);
  if (!prefix.has_value()) {
    return std::unexpected(std::move(prefix.error()));
  }
  std::optional<std::size_t> selected;
  std::size_t selected_difference = (std::numeric_limits<std::size_t>::max)();

  if (node.is_leaf()) {
    for (std::size_t boundary = 1U; boundary < count; ++boundary) {
      if (!NodeRangeFits(node, *prefix, 0U, boundary, geometry) ||
          !NodeRangeFits(node, *prefix, boundary, count, geometry)) {
        continue;
      }
      const std::size_t left_bytes = NodeRangeBytes(node, *prefix, 0U, boundary);
      const std::size_t right_bytes = NodeRangeBytes(node, *prefix, boundary, count);
      const std::size_t difference =
          left_bytes > right_bytes ? left_bytes - right_bytes : right_bytes - left_bytes;
      if (difference < selected_difference) {
        selected = boundary;
        selected_difference = difference;
      }
    }
  } else if (count >= 3U) {
    for (std::size_t promoted = 1U; promoted + 1U < count; ++promoted) {
      if (!NodeRangeFits(node, *prefix, 0U, promoted, geometry) ||
          !NodeRangeFits(node, *prefix, promoted + 1U, count, geometry)) {
        continue;
      }
      const std::size_t left_bytes = NodeRangeBytes(node, *prefix, 0U, promoted);
      const std::size_t right_bytes = NodeRangeBytes(node, *prefix, promoted + 1U, count);
      const std::size_t difference =
          left_bytes > right_bytes ? left_bytes - right_bytes : right_bytes - left_bytes;
      if (difference < selected_difference) {
        selected = promoted;
        selected_difference = difference;
      }
    }
  }
  if (!selected.has_value()) {
    return std::unexpected(TooLarge("B-tree page cannot be split into valid siblings"));
  }

  try {
    NodeImage left{
        .page_number = node.page_number,
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage right{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    std::optional<CellImage> divider;
    if (node.is_leaf()) {
      const std::size_t boundary = *selected;
      left.cells.reserve(boundary);
      right.cells.reserve(count - boundary);
      for (std::size_t index = 0; index < boundary; ++index) {
        left.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = boundary; index < count; ++index) {
        right.cells.push_back(std::move(node.cells[index]));
      }
      if (!left.cells.back().rowid.has_value()) {
        return std::unexpected(Corruption("table leaf cell is missing its rowid"));
      }
      auto encoded = EncodeTableInteriorCell(PageNumber{1}, left.cells.back().rowid.value_or(0),
                                             left.cells.get_allocator().resource());
      if (!encoded.has_value()) {
        return std::unexpected(std::move(encoded.error()));
      }
      divider.emplace(std::move(*encoded));
    } else {
      const std::size_t promoted = *selected;
      if (!node.cells[promoted].left_child.has_value() || !node.cells[promoted].rowid.has_value() ||
          !node.rightmost_child.has_value()) {
        return std::unexpected(Corruption("table interior split metadata is incomplete"));
      }
      left.cells.reserve(promoted);
      right.cells.reserve(count - promoted - 1U);
      for (std::size_t index = 0; index < promoted; ++index) {
        left.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = promoted + 1U; index < count; ++index) {
        right.cells.push_back(std::move(node.cells[index]));
      }
      left.rightmost_child = node.cells[promoted].left_child;
      right.rightmost_child = node.rightmost_child;
      divider.emplace(std::move(node.cells[promoted]));
    }
    return TableSplitPlan{
        .left = std::move(left),
        .right = std::move(right),
        .divider = std::move(*divider),
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<MultiSplitPlan> PlanTableInsertionSplit(NodeImage node,
                                                             BtreePageGeometry geometry) {
  try {
    auto pair_input = CloneNodeImage(node);
    if (!pair_input.has_value()) {
      return std::unexpected(std::move(pair_input.error()));
    }
    auto pair = PlanTableSplit(std::move(*pair_input), geometry);
    if (pair.has_value()) {
      MultiSplitPlan plan;
      plan.pages.reserve(2U);
      plan.dividers.reserve(1U);
      plan.pages.push_back(std::move(pair->left));
      plan.pages.push_back(std::move(pair->right));
      plan.dividers.push_back(std::move(pair->divider));
      return plan;
    }
    if (pair.error().code() != ErrorCode::kTooLarge) {
      return std::unexpected(std::move(pair.error()));
    }

    const std::size_t count = node.cells.size();
    auto prefix = BuildNodeSizePrefix(node);
    if (!prefix.has_value()) {
      return std::unexpected(std::move(prefix.error()));
    }
    std::optional<std::pair<std::size_t, std::size_t>> selected;
    std::size_t selected_maximum = (std::numeric_limits<std::size_t>::max)();
    std::size_t selected_spread = (std::numeric_limits<std::size_t>::max)();
    if (node.is_leaf() && count >= 3U) {
      for (std::size_t first = 1U; first + 1U < count; ++first) {
        for (std::size_t second = first + 1U; second < count; ++second) {
          if (!NodeRangeFits(node, *prefix, 0U, first, geometry) ||
              !NodeRangeFits(node, *prefix, first, second, geometry) ||
              !NodeRangeFits(node, *prefix, second, count, geometry)) {
            continue;
          }
          const std::array<std::size_t, 3> sizes{
              NodeRangeBytes(node, *prefix, 0U, first),
              NodeRangeBytes(node, *prefix, first, second),
              NodeRangeBytes(node, *prefix, second, count),
          };
          const auto [minimum, maximum] = std::ranges::minmax(sizes);
          const std::size_t spread = maximum - minimum;
          if (maximum < selected_maximum ||
              (maximum == selected_maximum && spread < selected_spread)) {
            selected = std::pair{first, second};
            selected_maximum = maximum;
            selected_spread = spread;
          }
        }
      }
    } else if (!node.is_leaf() && count >= 5U) {
      for (std::size_t first = 1U; first + 3U < count; ++first) {
        for (std::size_t second = first + 2U; second + 1U < count; ++second) {
          if (!NodeRangeFits(node, *prefix, 0U, first, geometry) ||
              !NodeRangeFits(node, *prefix, first + 1U, second, geometry) ||
              !NodeRangeFits(node, *prefix, second + 1U, count, geometry)) {
            continue;
          }
          const std::array<std::size_t, 3> sizes{
              NodeRangeBytes(node, *prefix, 0U, first),
              NodeRangeBytes(node, *prefix, first + 1U, second),
              NodeRangeBytes(node, *prefix, second + 1U, count),
          };
          const auto [minimum, maximum] = std::ranges::minmax(sizes);
          const std::size_t spread = maximum - minimum;
          if (maximum < selected_maximum ||
              (maximum == selected_maximum && spread < selected_spread)) {
            selected = std::pair{first, second};
            selected_maximum = maximum;
            selected_spread = spread;
          }
        }
      }
    }
    if (!selected.has_value()) {
      return std::unexpected(TooLarge("B-tree page cannot be split into valid child pages"));
    }

    const auto [first, second] = *selected;
    MultiSplitPlan plan;
    plan.pages.reserve(3U);
    plan.dividers.reserve(2U);
    NodeImage left{
        .page_number = node.page_number,
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage middle{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage right{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    if (node.is_leaf()) {
      left.cells.reserve(first);
      middle.cells.reserve(second - first);
      right.cells.reserve(count - second);
      for (std::size_t index = 0; index < first; ++index) {
        left.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = first; index < second; ++index) {
        middle.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = second; index < count; ++index) {
        right.cells.push_back(std::move(node.cells[index]));
      }
      if (!left.cells.back().rowid.has_value() || !middle.cells.back().rowid.has_value()) {
        return std::unexpected(Corruption("table leaf cell is missing its rowid"));
      }
      auto first_divider =
          EncodeTableInteriorCell(PageNumber{1}, left.cells.back().rowid.value_or(0),
                                  left.cells.get_allocator().resource());
      if (!first_divider.has_value()) {
        return std::unexpected(std::move(first_divider.error()));
      }
      auto second_divider =
          EncodeTableInteriorCell(PageNumber{1}, middle.cells.back().rowid.value_or(0),
                                  middle.cells.get_allocator().resource());
      if (!second_divider.has_value()) {
        return std::unexpected(std::move(second_divider.error()));
      }
      plan.dividers.push_back(std::move(*first_divider));
      plan.dividers.push_back(std::move(*second_divider));
    } else {
      if (!node.cells[first].left_child.has_value() || !node.cells[second].left_child.has_value() ||
          !node.rightmost_child.has_value()) {
        return std::unexpected(Corruption("table interior split metadata is incomplete"));
      }
      left.cells.reserve(first);
      middle.cells.reserve(second - first - 1U);
      right.cells.reserve(count - second - 1U);
      for (std::size_t index = 0; index < first; ++index) {
        left.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = first + 1U; index < second; ++index) {
        middle.cells.push_back(std::move(node.cells[index]));
      }
      for (std::size_t index = second + 1U; index < count; ++index) {
        right.cells.push_back(std::move(node.cells[index]));
      }
      left.rightmost_child = node.cells[first].left_child;
      middle.rightmost_child = node.cells[second].left_child;
      right.rightmost_child = node.rightmost_child;
      plan.dividers.push_back(std::move(node.cells[first]));
      plan.dividers.push_back(std::move(node.cells[second]));
    }
    plan.pages.push_back(std::move(left));
    plan.pages.push_back(std::move(middle));
    plan.pages.push_back(std::move(right));
    return plan;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<PromotedSplits> SplitTableNonRoot(BtreeMutationCore& core, NodeImage node) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto plan = PlanTableInsertionSplit(std::move(node), *geometry);
  if (!plan.has_value()) {
    return std::unexpected(std::move(plan.error()));
  }
  PromotedSplits promoted;
  try {
    promoted.dividers.reserve(plan->dividers.size());
    promoted.right_pages.reserve(plan->dividers.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  for (std::size_t index = 1U; index < plan->pages.size(); ++index) {
    auto right_page = AllocateBtreePage(core);
    if (!right_page.has_value()) {
      if (index != 1U) {
        core.MarkRollbackRequired(right_page.error().code());
      }
      return std::unexpected(std::move(right_page.error()));
    }
    plan->pages[index].page_number = *right_page;
    promoted.right_pages.push_back(*right_page);
  }
  for (std::size_t index = 0; index < plan->dividers.size(); ++index) {
    SetLeftChild(plan->dividers[index], plan->pages[index].page_number);
  }
  for (const NodeImage& page : plan->pages) {
    auto written = WriteNode(core, page);
    if (!written.has_value()) {
      core.MarkRollbackRequired(written.error().code());
      return std::unexpected(std::move(written.error()));
    }
  }
  promoted.dividers = std::move(plan->dividers);
  return promoted;
}

[[nodiscard]] Status SplitTableRoot(BtreeMutationCore& core, NodeImage root) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  if (root.page_number == PageNumber{1} && NodeRangeFits(root, 0U, root.cells.size(), *geometry)) {
    auto child_page = AllocateBtreePage(core);
    if (!child_page.has_value()) {
      return std::unexpected(std::move(child_page.error()));
    }
    NodeImage child = std::move(root);
    child.page_number = *child_page;
    const NodeImage replacement_root{
        .page_number = PageNumber{1},
        .type = BtreePageType::kInteriorTable,
        .rightmost_child = *child_page,
        .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
    };
    auto child_written = WriteNode(core, child);
    if (!child_written.has_value()) {
      core.MarkRollbackRequired(child_written.error().code());
      return child_written;
    }
    auto root_written = WriteNode(core, replacement_root);
    if (!root_written.has_value()) {
      core.MarkRollbackRequired(root_written.error().code());
      return root_written;
    }
    return {};
  }
  auto plan = PlanTableInsertionSplit(std::move(root), *geometry);
  if (!plan.has_value()) {
    return std::unexpected(std::move(plan.error()));
  }

  NodeImage replacement_root{
      .page_number = plan->pages.front().page_number,
      .type = BtreePageType::kInteriorTable,
      .rightmost_child = std::nullopt,
      .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
  };
  try {
    replacement_root.cells.reserve(plan->dividers.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }

  for (std::size_t index = 0; index < plan->pages.size(); ++index) {
    auto child_page = AllocateBtreePage(core);
    if (!child_page.has_value()) {
      if (index != 0U) {
        core.MarkRollbackRequired(child_page.error().code());
      }
      return std::unexpected(std::move(child_page.error()));
    }
    plan->pages[index].page_number = *child_page;
  }
  for (std::size_t index = 0; index < plan->dividers.size(); ++index) {
    SetLeftChild(plan->dividers[index], plan->pages[index].page_number);
    replacement_root.cells.push_back(std::move(plan->dividers[index]));
  }
  replacement_root.rightmost_child = plan->pages.back().page_number;

  for (const NodeImage& page : plan->pages) {
    auto written = WriteNode(core, page);
    if (!written.has_value()) {
      core.MarkRollbackRequired(written.error().code());
      return written;
    }
  }
  auto root_written = WriteNode(core, replacement_root);
  if (!root_written.has_value()) {
    core.MarkRollbackRequired(root_written.error().code());
    return root_written;
  }
  return {};
}

[[nodiscard]] Status InsertTableDivider(NodeImage& parent, std::size_t child_slot,
                                        CellImage divider, PageNumber right_page) {
  if (parent.is_leaf() || !parent.is_table() || child_slot > parent.cells.size()) {
    return std::unexpected(Corruption("table parent insertion slot is invalid"));
  }
  if (child_slot < parent.cells.size()) {
    SetLeftChild(parent.cells[child_slot], right_page);
  } else {
    parent.rightmost_child = right_page;
  }
  parent.cells.insert(parent.cells.begin() + static_cast<std::ptrdiff_t>(child_slot),
                      std::move(divider));
  return {};
}

[[nodiscard]] Status WriteTableInsertion(BtreeMutationCore& core, PageNumber root_page,
                                         TableSearchResult search) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const bool root_split_exceeds_depth = search.tree_depth >= kMaximumBtreeDepth;
  NodeImage current = std::move(search.leaf);
  while (true) {
    if (NodeFits(current, *geometry)) {
      return WriteNode(core, current);
    }
    if (search.path.empty()) {
      if (current.page_number != root_page) {
        return std::unexpected(Corruption("B-tree insertion path lost its root"));
      }
      if (root_split_exceeds_depth) {
        core.MarkRollbackRequired(ErrorCode::kTooLarge);
        return std::unexpected(TooLarge("B-tree insertion would exceed SQLite's depth limit"));
      }
      return SplitTableRoot(core, std::move(current));
    }

    auto split = SplitTableNonRoot(core, std::move(current));
    if (!split.has_value()) {
      return std::unexpected(std::move(split.error()));
    }
    TablePathEntry parent_entry = std::move(search.path.back());
    search.path.pop_back();
    for (std::size_t index = 0; index < split->dividers.size(); ++index) {
      auto inserted =
          InsertTableDivider(parent_entry.parent, parent_entry.child_slot + index,
                             std::move(split->dividers[index]), split->right_pages[index]);
      if (!inserted.has_value()) {
        core.MarkRollbackRequired(inserted.error().code());
        return inserted;
      }
    }
    current = std::move(parent_entry.parent);
  }
}

[[nodiscard]] bool IsSparseNonRoot(const NodeImage& node, BtreePageGeometry geometry) noexcept {
  if (node.cells.empty()) {
    return true;
  }
  const std::size_t used = NodeRangeBytes(node, 0U, node.cells.size(), false);
  const std::size_t free_bytes = geometry.usable_size().value() - used;
  return free_bytes > geometry.usable_size().value() * 2U / 3U;
}

[[nodiscard]] Status UpdateParentAfterMerge(NodeImage& parent, std::size_t divider_index,
                                            PageNumber merged_page) {
  if (divider_index >= parent.cells.size()) {
    return std::unexpected(Corruption("B-tree merge divider is out of range"));
  }
  parent.cells.erase(parent.cells.begin() + static_cast<std::ptrdiff_t>(divider_index));
  if (divider_index < parent.cells.size()) {
    SetLeftChild(parent.cells[divider_index], merged_page);
  } else {
    parent.rightmost_child = merged_page;
  }
  return {};
}

[[nodiscard]] Status CollapseOrWriteTableRoot(BtreeMutationCore& core, NodeImage root) {
  if (root.is_leaf() || !root.cells.empty()) {
    return WriteNode(core, root);
  }
  if (!root.rightmost_child.has_value()) {
    return std::unexpected(Corruption("empty table interior root has no child"));
  }
  const PageNumber child_page = *root.rightmost_child;
  auto child = ReadNode(core, child_page);
  if (!child.has_value()) {
    return std::unexpected(std::move(child.error()));
  }
  if (!child->is_table()) {
    return std::unexpected(Corruption("table root child has the wrong tree kind"));
  }
  child->page_number = root.page_number;
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  if (!NodeFits(*child, *geometry)) {
    return WriteNode(core, root);
  }
  auto root_written = WriteNode(core, *child);
  if (!root_written.has_value()) {
    return root_written;
  }
  auto freed = FreeBtreePage(core, child_page);
  if (!freed.has_value()) {
    core.MarkRollbackRequired(freed.error().code());
    return freed;
  }
  return {};
}

[[nodiscard]] Status RebalanceTableAfterDelete(BtreeMutationCore& core, PageNumber root_page,
                                               TableSearchResult search) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  NodeImage current = std::move(search.leaf);

  while (true) {
    if (search.path.empty()) {
      if (current.page_number != root_page) {
        return std::unexpected(Corruption("B-tree deletion path lost its root"));
      }
      return CollapseOrWriteTableRoot(core, std::move(current));
    }
    if (!NodeFits(current, *geometry)) {
      TableSearchResult insertion_path{
          .path = std::move(search.path),
          .leaf = std::move(current),
          .index = 0U,
          .exact = false,
          .tree_depth = search.tree_depth,
      };
      return WriteTableInsertion(core, root_page, std::move(insertion_path));
    }
    if (!IsSparseNonRoot(current, *geometry)) {
      return WriteNode(core, current);
    }

    TablePathEntry parent_entry = std::move(search.path.back());
    search.path.pop_back();
    NodeImage& parent = parent_entry.parent;
    const std::size_t child_slot = parent_entry.child_slot;
    if (parent.is_leaf() || !parent.is_table() || child_slot > parent.cells.size()) {
      return std::unexpected(Corruption("table deletion parent path is invalid"));
    }
    if (parent.cells.empty()) {
      if (!search.path.empty() || child_slot != 0U ||
          parent.rightmost_child != std::optional<PageNumber>{current.page_number}) {
        return std::unexpected(Corruption("non-root table page has only one child"));
      }
      auto child_written = WriteNode(core, current);
      if (!child_written.has_value()) {
        return child_written;
      }
      return CollapseOrWriteTableRoot(core, std::move(parent));
    }

    const bool use_left_sibling = child_slot > 0U;
    const std::size_t divider_index = use_left_sibling ? child_slot - 1U : child_slot;
    if (divider_index >= parent.cells.size()) {
      return std::unexpected(Corruption("sparse B-tree page has no adjacent sibling"));
    }
    const std::size_t sibling_slot = use_left_sibling ? child_slot - 1U : child_slot + 1U;
    auto sibling_page = ChildAt(parent, sibling_slot);
    if (!sibling_page.has_value()) {
      return std::unexpected(std::move(sibling_page.error()));
    }
    const PageNumber sibling_number = *sibling_page;
    const bool sibling_on_path =
        std::ranges::any_of(search.path, [sibling_number](const TablePathEntry& entry) {
          return entry.parent.page_number == sibling_number;
        });
    if (sibling_number == current.page_number || sibling_number == parent.page_number ||
        sibling_on_path) {
      return std::unexpected(Corruption("table deletion sibling aliases the mutation path"));
    }
    auto sibling = ReadNode(core, sibling_number);
    if (!sibling.has_value()) {
      return std::unexpected(std::move(sibling.error()));
    }
    if (sibling->type != current.type) {
      return std::unexpected(Corruption("B-tree siblings have different page types"));
    }

    auto ordered_siblings = [&]() {
      if (use_left_sibling) {
        return std::pair<NodeImage, NodeImage>{std::move(*sibling), std::move(current)};
      }
      return std::pair<NodeImage, NodeImage>{std::move(current), std::move(*sibling)};
    }();
    NodeImage left = std::move(ordered_siblings.first);
    NodeImage right = std::move(ordered_siblings.second);
    const PageNumber left_page = left.page_number;
    const PageNumber right_page = right.page_number;
    if (left_page == right_page) {
      return std::unexpected(Corruption("table deletion siblings reference the same page"));
    }
    try {
      NodeImage combined{
          .page_number = left_page,
          .type = left.type,
          .rightmost_child = right.rightmost_child,
          .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
      };
      const std::size_t extra_divider = left.is_leaf() ? 0U : 1U;
      combined.cells.reserve(left.cells.size() + right.cells.size() + extra_divider);
      for (CellImage& cell : left.cells) {
        combined.cells.push_back(std::move(cell));
      }
      if (!left.is_leaf()) {
        if (!left.rightmost_child.has_value()) {
          return std::unexpected(Corruption("table interior sibling has no rightmost child"));
        }
        CellImage divider = std::move(parent.cells[divider_index]);
        SetLeftChild(divider, *left.rightmost_child);
        combined.cells.push_back(std::move(divider));
      }
      for (CellImage& cell : right.cells) {
        combined.cells.push_back(std::move(cell));
      }

      if (NodeFits(combined, *geometry)) {
        auto merged = WriteNode(core, combined);
        if (!merged.has_value()) {
          return merged;
        }
        auto freed = FreeBtreePage(core, right_page);
        if (!freed.has_value()) {
          core.MarkRollbackRequired(freed.error().code());
          return freed;
        }
        auto updated = UpdateParentAfterMerge(parent, divider_index, left_page);
        if (!updated.has_value()) {
          core.MarkRollbackRequired(updated.error().code());
          return updated;
        }
      } else {
        auto plan = PlanTableSplit(std::move(combined), *geometry);
        if (!plan.has_value()) {
          return std::unexpected(std::move(plan.error()));
        }
        plan->left.page_number = left_page;
        plan->right.page_number = right_page;
        SetLeftChild(plan->divider, left_page);
        auto left_written = WriteNode(core, plan->left);
        if (!left_written.has_value()) {
          return left_written;
        }
        auto right_written = WriteNode(core, plan->right);
        if (!right_written.has_value()) {
          core.MarkRollbackRequired(right_written.error().code());
          return right_written;
        }
        parent.cells[divider_index] = std::move(plan->divider);
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
    current = std::move(parent);
  }
}

[[nodiscard]] Result<std::weak_ordering> CompareIndexCell(
    BtreeMutationCore& core, const CellImage& cell, BtreePageType type,
    std::span<const SqlValue> values, std::span<const IndexColumnOrder> columns) {
  auto options = core.RecordOptions();
  if (!options.has_value()) {
    return std::unexpected(std::move(options.error()));
  }
  auto payload = ReadCellPayload(core, cell, type);
  if (!payload.has_value()) {
    return std::unexpected(std::move(payload.error()));
  }
  auto record = RecordView::Parse(*payload, *options);
  if (!record.has_value()) {
    return std::unexpected(std::move(record.error()));
  }
  if (record->field_count() != columns.size()) {
    return std::unexpected(Corruption("index record field count does not match its schema"));
  }
  auto comparison = CompareIndexRecord(*record, values, columns, EqualPrefixResult::kEquivalent);
  if (!comparison.has_value()) {
    return std::unexpected(std::move(comparison.error()));
  }
  return comparison->ordering;
}

[[nodiscard]] Result<IndexSearchResult> SearchIndex(BtreeMutationCore& core, PageNumber root_page,
                                                    std::span<const SqlValue> values,
                                                    std::span<const IndexColumnOrder> columns) {
  std::pmr::vector<IndexPathEntry> path{core.scratch_resource()};
  try {
    path.reserve(kMaximumBtreeDepth);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }

  PageNumber page_number = root_page;
  for (std::size_t depth = 0; depth < kMaximumBtreeDepth; ++depth) {
    auto node = ReadNode(core, page_number);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (node->is_table()) {
      return std::unexpected(Corruption("index B-tree path contains a table page"));
    }
    std::size_t lower = 0;
    std::size_t upper = node->cells.size();
    while (lower < upper) {
      const std::size_t middle = lower + (upper - lower) / 2U;
      auto ordering = CompareIndexCell(core, node->cells[middle], node->type, values, columns);
      if (!ordering.has_value()) {
        return std::unexpected(std::move(ordering.error()));
      }
      if (*ordering == std::weak_ordering::equivalent) {
        const std::optional<std::size_t> tree_depth =
            node->is_leaf() ? std::optional<std::size_t>{depth + 1U} : std::nullopt;
        return IndexSearchResult{
            .path = std::move(path),
            .node = std::move(*node),
            .index = middle,
            .exact = true,
            .tree_depth = tree_depth,
        };
      }
      if (*ordering == std::weak_ordering::less) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }
    if (node->is_leaf()) {
      return IndexSearchResult{
          .path = std::move(path),
          .node = std::move(*node),
          .index = lower,
          .exact = false,
          .tree_depth = depth + 1U,
      };
    }
    auto child = ChildAt(*node, lower);
    if (!child.has_value()) {
      return std::unexpected(std::move(child.error()));
    }
    path.push_back(IndexPathEntry{
        .parent = std::move(*node),
        .child_slot = lower,
    });
    page_number = *child;
  }
  return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
}

[[nodiscard]] Result<IndexSplitPlan> PlanIndexSplit(NodeImage node, BtreePageGeometry geometry) {
  if (node.is_table()) {
    return std::unexpected(Corruption("index split received a table page"));
  }
  const std::size_t count = node.cells.size();
  auto prefix = BuildNodeSizePrefix(node);
  if (!prefix.has_value()) {
    return std::unexpected(std::move(prefix.error()));
  }
  std::optional<std::size_t> selected;
  std::size_t selected_difference = (std::numeric_limits<std::size_t>::max)();
  if (count >= 3U) {
    for (std::size_t promoted = 1U; promoted + 1U < count; ++promoted) {
      if (!NodeRangeFits(node, *prefix, 0U, promoted, geometry) ||
          !NodeRangeFits(node, *prefix, promoted + 1U, count, geometry)) {
        continue;
      }
      const std::size_t left_bytes = NodeRangeBytes(node, *prefix, 0U, promoted);
      const std::size_t right_bytes = NodeRangeBytes(node, *prefix, promoted + 1U, count);
      const std::size_t difference =
          left_bytes > right_bytes ? left_bytes - right_bytes : right_bytes - left_bytes;
      if (difference < selected_difference) {
        selected = promoted;
        selected_difference = difference;
      }
    }
  }
  if (!selected.has_value()) {
    return std::unexpected(TooLarge("index B-tree page cannot be split into valid siblings"));
  }

  try {
    const std::size_t promoted = *selected;
    NodeImage left{
        .page_number = node.page_number,
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage right{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    left.cells.reserve(promoted);
    right.cells.reserve(count - promoted - 1U);
    for (std::size_t index = 0; index < promoted; ++index) {
      left.cells.push_back(std::move(node.cells[index]));
    }
    for (std::size_t index = promoted + 1U; index < count; ++index) {
      right.cells.push_back(std::move(node.cells[index]));
    }

    CellImage divider = std::move(node.cells[promoted]);
    if (node.is_leaf()) {
      auto interior = IndexCellAsInterior(std::move(divider), PageNumber{1}, geometry);
      if (!interior.has_value()) {
        return std::unexpected(std::move(interior.error()));
      }
      divider = std::move(*interior);
    } else {
      if (!divider.left_child.has_value() || !node.rightmost_child.has_value()) {
        return std::unexpected(Corruption("index interior split metadata is incomplete"));
      }
      left.rightmost_child = divider.left_child;
      right.rightmost_child = node.rightmost_child;
    }
    return IndexSplitPlan{
        .left = std::move(left),
        .right = std::move(right),
        .divider = std::move(divider),
    };
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<MultiSplitPlan> PlanIndexInsertionSplit(NodeImage node,
                                                             BtreePageGeometry geometry) {
  try {
    auto pair_input = CloneNodeImage(node);
    if (!pair_input.has_value()) {
      return std::unexpected(std::move(pair_input.error()));
    }
    auto pair = PlanIndexSplit(std::move(*pair_input), geometry);
    if (pair.has_value()) {
      MultiSplitPlan plan;
      plan.pages.reserve(2U);
      plan.dividers.reserve(1U);
      plan.pages.push_back(std::move(pair->left));
      plan.pages.push_back(std::move(pair->right));
      plan.dividers.push_back(std::move(pair->divider));
      return plan;
    }
    if (pair.error().code() != ErrorCode::kTooLarge) {
      return std::unexpected(std::move(pair.error()));
    }

    const std::size_t count = node.cells.size();
    auto prefix = BuildNodeSizePrefix(node);
    if (!prefix.has_value()) {
      return std::unexpected(std::move(prefix.error()));
    }
    std::optional<std::pair<std::size_t, std::size_t>> selected;
    std::size_t selected_maximum = (std::numeric_limits<std::size_t>::max)();
    std::size_t selected_spread = (std::numeric_limits<std::size_t>::max)();
    if (count >= 5U) {
      for (std::size_t first = 1U; first + 3U < count; ++first) {
        for (std::size_t second = first + 2U; second + 1U < count; ++second) {
          if (!NodeRangeFits(node, *prefix, 0U, first, geometry) ||
              !NodeRangeFits(node, *prefix, first + 1U, second, geometry) ||
              !NodeRangeFits(node, *prefix, second + 1U, count, geometry)) {
            continue;
          }
          const std::array<std::size_t, 3> sizes{
              NodeRangeBytes(node, *prefix, 0U, first),
              NodeRangeBytes(node, *prefix, first + 1U, second),
              NodeRangeBytes(node, *prefix, second + 1U, count),
          };
          const auto [minimum, maximum] = std::ranges::minmax(sizes);
          const std::size_t spread = maximum - minimum;
          if (maximum < selected_maximum ||
              (maximum == selected_maximum && spread < selected_spread)) {
            selected = std::pair{first, second};
            selected_maximum = maximum;
            selected_spread = spread;
          }
        }
      }
    }
    if (!selected.has_value()) {
      return std::unexpected(TooLarge("index B-tree page cannot be split into valid child pages"));
    }

    const auto [first, second] = *selected;
    MultiSplitPlan plan;
    plan.pages.reserve(3U);
    plan.dividers.reserve(2U);
    NodeImage left{
        .page_number = node.page_number,
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage middle{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    NodeImage right{
        .page_number = PageNumber{},
        .type = node.type,
        .rightmost_child = std::nullopt,
        .cells = std::pmr::vector<CellImage>{node.cells.get_allocator().resource()},
    };
    left.cells.reserve(first);
    middle.cells.reserve(second - first - 1U);
    right.cells.reserve(count - second - 1U);
    for (std::size_t index = 0; index < first; ++index) {
      left.cells.push_back(std::move(node.cells[index]));
    }
    for (std::size_t index = first + 1U; index < second; ++index) {
      middle.cells.push_back(std::move(node.cells[index]));
    }
    for (std::size_t index = second + 1U; index < count; ++index) {
      right.cells.push_back(std::move(node.cells[index]));
    }
    if (node.is_leaf()) {
      auto first_divider =
          IndexCellAsInterior(std::move(node.cells[first]), PageNumber{1}, geometry);
      if (!first_divider.has_value()) {
        return std::unexpected(std::move(first_divider.error()));
      }
      auto second_divider =
          IndexCellAsInterior(std::move(node.cells[second]), PageNumber{1}, geometry);
      if (!second_divider.has_value()) {
        return std::unexpected(std::move(second_divider.error()));
      }
      plan.dividers.push_back(std::move(*first_divider));
      plan.dividers.push_back(std::move(*second_divider));
    } else {
      if (!node.cells[first].left_child.has_value() || !node.cells[second].left_child.has_value() ||
          !node.rightmost_child.has_value()) {
        return std::unexpected(Corruption("index interior split metadata is incomplete"));
      }
      left.rightmost_child = node.cells[first].left_child;
      middle.rightmost_child = node.cells[second].left_child;
      right.rightmost_child = node.rightmost_child;
      plan.dividers.push_back(std::move(node.cells[first]));
      plan.dividers.push_back(std::move(node.cells[second]));
    }
    plan.pages.push_back(std::move(left));
    plan.pages.push_back(std::move(middle));
    plan.pages.push_back(std::move(right));
    return plan;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<PromotedSplits> SplitIndexNonRoot(BtreeMutationCore& core, NodeImage node) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto plan = PlanIndexInsertionSplit(std::move(node), *geometry);
  if (!plan.has_value()) {
    return std::unexpected(std::move(plan.error()));
  }
  PromotedSplits promoted;
  try {
    promoted.dividers.reserve(plan->dividers.size());
    promoted.right_pages.reserve(plan->dividers.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  for (std::size_t index = 1U; index < plan->pages.size(); ++index) {
    auto right_page = AllocateBtreePage(core);
    if (!right_page.has_value()) {
      if (index != 1U) {
        core.MarkRollbackRequired(right_page.error().code());
      }
      return std::unexpected(std::move(right_page.error()));
    }
    plan->pages[index].page_number = *right_page;
    promoted.right_pages.push_back(*right_page);
  }
  for (std::size_t index = 0; index < plan->dividers.size(); ++index) {
    SetLeftChild(plan->dividers[index], plan->pages[index].page_number);
  }
  for (const NodeImage& page : plan->pages) {
    auto written = WriteNode(core, page);
    if (!written.has_value()) {
      core.MarkRollbackRequired(written.error().code());
      return std::unexpected(std::move(written.error()));
    }
  }
  promoted.dividers = std::move(plan->dividers);
  return promoted;
}

[[nodiscard]] Status SplitIndexRoot(BtreeMutationCore& core, NodeImage root) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  auto plan = PlanIndexInsertionSplit(std::move(root), *geometry);
  if (!plan.has_value()) {
    return std::unexpected(std::move(plan.error()));
  }
  NodeImage replacement_root{
      .page_number = plan->pages.front().page_number,
      .type = BtreePageType::kInteriorIndex,
      .rightmost_child = std::nullopt,
      .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
  };
  try {
    replacement_root.cells.reserve(plan->dividers.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  for (std::size_t index = 0; index < plan->pages.size(); ++index) {
    auto child_page = AllocateBtreePage(core);
    if (!child_page.has_value()) {
      if (index != 0U) {
        core.MarkRollbackRequired(child_page.error().code());
      }
      return std::unexpected(std::move(child_page.error()));
    }
    plan->pages[index].page_number = *child_page;
  }
  for (std::size_t index = 0; index < plan->dividers.size(); ++index) {
    SetLeftChild(plan->dividers[index], plan->pages[index].page_number);
    replacement_root.cells.push_back(std::move(plan->dividers[index]));
  }
  replacement_root.rightmost_child = plan->pages.back().page_number;

  for (const NodeImage& page : plan->pages) {
    auto written = WriteNode(core, page);
    if (!written.has_value()) {
      core.MarkRollbackRequired(written.error().code());
      return written;
    }
  }
  auto root_written = WriteNode(core, replacement_root);
  if (!root_written.has_value()) {
    core.MarkRollbackRequired(root_written.error().code());
    return root_written;
  }
  return {};
}

[[nodiscard]] Status InsertIndexDivider(NodeImage& parent, std::size_t child_slot,
                                        CellImage divider, PageNumber right_page) {
  if (parent.is_leaf() || parent.is_table() || child_slot > parent.cells.size()) {
    return std::unexpected(Corruption("index parent insertion slot is invalid"));
  }
  if (child_slot < parent.cells.size()) {
    SetLeftChild(parent.cells[child_slot], right_page);
  } else {
    parent.rightmost_child = right_page;
  }
  parent.cells.insert(parent.cells.begin() + static_cast<std::ptrdiff_t>(child_slot),
                      std::move(divider));
  return {};
}

[[nodiscard]] Status WriteIndexInsertion(BtreeMutationCore& core, PageNumber root_page,
                                         IndexSearchResult search) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  if (!search.tree_depth.has_value()) {
    return std::unexpected(Corruption("index insertion is missing the full tree depth"));
  }
  const bool root_split_exceeds_depth = *search.tree_depth >= kMaximumBtreeDepth;
  NodeImage current = std::move(search.node);
  while (true) {
    if (NodeFits(current, *geometry)) {
      return WriteNode(core, current);
    }
    if (search.path.empty()) {
      if (current.page_number != root_page) {
        return std::unexpected(Corruption("index insertion path lost its root"));
      }
      if (root_split_exceeds_depth) {
        core.MarkRollbackRequired(ErrorCode::kTooLarge);
        return std::unexpected(
            TooLarge("index B-tree insertion would exceed SQLite's depth limit"));
      }
      return SplitIndexRoot(core, std::move(current));
    }
    auto split = SplitIndexNonRoot(core, std::move(current));
    if (!split.has_value()) {
      return std::unexpected(std::move(split.error()));
    }
    IndexPathEntry parent_entry = std::move(search.path.back());
    search.path.pop_back();
    for (std::size_t index = 0; index < split->dividers.size(); ++index) {
      auto inserted =
          InsertIndexDivider(parent_entry.parent, parent_entry.child_slot + index,
                             std::move(split->dividers[index]), split->right_pages[index]);
      if (!inserted.has_value()) {
        core.MarkRollbackRequired(inserted.error().code());
        return inserted;
      }
    }
    current = std::move(parent_entry.parent);
  }
}

[[nodiscard]] Status CollapseOrWriteIndexRoot(BtreeMutationCore& core, NodeImage root) {
  if (root.is_leaf() || !root.cells.empty()) {
    return WriteNode(core, root);
  }
  if (!root.rightmost_child.has_value()) {
    return std::unexpected(Corruption("empty index interior root has no child"));
  }
  const PageNumber child_page = *root.rightmost_child;
  auto child = ReadNode(core, child_page);
  if (!child.has_value()) {
    return std::unexpected(std::move(child.error()));
  }
  if (child->is_table()) {
    return std::unexpected(Corruption("index root child has the wrong tree kind"));
  }
  child->page_number = root.page_number;
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  if (!NodeFits(*child, *geometry)) {
    return std::unexpected(Corruption("index root child does not fit its root page"));
  }
  auto root_written = WriteNode(core, *child);
  if (!root_written.has_value()) {
    return root_written;
  }
  auto freed = FreeBtreePage(core, child_page);
  if (!freed.has_value()) {
    core.MarkRollbackRequired(freed.error().code());
    return freed;
  }
  return {};
}

[[nodiscard]] Status RebalanceIndexAfterDelete(BtreeMutationCore& core, PageNumber root_page,
                                               IndexSearchResult search) {
  auto geometry = core.Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  NodeImage current = std::move(search.node);
  while (true) {
    if (search.path.empty()) {
      if (current.page_number != root_page) {
        return std::unexpected(Corruption("index deletion path lost its root"));
      }
      return CollapseOrWriteIndexRoot(core, std::move(current));
    }
    if (!NodeFits(current, *geometry)) {
      IndexSearchResult insertion_path{
          .path = std::move(search.path),
          .node = std::move(current),
          .index = 0U,
          .exact = false,
          .tree_depth = search.tree_depth,
      };
      return WriteIndexInsertion(core, root_page, std::move(insertion_path));
    }
    if (!IsSparseNonRoot(current, *geometry)) {
      return WriteNode(core, current);
    }

    IndexPathEntry parent_entry = std::move(search.path.back());
    search.path.pop_back();
    NodeImage& parent = parent_entry.parent;
    const std::size_t child_slot = parent_entry.child_slot;
    if (parent.is_leaf() || parent.is_table() || child_slot > parent.cells.size()) {
      return std::unexpected(Corruption("index deletion parent path is invalid"));
    }
    if (parent.cells.empty()) {
      if (!search.path.empty() || child_slot != 0U ||
          parent.rightmost_child != std::optional<PageNumber>{current.page_number}) {
        return std::unexpected(Corruption("non-root index page has only one child"));
      }
      auto child_written = WriteNode(core, current);
      if (!child_written.has_value()) {
        return child_written;
      }
      return CollapseOrWriteIndexRoot(core, std::move(parent));
    }

    const bool use_left_sibling = child_slot > 0U;
    const std::size_t divider_index = use_left_sibling ? child_slot - 1U : child_slot;
    if (divider_index >= parent.cells.size()) {
      return std::unexpected(Corruption("sparse index page has no adjacent sibling"));
    }
    const std::size_t sibling_slot = use_left_sibling ? child_slot - 1U : child_slot + 1U;
    auto sibling_page = ChildAt(parent, sibling_slot);
    if (!sibling_page.has_value()) {
      return std::unexpected(std::move(sibling_page.error()));
    }
    const PageNumber sibling_number = *sibling_page;
    const bool sibling_on_path =
        std::ranges::any_of(search.path, [sibling_number](const IndexPathEntry& entry) {
          return entry.parent.page_number == sibling_number;
        });
    if (sibling_number == current.page_number || sibling_number == parent.page_number ||
        sibling_on_path) {
      return std::unexpected(Corruption("index deletion sibling aliases the mutation path"));
    }
    auto sibling = ReadNode(core, sibling_number);
    if (!sibling.has_value()) {
      return std::unexpected(std::move(sibling.error()));
    }
    if (sibling->type != current.type) {
      return std::unexpected(Corruption("index siblings have different page types"));
    }

    auto ordered_siblings = [&]() {
      if (use_left_sibling) {
        return std::pair<NodeImage, NodeImage>{std::move(*sibling), std::move(current)};
      }
      return std::pair<NodeImage, NodeImage>{std::move(current), std::move(*sibling)};
    }();
    NodeImage left = std::move(ordered_siblings.first);
    NodeImage right = std::move(ordered_siblings.second);
    const PageNumber left_page = left.page_number;
    const PageNumber right_page = right.page_number;
    if (left_page == right_page) {
      return std::unexpected(Corruption("index deletion siblings reference the same page"));
    }
    try {
      NodeImage combined{
          .page_number = left_page,
          .type = left.type,
          .rightmost_child = right.rightmost_child,
          .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
      };
      combined.cells.reserve(left.cells.size() + right.cells.size() + 1U);
      for (CellImage& cell : left.cells) {
        combined.cells.push_back(std::move(cell));
      }
      CellImage divider = std::move(parent.cells[divider_index]);
      if (left.is_leaf()) {
        auto leaf_divider = IndexCellAsLeaf(std::move(divider));
        if (!leaf_divider.has_value()) {
          return std::unexpected(std::move(leaf_divider.error()));
        }
        combined.cells.push_back(std::move(*leaf_divider));
      } else {
        if (!left.rightmost_child.has_value()) {
          return std::unexpected(Corruption("index interior sibling has no rightmost child"));
        }
        SetLeftChild(divider, *left.rightmost_child);
        combined.cells.push_back(std::move(divider));
      }
      for (CellImage& cell : right.cells) {
        combined.cells.push_back(std::move(cell));
      }

      if (NodeFits(combined, *geometry)) {
        auto merged = WriteNode(core, combined);
        if (!merged.has_value()) {
          return merged;
        }
        auto freed = FreeBtreePage(core, right_page);
        if (!freed.has_value()) {
          core.MarkRollbackRequired(freed.error().code());
          return freed;
        }
        auto updated = UpdateParentAfterMerge(parent, divider_index, left_page);
        if (!updated.has_value()) {
          core.MarkRollbackRequired(updated.error().code());
          return updated;
        }
      } else {
        auto plan = PlanIndexSplit(std::move(combined), *geometry);
        if (!plan.has_value()) {
          return std::unexpected(std::move(plan.error()));
        }
        plan->left.page_number = left_page;
        plan->right.page_number = right_page;
        SetLeftChild(plan->divider, left_page);
        auto left_written = WriteNode(core, plan->left);
        if (!left_written.has_value()) {
          return left_written;
        }
        auto right_written = WriteNode(core, plan->right);
        if (!right_written.has_value()) {
          core.MarkRollbackRequired(right_written.error().code());
          return right_written;
        }
        parent.cells[divider_index] = std::move(plan->divider);
      }
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
    current = std::move(parent);
  }
}

struct IndexPredecessor {
  CellImage cell;
  std::vector<SqlValue> values;
  std::size_t tree_depth;
};

struct IndexPredecessorTarget {
  std::size_t index;
  std::size_t depth;
};

[[nodiscard]] Result<IndexPredecessor> ReadIndexPredecessor(BtreeMutationCore& core,
                                                            const NodeImage& target,
                                                            IndexPredecessorTarget location) {
  if (target.is_leaf() || location.index >= target.cells.size()) {
    return std::unexpected(Corruption("index predecessor target is invalid"));
  }
  auto child = ChildAt(target, location.index);
  if (!child.has_value()) {
    return std::unexpected(std::move(child.error()));
  }
  PageNumber page_number = *child;
  for (std::size_t descendant_depth = 1U;
       location.depth + descendant_depth <= kMaximumBtreeDepth; ++descendant_depth) {
    auto node = ReadNode(core, page_number);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (node->is_table() || node->cells.empty()) {
      return std::unexpected(Corruption("index predecessor path is invalid"));
    }
    if (!node->is_leaf()) {
      if (!node->rightmost_child.has_value()) {
        return std::unexpected(Corruption("index predecessor page has no rightmost child"));
      }
      page_number = *node->rightmost_child;
      continue;
    }

    CellImage predecessor = std::move(node->cells.back());
    auto payload = ReadCellPayload(core, predecessor, BtreePageType::kLeafIndex);
    if (!payload.has_value()) {
      return std::unexpected(std::move(payload.error()));
    }
    auto options = core.RecordOptions();
    if (!options.has_value()) {
      return std::unexpected(std::move(options.error()));
    }
    auto values = DecodeRecord(*payload, *options);
    if (!values.has_value()) {
      return std::unexpected(std::move(values.error()));
    }
    return IndexPredecessor{
        .cell = std::move(predecessor),
        .values = std::move(*values),
        .tree_depth = location.depth + descendant_depth,
    };
  }
  return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
}

[[nodiscard]] Result<IndexSearchResult> FindTransferredPredecessorLeaf(
    BtreeMutationCore& core, PageNumber root_page, std::span<const SqlValue> values,
    std::span<const IndexColumnOrder> columns) {
  auto installed = SearchIndex(core, root_page, values, columns);
  if (!installed.has_value()) {
    return std::unexpected(std::move(installed.error()));
  }
  if (!installed->exact || installed->node.is_leaf()) {
    return std::unexpected(Corruption("installed index predecessor is not an interior entry"));
  }
  auto child = ChildAt(installed->node, installed->index);
  if (!child.has_value()) {
    return std::unexpected(std::move(child.error()));
  }
  try {
    installed->path.reserve(kMaximumBtreeDepth);
    installed->path.push_back(IndexPathEntry{
        .parent = std::move(installed->node),
        .child_slot = installed->index,
    });
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }

  PageNumber page_number = *child;
  while (installed->path.size() < kMaximumBtreeDepth) {
    auto node = ReadNode(core, page_number);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (node->is_table() || node->cells.empty()) {
      return std::unexpected(Corruption("transferred index predecessor path is invalid"));
    }
    if (node->is_leaf()) {
      const std::size_t index = node->cells.size() - 1U;
      auto ordering = CompareIndexCell(core, node->cells[index], node->type, values, columns);
      if (!ordering.has_value()) {
        return std::unexpected(std::move(ordering.error()));
      }
      if (*ordering != std::weak_ordering::equivalent) {
        return std::unexpected(Corruption("transferred index predecessor leaf is missing"));
      }
      const std::size_t tree_depth = installed->path.size() + 1U;
      return IndexSearchResult{
          .path = std::move(installed->path),
          .node = std::move(*node),
          .index = index,
          .exact = true,
          .tree_depth = tree_depth,
      };
    }
    if (!node->rightmost_child.has_value()) {
      return std::unexpected(Corruption("index predecessor page has no rightmost child"));
    }
    page_number = *node->rightmost_child;
    const std::size_t child_slot = node->cells.size();
    installed->path.push_back(IndexPathEntry{
        .parent = std::move(*node),
        .child_slot = child_slot,
    });
  }
  return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
}

struct ClearPlan {
  explicit ClearPlan(std::pmr::memory_resource* resource)
      : pages_to_free(resource), overflow_pages_to_free(resource) {}

  std::pmr::vector<PageNumber> pages_to_free;
  std::pmr::vector<PageNumber> overflow_pages_to_free;
  std::uint64_t entry_count = 0;
};

[[nodiscard]] Result<ClearPlan> PlanClearTree(BtreeMutationCore& core, PageNumber root_page,
                                              bool table) {
  struct PendingPage {
    PageNumber page_number;
    std::size_t depth;
  };

  auto freelist_pages = FreelistPages(core);
  if (!freelist_pages.has_value()) {
    return std::unexpected(std::move(freelist_pages.error()));
  }
  try {
    std::pmr::vector<PendingPage> pending{core.scratch_resource()};
    PageOwnershipSet visited{core.scratch_resource()};
    PageOwnershipSet overflow_owners{core.scratch_resource()};
    ClearPlan plan{core.scratch_resource()};
    pending.push_back(PendingPage{.page_number = root_page, .depth = 0U});
    std::optional<std::size_t> leaf_depth;

    while (!pending.empty()) {
      const PendingPage item = pending.back();
      pending.pop_back();
      if (!visited.insert(item.page_number.value()).second) {
        return std::unexpected(Corruption("B-tree clear found a duplicate child reference"));
      }
      if ((*freelist_pages)->contains(item.page_number.value())) {
        return std::unexpected(Corruption("B-tree page is also owned by the freelist"));
      }
      auto node = ReadNode(core, item.page_number);
      if (!node.has_value()) {
        return std::unexpected(std::move(node.error()));
      }
      if (node->is_table() != table) {
        return std::unexpected(Corruption("B-tree clear found a page of the wrong tree kind"));
      }
      if (item.depth >= kMaximumBtreeDepth) {
        return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
      }
      if (item.page_number != root_page) {
        if (node->cells.empty()) {
          return std::unexpected(Corruption("non-root B-tree page is empty"));
        }
        plan.pages_to_free.push_back(item.page_number);
      }

      for (const CellImage& cell : node->cells) {
        if ((!table || node->is_leaf()) &&
            plan.entry_count == (std::numeric_limits<std::uint64_t>::max)()) {
          return std::unexpected(TooLarge("B-tree entry count exceeds uint64"));
        }
        if (!table || node->is_leaf()) {
          ++plan.entry_count;
        }
        if (cell.first_overflow_page.has_value()) {
          auto overflow_pages = CollectOverflowChain(core, cell, node->type);
          if (!overflow_pages.has_value()) {
            return std::unexpected(std::move(overflow_pages.error()));
          }
          for (const PageNumber overflow_page : *overflow_pages) {
            if ((*freelist_pages)->contains(overflow_page.value())) {
              return std::unexpected(Corruption("overflow page is also owned by the freelist"));
            }
            if (!overflow_owners.insert(overflow_page.value()).second) {
              return std::unexpected(
                  Corruption("B-tree clear found duplicate overflow-page ownership"));
            }
            plan.overflow_pages_to_free.push_back(overflow_page);
          }
        }
      }

      if (node->is_leaf()) {
        if (leaf_depth.has_value() && *leaf_depth != item.depth) {
          return std::unexpected(Corruption("B-tree leaves have unequal depths"));
        }
        leaf_depth = item.depth;
        continue;
      }
      if (!node->rightmost_child.has_value()) {
        return std::unexpected(Corruption("B-tree interior page has no rightmost child"));
      }
      pending.push_back(
          PendingPage{.page_number = node->rightmost_child.value(), .depth = item.depth + 1U});
      for (const CellImage& cell :
           std::ranges::subrange(node->cells.rbegin(), node->cells.rend())) {
        if (!cell.left_child.has_value()) {
          return std::unexpected(Corruption("B-tree interior cell has no left child"));
        }
        pending.push_back(
            PendingPage{.page_number = cell.left_child.value(), .depth = item.depth + 1U});
      }
    }
    for (const PageNumber overflow_page : plan.overflow_pages_to_free) {
      if (visited.contains(overflow_page.value())) {
        return std::unexpected(Corruption("B-tree page is also referenced by an overflow chain"));
      }
    }
    return plan;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

[[nodiscard]] Result<std::uint64_t> ClearTree(BtreeMutationCore& core, PageNumber root_page,
                                              bool table) {
  auto plan = PlanClearTree(core, root_page, table);
  if (!plan.has_value()) {
    return std::unexpected(std::move(plan.error()));
  }

  bool mutation_started = false;
  for (const PageNumber overflow_page : plan->overflow_pages_to_free) {
    auto freed = FreeBtreePage(core, overflow_page);
    if (!freed.has_value()) {
      if (mutation_started) {
        core.MarkRollbackRequired(freed.error().code());
      }
      return std::unexpected(std::move(freed.error()));
    }
    mutation_started = true;
  }
  for (const PageNumber page_number : plan->pages_to_free) {
    auto freed = FreeBtreePage(core, page_number);
    if (!freed.has_value()) {
      if (mutation_started) {
        core.MarkRollbackRequired(freed.error().code());
      }
      return std::unexpected(std::move(freed.error()));
    }
    mutation_started = true;
  }

  const NodeImage empty_root{
      .page_number = root_page,
      .type = table ? BtreePageType::kLeafTable : BtreePageType::kLeafIndex,
      .rightmost_child = std::nullopt,
      .cells = std::pmr::vector<CellImage>{core.scratch_resource()},
  };
  auto written = WriteNode(core, empty_root);
  if (!written.has_value()) {
    if (mutation_started) {
      core.MarkRollbackRequired(written.error().code());
    }
    return std::unexpected(std::move(written.error()));
  }
  return plan->entry_count;
}

[[nodiscard]] Result<TableSearchResult> SearchTable(BtreeMutationCore& core, PageNumber root_page,
                                                    std::int64_t rowid) {
  std::pmr::vector<TablePathEntry> path{core.scratch_resource()};
  try {
    path.reserve(kMaximumBtreeDepth);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  PageNumber page_number = root_page;
  for (std::size_t depth = 0; depth < kMaximumBtreeDepth; ++depth) {
    auto node = ReadNode(core, page_number);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (!node->is_table()) {
      return std::unexpected(Corruption("table B-tree path contains an index page"));
    }

    std::size_t lower = 0;
    std::size_t upper = node->cells.size();
    while (lower < upper) {
      const std::size_t middle = lower + (upper - lower) / 2U;
      if (!node->cells[middle].rowid.has_value()) {
        return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
      }
      if (node->cells[middle].rowid.value_or(0) < rowid) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }
    bool exact = false;
    if (lower < node->cells.size()) {
      if (!node->cells[lower].rowid.has_value()) {
        return std::unexpected(Corruption("table B-tree cell is missing its rowid"));
      }
      exact = node->cells[lower].rowid.value_or(0) == rowid;
    }
    if (node->is_leaf()) {
      return TableSearchResult{
          .path = std::move(path),
          .leaf = std::move(*node),
          .index = lower,
          .exact = exact,
          .tree_depth = depth + 1U,
      };
    }
    auto child = ChildAt(*node, lower);
    if (!child.has_value()) {
      return std::unexpected(std::move(child.error()));
    }
    path.push_back(TablePathEntry{
        .parent = std::move(*node),
        .child_slot = lower,
    });
    page_number = *child;
  }
  return std::unexpected(Corruption("B-tree depth exceeds SQLite's cursor limit"));
}

[[nodiscard]] Status InitializeDatabaseHeader(MutableByteView page, BtreePageGeometry geometry,
                                              BtreeDatabaseOptions options) {
  constexpr std::array<std::byte, 16> kMagic{
      std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
      std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
      std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
  };
  std::ranges::copy(kMagic, page.begin());
  const std::size_t page_size = geometry.page_size().value();
  Store16(page, 16U, page_size == 65536U ? 1U : static_cast<std::uint16_t>(page_size));
  page[18] = std::byte{1};
  page[19] = std::byte{1};
  page[20] = static_cast<std::byte>(options.reserved_bytes.value());
  page[21] = std::byte{64};
  page[22] = std::byte{32};
  page[23] = std::byte{32};
  Store32(page, 28U, 1U);
  Store32(page, 44U, SchemaFormatValue(options.schema_format));
  Store32(page, 56U, static_cast<std::uint32_t>(options.text_encoding));
  Store32(page, 96U, kSqliteVersion);
  return {};
}

}  // namespace

struct BtreeWriteSession::Impl {
  explicit Impl(std::shared_ptr<BtreeMutationCore> mutation_core) noexcept
      : core(std::move(mutation_core)) {}

  std::shared_ptr<BtreeMutationCore> core;
};

struct TableBtreeWriter::Impl {
  Impl(std::shared_ptr<BtreeMutationCore> mutation_core, PageNumber root,
       std::uint64_t root_incarnation) noexcept
      : core(std::move(mutation_core)), root_page(root), incarnation(root_incarnation) {}

  std::shared_ptr<BtreeMutationCore> core;
  PageNumber root_page;
  std::uint64_t incarnation;
};

struct IndexBtreeWriter::Impl {
  Impl(std::shared_ptr<BtreeMutationCore> mutation_core, PageNumber root,
       std::uint64_t root_incarnation, std::vector<IndexColumnOrder> column_orders) noexcept
      : core(std::move(mutation_core)),
        root_page(root),
        incarnation(root_incarnation),
        columns(std::move(column_orders)) {}

  std::shared_ptr<BtreeMutationCore> core;
  PageNumber root_page;
  std::uint64_t incarnation;
  std::vector<IndexColumnOrder> columns;
};

BtreeWriteSession::BtreeWriteSession(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

BtreeWriteSession::BtreeWriteSession(BtreeWriteSession&&) noexcept = default;

BtreeWriteSession& BtreeWriteSession::operator=(BtreeWriteSession&&) noexcept = default;

BtreeWriteSession::~BtreeWriteSession() = default;

Result<BtreeWriteSession> BtreeWriteSession::Open(Pager& pager) {
  try {
    if (!pager.in_write_transaction() || pager.state() == PagerState::kWriterFinished) {
      return std::unexpected(Misuse("B-tree write session requires a mutable write transaction"));
    }
    auto valid = ValidateExistingDatabase(pager);
    if (!valid.has_value()) {
      return std::unexpected(std::move(valid.error()));
    }
    const auto core =
        std::make_shared<BtreeMutationCore>(pager, pager.write_transaction_generation());
    auto impl = std::make_unique<Impl>(core);
    auto claimed = pager.ClaimWriteCoordinator();
    if (!claimed.has_value()) {
      return std::unexpected(std::move(claimed.error()));
    }
    return BtreeWriteSession{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

bool BtreeWriteSession::requires_rollback() const noexcept {
  return impl_ != nullptr && impl_->core->requires_rollback();
}

Status BtreeWriteSession::InitializeDatabase(BtreeDatabaseOptions options) {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("operation on a moved-from B-tree write session"));
    }
    auto active = impl_->core->CheckActive();
    if (!active.has_value()) {
      return active;
    }
    if (impl_->core->pager().page_count() != 0U) {
      return std::unexpected(Misuse("B-tree database initialization requires an empty pager"));
    }
    if (options.text_encoding != DatabaseTextEncoding::kUtf8) {
      return std::unexpected(Protocol("UTF-16 B-tree mutation is not implemented"));
    }
    if (!IsValidSchemaFormat(options.schema_format)) {
      return std::unexpected(Misuse("invalid database schema format"));
    }
    const std::size_t page_size = impl_->core->pager().page_size().value();
    if (options.reserved_bytes.value() > (std::numeric_limits<std::uint8_t>::max)() ||
        options.reserved_bytes.value() > page_size) {
      return std::unexpected(Misuse("B-tree reserved bytes exceed the pager page size"));
    }
    auto geometry =
        BtreePageGeometry::Create(ByteCount{page_size},
                                  ByteCount{page_size - options.reserved_bytes.value()});
    if (!geometry.has_value()) {
      return std::unexpected(std::move(geometry.error()));
    }

    const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
    {
      auto page_one = impl_->core->pager().AllocatePage();
      if (!page_one.has_value()) {
        impl_->core->MarkRollbackRequiredAfter(page_one.error().code(), mutation_sequence);
        return std::unexpected(std::move(page_one.error()));
      }
      impl_->core->NoteMutation();
      const MutableByteView bytes = page_one->mutable_bytes();
      std::ranges::fill(bytes, std::byte{0});
      auto initialized = InitializeDatabaseHeader(bytes, *geometry, options);
      if (!initialized.has_value()) {
        impl_->core->MarkRollbackRequiredAfter(initialized.error().code(), mutation_sequence);
        return initialized;
      }
      const std::size_t header_offset = 100U;
      bytes[header_offset] = static_cast<std::byte>(BtreePageType::kLeafTable);
      Store16(bytes, header_offset + 5U,
              geometry->usable_size().value() == 65536U
                  ? 0U
                  : static_cast<std::uint16_t>(geometry->usable_size().value()));
    }
    auto refreshed = impl_->core->pager().RefreshCurrentHeader();
    if (!refreshed.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(refreshed.error().code(), mutation_sequence);
      return refreshed;
    }
    impl_->core->SetGeometry(*geometry);
    impl_->core->SetRecordOptions(RecordCodecOptions{.schema_format = options.schema_format});
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<TableBtreeWriter> BtreeWriteSession::OpenTableBtree(PageNumber root_page) {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("operation on a moved-from B-tree write session"));
    }
    auto freelist_owned = FreelistContainsPage(*impl_->core, root_page);
    if (!freelist_owned.has_value()) {
      return std::unexpected(std::move(freelist_owned.error()));
    }
    if (*freelist_owned) {
      return std::unexpected(Corruption("B-tree root page is owned by the freelist"));
    }
    auto node = ReadNode(*impl_->core, root_page);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (!node->is_table()) {
      return std::unexpected(Corruption("B-tree root is not a table tree"));
    }
    const std::uint64_t incarnation = impl_->core->RootIncarnation(root_page);
    auto writer = std::make_unique<TableBtreeWriter::Impl>(impl_->core, root_page, incarnation);
    return TableBtreeWriter{std::move(writer)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<IndexBtreeWriter> BtreeWriteSession::OpenIndexBtree(
    PageNumber root_page, std::span<const IndexColumnOrder> columns) {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("operation on a moved-from B-tree write session"));
    }
    if (columns.empty()) {
      return std::unexpected(Misuse("index B-tree writer requires comparison metadata"));
    }
    for (const IndexColumnOrder& column : columns) {
      if (!IsValidDirection(column.direction()) || !IsValidNullPlacement(column.null_placement())) {
        return std::unexpected(Misuse("index B-tree writer has invalid comparison metadata"));
      }
    }
    auto freelist_owned = FreelistContainsPage(*impl_->core, root_page);
    if (!freelist_owned.has_value()) {
      return std::unexpected(std::move(freelist_owned.error()));
    }
    if (*freelist_owned) {
      return std::unexpected(Corruption("B-tree root page is owned by the freelist"));
    }
    auto node = ReadNode(*impl_->core, root_page);
    if (!node.has_value()) {
      return std::unexpected(std::move(node.error()));
    }
    if (node->is_table()) {
      return std::unexpected(Corruption("B-tree root is not an index tree"));
    }
    const std::uint64_t incarnation = impl_->core->RootIncarnation(root_page);
    std::vector<IndexColumnOrder> copied_columns;
    copied_columns.reserve(columns.size());
    copied_columns.insert(copied_columns.end(), columns.begin(), columns.end());
    auto writer = std::make_unique<IndexBtreeWriter::Impl>(impl_->core, root_page, incarnation,
                                                           std::move(copied_columns));
    return IndexBtreeWriter{std::move(writer)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<TableBtreeWriter> BtreeWriteSession::CreateTableBtree() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from B-tree write session"));
  }
  auto geometry = impl_->core->Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto allocated = AllocateBtreePage(*impl_->core);
  if (!allocated.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(allocated.error().code(), mutation_sequence);
    return std::unexpected(std::move(allocated.error()));
  }
  const PageNumber root_page = *allocated;
  const NodeImage node{
      .page_number = root_page,
      .type = BtreePageType::kLeafTable,
      .rightmost_child = std::nullopt,
      .cells = std::pmr::vector<CellImage>{impl_->core->scratch_resource()},
  };
  auto written = WriteNode(*impl_->core, node);
  if (!written.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(written.error().code(), mutation_sequence);
    return std::unexpected(std::move(written.error()));
  }
  auto opened = OpenTableBtree(root_page);
  if (!opened.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(opened.error().code(), mutation_sequence);
  }
  return opened;
}

Result<IndexBtreeWriter> BtreeWriteSession::CreateIndexBtree(
    std::span<const IndexColumnOrder> columns) {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from B-tree write session"));
  }
  if (columns.empty()) {
    return std::unexpected(Misuse("index B-tree writer requires comparison metadata"));
  }
  for (const IndexColumnOrder& column : columns) {
    if (!IsValidDirection(column.direction()) || !IsValidNullPlacement(column.null_placement())) {
      return std::unexpected(Misuse("index B-tree writer has invalid comparison metadata"));
    }
  }
  auto geometry = impl_->core->Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto allocated = AllocateBtreePage(*impl_->core);
  if (!allocated.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(allocated.error().code(), mutation_sequence);
    return std::unexpected(std::move(allocated.error()));
  }
  const PageNumber root_page = *allocated;
  const NodeImage node{
      .page_number = root_page,
      .type = BtreePageType::kLeafIndex,
      .rightmost_child = std::nullopt,
      .cells = std::pmr::vector<CellImage>{impl_->core->scratch_resource()},
  };
  auto written = WriteNode(*impl_->core, node);
  if (!written.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(written.error().code(), mutation_sequence);
    return std::unexpected(std::move(written.error()));
  }
  auto opened = OpenIndexBtree(root_page, columns);
  if (!opened.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(opened.error().code(), mutation_sequence);
  }
  return opened;
}

TableBtreeWriter::TableBtreeWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TableBtreeWriter::TableBtreeWriter(TableBtreeWriter&&) noexcept = default;

TableBtreeWriter& TableBtreeWriter::operator=(TableBtreeWriter&&) noexcept = default;

TableBtreeWriter::~TableBtreeWriter() = default;

PageNumber TableBtreeWriter::root_page() const noexcept {
  return impl_ == nullptr ? PageNumber{} : impl_->root_page;
}

bool TableBtreeWriter::requires_rollback() const noexcept {
  return impl_ != nullptr && impl_->core->requires_rollback();
}

Status TableBtreeWriter::Insert(std::int64_t rowid, ByteView payload, BtreeInsertMode mode) {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from table B-tree writer"));
  }
  if (mode != BtreeInsertMode::kInsertOnly && mode != BtreeInsertMode::kReplace) {
    return std::unexpected(Misuse("invalid B-tree insert mode"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return active;
  }
  auto geometry = impl_->core->Geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  try {
    auto found = SearchTable(*impl_->core, impl_->root_page, rowid);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (found->exact && mode == BtreeInsertMode::kInsertOnly) {
      return std::unexpected(Constraint("table rowid already exists"));
    }
    if (!found->exact) {
      found->leaf.cells.reserve(found->leaf.cells.size() + 1U);
    }
    for (TablePathEntry& entry : found->path) {
      entry.parent.cells.reserve(entry.parent.cells.size() + 2U);
    }
    std::optional<std::pmr::vector<PageNumber>> replaced_overflow_pages;
    if (found->exact) {
      auto collected = CollectOverflowChain(*impl_->core, found->leaf.cells[found->index],
                                            BtreePageType::kLeafTable);
      if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
      }
      replaced_overflow_pages.emplace(std::move(*collected));
    }
    auto encoded = EncodeTableLeafCell(*impl_->core, rowid, payload, *geometry);
    if (!encoded.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(encoded.error().code(), mutation_sequence);
      return std::unexpected(std::move(encoded.error()));
    }

    if (found->exact) {
      found->leaf.cells[found->index] = std::move(*encoded);
    } else {
      found->leaf.cells.insert(
          found->leaf.cells.begin() + static_cast<std::ptrdiff_t>(found->index),
          std::move(*encoded));
    }
    auto written = WriteTableInsertion(*impl_->core, impl_->root_page, std::move(*found));
    if (!written.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(written.error().code(), mutation_sequence);
      return written;
    }
    if (replaced_overflow_pages.has_value()) {
      auto freed = FreeOverflowPages(*impl_->core, *replaced_overflow_pages);
      if (!freed.has_value()) {
        impl_->core->MarkRollbackRequired(freed.error().code());
        return freed;
      }
    }
    return {};
  } catch (const std::bad_alloc&) {
    impl_->core->MarkRollbackRequiredAfter(ErrorCode::kOutOfMemory, mutation_sequence);
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> TableBtreeWriter::Delete(std::int64_t rowid) {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from table B-tree writer"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  try {
    auto found = SearchTable(*impl_->core, impl_->root_page, rowid);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!found->exact) {
      return false;
    }
    for (TablePathEntry& entry : found->path) {
      entry.parent.cells.reserve(entry.parent.cells.size() + 2U);
    }
    auto removed_overflow_pages = CollectOverflowChain(
        *impl_->core, found->leaf.cells[found->index], BtreePageType::kLeafTable);
    if (!removed_overflow_pages.has_value()) {
      return std::unexpected(std::move(removed_overflow_pages.error()));
    }
    found->leaf.cells.erase(found->leaf.cells.begin() + static_cast<std::ptrdiff_t>(found->index));
    auto written = RebalanceTableAfterDelete(*impl_->core, impl_->root_page, std::move(*found));
    if (!written.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(written.error().code(), mutation_sequence);
      return std::unexpected(std::move(written.error()));
    }
    auto freed = FreeOverflowPages(*impl_->core, *removed_overflow_pages);
    if (!freed.has_value()) {
      impl_->core->MarkRollbackRequired(freed.error().code());
      return std::unexpected(std::move(freed.error()));
    }
    return true;
  } catch (const std::bad_alloc&) {
    impl_->core->MarkRollbackRequiredAfter(ErrorCode::kOutOfMemory, mutation_sequence);
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::uint64_t> TableBtreeWriter::Clear() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from table B-tree writer"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto cleared = ClearTree(*impl_->core, impl_->root_page, true);
  if (!cleared.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(cleared.error().code(), mutation_sequence);
  }
  return cleared;
}

Status TableBtreeWriter::Drop() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from table B-tree writer"));
  }
  if (impl_->root_page == PageNumber{1}) {
    return std::unexpected(Misuse("database page one cannot be dropped"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return active;
  }
  auto tracked = impl_->core->EnsureRootIncarnationTracked(impl_->root_page);
  if (!tracked.has_value()) {
    return tracked;
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto cleared = ClearTree(*impl_->core, impl_->root_page, true);
  if (!cleared.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(cleared.error().code(), mutation_sequence);
    return std::unexpected(std::move(cleared.error()));
  }
  auto freed = FreeBtreePage(*impl_->core, impl_->root_page);
  if (!freed.has_value()) {
    impl_->core->MarkRollbackRequired(freed.error().code());
    return freed;
  }
  impl_->core->AdvanceRootIncarnation(impl_->root_page);
  return {};
}

IndexBtreeWriter::IndexBtreeWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

IndexBtreeWriter::IndexBtreeWriter(IndexBtreeWriter&&) noexcept = default;

IndexBtreeWriter& IndexBtreeWriter::operator=(IndexBtreeWriter&&) noexcept = default;

IndexBtreeWriter::~IndexBtreeWriter() = default;

PageNumber IndexBtreeWriter::root_page() const noexcept {
  return impl_ == nullptr ? PageNumber{} : impl_->root_page;
}

bool IndexBtreeWriter::requires_rollback() const noexcept {
  return impl_ != nullptr && impl_->core->requires_rollback();
}

Status IndexBtreeWriter::Insert(std::span<const SqlValue> values) {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from index B-tree writer"));
  }
  if (values.size() != impl_->columns.size()) {
    return std::unexpected(Misuse("index value count does not match comparison metadata"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return active;
  }
  auto options = impl_->core->RecordOptions();
  if (!options.has_value()) {
    return std::unexpected(std::move(options.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  try {
    auto encoded_size = EncodedRecordSize(values, *options);
    if (!encoded_size.has_value()) {
      return std::unexpected(std::move(encoded_size.error()));
    }
    auto encoded_record = impl_->core->ResizeEncodedRecordScratch(encoded_size->value());
    if (!encoded_record.has_value()) {
      return std::unexpected(std::move(encoded_record.error()));
    }
    auto encoded = EncodeRecordInto(values, *encoded_record, *options);
    if (!encoded.has_value()) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto found = SearchIndex(*impl_->core, impl_->root_page, values, impl_->columns);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (found->exact) {
      return std::unexpected(Constraint("index record already exists"));
    }
    found->node.cells.reserve(found->node.cells.size() + 1U);
    for (IndexPathEntry& entry : found->path) {
      entry.parent.cells.reserve(entry.parent.cells.size() + 2U);
    }
    auto cell =
        EncodeIndexCell(*impl_->core, ByteView{*encoded_record}.first(encoded->value()),
                        BtreePageType::kLeafIndex, std::nullopt);
    if (!cell.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(cell.error().code(), mutation_sequence);
      return std::unexpected(std::move(cell.error()));
    }
    found->node.cells.insert(found->node.cells.begin() + static_cast<std::ptrdiff_t>(found->index),
                             std::move(*cell));
    auto written = WriteIndexInsertion(*impl_->core, impl_->root_page, std::move(*found));
    if (!written.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(written.error().code(), mutation_sequence);
    }
    return written;
  } catch (const std::bad_alloc&) {
    impl_->core->MarkRollbackRequiredAfter(ErrorCode::kOutOfMemory, mutation_sequence);
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> IndexBtreeWriter::Delete(std::span<const SqlValue> values) {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from index B-tree writer"));
  }
  if (values.size() != impl_->columns.size()) {
    return std::unexpected(Misuse("index value count does not match comparison metadata"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  try {
    auto found = SearchIndex(*impl_->core, impl_->root_page, values, impl_->columns);
    if (!found.has_value()) {
      return std::unexpected(std::move(found.error()));
    }
    if (!found->exact) {
      return false;
    }
    for (IndexPathEntry& entry : found->path) {
      entry.parent.cells.reserve(entry.parent.cells.size() + 2U);
    }
    auto removed_overflow_pages =
        CollectOverflowChain(*impl_->core, found->node.cells[found->index], found->node.type);
    if (!removed_overflow_pages.has_value()) {
      return std::unexpected(std::move(removed_overflow_pages.error()));
    }

    if (found->node.is_leaf()) {
      found->node.cells.erase(found->node.cells.begin() +
                              static_cast<std::ptrdiff_t>(found->index));
      auto rebalanced =
          RebalanceIndexAfterDelete(*impl_->core, impl_->root_page, std::move(*found));
      if (!rebalanced.has_value()) {
        impl_->core->MarkRollbackRequiredAfter(rebalanced.error().code(), mutation_sequence);
        return std::unexpected(std::move(rebalanced.error()));
      }
      auto freed = FreeOverflowPages(*impl_->core, *removed_overflow_pages);
      if (!freed.has_value()) {
        impl_->core->MarkRollbackRequired(freed.error().code());
        return std::unexpected(std::move(freed.error()));
      }
      return true;
    }

    const std::size_t target_depth = found->path.size() + 1U;
    auto predecessor = ReadIndexPredecessor(
        *impl_->core, found->node,
        IndexPredecessorTarget{.index = found->index, .depth = target_depth});
    if (!predecessor.has_value()) {
      return std::unexpected(std::move(predecessor.error()));
    }
    if (predecessor->values.size() != impl_->columns.size()) {
      return std::unexpected(Corruption("index predecessor field count does not match its schema"));
    }
    auto predecessor_overflow_pages = CollectOverflowChain(
        *impl_->core, predecessor->cell, BtreePageType::kLeafIndex);
    if (!predecessor_overflow_pages.has_value()) {
      return std::unexpected(std::move(predecessor_overflow_pages.error()));
    }
    PageOwnershipSet removed_overflow_owners{impl_->core->scratch_resource()};
    removed_overflow_owners.reserve(removed_overflow_pages->size());
    for (const PageNumber page : *removed_overflow_pages) {
      static_cast<void>(removed_overflow_owners.insert(page.value()));
    }
    for (const PageNumber page : *predecessor_overflow_pages) {
      if (removed_overflow_owners.contains(page.value())) {
        return std::unexpected(
            Corruption("index predecessor and removed record share overflow ownership"));
      }
    }
    if (!found->node.cells[found->index].left_child.has_value()) {
      return std::unexpected(Corruption("index interior entry has no left child"));
    }
    const PageNumber original_left_child =
        found->node.cells[found->index].left_child.value_or(PageNumber{});
    auto geometry = impl_->core->Geometry();
    if (!geometry.has_value()) {
      return std::unexpected(std::move(geometry.error()));
    }
    auto replacement =
        IndexCellAsInterior(std::move(predecessor->cell), original_left_child, *geometry);
    if (!replacement.has_value()) {
      return std::unexpected(std::move(replacement.error()));
    }
    found->node.cells[found->index] = std::move(*replacement);
    found->tree_depth = predecessor->tree_depth;
    auto installed = WriteIndexInsertion(*impl_->core, impl_->root_page, std::move(*found));
    if (!installed.has_value()) {
      impl_->core->MarkRollbackRequiredAfter(installed.error().code(), mutation_sequence);
      return std::unexpected(std::move(installed.error()));
    }

    auto duplicate = FindTransferredPredecessorLeaf(*impl_->core, impl_->root_page,
                                                    predecessor->values, impl_->columns);
    if (!duplicate.has_value()) {
      impl_->core->MarkRollbackRequired(duplicate.error().code());
      return std::unexpected(std::move(duplicate.error()));
    }
    for (IndexPathEntry& entry : duplicate->path) {
      entry.parent.cells.reserve(entry.parent.cells.size() + 2U);
    }
    duplicate->node.cells.erase(duplicate->node.cells.begin() +
                                static_cast<std::ptrdiff_t>(duplicate->index));
    auto rebalanced =
        RebalanceIndexAfterDelete(*impl_->core, impl_->root_page, std::move(*duplicate));
    if (!rebalanced.has_value()) {
      impl_->core->MarkRollbackRequired(rebalanced.error().code());
      return std::unexpected(std::move(rebalanced.error()));
    }
    auto freed = FreeOverflowPages(*impl_->core, *removed_overflow_pages);
    if (!freed.has_value()) {
      impl_->core->MarkRollbackRequired(freed.error().code());
      return std::unexpected(std::move(freed.error()));
    }
    return true;
  } catch (const std::bad_alloc&) {
    impl_->core->MarkRollbackRequiredAfter(ErrorCode::kOutOfMemory, mutation_sequence);
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::uint64_t> IndexBtreeWriter::Clear() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from index B-tree writer"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto cleared = ClearTree(*impl_->core, impl_->root_page, false);
  if (!cleared.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(cleared.error().code(), mutation_sequence);
  }
  return cleared;
}

Status IndexBtreeWriter::Drop() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("operation on a moved-from index B-tree writer"));
  }
  auto active = impl_->core->CheckHandle(impl_->root_page, impl_->incarnation);
  if (!active.has_value()) {
    return active;
  }
  auto tracked = impl_->core->EnsureRootIncarnationTracked(impl_->root_page);
  if (!tracked.has_value()) {
    return tracked;
  }
  const std::uint64_t mutation_sequence = impl_->core->mutation_sequence();
  auto cleared = ClearTree(*impl_->core, impl_->root_page, false);
  if (!cleared.has_value()) {
    impl_->core->MarkRollbackRequiredAfter(cleared.error().code(), mutation_sequence);
    return std::unexpected(std::move(cleared.error()));
  }
  auto freed = FreeBtreePage(*impl_->core, impl_->root_page);
  if (!freed.has_value()) {
    impl_->core->MarkRollbackRequired(freed.error().code());
    return freed;
  }
  impl_->core->AdvanceRootIncarnation(impl_->root_page);
  return {};
}

}  // namespace modern_sqlite
