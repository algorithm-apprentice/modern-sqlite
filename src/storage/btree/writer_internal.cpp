#include "writer_internal.hpp"

#include <algorithm>
#include <bit>
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

#include "modern_sqlite/base/coding.hpp"

namespace modern_sqlite::btree_internal {
namespace {

constexpr std::uint32_t kMaximumPageNumber = 0xfffffffeU;
constexpr std::uint64_t kMaximumPayloadSize = 0x7fffffffULL;

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

[[nodiscard]] std::uint16_t Load16(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint16_t>(std::span<const std::byte, sizeof(std::uint16_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint16_t)});
}

struct BigEndian16 {
  std::size_t offset;
  std::size_t value;
};

void Store16(MutableByteView bytes, BigEndian16 encoded_value) noexcept {
  const auto encoded =
      static_cast<std::uint16_t>(encoded_value.value == 65536U ? 0U : encoded_value.value);
  StoreBigEndian<std::uint16_t>(
      std::span<std::byte, sizeof(std::uint16_t)>{
          bytes.data() + static_cast<std::ptrdiff_t>(encoded_value.offset), sizeof(std::uint16_t)},
      encoded);
}

void Store32(MutableByteView bytes, std::size_t offset, std::uint32_t value) noexcept {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{
          bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint32_t)},
      value);
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
      minimum + (payload_size - minimum) % geometry.overflow_payload_capacity().value();
  return candidate <= maximum ? candidate : minimum;
}

[[nodiscard]] bool IsValidPageReference(PageNumber page_number,
                                        BtreePageGeometry geometry) noexcept {
  return page_number.value() != 0U && page_number.value() <= kMaximumPageNumber &&
         page_number != geometry.locking_page();
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

Result<BtreeWriteWorkspace> BtreeWriteWorkspace::Create(ByteCount page_size) {
  if (page_size.value() < 512U || page_size.value() > 65536U ||
      !std::has_single_bit(page_size.value())) {
    return std::unexpected(
        Misuse("B-tree workspace page size must be a power of two from 512 through 65536"));
  }
  try {
    ByteBuffer cell_scratch{ByteCount{page_size.value() + 4U}};
    ByteBuffer rebuild_scratch{page_size};
    std::ranges::fill(cell_scratch.mutable_view().first(4U), std::byte{0});
    return BtreeWriteWorkspace{std::move(cell_scratch), std::move(rebuild_scratch)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

BtreeWriteWorkspace::BtreeWriteWorkspace(ByteBuffer cell_scratch,
                                         ByteBuffer rebuild_scratch) noexcept
    : cell_scratch_(std::move(cell_scratch)), rebuild_scratch_(std::move(rebuild_scratch)) {}

MutableByteView BtreeWriteWorkspace::cell_scratch_with_prefix() noexcept {
  return cell_scratch_.mutable_view();
}

MutableByteView BtreeWriteWorkspace::cell_scratch() noexcept {
  return cell_scratch_.mutable_view().subspan(4U);
}

MutableByteView BtreeWriteWorkspace::rebuild_scratch() noexcept {
  return rebuild_scratch_.mutable_view();
}

Result<MutableBtreePage> MutableBtreePage::Open(MutationPageOwner& owner, std::size_t owner_slot,
                                                BtreePageGeometry geometry) {
  auto active = owner.CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (!owner.IsWritable(owner_slot)) {
    return std::unexpected(Misuse("mutable B-tree page requires a writable owned page"));
  }
  auto frame = owner.Frame(owner_slot);
  if (!frame.has_value()) {
    return std::unexpected(std::move(frame.error()));
  }
  auto page = BtreePageView::Parse(frame->get().bytes(), frame->get().page_number(), geometry);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  auto free = page->AnalyzeFreeSpace();
  if (!free.has_value()) {
    return std::unexpected(std::move(free.error()));
  }
  const std::size_t header_offset = frame->get().page_number() == PageNumber{1} ? 100U : 0U;
  const std::size_t cell_pointer_offset = header_offset + (page->is_leaf() ? 8U : 12U);
  return MutableBtreePage{
      owner,
      owner_slot,
      geometry,
      frame->get().page_number(),
      page->type(),
      Metadata{
          .header_offset = header_offset,
          .cell_pointer_offset = cell_pointer_offset,
          .cell_count = page->cell_count(),
          .free_bytes = free->total().value(),
      },
  };
}

Result<MutableBtreePage> MutableBtreePage::Initialize(MutationPageOwner& owner,
                                                      std::size_t owner_slot,
                                                      BtreePageGeometry geometry,
                                                      BtreePageType type) {
  auto active = owner.CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (!owner.IsWritable(owner_slot)) {
    return std::unexpected(Misuse("mutable B-tree page requires a writable owned page"));
  }
  auto frame = owner.Frame(owner_slot);
  if (!frame.has_value()) {
    return std::unexpected(std::move(frame.error()));
  }
  if (frame->get().bytes().size() != geometry.page_size().value()) {
    return std::unexpected(Misuse("mutable B-tree page has the wrong page size"));
  }
  if (frame->get().page_number() == PageNumber{1} && type != BtreePageType::kLeafTable &&
      type != BtreePageType::kInteriorTable) {
    return std::unexpected(Corruption("database page one must contain a table B-tree"));
  }
  const std::size_t header_offset = frame->get().page_number() == PageNumber{1} ? 100U : 0U;
  const bool leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  MutableBtreePage page{
      owner,
      owner_slot,
      geometry,
      frame->get().page_number(),
      type,
      Metadata{
          .header_offset = header_offset,
          .cell_pointer_offset = header_offset + (leaf ? 8U : 12U),
          .cell_count = 0U,
          .free_bytes = 0U,
      },
  };
  auto zeroed = page.Zero(type);
  if (!zeroed.has_value()) {
    return std::unexpected(std::move(zeroed.error()));
  }
  return page;
}

Status MutableBtreePage::Zero(BtreePageType type) {
  if (page_number_ == PageNumber{1} && type != BtreePageType::kLeafTable &&
      type != BtreePageType::kInteriorTable) {
    return std::unexpected(Corruption("database page one must contain a table B-tree"));
  }
  const bool leaf = type == BtreePageType::kLeafIndex || type == BtreePageType::kLeafTable;
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  const std::size_t header_size = leaf ? 8U : 12U;
  if (header_offset_ + header_size > geometry_.usable_size().value()) {
    return std::unexpected(Corruption("B-tree page header exceeds the usable region"));
  }
  (*bytes)[header_offset_] = static_cast<std::byte>(type);
  std::ranges::fill(bytes->subspan(header_offset_ + 1U, 4U), std::byte{0});
  Store16(*bytes, BigEndian16{
                      .offset = header_offset_ + 5U,
                      .value = geometry_.usable_size().value(),
                  });
  (*bytes)[header_offset_ + 7U] = std::byte{0};

  type_ = type;
  cell_pointer_offset_ = header_offset_ + header_size;
  cell_count_ = 0U;
  free_bytes_ = geometry_.usable_size().value() - cell_pointer_offset_;
  ClearStagedCells();
  return {};
}

Status MutableBtreePage::Defragment(std::size_t maximum_fragments, BtreeWriteWorkspace& workspace) {
  if (staged_count_ != 0U) {
    return std::unexpected(Misuse("B-tree page with staged cells cannot be defragmented"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  const std::size_t pointer_end = cell_pointer_offset_ + cell_count_ * 2U;
  const std::size_t usable_size = geometry_.usable_size().value();
  const std::size_t fragments = std::to_integer<std::uint8_t>((*bytes)[header_offset_ + 7U]);

  if (fragments <= maximum_fragments) {
    const std::size_t first_freeblock = Load16(*bytes, header_offset_ + 1U);
    if (first_freeblock > usable_size - 4U) {
      return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
    }
    if (first_freeblock != 0U) {
      const std::size_t second_freeblock = Load16(*bytes, first_freeblock);
      if (second_freeblock > usable_size - 4U) {
        return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
      }
      if (second_freeblock == 0U || Load16(*bytes, second_freeblock) == 0U) {
        std::size_t second_size = 0U;
        std::size_t combined_size = Load16(*bytes, first_freeblock + 2U);
        std::size_t top = Load16(*bytes, header_offset_ + 5U);
        if (top == 0U && usable_size == 65536U) {
          top = 65536U;
        }
        if (top >= first_freeblock) {
          return std::unexpected(Corruption("B-tree freeblock precedes its content area"));
        }
        if (second_freeblock != 0U) {
          if (first_freeblock + combined_size > second_freeblock) {
            return std::unexpected(Corruption("B-tree freeblocks overlap"));
          }
          second_size = Load16(*bytes, second_freeblock + 2U);
          if (second_freeblock + second_size > usable_size) {
            return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
          }
          std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(first_freeblock + combined_size +
                                                                   second_size),
                       bytes->data() + static_cast<std::ptrdiff_t>(first_freeblock + combined_size),
                       second_freeblock - (first_freeblock + combined_size));
          combined_size += second_size;
        } else if (first_freeblock + combined_size > usable_size) {
          return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
        }

        const std::size_t content = top + combined_size;
        std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(content),
                     bytes->data() + static_cast<std::ptrdiff_t>(top), first_freeblock - top);
        for (std::size_t pointer = cell_pointer_offset_; pointer < pointer_end; pointer += 2U) {
          std::size_t cell_offset = Load16(*bytes, pointer);
          if (cell_offset < first_freeblock) {
            cell_offset += combined_size;
          } else if (cell_offset < second_freeblock) {
            cell_offset += second_size;
          }
          Store16(*bytes, BigEndian16{.offset = pointer, .value = cell_offset});
        }
        if (fragments + content - pointer_end != free_bytes_) {
          return std::unexpected(
              Corruption("B-tree free-space total changed during defragmentation"));
        }
        Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = content});
        Store16(*bytes, BigEndian16{.offset = header_offset_ + 1U, .value = 0U});
        std::ranges::fill(bytes->subspan(pointer_end, content - pointer_end), std::byte{0});
        return {};
      }
    }
  }

  const MutableByteView scratch = workspace.rebuild_scratch();
  if (scratch.size() != bytes->size()) {
    return std::unexpected(Misuse("B-tree rebuild scratch has the wrong size"));
  }
  std::ranges::copy(*bytes, scratch.begin());
  const ByteView source{scratch};
  auto source_page = BtreePageView::Parse(source, page_number_, geometry_);
  if (!source_page.has_value()) {
    return std::unexpected(std::move(source_page.error()));
  }

  const std::size_t original_start = source_page->cell_content_offset().value();
  std::size_t content = geometry_.usable_size().value();
  for (std::size_t index = 0; index < cell_count_; ++index) {
    auto offset = source_page->cell_offset(index);
    auto cell = source_page->cell(index);
    if (!offset.has_value()) {
      return std::unexpected(std::move(offset.error()));
    }
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    const std::size_t size = cell->encoded_size().value();
    if (size > content || content - size < original_start ||
        offset->value() > geometry_.usable_size().value() - size) {
      return std::unexpected(Corruption("B-tree cell cannot be defragmented safely"));
    }
    content -= size;
    Store16(*bytes, BigEndian16{
                        .offset = cell_pointer_offset_ + index * 2U,
                        .value = content,
                    });
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(content),
                 source.data() + static_cast<std::ptrdiff_t>(offset->value()), size);
  }
  if (content < pointer_end || content - pointer_end != free_bytes_) {
    return std::unexpected(Corruption("B-tree free-space total changed during defragmentation"));
  }
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 1U, .value = 0U});
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = content});
  (*bytes)[header_offset_ + 7U] = std::byte{0};
  std::ranges::fill(bytes->subspan(pointer_end, content - pointer_end), std::byte{0});
  if (maximum_fragments < std::to_integer<std::uint8_t>((*bytes)[header_offset_ + 7U])) {
    return std::unexpected(Corruption("B-tree defragmentation left too many fragments"));
  }
  return {};
}

Result<std::size_t> MutableBtreePage::AllocateSpace(std::size_t size,
                                                    BtreeWriteWorkspace& workspace) {
  if (size < 4U || size >= geometry_.usable_size().value() - 8U) {
    return std::unexpected(Misuse("B-tree cell size is outside SQLite's page-local range"));
  }
  if (staged_count_ != 0U) {
    return std::unexpected(Misuse("B-tree page with staged cells cannot allocate local space"));
  }
  if (size + 2U > free_bytes_) {
    return std::unexpected(Misuse("B-tree page does not have enough free space"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }

  const std::size_t gap = cell_pointer_offset_ + 2U * cell_count_;
  std::size_t top = Load16(*bytes, header_offset_ + 5U);
  if (top == 0U && geometry_.usable_size().value() == 65536U) {
    top = 65536U;
  }
  if (gap > top || top > geometry_.usable_size().value()) {
    return std::unexpected(Corruption("B-tree unallocated region is malformed"));
  }

  if (Load16(*bytes, header_offset_ + 1U) != 0U && gap + 2U <= top) {
    auto freeblock = FindFreeblock(size);
    if (!freeblock.has_value()) {
      return std::unexpected(std::move(freeblock.error()));
    }
    if (freeblock->has_value()) {
      if (**freeblock <= gap) {
        return std::unexpected(Corruption("B-tree freeblock overlaps the pointer array"));
      }
      return **freeblock;
    }
  }

  if (gap + 2U + size > top) {
    const std::size_t available_fragments = free_bytes_ - (2U + size);
    auto defragmented = Defragment(std::min<std::size_t>(4U, available_fragments), workspace);
    if (!defragmented.has_value()) {
      return std::unexpected(std::move(defragmented.error()));
    }
    bytes = Bytes();
    if (!bytes.has_value()) {
      return std::unexpected(std::move(bytes.error()));
    }
    top = Load16(*bytes, header_offset_ + 5U);
    if (top == 0U && geometry_.usable_size().value() == 65536U) {
      top = 65536U;
    }
    if (gap + 2U + size > top) {
      return std::unexpected(Corruption("B-tree defragmentation did not create enough space"));
    }
  }

  top -= size;
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = top});
  return top;
}

Status MutableBtreePage::FreeSpace(std::size_t offset, std::size_t size) {
  if (size < 4U || offset < header_offset_ + 6U + (is_leaf() ? 0U : 4U) ||
      offset > geometry_.usable_size().value() - 4U ||
      size > geometry_.usable_size().value() - offset) {
    return std::unexpected(Corruption("freed B-tree cell range is invalid"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }

  const std::size_t original_size = size;
  std::size_t start = offset;
  std::size_t end = start + size;
  std::size_t pointer = header_offset_ + 1U;
  std::size_t next = 0U;
  std::size_t fragments = 0U;
  if (Load16(*bytes, pointer) != 0U) {
    while ((next = Load16(*bytes, pointer)) < start) {
      if (next <= pointer) {
        if (next == 0U) {
          break;
        }
        return std::unexpected(Corruption("B-tree freeblock chain is unordered"));
      }
      pointer = next;
    }
    if (next > geometry_.usable_size().value() - 4U) {
      return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
    }
    if (next != 0U && end + 3U >= next) {
      fragments = next - end;
      if (end > next) {
        return std::unexpected(Corruption("B-tree freeblocks overlap"));
      }
      const std::size_t next_size = Load16(*bytes, next + 2U);
      if (next_size > geometry_.usable_size().value() - next) {
        return std::unexpected(Corruption("B-tree freeblock size is invalid"));
      }
      end = next + next_size;
      size = end - start;
      next = Load16(*bytes, next);
    }
    if (pointer > header_offset_ + 1U) {
      const std::size_t pointer_size = Load16(*bytes, pointer + 2U);
      if (pointer_size > geometry_.usable_size().value() - pointer) {
        return std::unexpected(Corruption("B-tree freeblock size is invalid"));
      }
      const std::size_t pointer_end = pointer + pointer_size;
      if (pointer_end + 3U >= start) {
        if (pointer_end > start) {
          return std::unexpected(Corruption("B-tree freeblocks overlap"));
        }
        fragments += start - pointer_end;
        size = end - pointer;
        start = pointer;
      }
    }
    const std::size_t recorded_fragments =
        std::to_integer<std::uint8_t>((*bytes)[header_offset_ + 7U]);
    if (fragments > recorded_fragments) {
      return std::unexpected(Corruption("B-tree fragment count is inconsistent"));
    }
    (*bytes)[header_offset_ + 7U] = static_cast<std::byte>(recorded_fragments - fragments);
  }

  std::size_t content = Load16(*bytes, header_offset_ + 5U);
  if (content == 0U && geometry_.usable_size().value() == 65536U) {
    content = 65536U;
  }
  if (start <= content) {
    if (start != content || pointer != header_offset_ + 1U) {
      return std::unexpected(Corruption("B-tree free range overlaps the unallocated region"));
    }
    Store16(*bytes, BigEndian16{.offset = header_offset_ + 1U, .value = next});
    Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = end});
  } else {
    Store16(*bytes, BigEndian16{.offset = pointer, .value = start});
    Store16(*bytes, BigEndian16{.offset = start, .value = next});
    Store16(*bytes, BigEndian16{.offset = start + 2U, .value = size});
  }
  free_bytes_ += original_size;
  return {};
}

Status MutableBtreePage::DropCell(std::size_t index) {
  if (index >= cell_count_) {
    return std::unexpected(Misuse("dropped B-tree cell index is out of range"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  auto page = BtreePageView::Parse(*bytes, page_number_, geometry_);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  auto offset = page->cell_offset(index);
  auto cell = page->cell(index);
  if (!offset.has_value()) {
    return std::unexpected(std::move(offset.error()));
  }
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  auto freed = FreeSpace(offset->value(), cell->encoded_size().value());
  if (!freed.has_value()) {
    return freed;
  }

  bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  const std::size_t pointer = cell_pointer_offset_ + index * 2U;
  --cell_count_;
  if (cell_count_ == 0U) {
    std::ranges::fill(bytes->subspan(header_offset_ + 1U, 4U), std::byte{0});
    (*bytes)[header_offset_ + 7U] = std::byte{0};
    Store16(*bytes, BigEndian16{
                        .offset = header_offset_ + 5U,
                        .value = geometry_.usable_size().value(),
                    });
    free_bytes_ = geometry_.usable_size().value() - cell_pointer_offset_;
  } else {
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(pointer),
                 bytes->data() + static_cast<std::ptrdiff_t>(pointer + 2U),
                 2U * (cell_count_ - index));
    Store16(*bytes, BigEndian16{.offset = header_offset_ + 3U, .value = cell_count_});
    free_bytes_ += 2U;
  }
  return {};
}

Status MutableBtreePage::InsertCell(std::size_t index, ByteView cell,
                                    std::optional<PageNumber> left_child,
                                    MutableByteView staged_copy, BtreeWriteWorkspace& workspace) {
  if (index > cell_count_ + staged_count_) {
    return std::unexpected(Misuse("inserted B-tree cell index is out of range"));
  }
  auto valid = ValidateCellImage(cell, left_child);
  if (!valid.has_value()) {
    return valid;
  }

  if (staged_count_ != 0U || cell.size() + 2U > free_bytes_) {
    if (staged_count_ >= kStagedCellSlots - 1U) {
      return std::unexpected(TooLarge("B-tree page exceeded SQLite's staged-cell bound"));
    }
    ByteView staged = cell;
    if (!staged_copy.empty()) {
      if (staged_copy.size() < cell.size()) {
        return std::unexpected(Misuse("staged B-tree cell buffer is too small"));
      }
      std::memmove(staged_copy.data(), cell.data(), cell.size());
      staged = ByteView{staged_copy.first(cell.size())};
    }
    if (left_child.has_value()) {
      if (staged_copy.empty()) {
        return std::unexpected(
            Misuse("staged interior B-tree cell requires writable copied storage"));
      }
      Store32(staged_copy, 0U, left_child->value());
    }
    if (staged_count_ != 0U) {
      const StagedCell prior =
          staged_cells_[staged_count_ - 1U].value_or(StagedCell{.index = 0U, .bytes = {}});
      if (prior.bytes.empty()) {
        return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
      }
      if (index != prior.index + 1U) {
        return std::unexpected(Corruption("staged B-tree cells are not adjacent and ordered"));
      }
    }
    staged_cells_[staged_count_] = StagedCell{
        .index = index,
        .bytes = staged,
    };
    ++staged_count_;
    return {};
  }

  auto offset = AllocateSpace(cell.size(), workspace);
  if (!offset.has_value()) {
    return std::unexpected(std::move(offset.error()));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  if (left_child.has_value()) {
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(*offset + 4U), cell.data() + 4U,
                 cell.size() - 4U);
    Store32(*bytes, *offset, left_child->value());
  } else {
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(*offset), cell.data(), cell.size());
  }
  const std::size_t pointer = cell_pointer_offset_ + index * 2U;
  std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(pointer + 2U),
               bytes->data() + static_cast<std::ptrdiff_t>(pointer), 2U * (cell_count_ - index));
  Store16(*bytes, BigEndian16{.offset = pointer, .value = *offset});
  ++cell_count_;
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 3U, .value = cell_count_});
  free_bytes_ -= cell.size() + 2U;
  return {};
}

void MutableBtreePage::ClearStagedCells() noexcept {
  staged_cells_.fill(std::nullopt);
  staged_count_ = 0;
}

bool MutableBtreePage::is_leaf() const noexcept {
  return type_ == BtreePageType::kLeafIndex || type_ == BtreePageType::kLeafTable;
}

bool MutableBtreePage::is_table() const noexcept {
  return type_ == BtreePageType::kInteriorTable || type_ == BtreePageType::kLeafTable;
}

std::optional<StagedCell> MutableBtreePage::staged_cell(std::size_t slot) const noexcept {
  return slot < staged_cells_.size() ? staged_cells_[slot] : std::nullopt;
}

MutableBtreePage::MutableBtreePage(MutationPageOwner& owner, std::size_t owner_slot,
                                   BtreePageGeometry geometry, PageNumber page_number,
                                   BtreePageType type, Metadata metadata) noexcept
    : owner_(&owner),
      owner_slot_(owner_slot),
      geometry_(geometry),
      page_number_(page_number),
      type_(type),
      header_offset_(metadata.header_offset),
      cell_pointer_offset_(metadata.cell_pointer_offset),
      cell_count_(metadata.cell_count),
      free_bytes_(metadata.free_bytes) {}

MutableBtreePage::MutableBtreePage(MutableBtreePage&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      owner_slot_(other.owner_slot_),
      geometry_(other.geometry_),
      page_number_(other.page_number_),
      type_(other.type_),
      header_offset_(other.header_offset_),
      cell_pointer_offset_(other.cell_pointer_offset_),
      cell_count_(std::exchange(other.cell_count_, 0U)),
      free_bytes_(std::exchange(other.free_bytes_, 0U)),
      staged_cells_(other.staged_cells_),
      staged_count_(std::exchange(other.staged_count_, 0U)) {
  other.staged_cells_.fill(std::nullopt);
}

Result<MutableByteView> MutableBtreePage::Bytes() {
  if (owner_ == nullptr) {
    return std::unexpected(Misuse("mutable B-tree page is moved from"));
  }
  return owner_->MutableBytes(owner_slot_);
}

Result<ByteView> MutableBtreePage::Bytes() const {
  if (owner_ == nullptr) {
    return std::unexpected(Misuse("mutable B-tree page is moved from"));
  }
  auto frame = owner_->Frame(owner_slot_);
  if (!frame.has_value()) {
    return std::unexpected(std::move(frame.error()));
  }
  return frame->get().bytes();
}

Status MutableBtreePage::ValidateCellImage(ByteView cell,
                                           std::optional<PageNumber> left_child) const {
  if (cell.size() < 4U || cell.size() >= geometry_.usable_size().value() - 8U) {
    return std::unexpected(Corruption("B-tree cell image has an invalid size"));
  }
  if (is_leaf() == left_child.has_value()) {
    return std::unexpected(Misuse("B-tree cell child pointer does not match the page kind"));
  }
  if (left_child.has_value() && !IsValidPageReference(*left_child, geometry_)) {
    return std::unexpected(Corruption("B-tree cell has an invalid left child"));
  }

  std::size_t cursor = is_leaf() ? 0U : 4U;
  if (type_ == BtreePageType::kInteriorTable) {
    const auto rowid = DecodeSqliteVarint(cell.subspan(cursor));
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("table interior cell rowid is truncated"));
    }
    cursor += rowid->bytes_consumed.value();
    if (cursor != cell.size()) {
      return std::unexpected(Corruption("table interior cell size is inconsistent"));
    }
    return {};
  }

  const auto payload = DecodeSqliteVarint(cell.subspan(cursor));
  if (!payload.has_value() || payload->value > kMaximumPayloadSize) {
    return std::unexpected(Corruption("B-tree cell payload size is invalid"));
  }
  cursor += payload->bytes_consumed.value();
  if (type_ == BtreePageType::kLeafTable) {
    const auto rowid = DecodeSqliteVarint(cell.subspan(cursor));
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("table leaf cell rowid is truncated"));
    }
    cursor += rowid->bytes_consumed.value();
  }
  const auto payload_size = static_cast<std::size_t>(payload->value);
  const std::size_t local = LocalPayloadSize(geometry_, type_, payload_size);
  const bool overflow = local < payload_size;
  if (cursor > cell.size() || local > cell.size() - cursor ||
      (overflow ? 4U : 0U) > cell.size() - cursor - local) {
    return std::unexpected(Corruption("B-tree cell body is truncated"));
  }
  cursor += local;
  if (overflow) {
    const PageNumber first_overflow{
        LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
            cell.data() + static_cast<std::ptrdiff_t>(cursor), sizeof(std::uint32_t)})};
    if (!IsValidPageReference(first_overflow, geometry_)) {
      return std::unexpected(Corruption("B-tree cell has an invalid overflow page"));
    }
    cursor += 4U;
  }
  if (is_leaf()) {
    cursor = std::max<std::size_t>(cursor, 4U);
  }
  if (cursor != cell.size()) {
    return std::unexpected(Corruption("B-tree cell size is inconsistent"));
  }
  return {};
}

Result<std::optional<std::size_t>> MutableBtreePage::FindFreeblock(std::size_t size) {
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  std::size_t pointer = header_offset_ + 1U;
  std::size_t block = Load16(*bytes, pointer);
  const std::size_t maximum = geometry_.usable_size().value() - size;
  while (block <= maximum && block != 0U) {
    if (block > geometry_.usable_size().value() - 4U) {
      return std::unexpected(Corruption("B-tree freeblock header exceeds the usable region"));
    }
    const std::size_t block_size = Load16(*bytes, block + 2U);
    if (block_size >= size) {
      const std::size_t excess = block_size - size;
      if (excess < 4U) {
        const std::size_t fragments = std::to_integer<std::uint8_t>((*bytes)[header_offset_ + 7U]);
        if (fragments > 57U) {
          return std::optional<std::size_t>{};
        }
        Store16(*bytes, BigEndian16{
                            .offset = pointer,
                            .value = Load16(*bytes, block),
                        });
        (*bytes)[header_offset_ + 7U] = static_cast<std::byte>(fragments + excess);
        return std::optional<std::size_t>{block};
      }
      if (excess + block > maximum) {
        return std::unexpected(Corruption("B-tree freeblock exceeds the usable region"));
      }
      Store16(*bytes, BigEndian16{.offset = block + 2U, .value = excess});
      return std::optional<std::size_t>{block + excess};
    }
    pointer = block;
    const std::size_t next = Load16(*bytes, block);
    if (next <= pointer) {
      if (next != 0U) {
        return std::unexpected(Corruption("B-tree freeblock chain is unordered"));
      }
      return std::optional<std::size_t>{};
    }
    block = next;
  }
  if (block > maximum + size - 4U) {
    return std::unexpected(Corruption("B-tree freeblock chain exceeds the usable region"));
  }
  return std::optional<std::size_t>{};
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
