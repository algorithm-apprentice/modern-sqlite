#include "writer_internal.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
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

[[nodiscard]] Error Constraint(std::string_view message) noexcept {
  return MakeError(ErrorCode::kConstraint, message);
}

[[nodiscard]] Error NotFound(std::string_view message) noexcept {
  return MakeError(ErrorCode::kNotFound, message);
}

[[nodiscard]] Error Busy(std::string_view message) noexcept {
  return MakeError(ErrorCode::kBusy, message);
}

[[nodiscard]] Error Internal(std::string_view message) noexcept {
  return MakeError(ErrorCode::kInternal, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) noexcept {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] std::uint16_t Load16(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint16_t>(std::span<const std::byte, sizeof(std::uint16_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint16_t)});
}

[[nodiscard]] std::uint32_t Load32(ByteView bytes, std::size_t offset) noexcept {
  return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
      bytes.data() + static_cast<std::ptrdiff_t>(offset), sizeof(std::uint32_t)});
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

[[nodiscard]] Result<std::size_t> IndexLeafUnpaddedCellSize(ByteView cell,
                                                            BtreePageGeometry geometry) {
  const auto payload = DecodeSqliteVarint(cell);
  if (!payload.has_value() || payload->value > kMaximumPayloadSize) {
    return std::unexpected(Corruption("index leaf payload header is invalid"));
  }
  const std::size_t local = LocalPayloadSize(geometry, BtreePageType::kLeafIndex, payload->value);
  const std::size_t size = payload->bytes_consumed.value() + local +
                           (local < payload->value ? sizeof(std::uint32_t) : 0U);
  if (size > cell.size() || std::max<std::size_t>(4U, size) != cell.size()) {
    return std::unexpected(Corruption("index leaf cell size is inconsistent"));
  }
  return size;
}

[[nodiscard]] bool IsValidPageReference(PageNumber page_number,
                                        BtreePageGeometry geometry) noexcept {
  return page_number.value() != 0U && page_number.value() <= kMaximumPageNumber &&
         page_number != geometry.locking_page();
}

[[nodiscard]] Result<std::size_t> OwnRead(MutationPageOwner& owner, PageNumber page_number) {
  if (owner.Find(page_number).has_value()) {
    return owner.Borrow(page_number);
  }
  return owner.AcquireRead(page_number);
}

[[nodiscard]] Result<std::size_t> OwnWrite(MutationPageOwner& owner, PageNumber page_number) {
  if (owner.Find(page_number).has_value()) {
    auto slot = owner.Borrow(page_number);
    if (!slot.has_value()) {
      return std::unexpected(std::move(slot.error()));
    }
    auto promoted = owner.Promote(*slot);
    if (!promoted.has_value()) {
      return std::unexpected(std::move(promoted.error()));
    }
    return *slot;
  }
  return owner.AcquireWrite(page_number);
}

class TemporaryOwnedPage final {
 public:
  TemporaryOwnedPage(MutationPageOwner& owner, PageNumber page_number, bool temporary) noexcept
      : owner_(&owner), page_number_(page_number), temporary_(temporary) {}

  TemporaryOwnedPage(const TemporaryOwnedPage&) = delete;
  TemporaryOwnedPage& operator=(const TemporaryOwnedPage&) = delete;
  void Keep() noexcept { temporary_ = false; }
  ~TemporaryOwnedPage() {
    if (!temporary_) {
      return;
    }
    const std::optional<std::size_t> slot = owner_->Find(page_number_);
    if (slot.has_value()) {
      owner_->Release(*slot);
    }
  }

 private:
  MutationPageOwner* owner_;
  PageNumber page_number_;
  bool temporary_;
};

[[nodiscard]] Result<std::optional<PageNumber>> WriteOverflowPayload(MutationPageOwner& owner,
                                                                     BtreePageGeometry geometry,
                                                                     MutableByteView first_pointer,
                                                                     ByteView payload) {
  if (payload.empty()) {
    return std::optional<PageNumber>{};
  }
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Result<std::optional<PageNumber>> {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };

  std::optional<PageNumber> first_page;
  std::optional<std::size_t> previous_slot;
  MutableByteView previous_pointer = first_pointer;
  ByteView remaining = payload;
  while (!remaining.empty()) {
    auto allocated = AllocateBtreePage(owner, geometry);
    if (!allocated.has_value()) {
      if (previous_slot.has_value()) {
        owner.Release(*previous_slot);
      }
      return fail(std::move(allocated.error()));
    }
    auto bytes = owner.MutableBytes(allocated->owner_slot);
    if (!bytes.has_value()) {
      owner.Release(allocated->owner_slot);
      if (previous_slot.has_value()) {
        owner.Release(*previous_slot);
      }
      return fail(std::move(bytes.error()));
    }
    const std::size_t count =
        std::min(geometry.overflow_payload_capacity().value(), remaining.size());
    owner.NoteMutation();
    Store32(*bytes, 0U, 0U);
    std::ranges::copy(remaining.first(count),
                      bytes->begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
    Store32(previous_pointer, 0U, allocated->page_number.value());
    if (!first_page.has_value()) {
      first_page = allocated->page_number;
    }
    if (previous_slot.has_value()) {
      owner.Release(*previous_slot);
    }
    previous_slot = allocated->owner_slot;
    previous_pointer = bytes->first(sizeof(std::uint32_t));
    remaining = remaining.subspan(count);
  }
  if (previous_slot.has_value()) {
    owner.Release(*previous_slot);
  }
  return first_page;
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
  page.staged_cells.fill(std::nullopt);
  page.staged_count = 0U;
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
  page.staged_cells.fill(std::nullopt);
  page.staged_count = 0U;
  ++size_;
  return *slot;
}

Result<std::size_t> MutationPageOwner::AllocatePage() {
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  auto slot = EmptySlot();
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  auto pin = pager_->AllocatePage();
  if (!pin.has_value()) {
    return std::unexpected(std::move(pin.error()));
  }
  OwnedPage& page = pages_[*slot];
  page.page_number = pin->frame().page_number();
  page.pin.emplace<WritePagePin>(std::move(*pin));
  page.staged_cells.fill(std::nullopt);
  page.staged_count = 0U;
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
  page.staged_cells.fill(std::nullopt);
  page.staged_count = 0U;
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

Status MutationPageOwner::RequireSolePin(std::size_t slot) const {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (slot >= pages_.size()) {
    return std::unexpected(Misuse("mutation page slot is out of range"));
  }
  const OwnedPage& page = pages_[slot];
  if (const auto* read = std::get_if<ReadPagePin>(&page.pin); read != nullptr) {
    return read->sole()
               ? Status{}
               : Status{std::unexpected(Busy("B-tree mutation requires the sole page pin"))};
  }
  if (const auto* write = std::get_if<WritePagePin>(&page.pin); write != nullptr) {
    return write->sole()
               ? Status{}
               : Status{std::unexpected(Busy("B-tree mutation requires the sole page pin"))};
  }
  return std::unexpected(Misuse("mutation page slot is empty"));
}

Status MutationPageOwner::PermutePageNumbers(std::span<const MutationPageRekey> pages) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (pages.size() > 5U) {
    return std::unexpected(Misuse("mutation page permutation supports at most five pages"));
  }

  std::array<PageNumberRekey, 5> pager_rekeys{};
  bool changes_page_number = false;
  for (std::size_t index = 0U; index < pages.size(); ++index) {
    const MutationPageRekey& rekey = pages[index];
    if (rekey.owner_slot >= pages_.size()) {
      return std::unexpected(Misuse("mutation page permutation slot is out of range"));
    }
    auto* pin = std::get_if<WritePagePin>(&pages_[rekey.owner_slot].pin);
    if (pin == nullptr) {
      return std::unexpected(Misuse("mutation page permutation requires writable owned pages"));
    }
    pager_rekeys[index] = PageNumberRekey{
        .pin = pin,
        .final_page = rekey.final_page,
    };
    changes_page_number = changes_page_number || pin->frame().page_number() != rekey.final_page;
  }

  auto permuted = pager_->PermutePageNumbers(std::span{pager_rekeys}.first(pages.size()));
  if (!permuted.has_value()) {
    return permuted;
  }
  for (const MutationPageRekey& rekey : pages) {
    const auto* pin = std::get_if<WritePagePin>(&pages_[rekey.owner_slot].pin);
    if (pin == nullptr) {
      return std::unexpected(Internal("mutation page permutation lost a writable pin"));
    }
    pages_[rekey.owner_slot].page_number = pin->frame().page_number();
  }
  if (changes_page_number) {
    NoteMutation();
  }
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

void MutationPageOwner::MarkRollbackRequiredAfter(ErrorCode code,
                                                  std::uint64_t checkpoint) noexcept {
  if (mutation_sequence_ != checkpoint) {
    pager_->ReportWriteCoordinatorFailure(code);
  }
}

Result<std::size_t> MutationPageOwner::EmptySlot() const {
  for (std::size_t slot = 0; slot < pages_.size(); ++slot) {
    if (std::holds_alternative<std::monostate>(pages_[slot].pin)) {
      return slot;
    }
  }
  return std::unexpected(TooLarge("mutation operation exceeded its bounded page ownership"));
}

Result<AllocatedBtreePage> AllocateBtreePage(MutationPageOwner& owner, BtreePageGeometry geometry) {
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Result<AllocatedBtreePage> {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  auto page_one_slot = OwnWrite(owner, PageNumber{1});
  if (!page_one_slot.has_value()) {
    return std::unexpected(std::move(page_one_slot.error()));
  }
  auto page_one = owner.MutableBytes(*page_one_slot);
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }

  const std::uint32_t page_count = owner.pager().page_count();
  const std::uint32_t free_count = Load32(*page_one, 36U);
  if (free_count >= page_count) {
    return std::unexpected(Corruption("database freelist count exceeds its page count"));
  }

  if (free_count != 0U) {
    owner.NoteMutation();
    Store32(*page_one, 36U, free_count - 1U);
    const PageNumber trunk_page{Load32(*page_one, 32U)};
    if (trunk_page.value() < 2U || trunk_page.value() > page_count ||
        trunk_page == geometry.locking_page()) {
      return fail(Corruption("database freelist trunk reference is invalid"));
    }
    const bool trunk_already_owned = owner.Find(trunk_page).has_value();
    auto trunk_slot = OwnRead(owner, trunk_page);
    if (!trunk_slot.has_value()) {
      return fail(std::move(trunk_slot.error()));
    }
    TemporaryOwnedPage trunk_lease{owner, trunk_page, !trunk_already_owned};
    auto trunk_frame = owner.Frame(*trunk_slot);
    if (!trunk_frame.has_value()) {
      return fail(std::move(trunk_frame.error()));
    }
    auto trunk = FreelistTrunkView::Parse(trunk_frame->get().bytes(), geometry);
    if (!trunk.has_value()) {
      return fail(std::move(trunk.error()));
    }

    if (trunk->leaf_count() == 0U) {
      auto promoted = owner.Promote(*trunk_slot);
      if (!promoted.has_value()) {
        return fail(std::move(promoted.error()));
      }
      page_one = owner.MutableBytes(*page_one_slot);
      if (!page_one.has_value()) {
        return fail(std::move(page_one.error()));
      }
      Store32(*page_one, 32U, trunk->next_trunk().has_value() ? trunk->next_trunk()->value() : 0U);
      trunk_lease.Keep();
      return AllocatedBtreePage{
          .page_number = trunk_page,
          .owner_slot = *trunk_slot,
          .reused_freelist = true,
      };
    }

    auto allocated_page = trunk->leaf_page(0U);
    if (!allocated_page.has_value()) {
      return fail(std::move(allocated_page.error()));
    }
    if (allocated_page->value() < 2U || allocated_page->value() > page_count ||
        *allocated_page == geometry.locking_page() || *allocated_page == trunk_page) {
      return fail(Corruption("database freelist leaf reference is invalid"));
    }
    auto last_page = trunk->leaf_page(trunk->leaf_count() - 1U);
    if (!last_page.has_value()) {
      return fail(std::move(last_page.error()));
    }
    auto promoted = owner.Promote(*trunk_slot);
    if (!promoted.has_value()) {
      return fail(std::move(promoted.error()));
    }
    auto trunk_bytes = owner.MutableBytes(*trunk_slot);
    if (!trunk_bytes.has_value()) {
      return fail(std::move(trunk_bytes.error()));
    }
    if (trunk->leaf_count() > 1U) {
      Store32(*trunk_bytes, 8U, last_page->value());
    }
    Store32(*trunk_bytes, 4U, static_cast<std::uint32_t>(trunk->leaf_count() - 1U));

    auto allocated_slot = owner.AcquireWrite(*allocated_page);
    if (!allocated_slot.has_value()) {
      return fail(std::move(allocated_slot.error()));
    }
    return AllocatedBtreePage{
        .page_number = *allocated_page,
        .owner_slot = *allocated_slot,
        .reused_freelist = true,
    };
  }

  if (page_count >= kMaximumPageNumber) {
    return std::unexpected(TooLarge("database page-number space is exhausted"));
  }
  auto allocated_slot = owner.AllocatePage();
  if (!allocated_slot.has_value()) {
    return std::unexpected(std::move(allocated_slot.error()));
  }
  owner.NoteMutation();
  auto allocated_frame = owner.Frame(*allocated_slot);
  if (!allocated_frame.has_value()) {
    return fail(std::move(allocated_frame.error()));
  }
  page_one = owner.MutableBytes(*page_one_slot);
  if (!page_one.has_value()) {
    return fail(std::move(page_one.error()));
  }
  Store32(*page_one, 28U, owner.pager().page_count());
  return AllocatedBtreePage{
      .page_number = allocated_frame->get().page_number(),
      .owner_slot = *allocated_slot,
      .reused_freelist = false,
  };
}

Status FreeBtreePage(MutationPageOwner& owner, BtreePageGeometry geometry, PageNumber page_number) {
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Status {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  const std::uint32_t page_count = owner.pager().page_count();
  if (page_number.value() < 2U || page_number.value() > page_count ||
      page_number == geometry.locking_page()) {
    return std::unexpected(Corruption("attempted to free an invalid B-tree page"));
  }

  auto page_one_slot = OwnWrite(owner, PageNumber{1});
  if (!page_one_slot.has_value()) {
    return std::unexpected(std::move(page_one_slot.error()));
  }
  auto page_one = owner.MutableBytes(*page_one_slot);
  if (!page_one.has_value()) {
    return std::unexpected(std::move(page_one.error()));
  }
  const std::uint32_t free_count = Load32(*page_one, 36U);
  if (free_count == (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(Corruption("database freelist count overflows"));
  }
  owner.NoteMutation();
  Store32(*page_one, 36U, free_count + 1U);

  PageNumber first_trunk;
  std::size_t leaf_count = 0U;
  std::optional<std::size_t> trunk_slot;
  if (free_count != 0U) {
    first_trunk = PageNumber{Load32(*page_one, 32U)};
    if (first_trunk.value() < 2U || first_trunk.value() > page_count ||
        first_trunk == geometry.locking_page() || first_trunk == page_number) {
      return fail(Corruption("database freelist trunk reference is invalid"));
    }
    const bool trunk_already_owned = owner.Find(first_trunk).has_value();
    auto owned_trunk = OwnRead(owner, first_trunk);
    if (!owned_trunk.has_value()) {
      return fail(std::move(owned_trunk.error()));
    }
    const TemporaryOwnedPage trunk_lease{owner, first_trunk, !trunk_already_owned};
    trunk_slot = *owned_trunk;
    auto trunk_frame = owner.Frame(*trunk_slot);
    if (!trunk_frame.has_value()) {
      return fail(std::move(trunk_frame.error()));
    }
    auto trunk = FreelistTrunkView::Parse(trunk_frame->get().bytes(), geometry);
    if (!trunk.has_value()) {
      return fail(std::move(trunk.error()));
    }
    leaf_count = trunk->leaf_count();
    for (std::size_t index = 0U; index < leaf_count; ++index) {
      auto leaf = trunk->leaf_page(index);
      if (!leaf.has_value()) {
        return fail(std::move(leaf.error()));
      }
      if (*leaf == page_number) {
        return fail(Corruption("database page is already on the freelist"));
      }
    }
    const std::size_t compatible_capacity = geometry.usable_size().value() / 4U - 8U;
    if (leaf_count < compatible_capacity) {
      auto promoted = owner.Promote(*trunk_slot);
      if (!promoted.has_value()) {
        return fail(std::move(promoted.error()));
      }
      auto trunk_bytes = owner.MutableBytes(*trunk_slot);
      if (!trunk_bytes.has_value()) {
        return fail(std::move(trunk_bytes.error()));
      }
      Store32(*trunk_bytes, 4U, static_cast<std::uint32_t>(leaf_count + 1U));
      Store32(*trunk_bytes, 8U + leaf_count * 4U, page_number.value());
      auto marked = owner.pager().MarkPageContentRequired(page_number);
      if (!marked.has_value()) {
        return fail(std::move(marked.error()));
      }
      return {};
    }
  }

  auto freed_slot = OwnWrite(owner, page_number);
  if (!freed_slot.has_value()) {
    return fail(std::move(freed_slot.error()));
  }
  auto freed = owner.MutableBytes(*freed_slot);
  if (!freed.has_value()) {
    return fail(std::move(freed.error()));
  }
  Store32(*freed, 0U, first_trunk.value());
  Store32(*freed, 4U, 0U);
  page_one = owner.MutableBytes(*page_one_slot);
  if (!page_one.has_value()) {
    return fail(std::move(page_one.error()));
  }
  Store32(*page_one, 32U, page_number.value());
  return {};
}

Result<PageNumber> CreateBtreeRoot(MutationPageOwner& owner, BtreePageGeometry geometry,
                                   bool table) {
  const std::uint64_t checkpoint = owner.operation_checkpoint();
  const auto fail = [&owner, checkpoint](Error error) -> Result<PageNumber> {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  auto allocated = AllocateBtreePage(owner, geometry);
  if (!allocated.has_value()) {
    return fail(std::move(allocated.error()));
  }
  auto root =
      MutableBtreePage::Initialize(owner, allocated->owner_slot, geometry,
                                   table ? BtreePageType::kLeafTable : BtreePageType::kLeafIndex);
  if (!root.has_value()) {
    owner.Release(allocated->owner_slot);
    return fail(std::move(root.error()));
  }
  return allocated->page_number;
}

Result<FormattedCell> FillTableLeafCell(MutationPageOwner& owner, BtreePageGeometry geometry,
                                        BtreeWriteWorkspace& workspace, std::int64_t rowid,
                                        ByteView payload) {
  if (payload.size() > kMaximumPayloadSize) {
    return std::unexpected(TooLarge("table payload exceeds SQLite's limit"));
  }
  std::array<std::byte, 9> payload_size_varint{};
  auto payload_size = EncodeSqliteVarint(payload.size(), MutableByteView{payload_size_varint});
  if (!payload_size.has_value()) {
    return std::unexpected(Misuse("table payload size cannot be encoded"));
  }
  std::array<std::byte, 9> rowid_varint{};
  auto encoded_rowid =
      EncodeSqliteVarint(std::bit_cast<std::uint64_t>(rowid), MutableByteView{rowid_varint});
  if (!encoded_rowid.has_value()) {
    return std::unexpected(Misuse("table rowid cannot be encoded"));
  }

  const std::size_t local = LocalPayloadSize(geometry, BtreePageType::kLeafTable, payload.size());
  const bool overflow = local < payload.size();
  const std::size_t unpadded_size = payload_size->value() + encoded_rowid->value() + local +
                                    (overflow ? sizeof(std::uint32_t) : 0U);
  const std::size_t encoded_size = std::max<std::size_t>(4U, unpadded_size);
  MutableByteView output = workspace.cell_scratch();
  if (encoded_size > output.size()) {
    return std::unexpected(Corruption("formatted table cell exceeds SQLite's scratch page"));
  }
  output = output.first(encoded_size);
  std::ranges::fill(output, std::byte{0});
  std::size_t offset = 0U;
  std::ranges::copy(std::span{payload_size_varint}.first(payload_size->value()), output.begin());
  offset += payload_size->value();
  std::ranges::copy(std::span{rowid_varint}.first(encoded_rowid->value()),
                    output.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += encoded_rowid->value();
  std::ranges::copy(payload.first(local), output.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += local;

  std::optional<PageNumber> first_overflow;
  if (overflow) {
    auto written = WriteOverflowPayload(
        owner, geometry, output.subspan(offset, sizeof(std::uint32_t)), payload.subspan(local));
    if (!written.has_value()) {
      return std::unexpected(std::move(written.error()));
    }
    first_overflow = *written;
  }
  return FormattedCell{
      .bytes = ByteView{output},
      .payload_size = payload.size(),
      .first_overflow_page = first_overflow,
  };
}

Result<FormattedCell> FillIndexCell(MutationPageOwner& owner, BtreePageGeometry geometry,
                                    BtreeWriteWorkspace& workspace, ByteView payload,
                                    BtreePageType type, std::optional<PageNumber> left_child) {
  const bool interior = type == BtreePageType::kInteriorIndex;
  if ((type != BtreePageType::kLeafIndex && !interior) || interior != left_child.has_value()) {
    return std::unexpected(Misuse("index cell kind and child metadata do not match"));
  }
  if (left_child.has_value() && !IsValidPageReference(*left_child, geometry)) {
    return std::unexpected(Corruption("index cell has an invalid left child"));
  }
  if (payload.size() > kMaximumPayloadSize) {
    return std::unexpected(TooLarge("index payload exceeds SQLite's limit"));
  }
  std::array<std::byte, 9> payload_size_varint{};
  auto payload_size = EncodeSqliteVarint(payload.size(), MutableByteView{payload_size_varint});
  if (!payload_size.has_value()) {
    return std::unexpected(Misuse("index payload size cannot be encoded"));
  }

  const std::size_t local = LocalPayloadSize(geometry, type, payload.size());
  const bool overflow = local < payload.size();
  const std::size_t prefix = interior ? sizeof(std::uint32_t) : 0U;
  const std::size_t unpadded_size =
      prefix + payload_size->value() + local + (overflow ? sizeof(std::uint32_t) : 0U);
  const std::size_t encoded_size =
      interior ? unpadded_size : std::max<std::size_t>(4U, unpadded_size);
  MutableByteView output =
      interior ? workspace.cell_scratch_with_prefix() : workspace.cell_scratch();
  if (encoded_size > output.size()) {
    return std::unexpected(Corruption("formatted index cell exceeds SQLite's scratch page"));
  }
  output = output.first(encoded_size);
  std::ranges::fill(output, std::byte{0});
  std::size_t offset = 0U;
  if (left_child.has_value()) {
    Store32(output, 0U, left_child->value());
    offset += sizeof(std::uint32_t);
  }
  std::ranges::copy(std::span{payload_size_varint}.first(payload_size->value()),
                    output.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += payload_size->value();
  std::ranges::copy(payload.first(local), output.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += local;

  std::optional<PageNumber> first_overflow;
  if (overflow) {
    auto written = WriteOverflowPayload(
        owner, geometry, output.subspan(offset, sizeof(std::uint32_t)), payload.subspan(local));
    if (!written.has_value()) {
      return std::unexpected(std::move(written.error()));
    }
    first_overflow = *written;
  }
  return FormattedCell{
      .bytes = ByteView{output},
      .payload_size = payload.size(),
      .first_overflow_page = first_overflow,
  };
}

Status ClearCellOverflow(MutationPageOwner& owner, BtreePageGeometry geometry,
                         const BtreeCellView& cell) {
  if (!cell.first_overflow_page().has_value()) {
    return cell.local_payload().size() == cell.payload_size().value()
               ? Status{}
               : Status{std::unexpected(Corruption("B-tree cell is missing its overflow chain"))};
  }
  if (cell.local_payload().size() >= cell.payload_size().value()) {
    return std::unexpected(Corruption("B-tree cell has an unexpected overflow reference"));
  }
  const std::size_t remaining = cell.payload_size().value() - cell.local_payload().size();
  const std::size_t capacity = geometry.overflow_payload_capacity().value();
  const std::size_t expected_pages = 1U + (remaining - 1U) / capacity;
  if (expected_pages > owner.pager().page_count()) {
    return std::unexpected(Corruption("B-tree overflow chain cannot fit in the database"));
  }

  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Status {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  PageNumber current = *cell.first_overflow_page();
  for (std::size_t index = 0U; index < expected_pages; ++index) {
    if (owner.Find(current).has_value()) {
      return fail(Corruption("overflow page aliases another mutation page"));
    }
    auto slot = owner.AcquireRead(current);
    if (!slot.has_value()) {
      return fail(std::move(slot.error()));
    }
    auto sole = owner.RequireSolePin(*slot);
    if (!sole.has_value()) {
      owner.Release(*slot);
      return fail(std::move(sole.error()));
    }
    auto frame = owner.Frame(*slot);
    if (!frame.has_value()) {
      owner.Release(*slot);
      return fail(std::move(frame.error()));
    }
    std::optional<PageNumber> next;
    if (index + 1U < expected_pages) {
      auto overflow = OverflowPageView::Parse(frame->get().bytes(), geometry);
      if (!overflow.has_value()) {
        owner.Release(*slot);
        return fail(std::move(overflow.error()));
      }
      next = overflow->next_page();
      if (!next.has_value()) {
        owner.Release(*slot);
        return fail(Corruption("B-tree overflow chain ends prematurely"));
      }
    }

    auto freed = FreeBtreePage(owner, geometry, current);
    if (!freed.has_value()) {
      owner.Release(*slot);
      return fail(std::move(freed.error()));
    }
    owner.Release(*slot);
    if (next.has_value()) {
      current = *next;
    }
  }
  return {};
}

namespace {

[[nodiscard]] Result<std::uint64_t> ClearBtreePage(MutationPageOwner& owner,
                                                   BtreePageGeometry geometry,
                                                   PageNumber page_number, std::size_t depth,
                                                   bool free_page) {
  if (depth >= kMaximumBtreeDepth || page_number.value() == 0U ||
      page_number.value() > owner.pager().page_count() || page_number == geometry.locking_page() ||
      (free_page && page_number == PageNumber{1})) {
    return std::unexpected(Corruption("cleared B-tree page or depth is invalid"));
  }
  const bool already_owned = owner.Find(page_number).has_value();
  if (depth != 0U && already_owned) {
    return std::unexpected(Corruption("cleared B-tree child aliases its ancestor path"));
  }
  auto slot = already_owned ? owner.Borrow(page_number) : owner.AcquireRead(page_number);
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  const TemporaryOwnedPage lease{owner, page_number, !already_owned};
  auto sole = owner.RequireSolePin(*slot);
  if (!sole.has_value()) {
    return std::unexpected(std::move(sole.error()));
  }
  auto frame = owner.Frame(*slot);
  if (!frame.has_value()) {
    return std::unexpected(std::move(frame.error()));
  }
  auto page = BtreePageView::Parse(frame->get().bytes(), page_number, geometry);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }

  std::uint64_t cleared =
      page->is_table() && !page->is_leaf() ? 0U : static_cast<std::uint64_t>(page->cell_count());
  for (std::size_t index = 0U; index < page->cell_count(); ++index) {
    auto cell = page->cell(index);
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    if (!page->is_leaf()) {
      if (!cell->left_child().has_value()) {
        return std::unexpected(Corruption("cleared interior B-tree cell has no child"));
      }
      auto child = ClearBtreePage(owner, geometry, *cell->left_child(), depth + 1U, true);
      if (!child.has_value()) {
        return std::unexpected(std::move(child.error()));
      }
      if (*child > (std::numeric_limits<std::uint64_t>::max)() - cleared) {
        return std::unexpected(Corruption("cleared B-tree entry count overflows"));
      }
      cleared += *child;
    }
    auto overflow = ClearCellOverflow(owner, geometry, *cell);
    if (!overflow.has_value()) {
      return std::unexpected(std::move(overflow.error()));
    }
  }
  if (!page->is_leaf()) {
    if (!page->rightmost_child().has_value()) {
      return std::unexpected(Corruption("cleared interior B-tree page has no rightmost child"));
    }
    auto child = ClearBtreePage(owner, geometry, *page->rightmost_child(), depth + 1U, true);
    if (!child.has_value()) {
      return std::unexpected(std::move(child.error()));
    }
    if (*child > (std::numeric_limits<std::uint64_t>::max)() - cleared) {
      return std::unexpected(Corruption("cleared B-tree entry count overflows"));
    }
    cleared += *child;
  }

  if (free_page) {
    auto freed = FreeBtreePage(owner, geometry, page_number);
    if (!freed.has_value()) {
      return std::unexpected(std::move(freed.error()));
    }
  } else {
    auto promoted = owner.Promote(*slot);
    if (!promoted.has_value()) {
      return std::unexpected(std::move(promoted.error()));
    }
    auto root = MutableBtreePage::Open(owner, *slot, geometry);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    auto zeroed =
        root->Zero(page->is_table() ? BtreePageType::kLeafTable : BtreePageType::kLeafIndex);
    if (!zeroed.has_value()) {
      return std::unexpected(std::move(zeroed.error()));
    }
  }
  return cleared;
}

}  // namespace

Result<std::uint64_t> ClearBtree(MutationPageOwner& owner, BtreePageGeometry geometry,
                                 PageNumber root_page) {
  const std::uint64_t checkpoint = owner.operation_checkpoint();
  auto cleared = ClearBtreePage(owner, geometry, root_page, 0U, false);
  if (!cleared.has_value()) {
    owner.MarkRollbackRequiredAfter(cleared.error().code(), checkpoint);
  }
  return cleared;
}

Status DropBtree(MutationPageOwner& owner, BtreePageGeometry geometry, PageNumber root_page) {
  if (root_page.value() < 2U) {
    return std::unexpected(Misuse("page one B-tree root cannot be dropped"));
  }
  const std::uint64_t checkpoint = owner.operation_checkpoint();
  auto cleared = ClearBtreePage(owner, geometry, root_page, 0U, true);
  if (!cleared.has_value()) {
    owner.MarkRollbackRequiredAfter(cleared.error().code(), checkpoint);
    return std::unexpected(std::move(cleared.error()));
  }
  return {};
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
  owner_->NoteMutation();
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

        owner_->NoteMutation();
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
  owner_->NoteMutation();
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
  owner_->NoteMutation();
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
  owner_->NoteMutation();
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

Status MutableBtreePage::OverwritePayload(std::size_t index, ByteView payload,
                                          BtreeWriteWorkspace& workspace) {
  if (index >= cell_count_) {
    return std::unexpected(Misuse("overwritten B-tree cell index is out of range"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  auto page = BtreePageView::Parse(*bytes, page_number_, geometry_);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  auto cell_offset = page->cell_offset(index);
  auto cell = page->cell(index);
  if (!cell_offset.has_value()) {
    return std::unexpected(std::move(cell_offset.error()));
  }
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  if (payload.size() != cell->payload_size().value()) {
    return std::unexpected(Misuse("B-tree payload overwrite requires the same logical size"));
  }
  if (!payload.empty()) {
    const std::less<> before;
    const bool aliases_page = before(payload.data(), bytes->data() + bytes->size()) &&
                              before(bytes->data(), payload.data() + payload.size());
    if (aliases_page) {
      const MutableByteView scratch = workspace.cell_scratch();
      if (scratch.size() < payload.size()) {
        return std::unexpected(Misuse("aliased B-tree payload exceeds retained scratch"));
      }
      std::memmove(scratch.data(), payload.data(), payload.size());
      payload = ByteView{scratch.first(payload.size())};
    }
  }

  const std::uint64_t checkpoint = owner_->mutation_sequence();
  const auto fail = [this, checkpoint](Error error) -> Status {
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  std::size_t payload_offset = cell_offset->value();
  if (!is_leaf()) {
    payload_offset += sizeof(std::uint32_t);
  }
  const auto encoded_payload_size = DecodeSqliteVarint(bytes->subspan(payload_offset));
  if (!encoded_payload_size.has_value() || encoded_payload_size->value != payload.size()) {
    return std::unexpected(Corruption("B-tree payload size metadata is inconsistent"));
  }
  payload_offset += encoded_payload_size->bytes_consumed.value();
  if (type_ == BtreePageType::kLeafTable) {
    const auto rowid = DecodeSqliteVarint(bytes->subspan(payload_offset));
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("table leaf rowid is truncated"));
    }
    payload_offset += rowid->bytes_consumed.value();
  }

  const std::size_t local = cell->local_payload().size();
  if (payload_offset > bytes->size() || local > bytes->size() - payload_offset) {
    return std::unexpected(Corruption("B-tree local payload range is invalid"));
  }
  owner_->NoteMutation();
  if (local != 0U) {
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(payload_offset), payload.data(),
                 local);
  }

  std::size_t offset = local;
  std::optional<PageNumber> overflow_page = cell->first_overflow_page();
  while (offset < payload.size()) {
    if (!overflow_page.has_value() || owner_->Find(*overflow_page).has_value()) {
      return fail(Corruption("B-tree overflow chain is missing or aliases a mutation page"));
    }
    auto slot = owner_->AcquireRead(*overflow_page);
    if (!slot.has_value()) {
      return fail(std::move(slot.error()));
    }
    auto frame = owner_->Frame(*slot);
    if (!frame.has_value()) {
      owner_->Release(*slot);
      return fail(std::move(frame.error()));
    }
    std::optional<PageNumber> next;
    const std::size_t count =
        std::min(geometry_.overflow_payload_capacity().value(), payload.size() - offset);
    if (offset + count < payload.size()) {
      auto overflow = OverflowPageView::Parse(frame->get().bytes(), geometry_);
      if (!overflow.has_value()) {
        owner_->Release(*slot);
        return fail(std::move(overflow.error()));
      }
      next = overflow->next_page();
      if (!next.has_value()) {
        owner_->Release(*slot);
        return fail(Corruption("B-tree overflow chain ends prematurely"));
      }
    }
    auto promoted = owner_->Promote(*slot);
    if (!promoted.has_value()) {
      return fail(std::move(promoted.error()));
    }
    auto overflow_bytes = owner_->MutableBytes(*slot);
    if (!overflow_bytes.has_value()) {
      owner_->Release(*slot);
      return fail(std::move(overflow_bytes.error()));
    }
    owner_->NoteMutation();
    std::memmove(overflow_bytes->data() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)),
                 payload.data() + static_cast<std::ptrdiff_t>(offset), count);
    owner_->Release(*slot);
    offset += count;
    overflow_page = next;
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
    MutableByteView writable_staged;
    if (!staged_copy.empty()) {
      if (staged_copy.size() < cell.size()) {
        return std::unexpected(Misuse("staged B-tree cell buffer is too small"));
      }
      std::memmove(staged_copy.data(), cell.data(), cell.size());
      writable_staged = staged_copy.first(cell.size());
      staged = ByteView{writable_staged};
    }
    if (left_child.has_value()) {
      if (staged_copy.empty()) {
        return std::unexpected(
            Misuse("staged interior B-tree cell requires writable copied storage"));
      }
      Store32(staged_copy, 0U, left_child->value());
    }
    if (staged_count_ != 0U) {
      const StagedCell prior = staged_cells_[staged_count_ - 1U].value_or(
          StagedCell{.index = 0U, .bytes = {}, .writable_bytes = {}});
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
        .writable_bytes = writable_staged,
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

Status MutableBtreePage::SetRightmostChild(PageNumber child) {
  if (is_leaf()) {
    return std::unexpected(Misuse("leaf B-tree pages have no rightmost child"));
  }
  if (!IsValidPageReference(child, geometry_)) {
    return std::unexpected(Corruption("B-tree page has an invalid rightmost child"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  owner_->NoteMutation();
  Store32(*bytes, header_offset_ + 8U, child.value());
  return {};
}

Status MutableBtreePage::Rebuild(const CellArray& cells, std::size_t first, std::size_t count,
                                 BtreeWriteWorkspace& workspace) {
  if (cells.geometry_.page_size() != geometry_.page_size() ||
      cells.geometry_.usable_size() != geometry_.usable_size()) {
    return std::unexpected(Misuse("rebuilt B-tree cells use different page geometry"));
  }
  if (first > cells.size() || count > cells.size() - first) {
    return std::unexpected(Misuse("rebuilt B-tree cell range is invalid"));
  }
  const std::size_t maximum_cells = (geometry_.page_size().value() - 8U) / 6U;
  if (count > maximum_cells) {
    return std::unexpected(Corruption("rebuilt B-tree page has too many cells"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  const MutableByteView scratch = workspace.rebuild_scratch();
  if (scratch.size() != bytes->size()) {
    return std::unexpected(Misuse("B-tree rebuild scratch has the wrong size"));
  }
  std::ranges::copy(*bytes, scratch.begin());
  return RebuildFromSnapshot(cells, first, count, ByteView{scratch});
}

Status MutableBtreePage::RebuildFromSnapshot(const CellArray& cells, std::size_t first,
                                             std::size_t count, ByteView target_copy) {
  if (cells.geometry_.page_size() != geometry_.page_size() ||
      cells.geometry_.usable_size() != geometry_.usable_size()) {
    return std::unexpected(Misuse("rebuilt B-tree cells use different page geometry"));
  }
  if (first > cells.size() || count > cells.size() - first) {
    return std::unexpected(Misuse("rebuilt B-tree cell range is invalid"));
  }
  const std::size_t maximum_cells = (geometry_.page_size().value() - 8U) / 6U;
  if (count > maximum_cells) {
    return std::unexpected(Corruption("rebuilt B-tree page has too many cells"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  if (target_copy.size() != bytes->size()) {
    return std::unexpected(Misuse("B-tree rebuild snapshot has the wrong size"));
  }
  const ByteView target{*bytes};
  const std::size_t pointer_end = cell_pointer_offset_ + count * 2U;
  std::size_t total_size = 0U;
  for (std::size_t index = 0U; index < count; ++index) {
    auto resolved = cells.ResolveForTarget(first + index, target, target_copy);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    std::optional<PageNumber> left_child;
    if (!is_leaf()) {
      if (resolved->size() < sizeof(std::uint32_t)) {
        return std::unexpected(Corruption("rebuilt interior B-tree cell is truncated"));
      }
      left_child = PageNumber{Load32(*resolved, 0U)};
    }
    auto valid = ValidateCellImage(*resolved, left_child);
    if (!valid.has_value()) {
      return valid;
    }
    if (resolved->size() > geometry_.usable_size().value() - total_size) {
      return std::unexpected(Corruption("rebuilt B-tree cells exceed the usable page"));
    }
    total_size += resolved->size();
  }
  if (pointer_end > geometry_.usable_size().value() ||
      total_size > geometry_.usable_size().value() - pointer_end) {
    return std::unexpected(Corruption("rebuilt B-tree cells do not fit on the page"));
  }

  owner_->NoteMutation();
  std::size_t content = geometry_.usable_size().value();
  for (std::size_t index = 0U; index < count; ++index) {
    auto resolved = cells.ResolveForTarget(first + index, target, target_copy);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    content -= resolved->size();
    Store16(*bytes, BigEndian16{
                        .offset = cell_pointer_offset_ + index * 2U,
                        .value = content,
                    });
    std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(content), resolved->data(),
                 resolved->size());
  }
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 1U, .value = 0U});
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 3U, .value = count});
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = content});
  (*bytes)[header_offset_ + 7U] = std::byte{0};
  cell_count_ = count;
  free_bytes_ = content - pointer_end;
  ClearStagedCells();
  return {};
}

Status MutableBtreePage::BalanceQuick(MutableBtreePage& parent, MutableBtreePage& page,
                                      BtreeWriteWorkspace& workspace) {
  if (parent.owner_ == nullptr || page.owner_ == nullptr) {
    return std::unexpected(Misuse("quick balance requires live mutable B-tree pages"));
  }
  if (parent.owner_ != page.owner_ || parent.owner_slot_ == page.owner_slot_ ||
      parent.geometry_.page_size() != page.geometry_.page_size() ||
      parent.geometry_.usable_size() != page.geometry_.usable_size()) {
    return std::unexpected(Misuse("quick balance pages do not share one mutation owner"));
  }
  if (parent.page_number_ == PageNumber{1} || parent.type_ != BtreePageType::kInteriorTable ||
      page.type_ != BtreePageType::kLeafTable) {
    return std::unexpected(Misuse("quick balance requires a non-root table parent and leaf"));
  }
  if (parent.staged_count_ != 0U || page.cell_count_ == 0U || page.staged_count_ != 1U) {
    return std::unexpected(Corruption("quick balance staged-cell state is invalid"));
  }
  const StagedCell staged_cell =
      page.staged_cells_[0].value_or(StagedCell{.index = 0U, .bytes = {}, .writable_bytes = {}});
  if (staged_cell.bytes.empty() || staged_cell.index != page.cell_count_) {
    return std::unexpected(Corruption("quick balance staged-cell state is invalid"));
  }

  auto parent_bytes = parent.Bytes();
  if (!parent_bytes.has_value()) {
    return std::unexpected(std::move(parent_bytes.error()));
  }
  auto parent_view = BtreePageView::Parse(*parent_bytes, parent.page_number_, parent.geometry_);
  if (!parent_view.has_value()) {
    return std::unexpected(std::move(parent_view.error()));
  }
  if (parent_view->rightmost_child() != page.page_number_) {
    return std::unexpected(Corruption("quick balance leaf is not the parent's rightmost child"));
  }

  auto page_bytes = page.Bytes();
  if (!page_bytes.has_value()) {
    return std::unexpected(std::move(page_bytes.error()));
  }
  auto page_view = BtreePageView::Parse(*page_bytes, page.page_number_, page.geometry_);
  if (!page_view.has_value()) {
    return std::unexpected(std::move(page_view.error()));
  }
  auto last_offset = page_view->cell_offset(page.cell_count_ - 1U);
  auto last_cell = page_view->cell(page.cell_count_ - 1U);
  if (!last_offset.has_value()) {
    return std::unexpected(std::move(last_offset.error()));
  }
  if (!last_cell.has_value()) {
    return std::unexpected(std::move(last_cell.error()));
  }
  const ByteView last_bytes =
      page_bytes->subspan(last_offset->value(), last_cell->encoded_size().value());
  const ByteView staged_bytes = staged_cell.bytes;
  auto staged_valid = page.ValidateCellImage(staged_bytes, std::nullopt);
  if (!staged_valid.has_value()) {
    return staged_valid;
  }

  struct TableLeafKey {
    std::int64_t value;
    ByteView encoded;
  };
  const auto table_leaf_key = [](ByteView cell) -> Result<TableLeafKey> {
    const auto payload = DecodeSqliteVarint(cell);
    if (!payload.has_value() || payload->bytes_consumed.value() >= cell.size()) {
      return std::unexpected(Corruption("quick balance table payload header is invalid"));
    }
    const ByteView rowid_bytes = cell.subspan(payload->bytes_consumed.value());
    const auto rowid = DecodeSqliteVarint(rowid_bytes);
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("quick balance table rowid is truncated"));
    }
    return TableLeafKey{
        .value = std::bit_cast<std::int64_t>(rowid->value),
        .encoded = rowid_bytes.first(rowid->bytes_consumed.value()),
    };
  };
  auto old_key = table_leaf_key(last_bytes);
  if (!old_key.has_value()) {
    return std::unexpected(std::move(old_key.error()));
  }
  auto new_key = table_leaf_key(staged_bytes);
  if (!new_key.has_value()) {
    return std::unexpected(std::move(new_key.error()));
  }
  if (new_key->value <= old_key->value) {
    return std::unexpected(Corruption("quick balance overflow cell is not the largest table key"));
  }

  auto cells = CellArray::Create(page.geometry_);
  if (!cells.has_value()) {
    return std::unexpected(std::move(cells.error()));
  }
  auto appended = cells->AppendPage(page);
  if (!appended.has_value()) {
    return appended;
  }
  const MutableByteView divider = workspace.cell_scratch_with_prefix();
  if (divider.size() < sizeof(std::uint32_t) + old_key->encoded.size()) {
    return std::unexpected(Misuse("quick balance divider scratch is too small"));
  }

  MutationPageOwner& owner = *page.owner_;
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Status {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  auto allocated = AllocateBtreePage(owner, page.geometry_);
  if (!allocated.has_value()) {
    return fail(std::move(allocated.error()));
  }
  const TemporaryOwnedPage new_page_lease{owner, allocated->page_number, true};
  auto new_page = MutableBtreePage::Initialize(owner, allocated->owner_slot, page.geometry_,
                                               BtreePageType::kLeafTable);
  if (!new_page.has_value()) {
    return fail(std::move(new_page.error()));
  }
  auto rebuilt = new_page->Rebuild(*cells, page.cell_count_, 1U, workspace);
  if (!rebuilt.has_value()) {
    return fail(std::move(rebuilt.error()));
  }

  Store32(divider, 0U, page.page_number_.value());
  std::ranges::copy(old_key->encoded,
                    divider.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
  const ByteView divider_cell =
      ByteView{divider}.first(sizeof(std::uint32_t) + old_key->encoded.size());
  auto inserted =
      parent.InsertCell(parent.cell_count_, divider_cell, page.page_number_, divider, workspace);
  auto rightmost = parent.SetRightmostChild(allocated->page_number);
  if (!inserted.has_value()) {
    return fail(std::move(inserted.error()));
  }
  if (!rightmost.has_value()) {
    return fail(std::move(rightmost.error()));
  }
  return {};
}

Status MutableBtreePage::BalanceNonroot(MutableBtreePage& parent, MutableBtreePage& page,
                                        std::size_t parent_child_index,
                                        MutableByteView parent_overflow,
                                        BtreeWriteWorkspace& workspace, bool parent_is_root) {
  constexpr std::size_t kNeighborCount = 1U;
  constexpr std::size_t kMaximumOldPages = kNeighborCount * 2U + 1U;
  constexpr std::size_t kMaximumNewPages = kMaximumOldPages + 2U;

  if (parent.owner_ == nullptr || page.owner_ == nullptr) {
    return std::unexpected(Misuse("non-root balance requires live mutable B-tree pages"));
  }
  if (parent.owner_ != page.owner_ || parent.owner_slot_ == page.owner_slot_ ||
      parent.geometry_.page_size() != page.geometry_.page_size() ||
      parent.geometry_.usable_size() != page.geometry_.usable_size()) {
    return std::unexpected(Misuse("non-root balance pages do not share one mutation owner"));
  }
  if (parent.is_leaf() || parent.type_ != (page.is_table() ? BtreePageType::kInteriorTable
                                                           : BtreePageType::kInteriorIndex)) {
    return std::unexpected(Misuse("non-root balance parent and child page kinds do not match"));
  }
  if (parent_overflow.size() != parent.geometry_.page_size().value()) {
    return std::unexpected(Misuse("non-root balance parent overflow storage has the wrong size"));
  }

  const std::size_t parent_cell_count = parent.cell_count_ + parent.staged_count_;
  if (parent_child_index > parent_cell_count) {
    return std::unexpected(Misuse("non-root balance child index is out of range"));
  }
  if (parent.staged_count_ > 1U) {
    return std::unexpected(Corruption("non-root balance parent has too many staged cells"));
  }
  if (parent.staged_count_ == 1U) {
    const std::optional<StagedCell>& staged = parent.staged_cells_[0];
    if (!staged.has_value() || staged->index != parent_child_index) {
      return std::unexpected(
          Corruption("non-root balance parent staged cell is at the wrong index"));
    }
  }

  auto parent_bytes = parent.Bytes();
  auto page_bytes = page.Bytes();
  if (!parent_bytes.has_value()) {
    return std::unexpected(std::move(parent_bytes.error()));
  }
  if (!page_bytes.has_value()) {
    return std::unexpected(std::move(page_bytes.error()));
  }
  const std::less<> before;
  const auto overlaps = [&before](ByteView left, ByteView right) noexcept {
    return !left.empty() && !right.empty() &&
           before(left.data(), right.data() + static_cast<std::ptrdiff_t>(right.size())) &&
           before(right.data(), left.data() + static_cast<std::ptrdiff_t>(left.size()));
  };
  if (overlaps(ByteView{parent_overflow}, *parent_bytes) ||
      overlaps(ByteView{parent_overflow}, *page_bytes)) {
    return std::unexpected(
        Misuse("non-root balance parent overflow storage aliases a balanced page"));
  }
  for (std::size_t index = 0U; index < parent.staged_count_; ++index) {
    const std::optional<StagedCell>& staged = parent.staged_cells_[index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("non-root balance parent staged state is inconsistent"));
    }
    if (overlaps(ByteView{parent_overflow}, staged->bytes)) {
      return std::unexpected(
          Misuse("non-root balance parent overflow storage aliases a staged divider"));
    }
  }
  for (std::size_t index = 0U; index < page.staged_count_; ++index) {
    const std::optional<StagedCell>& staged = page.staged_cells_[index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("non-root balance child staged state is inconsistent"));
    }
    if (overlaps(ByteView{parent_overflow}, staged->bytes)) {
      return std::unexpected(
          Misuse("non-root balance parent overflow storage aliases a staged child cell"));
    }
  }

  auto parent_view = BtreePageView::Parse(*parent_bytes, parent.page_number_, parent.geometry_);
  if (!parent_view.has_value()) {
    return std::unexpected(std::move(parent_view.error()));
  }

  const auto child_at = [&parent, &parent_view,
                         parent_cell_count](std::size_t child_index) -> Result<PageNumber> {
    if (child_index > parent_cell_count) {
      return std::unexpected(Misuse("non-root balance child slot is out of range"));
    }
    PageNumber child;
    if (child_index == parent_cell_count) {
      child = parent_view->rightmost_child().value_or(PageNumber{});
    } else {
      auto cell = parent.LogicalCell(child_index);
      if (!cell.has_value()) {
        return std::unexpected(std::move(cell.error()));
      }
      if (cell->size() < sizeof(std::uint32_t)) {
        return std::unexpected(Corruption("non-root balance parent divider is truncated"));
      }
      child = PageNumber{Load32(*cell, 0U)};
    }
    if (!IsValidPageReference(child, parent.geometry_) || child == parent.page_number_) {
      return std::unexpected(Corruption("non-root balance parent has an invalid child"));
    }
    return child;
  };

  auto current_child = child_at(parent_child_index);
  if (!current_child.has_value()) {
    return std::unexpected(std::move(current_child.error()));
  }
  if (*current_child != page.page_number_) {
    return std::unexpected(Corruption("non-root balance page is not the selected parent child"));
  }

  // SQLite balance_nonroot(): NN=1 selects at most three adjacent children,
  // borrowing both neighbors from one side at the edges.
  std::size_t first_divider = 0U;
  std::size_t old_page_count = 0U;
  if (parent_cell_count < 2U) {
    old_page_count = parent_cell_count + 1U;
  } else {
    if (parent_child_index == 0U) {
      first_divider = 0U;
    } else if (parent_child_index == parent_cell_count) {
      first_divider = parent_cell_count - 2U;
    } else {
      first_divider = parent_child_index - 1U;
    }
    old_page_count = kMaximumOldPages;
  }
  if (old_page_count == 0U || old_page_count > kMaximumOldPages ||
      parent_child_index < first_divider || parent_child_index >= first_divider + old_page_count) {
    return std::unexpected(Corruption("non-root balance sibling selection is inconsistent"));
  }

  MutationPageOwner& owner = *parent.owner_;
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Status {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };

  class TemporarySlots final {
   public:
    explicit TemporarySlots(MutationPageOwner& owner) noexcept : owner_(&owner) {}
    TemporarySlots(const TemporarySlots&) = delete;
    TemporarySlots& operator=(const TemporarySlots&) = delete;
    ~TemporarySlots() {
      for (std::size_t index = count_; index > 0U; --index) {
        const std::size_t slot = slots_[index - 1U];
        if (owner_->Frame(slot).has_value()) {
          owner_->Release(slot);
        }
      }
    }

    [[nodiscard]] Status Add(std::size_t slot) {
      if (count_ >= slots_.size()) {
        return std::unexpected(
            TooLarge("non-root balance exceeded its temporary page ownership bound"));
      }
      slots_[count_++] = slot;
      return {};
    }

   private:
    MutationPageOwner* owner_;
    std::array<std::size_t, kMaximumNewPages> slots_{};
    std::size_t count_ = 0U;
  };
  TemporarySlots temporary_slots{owner};

  // Copy dividers before dropLogicalCell() reuses their first four bytes as
  // freeblock metadata. The caller buffer also retains any new parent overflow.
  std::array<std::size_t, kMaximumOldPages - 1U> divider_offsets{};
  std::array<std::size_t, kMaximumOldPages - 1U> divider_sizes{};
  std::size_t copied_divider_bytes = 0U;
  for (std::size_t index = 0U; index + 1U < old_page_count; ++index) {
    auto divider = parent.LogicalCell(first_divider + index);
    if (!divider.has_value()) {
      return fail(std::move(divider.error()));
    }
    if (divider->size() < sizeof(std::uint32_t) ||
        divider->size() > parent_overflow.size() - copied_divider_bytes) {
      return fail(Corruption("non-root balance divider copies exceed one page"));
    }
    divider_offsets[index] = copied_divider_bytes;
    divider_sizes[index] = divider->size();
    std::memmove(parent_overflow.data() + static_cast<std::ptrdiff_t>(copied_divider_bytes),
                 divider->data(), divider->size());
    copied_divider_bytes += divider->size();
  }

  std::array<PageNumber, kMaximumOldPages> old_page_numbers{};
  std::array<std::size_t, kMaximumOldPages> old_slots{};
  std::array<bool, kMaximumOldPages> old_is_current{};
  std::array<std::optional<BtreePageView>, kMaximumOldPages> old_views{};
  std::array<std::size_t, kMaximumOldPages> old_free_bytes{};
  std::size_t current_old_index = old_page_count;
  for (std::size_t index = old_page_count; index > 0U; --index) {
    const std::size_t old_index = index - 1U;
    auto child = child_at(first_divider + old_index);
    if (!child.has_value()) {
      return fail(std::move(child.error()));
    }
    old_page_numbers[old_index] = *child;
    if (*child == page.page_number_) {
      if (current_old_index != old_page_count) {
        return fail(Corruption("non-root balance selected the current page more than once"));
      }
      current_old_index = old_index;
      old_slots[old_index] = page.owner_slot_;
      old_is_current[old_index] = true;
    } else {
      if (owner.Find(*child).has_value()) {
        return fail(Corruption("non-root balance sibling aliases another mutation page"));
      }
      auto acquired = owner.AcquireRead(*child);
      if (!acquired.has_value()) {
        return fail(std::move(acquired.error()));
      }
      auto retained = temporary_slots.Add(*acquired);
      if (!retained.has_value()) {
        owner.Release(*acquired);
        return fail(std::move(retained.error()));
      }
      old_slots[old_index] = *acquired;
    }

    auto frame = owner.Frame(old_slots[old_index]);
    if (!frame.has_value()) {
      return fail(std::move(frame.error()));
    }
    auto view = BtreePageView::Parse(frame->get().bytes(), *child, parent.geometry_);
    if (!view.has_value()) {
      return fail(std::move(view.error()));
    }
    if (view->type() != page.type_ || child->value() == 1U) {
      return fail(Corruption("non-root balance siblings have inconsistent page kinds"));
    }
    auto free_space = view->AnalyzeFreeSpace();
    if (!free_space.has_value()) {
      return fail(std::move(free_space.error()));
    }
    old_free_bytes[old_index] = free_space->total().value();
    old_views[old_index] = *view;
  }
  if (current_old_index == old_page_count) {
    return fail(Corruption("non-root balance did not select the current page"));
  }

  std::optional<PageNumber> original_rightmost_child;
  if (!page.is_leaf()) {
    const std::optional<BtreePageView>& last_old_view = old_views[old_page_count - 1U];
    if (!last_old_view.has_value()) {
      return fail(Internal("non-root balance lost its final sibling view"));
    }
    original_rightmost_child = last_old_view->rightmost_child();
    if (!original_rightmost_child.has_value() ||
        !IsValidPageReference(*original_rightmost_child, page.geometry_)) {
      return fail(Corruption("non-root balance interior sibling has no rightmost child"));
    }
  }

  for (std::size_t index = old_page_count - 1U; index > 0U; --index) {
    auto dropped = parent.DropLogicalCell(first_divider + index - 1U);
    if (!dropped.has_value()) {
      return fail(std::move(dropped.error()));
    }
  }

  auto cells = CellArray::Create(page.geometry_);
  if (!cells.has_value()) {
    return fail(std::move(cells.error()));
  }
  const bool leaf_data = page.type_ == BtreePageType::kLeafTable;
  const std::size_t leaf_correction = page.is_leaf() ? sizeof(std::uint32_t) : 0U;
  const std::size_t usable_space = page.geometry_.usable_size().value() - 12U + leaf_correction;
  const auto signed_usable_space = static_cast<std::int64_t>(usable_space);
  std::array<std::size_t, kMaximumNewPages> old_cell_ends{};
  std::array<std::size_t, kMaximumNewPages> new_cell_ends{};
  std::array<std::int64_t, kMaximumNewPages> new_sizes{};

  for (std::size_t index = 0U; index < old_page_count; ++index) {
    const std::size_t cells_before = cells->size();
    Status appended;
    if (old_is_current[index]) {
      appended = cells->AppendPage(page);
    } else {
      auto frame = owner.Frame(old_slots[index]);
      if (!frame.has_value()) {
        return fail(std::move(frame.error()));
      }
      appended = cells->AppendPage(frame->get().bytes(), old_page_numbers[index]);
    }
    if (!appended.has_value()) {
      return fail(std::move(appended.error()));
    }
    const std::optional<BtreePageView>& old_view = old_views[index];
    if (!old_view.has_value()) {
      return fail(Internal("non-root balance lost a sibling view"));
    }
    const std::size_t logical_count =
        old_view->cell_count() + (old_is_current[index] ? page.staged_count_ : 0U);
    if (cells->size() - cells_before != logical_count) {
      return fail(Corruption("non-root balance assembled the wrong sibling cell count"));
    }
    old_cell_ends[index] = cells->size();
    new_cell_ends[index] = cells->size();
    if (old_free_bytes[index] > usable_space) {
      return fail(Corruption("non-root balance sibling free space exceeds its page"));
    }
    new_sizes[index] = static_cast<std::int64_t>(usable_space - old_free_bytes[index]);
    if (old_is_current[index]) {
      for (std::size_t staged_index = 0U; staged_index < page.staged_count_; ++staged_index) {
        const std::optional<StagedCell>& staged = page.staged_cells_[staged_index];
        if (!staged.has_value() ||
            staged->bytes.size() >
                static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)() - 2)) {
          return fail(Corruption("non-root balance staged cell size is inconsistent"));
        }
        new_sizes[index] += static_cast<std::int64_t>(staged->bytes.size() + 2U);
      }
    }

    if (index + 1U < old_page_count && !leaf_data) {
      const MutableByteView divider =
          parent_overflow.subspan(divider_offsets[index], divider_sizes[index]);
      if (page.is_leaf()) {
        if (divider.size() < sizeof(std::uint32_t)) {
          return fail(Corruption("non-root balance leaf divider is truncated"));
        }
        const std::size_t stripped_size = divider.size() - sizeof(std::uint32_t);
        const std::size_t leaf_size = std::max<std::size_t>(4U, stripped_size);
        Status copied;
        if (leaf_size == stripped_size) {
          copied = cells->AppendCopied(ByteView{divider}.subspan(sizeof(std::uint32_t)));
        } else {
          std::array<std::byte, 4> padded{};
          std::ranges::copy(ByteView{divider}.subspan(sizeof(std::uint32_t)), padded.begin());
          copied = cells->AppendCopied(padded);
        }
        if (!copied.has_value()) {
          return fail(std::move(copied.error()));
        }
      } else {
        const std::optional<PageNumber> rightmost = old_view->rightmost_child();
        if (!rightmost.has_value() || !IsValidPageReference(*rightmost, page.geometry_)) {
          return fail(Corruption("non-root balance interior sibling has no rightmost child"));
        }
        Store32(divider, 0U, rightmost->value());
        auto copied = cells->AppendCopied(ByteView{divider});
        if (!copied.has_value()) {
          return fail(std::move(copied.error()));
        }
      }
    }
  }

  const auto cell_cost = [&cells](std::size_t index) -> Result<std::size_t> {
    if (index >= cells->size()) {
      return std::unexpected(Corruption("non-root balance cell index is out of range"));
    }
    return static_cast<std::size_t>(cells->locator(index).size) + 2U;
  };

  // First reproduce SQLite's left-biased packing. Signed totals are required:
  // a page may temporarily lend more bytes than it originally contained.
  std::size_t new_page_count = old_page_count;
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    while (new_sizes[index] > signed_usable_space) {
      if (new_cell_ends[index] == 0U) {
        return fail(Corruption("non-root balance cannot move a cell from an empty page"));
      }
      if (index + 1U >= new_page_count) {
        if (new_page_count >= kMaximumNewPages) {
          return fail(Corruption("non-root balance requires too many output pages"));
        }
        new_sizes[new_page_count] = 0;
        new_cell_ends[new_page_count] = cells->size();
        ++new_page_count;
      }
      auto removed = cell_cost(new_cell_ends[index] - 1U);
      if (!removed.has_value()) {
        return fail(std::move(removed.error()));
      }
      const auto removed_size = static_cast<std::int64_t>(*removed);
      if (removed_size > new_sizes[index]) {
        return fail(Corruption("non-root balance page size accounting underflows"));
      }
      new_sizes[index] -= removed_size;
      std::size_t transferred = *removed;
      if (!leaf_data) {
        if (new_cell_ends[index] < cells->size()) {
          auto divider = cell_cost(new_cell_ends[index]);
          if (!divider.has_value()) {
            return fail(std::move(divider.error()));
          }
          transferred = *divider;
        } else {
          transferred = 0U;
        }
      }
      new_sizes[index + 1U] += static_cast<std::int64_t>(transferred);
      --new_cell_ends[index];
    }

    while (new_cell_ends[index] < cells->size()) {
      auto added = cell_cost(new_cell_ends[index]);
      if (!added.has_value()) {
        return fail(std::move(added.error()));
      }
      const auto added_size = static_cast<std::int64_t>(*added);
      if (new_sizes[index] + added_size > signed_usable_space) {
        break;
      }
      new_sizes[index] += added_size;
      ++new_cell_ends[index];
      std::size_t transferred = *added;
      if (!leaf_data) {
        if (new_cell_ends[index] < cells->size()) {
          auto divider = cell_cost(new_cell_ends[index]);
          if (!divider.has_value()) {
            return fail(std::move(divider.error()));
          }
          transferred = *divider;
        } else {
          transferred = 0U;
        }
      }
      new_sizes[index + 1U] -= static_cast<std::int64_t>(transferred);
    }
    if (new_cell_ends[index] >= cells->size()) {
      new_page_count = index + 1U;
    } else if (new_cell_ends[index] <= (index == 0U ? 0U : new_cell_ends[index - 1U])) {
      return fail(Corruption("non-root balance produced an empty interior sibling"));
    }
  }

  // The right-to-left adjustment is mandatory; without it the final sibling
  // may be empty even though the initial packing fits.
  for (std::size_t index = new_page_count; index > 1U; --index) {
    const std::size_t right_index = index - 1U;
    std::int64_t right_size = new_sizes[right_index];
    std::int64_t left_size = new_sizes[right_index - 1U];
    if (new_cell_ends[right_index - 1U] == 0U) {
      return fail(Corruption("non-root balance left sibling has no boundary cell"));
    }
    std::ptrdiff_t rightmost_left =
        static_cast<std::ptrdiff_t>(new_cell_ends[right_index - 1U]) - 1;
    std::ptrdiff_t first_right =
        rightmost_left + 1 - static_cast<std::ptrdiff_t>(leaf_data ? 1U : 0U);
    while (rightmost_left >= 0 && first_right >= 0 &&
           static_cast<std::size_t>(first_right) < cells->size()) {
      auto rightmost_cost = cell_cost(static_cast<std::size_t>(rightmost_left));
      auto divider_cost = cell_cost(static_cast<std::size_t>(first_right));
      if (!rightmost_cost.has_value()) {
        return fail(std::move(rightmost_cost.error()));
      }
      if (!divider_cost.has_value()) {
        return fail(std::move(divider_cost.error()));
      }
      const auto rightmost_size = static_cast<std::int64_t>(*rightmost_cost - 2U);
      const auto divider_size = static_cast<std::int64_t>(*divider_cost - 2U);
      const std::int64_t left_reserve =
          rightmost_size + (right_index + 1U == new_page_count ? 0 : 2);
      if (right_size != 0 && right_size + divider_size + 2 > left_size - left_reserve) {
        break;
      }
      right_size += divider_size + 2;
      left_size -= rightmost_size + 2;
      new_cell_ends[right_index - 1U] = static_cast<std::size_t>(rightmost_left);
      --rightmost_left;
      --first_right;
    }
    new_sizes[right_index] = right_size;
    new_sizes[right_index - 1U] = left_size;
    if (right_size < 0 || left_size < 0) {
      return fail(Corruption("non-root balance produced a negative page size"));
    }
    if (new_cell_ends[right_index - 1U] <=
        (right_index > 1U ? new_cell_ends[right_index - 2U] : 0U)) {
      return fail(Corruption("non-root balance right adjustment emptied a sibling"));
    }
  }

  if (new_cell_ends[0] == 0U &&
      !(parent.page_number_ == PageNumber{1} && parent_cell_count == 0U)) {
    return fail(Corruption("non-root balance produced an empty first sibling"));
  }

  // Reuse old pages first, allocate only the surplus, then assign ascending
  // page numbers through Pager's allocation-free permutation seam.
  std::array<std::size_t, kMaximumNewPages> new_slots{};
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    if (index < old_page_count) {
      new_slots[index] = old_slots[index];
      auto promoted = owner.Promote(new_slots[index]);
      if (!promoted.has_value()) {
        return fail(std::move(promoted.error()));
      }
    } else {
      auto allocated = AllocateBtreePage(owner, page.geometry_);
      if (!allocated.has_value()) {
        return fail(std::move(allocated.error()));
      }
      auto retained = temporary_slots.Add(allocated->owner_slot);
      if (!retained.has_value()) {
        owner.Release(allocated->owner_slot);
        return fail(std::move(retained.error()));
      }
      new_slots[index] = allocated->owner_slot;
      old_cell_ends[index] = cells->size();
    }
  }

  std::array<PageNumber, kMaximumNewPages> sorted_page_numbers{};
  std::array<MutationPageRekey, kMaximumNewPages> rekeys{};
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    auto frame = owner.Frame(new_slots[index]);
    if (!frame.has_value()) {
      return fail(std::move(frame.error()));
    }
    sorted_page_numbers[index] = frame->get().page_number();
  }
  std::ranges::sort(std::span{sorted_page_numbers}.first(new_page_count));
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    rekeys[index] = MutationPageRekey{
        .owner_slot = new_slots[index],
        .final_page = sorted_page_numbers[index],
    };
  }
  auto permuted = owner.PermutePageNumbers(std::span{rekeys}.first(new_page_count));
  if (!permuted.has_value()) {
    return fail(std::move(permuted.error()));
  }
  auto current_frame = owner.Frame(page.owner_slot_);
  if (!current_frame.has_value()) {
    return fail(std::move(current_frame.error()));
  }
  page.page_number_ = current_frame->get().page_number();

  std::array<std::optional<MutableBtreePage>, kMaximumNewPages> page_storage{};
  std::array<MutableBtreePage*, kMaximumNewPages> new_pages{};
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    if (new_slots[index] == page.owner_slot_) {
      new_pages[index] = &page;
      continue;
    }
    Result<MutableBtreePage> opened =
        index < old_page_count
            ? MutableBtreePage::Open(owner, new_slots[index], page.geometry_)
            : MutableBtreePage::Initialize(owner, new_slots[index], page.geometry_, page.type_);
    if (!opened.has_value()) {
      return fail(std::move(opened.error()));
    }
    MutableBtreePage& stored = page_storage[index].emplace(std::move(*opened));
    new_pages[index] = &stored;
  }

  auto linked = parent.SetChildAt(first_divider, new_pages[new_page_count - 1U]->page_number_);
  if (!linked.has_value()) {
    return fail(std::move(linked.error()));
  }
  if (original_rightmost_child.has_value()) {
    auto rightmost = new_pages[new_page_count - 1U]->SetRightmostChild(*original_rightmost_child);
    if (!rightmost.has_value()) {
      return fail(std::move(rightmost.error()));
    }
  }

  const auto table_leaf_rowid = [](ByteView cell) -> Result<std::uint64_t> {
    const auto payload = DecodeSqliteVarint(cell);
    if (!payload.has_value() || payload->bytes_consumed.value() >= cell.size()) {
      return std::unexpected(Corruption("non-root balance table leaf header is invalid"));
    }
    const ByteView encoded = cell.subspan(payload->bytes_consumed.value());
    const auto rowid = DecodeSqliteVarint(encoded);
    if (!rowid.has_value()) {
      return std::unexpected(Corruption("non-root balance table rowid is truncated"));
    }
    return rowid->value;
  };
  // Rebuild parent dividers before editing siblings, while every CellArray
  // source is still intact.
  std::size_t output_offset = 0U;
  for (std::size_t index = 0U; index + 1U < new_page_count; ++index) {
    const std::size_t boundary = new_cell_ends[index];
    if (boundary >= cells->size()) {
      return fail(Corruption("non-root balance divider index is out of range"));
    }
    ByteView divider;
    if (!page.is_leaf()) {
      auto boundary_cell = cells->Cell(boundary);
      if (!boundary_cell.has_value() || boundary_cell->size() < sizeof(std::uint32_t)) {
        return fail(boundary_cell.has_value()
                        ? Corruption("non-root balance interior divider is truncated")
                        : std::move(boundary_cell.error()));
      }
      const PageNumber right_child{Load32(*boundary_cell, 0U)};
      auto rightmost = new_pages[index]->SetRightmostChild(right_child);
      if (!rightmost.has_value()) {
        return fail(std::move(rightmost.error()));
      }
      divider = *boundary_cell;
    } else if (leaf_data) {
      if (boundary == 0U) {
        return fail(Corruption("non-root balance table divider has no left cell"));
      }
      auto boundary_cell = cells->Cell(boundary - 1U);
      if (!boundary_cell.has_value()) {
        return fail(std::move(boundary_cell.error()));
      }
      auto rowid = table_leaf_rowid(*boundary_cell);
      if (!rowid.has_value()) {
        return fail(std::move(rowid.error()));
      }
      std::array<std::byte, 9> encoded_rowid{};
      auto encoded = EncodeSqliteVarint(*rowid, MutableByteView{encoded_rowid});
      if (!encoded.has_value()) {
        return fail(Internal("non-root balance table rowid could not be encoded"));
      }
      const std::size_t divider_size = sizeof(std::uint32_t) + encoded->value();
      if (divider_size > parent_overflow.size() - output_offset) {
        return fail(Corruption("non-root balance parent dividers exceed one page"));
      }
      const MutableByteView output = parent_overflow.subspan(output_offset, divider_size);
      Store32(output, 0U, new_pages[index]->page_number_.value());
      std::ranges::copy(std::span{encoded_rowid}.first(encoded->value()),
                        output.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
      divider = ByteView{output};
    } else {
      auto boundary_cell = cells->Cell(boundary);
      if (!boundary_cell.has_value()) {
        return fail(std::move(boundary_cell.error()));
      }
      auto unpadded_size = IndexLeafUnpaddedCellSize(*boundary_cell, page.geometry_);
      if (!unpadded_size.has_value()) {
        return fail(std::move(unpadded_size.error()));
      }
      const std::size_t divider_size = sizeof(std::uint32_t) + *unpadded_size;
      if (divider_size > parent_overflow.size() - output_offset) {
        return fail(Corruption("non-root balance parent dividers exceed one page"));
      }
      const MutableByteView output = parent_overflow.subspan(output_offset, divider_size);
      Store32(output, 0U, new_pages[index]->page_number_.value());
      std::ranges::copy(boundary_cell->first(*unpadded_size),
                        output.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)));
      divider = ByteView{output};
    }

    if (divider.size() > parent_overflow.size() - output_offset) {
      return fail(Corruption("non-root balance parent dividers exceed one page"));
    }
    const MutableByteView staged_copy = parent_overflow.subspan(output_offset, divider.size());
    auto inserted = parent.InsertCell(first_divider + index, divider,
                                      new_pages[index]->page_number_, staged_copy, workspace);
    if (!inserted.has_value()) {
      return fail(std::move(inserted.error()));
    }
    output_offset += divider.size();
  }

  // SQLite's down-then-up pass avoids overwriting a page that still supplies
  // cells to a neighbor.
  std::array<bool, kMaximumNewPages> edited{};
  const auto signed_page_count = static_cast<std::ptrdiff_t>(new_page_count);
  for (std::ptrdiff_t pass = 1 - signed_page_count; pass < signed_page_count; ++pass) {
    const auto page_index = static_cast<std::size_t>(pass < 0 ? -pass : pass);
    if (edited[page_index]) {
      continue;
    }
    const bool left_dependency_satisfied =
        page_index == 0U || old_cell_ends[page_index - 1U] >= new_cell_ends[page_index - 1U] ||
        edited[page_index - 1U];
    if (pass < 0 && !left_dependency_satisfied) {
      continue;
    }
    const bool right_dependency_satisfied =
        new_cell_ends[page_index] >= old_cell_ends[page_index] ||
        (page_index + 1U < new_page_count && edited[page_index + 1U]);
    if (!left_dependency_satisfied || !right_dependency_satisfied) {
      return fail(Corruption("non-root balance page edit dependency is inconsistent"));
    }

    const std::size_t old_first =
        page_index == 0U
            ? 0U
            : (page_index < old_page_count ? old_cell_ends[page_index - 1U] + (leaf_data ? 0U : 1U)
                                           : cells->size());
    const std::size_t new_first =
        page_index == 0U ? 0U : new_cell_ends[page_index - 1U] + (leaf_data ? 0U : 1U);
    if (new_cell_ends[page_index] < new_first) {
      return fail(Corruption("non-root balance page cell range is inverted"));
    }
    const std::size_t count = new_cell_ends[page_index] - new_first;
    auto edited_page = new_pages[page_index]->Edit(*cells, old_first, new_first, count, workspace);
    if (!edited_page.has_value()) {
      return fail(std::move(edited_page.error()));
    }
    edited[page_index] = true;
  }
  for (std::size_t index = 0U; index < new_page_count; ++index) {
    if (!edited[index]) {
      return fail(Corruption("non-root balance did not edit every output page"));
    }
  }

  // balance-shallower: defragment before copying because page 1 has a
  // 100-byte database-header prefix that ordinary child pages do not.
  if (parent_is_root && parent.cell_count_ + parent.staged_count_ == 0U &&
      parent.header_offset_ <= new_pages[0]->free_bytes_) {
    if (new_page_count != 1U) {
      return fail(Corruption("non-root balance empty root has multiple children"));
    }
    auto defragmented = new_pages[0]->Defragment(0U, workspace);
    if (!defragmented.has_value()) {
      return fail(std::move(defragmented.error()));
    }
    auto child_bytes = new_pages[0]->Bytes();
    auto target_bytes = parent.Bytes();
    if (!child_bytes.has_value()) {
      return fail(std::move(child_bytes.error()));
    }
    if (!target_bytes.has_value()) {
      return fail(std::move(target_bytes.error()));
    }
    std::size_t content = Load16(*child_bytes, new_pages[0]->header_offset_ + 5U);
    if (content == 0U && parent.geometry_.usable_size().value() == 65536U) {
      content = 65536U;
    }
    const std::size_t prefix = new_pages[0]->cell_pointer_offset_ + new_pages[0]->cell_count_ * 2U;
    if (content > parent.geometry_.usable_size().value() || prefix > content ||
        parent.header_offset_ > parent.geometry_.usable_size().value() - prefix) {
      return fail(Corruption("non-root balance child cannot be copied into the root"));
    }
    owner.NoteMutation();
    std::memmove(target_bytes->data() + static_cast<std::ptrdiff_t>(content),
                 child_bytes->data() + static_cast<std::ptrdiff_t>(content),
                 parent.geometry_.usable_size().value() - content);
    std::memmove(target_bytes->data() + static_cast<std::ptrdiff_t>(parent.header_offset_),
                 child_bytes->data() + static_cast<std::ptrdiff_t>(new_pages[0]->header_offset_),
                 prefix);
    auto root_view = BtreePageView::Parse(*target_bytes, parent.page_number_, parent.geometry_);
    if (!root_view.has_value()) {
      return fail(std::move(root_view.error()));
    }
    auto root_free = root_view->AnalyzeFreeSpace();
    if (!root_free.has_value()) {
      return fail(std::move(root_free.error()));
    }
    parent.type_ = root_view->type();
    parent.cell_pointer_offset_ = parent.header_offset_ + (root_view->is_leaf() ? 8U : 12U);
    parent.cell_count_ = root_view->cell_count();
    parent.free_bytes_ = root_free->total().value();
    parent.ClearStagedCells();
    auto freed = FreeBtreePage(owner, parent.geometry_, new_pages[0]->page_number_);
    if (!freed.has_value()) {
      return fail(std::move(freed.error()));
    }
  }

  for (std::size_t index = new_page_count; index < old_page_count; ++index) {
    auto frame = owner.Frame(old_slots[index]);
    if (!frame.has_value()) {
      return fail(std::move(frame.error()));
    }
    auto freed = FreeBtreePage(owner, parent.geometry_, frame->get().page_number());
    if (!freed.has_value()) {
      return fail(std::move(freed.error()));
    }
    if (old_is_current[index]) {
      page.ClearStagedCells();
    }
  }
  return {};
}

Result<MutableBtreePage> MutableBtreePage::BalanceDeeper(MutableBtreePage& root) {
  if (root.owner_ == nullptr) {
    return std::unexpected(Misuse("root deepening requires a live mutable B-tree page"));
  }
  if (root.staged_count_ == 0U) {
    return std::unexpected(Misuse("root deepening requires staged overflow cells"));
  }
  auto root_bytes = root.Bytes();
  if (!root_bytes.has_value()) {
    return std::unexpected(std::move(root_bytes.error()));
  }
  auto root_view = BtreePageView::Parse(*root_bytes, root.page_number_, root.geometry_);
  if (!root_view.has_value()) {
    return std::unexpected(std::move(root_view.error()));
  }
  auto root_free = root_view->AnalyzeFreeSpace();
  if (!root_free.has_value()) {
    return std::unexpected(std::move(root_free.error()));
  }

  std::optional<std::size_t> previous_staged_index;
  for (std::size_t index = 0U; index < root.staged_count_; ++index) {
    const StagedCell staged = root.staged_cells_[index].value_or(
        StagedCell{.index = 0U, .bytes = {}, .writable_bytes = {}});
    if (staged.bytes.empty() || staged.index > root.cell_count_ + index ||
        (previous_staged_index.has_value() && staged.index != *previous_staged_index + 1U)) {
      return std::unexpected(Corruption("root staged-cell state is inconsistent"));
    }
    std::optional<PageNumber> left_child;
    if (!root.is_leaf()) {
      if (staged.bytes.size() < sizeof(std::uint32_t)) {
        return std::unexpected(Corruption("root staged interior cell is truncated"));
      }
      left_child = PageNumber{Load32(staged.bytes, 0U)};
    }
    auto valid = root.ValidateCellImage(staged.bytes, left_child);
    if (!valid.has_value()) {
      return std::unexpected(std::move(valid.error()));
    }
    const std::less<> before;
    const bool overlaps_root =
        before(staged.bytes.data(), root_bytes->data() + root_bytes->size()) &&
        before(root_bytes->data(), staged.bytes.data() + staged.bytes.size());
    if (overlaps_root) {
      return std::unexpected(Corruption("root staged cell aliases bytes overwritten by deepening"));
    }
    previous_staged_index = staged.index;
  }

  std::size_t content = Load16(*root_bytes, root.header_offset_ + 5U);
  if (content == 0U && root.geometry_.usable_size().value() == 65536U) {
    content = 65536U;
  }
  const std::size_t copied_prefix_size = root.cell_pointer_offset_ + root.cell_count_ * 2U;
  if (content > root.geometry_.usable_size().value() || copied_prefix_size > content ||
      root.header_offset_ > root.geometry_.usable_size().value() - copied_prefix_size) {
    return std::unexpected(Corruption("root node cannot be copied into an ordinary child page"));
  }

  BtreePageType replacement_type = root.type_;
  if (replacement_type == BtreePageType::kLeafIndex) {
    replacement_type = BtreePageType::kInteriorIndex;
  } else if (replacement_type == BtreePageType::kLeafTable) {
    replacement_type = BtreePageType::kInteriorTable;
  }

  MutationPageOwner& owner = *root.owner_;
  const std::uint64_t checkpoint = owner.mutation_sequence();
  const auto fail = [&owner, checkpoint](Error error) -> Result<MutableBtreePage> {
    owner.MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  auto allocated = AllocateBtreePage(owner, root.geometry_);
  if (!allocated.has_value()) {
    return fail(std::move(allocated.error()));
  }
  TemporaryOwnedPage child_lease{owner, allocated->page_number, true};
  root_bytes = root.Bytes();
  if (!root_bytes.has_value()) {
    return fail(std::move(root_bytes.error()));
  }
  auto child_bytes = owner.MutableBytes(allocated->owner_slot);
  if (!child_bytes.has_value()) {
    return fail(std::move(child_bytes.error()));
  }

  owner.NoteMutation();
  std::memmove(child_bytes->data() + static_cast<std::ptrdiff_t>(content),
               root_bytes->data() + static_cast<std::ptrdiff_t>(content),
               root.geometry_.usable_size().value() - content);
  std::memmove(child_bytes->data(),
               root_bytes->data() + static_cast<std::ptrdiff_t>(root.header_offset_),
               copied_prefix_size);
  auto child_view = BtreePageView::Parse(*child_bytes, allocated->page_number, root.geometry_);
  if (!child_view.has_value()) {
    return fail(std::move(child_view.error()));
  }
  auto child_free = child_view->AnalyzeFreeSpace();
  if (!child_free.has_value()) {
    return fail(std::move(child_free.error()));
  }
  if (child_view->type() != root.type_ || child_view->cell_count() != root.cell_count_) {
    return fail(Corruption("deepened child does not match the copied root node"));
  }

  MutableBtreePage child{
      owner,
      allocated->owner_slot,
      root.geometry_,
      allocated->page_number,
      root.type_,
      Metadata{
          .header_offset = 0U,
          .cell_pointer_offset = child_view->is_leaf() ? 8U : 12U,
          .cell_count = child_view->cell_count(),
          .free_bytes = child_free->total().value(),
      },
  };
  child.staged_cells_ = root.staged_cells_;
  child.staged_count_ = root.staged_count_;

  auto zeroed = root.Zero(replacement_type);
  if (!zeroed.has_value()) {
    return fail(std::move(zeroed.error()));
  }
  auto rightmost = root.SetRightmostChild(allocated->page_number);
  if (!rightmost.has_value()) {
    return fail(std::move(rightmost.error()));
  }
  child_lease.Keep();
  return child;
}

Status MutableBtreePage::Edit(const CellArray& cells, std::size_t old_first, std::size_t new_first,
                              std::size_t count, BtreeWriteWorkspace& workspace) {
  if (cells.geometry_.page_size() != geometry_.page_size() ||
      cells.geometry_.usable_size() != geometry_.usable_size()) {
    return std::unexpected(Misuse("edited B-tree cells use different page geometry"));
  }
  const std::size_t old_count = cell_count_ + staged_count_;
  if (old_first > cells.size() || old_count > cells.size() - old_first ||
      new_first > cells.size() || count > cells.size() - new_first) {
    return std::unexpected(Misuse("edited B-tree cell range is invalid"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  const MutableByteView page_copy = workspace.rebuild_scratch();
  if (page_copy.size() != bytes->size()) {
    return std::unexpected(Misuse("B-tree rebuild scratch has the wrong size"));
  }
  std::ranges::copy(*bytes, page_copy.begin());
  const ByteView target_copy{page_copy};
  const ByteView target{*bytes};

  auto original_page = BtreePageView::Parse(target_copy, page_number_, geometry_);
  if (!original_page.has_value()) {
    return std::unexpected(std::move(original_page.error()));
  }
  std::size_t persisted = 0U;
  std::size_t staged = 0U;
  for (std::size_t logical = 0U; logical < old_count; ++logical) {
    const std::size_t index = old_first + logical;
    auto listed = cells.Cell(index);
    if (!listed.has_value()) {
      return std::unexpected(std::move(listed.error()));
    }
    if (staged < staged_count_) {
      const std::optional<StagedCell>& candidate = staged_cells_[staged];
      if (!candidate.has_value()) {
        return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
      }
      if (candidate->index == logical) {
        if (cells.locator(index).flags != static_cast<std::uint8_t>(CellLocatorFlags::kStaged) ||
            !std::ranges::equal(candidate->bytes, *listed)) {
          return std::unexpected(
              Corruption("CellArray staged cell does not match the edited page"));
        }
        ++staged;
        continue;
      }
      if (candidate->index < logical) {
        return std::unexpected(Corruption("staged B-tree cells are unordered"));
      }
    }
    if (persisted >= cell_count_) {
      return std::unexpected(Corruption("CellArray page range exceeds persisted page cells"));
    }
    auto expected_offset = original_page->cell_offset(persisted);
    auto expected_cell = original_page->cell(persisted);
    auto listed_offset = cells.OffsetWithinTarget(index, target);
    if (!expected_offset.has_value()) {
      return std::unexpected(std::move(expected_offset.error()));
    }
    if (!expected_cell.has_value()) {
      return std::unexpected(std::move(expected_cell.error()));
    }
    if (!listed_offset.has_value()) {
      return std::unexpected(std::move(listed_offset.error()));
    }
    if (!listed_offset->has_value() || **listed_offset != expected_offset->value() ||
        cells.locator(index).size != expected_cell->encoded_size().value() ||
        cells.locator(index).flags != static_cast<std::uint8_t>(CellLocatorFlags::kBorrowed)) {
      return std::unexpected(Corruption("CellArray persisted cell does not match the edited page"));
    }
    ++persisted;
  }
  if (persisted != cell_count_ || staged != staged_count_) {
    return std::unexpected(Corruption("CellArray does not contain the complete edited page range"));
  }

  const std::size_t pointer_end = cell_pointer_offset_ + count * 2U;
  std::size_t total_size = 0U;
  for (std::size_t index = 0U; index < count; ++index) {
    auto resolved = cells.ResolveForTarget(new_first + index, target, target_copy);
    if (!resolved.has_value()) {
      return std::unexpected(std::move(resolved.error()));
    }
    std::optional<PageNumber> left_child;
    if (!is_leaf()) {
      if (resolved->size() < sizeof(std::uint32_t)) {
        return std::unexpected(Corruption("edited interior B-tree cell is truncated"));
      }
      left_child = PageNumber{Load32(*resolved, 0U)};
    }
    auto valid = ValidateCellImage(*resolved, left_child);
    if (!valid.has_value()) {
      return valid;
    }
    if (resolved->size() > geometry_.usable_size().value() - total_size) {
      return std::unexpected(Corruption("edited B-tree cells exceed the usable page"));
    }
    total_size += resolved->size();
  }
  if (pointer_end > geometry_.usable_size().value() ||
      total_size > geometry_.usable_size().value() - pointer_end) {
    return std::unexpected(Corruption("edited B-tree cells do not fit on the page"));
  }

  const std::uint64_t checkpoint = owner_->mutation_sequence();
  const auto fail = [this, checkpoint](Error error) -> Status {
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    return std::unexpected(std::move(error));
  };
  const auto rebuild = [this, &cells, new_first, count, target_copy, &fail]() -> Status {
    auto rebuilt = RebuildFromSnapshot(cells, new_first, count, target_copy);
    if (!rebuilt.has_value()) {
      return fail(std::move(rebuilt.error()));
    }
    return {};
  };

  // Mirrors SQLite pageFreeArray(): batch adjacent target-resident cells
  // before returning their bytes to the page freeblock chain.
  const auto free_range = [this, &cells, target](std::size_t first,
                                                 std::size_t range_count) -> Result<std::size_t> {
    std::array<std::size_t, 10> starts{};
    std::array<std::size_t, 10> ends{};
    std::size_t pending = 0U;
    std::size_t freed = 0U;
    const auto flush = [this, &starts, &ends, &pending]() -> Status {
      for (std::size_t index = 0U; index < pending; ++index) {
        auto status = FreeSpace(starts[index], ends[index] - starts[index]);
        if (!status.has_value()) {
          return status;
        }
      }
      pending = 0U;
      return {};
    };

    for (std::size_t index = first; index < first + range_count; ++index) {
      auto offset = cells.OffsetWithinTarget(index, target);
      if (!offset.has_value()) {
        return std::unexpected(std::move(offset.error()));
      }
      if (!offset->has_value()) {
        continue;
      }
      const std::size_t start = **offset;
      const std::size_t size = cells.locator(index).size;
      if (start < cell_pointer_offset_ || size > geometry_.usable_size().value() - start) {
        return std::unexpected(Corruption("edited B-tree cell range exceeds the target page"));
      }
      const std::size_t end = start + size;
      std::size_t adjacent = 0U;
      for (; adjacent < pending; ++adjacent) {
        if (starts[adjacent] == end) {
          starts[adjacent] = start;
          break;
        }
        if (ends[adjacent] == start) {
          ends[adjacent] = end;
          break;
        }
      }
      if (adjacent == pending) {
        if (pending == starts.size()) {
          auto flushed = flush();
          if (!flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
          }
        }
        starts[pending] = start;
        ends[pending] = end;
        ++pending;
      }
      ++freed;
    }
    auto flushed = flush();
    if (!flushed.has_value()) {
      return std::unexpected(std::move(flushed.error()));
    }
    return freed;
  };

  // Mirrors SQLite pageInsertArray(). A false result selects rebuildPage()
  // after the in-place layout cannot accommodate the requested cells.
  const auto insert_range = [this, &cells, target, target_copy, pointer_end](
                                std::size_t pointer, std::size_t first, std::size_t range_count,
                                std::size_t& content) -> Result<bool> {
    auto destination = Bytes();
    if (!destination.has_value()) {
      return std::unexpected(std::move(destination.error()));
    }
    for (std::size_t index = 0U; index < range_count; ++index) {
      auto cell = cells.ResolveForTarget(first + index, target, target_copy);
      if (!cell.has_value()) {
        return std::unexpected(std::move(cell.error()));
      }
      std::optional<std::size_t> slot;
      if (Load16(*destination, header_offset_ + 1U) != 0U) {
        auto found = FindFreeblock(cell->size());
        if (!found.has_value()) {
          return std::unexpected(std::move(found.error()));
        }
        slot = *found;
      }
      if (slot.has_value() && *slot < pointer_end) {
        return std::unexpected(Corruption("edited B-tree freeblock overlaps the pointer array"));
      }
      if (!slot.has_value()) {
        if (content < pointer_end || cell->size() > content - pointer_end) {
          return false;
        }
        content -= cell->size();
        slot = content;
      }
      if (*slot > geometry_.usable_size().value() ||
          cell->size() > geometry_.usable_size().value() - *slot || pointer > pointer_end ||
          2U > pointer_end - pointer) {
        return std::unexpected(Corruption("edited B-tree insertion range exceeds the page"));
      }
      owner_->NoteMutation();
      std::memmove(destination->data() + static_cast<std::ptrdiff_t>(*slot), cell->data(),
                   cell->size());
      Store16(*destination, BigEndian16{.offset = pointer, .value = *slot});
      pointer += 2U;
    }
    return true;
  };

  std::size_t stored_cells = cell_count_;
  const std::size_t old_end = old_first + old_count;
  const std::size_t new_end = new_first + count;
  if (old_first < new_first) {
    auto shifted = free_range(old_first, new_first - old_first);
    if (!shifted.has_value()) {
      return fail(std::move(shifted.error()));
    }
    if (*shifted > stored_cells) {
      return fail(Corruption("edited B-tree prefix exceeds stored cells"));
    }
    if (*shifted != 0U) {
      owner_->NoteMutation();
      std::memmove(
          bytes->data() + static_cast<std::ptrdiff_t>(cell_pointer_offset_),
          bytes->data() + static_cast<std::ptrdiff_t>(cell_pointer_offset_ + *shifted * 2U),
          (stored_cells - *shifted) * 2U);
      stored_cells -= *shifted;
    }
  }
  if (new_end < old_end) {
    auto tail = free_range(new_end, old_end - new_end);
    if (!tail.has_value()) {
      return fail(std::move(tail.error()));
    }
    if (*tail > stored_cells) {
      return fail(Corruption("edited B-tree suffix exceeds stored cells"));
    }
    stored_cells -= *tail;
  }

  std::size_t content = Load16(*bytes, header_offset_ + 5U);
  if (content == 0U && geometry_.usable_size().value() == 65536U) {
    content = 65536U;
  }
  if (content < pointer_end || content > geometry_.usable_size().value()) {
    return rebuild();
  }

  if (new_first < old_first) {
    const std::size_t add = std::min(count, old_first - new_first);
    if (stored_cells > count || add > count - stored_cells) {
      return fail(Corruption("edited B-tree prefix insertion is inconsistent"));
    }
    if (add != 0U) {
      owner_->NoteMutation();
      std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(cell_pointer_offset_ + add * 2U),
                   bytes->data() + static_cast<std::ptrdiff_t>(cell_pointer_offset_),
                   stored_cells * 2U);
      auto inserted = insert_range(cell_pointer_offset_, new_first, add, content);
      if (!inserted.has_value()) {
        return fail(std::move(inserted.error()));
      }
      if (!*inserted) {
        return rebuild();
      }
      stored_cells += add;
    }
  }

  for (std::size_t index = 0U; index < staged_count_; ++index) {
    const std::optional<StagedCell>& staged_cell = staged_cells_[index];
    if (!staged_cell.has_value()) {
      return fail(Corruption("staged B-tree cell state is inconsistent"));
    }
    const std::size_t global = old_first + staged_cell->index;
    if (global < new_first || global >= new_end) {
      continue;
    }
    const std::size_t destination_index = global - new_first;
    if (destination_index > stored_cells || stored_cells >= count) {
      return fail(Corruption("staged B-tree cell destination is inconsistent"));
    }
    if (stored_cells > destination_index) {
      owner_->NoteMutation();
      std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(cell_pointer_offset_ +
                                                               (destination_index + 1U) * 2U),
                   bytes->data() +
                       static_cast<std::ptrdiff_t>(cell_pointer_offset_ + destination_index * 2U),
                   (stored_cells - destination_index) * 2U);
    }
    auto inserted =
        insert_range(cell_pointer_offset_ + destination_index * 2U, global, 1U, content);
    if (!inserted.has_value()) {
      return fail(std::move(inserted.error()));
    }
    if (!*inserted) {
      return rebuild();
    }
    ++stored_cells;
  }

  if (stored_cells > count) {
    return fail(Corruption("edited B-tree page retained too many cells"));
  }
  auto appended = insert_range(cell_pointer_offset_ + stored_cells * 2U, new_first + stored_cells,
                               count - stored_cells, content);
  if (!appended.has_value()) {
    return fail(std::move(appended.error()));
  }
  if (!*appended) {
    return rebuild();
  }

  Store16(*bytes, BigEndian16{.offset = header_offset_ + 3U, .value = count});
  Store16(*bytes, BigEndian16{.offset = header_offset_ + 5U, .value = content});
  cell_count_ = count;
  free_bytes_ = geometry_.usable_size().value() - pointer_end - total_size;
  ClearStagedCells();
  return {};
}

void MutableBtreePage::ClearStagedCells() noexcept {
  if (owner_ == nullptr) {
    return;
  }
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
  return owner_ != nullptr && slot < staged_cells_.size() ? staged_cells_[slot] : std::nullopt;
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
      free_bytes_(metadata.free_bytes),
      staged_cells_(owner.pages_[owner_slot].staged_cells),
      staged_count_(owner.pages_[owner_slot].staged_count) {}

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
      staged_count_(other.staged_count_) {}

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

Result<ByteView> MutableBtreePage::LogicalCell(std::size_t index) const {
  if (index >= cell_count_ + staged_count_) {
    return std::unexpected(Misuse("logical B-tree cell index is out of range"));
  }
  std::size_t persisted_index = index;
  for (std::size_t staged_index = 0U; staged_index < staged_count_; ++staged_index) {
    const std::optional<StagedCell>& staged = staged_cells_[staged_index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
    }
    if (staged->index == index) {
      return staged->bytes;
    }
    if (staged->index < index) {
      if (persisted_index == 0U) {
        return std::unexpected(Corruption("staged B-tree cell indexes are inconsistent"));
      }
      --persisted_index;
    }
  }
  if (persisted_index >= cell_count_) {
    return std::unexpected(Corruption("logical B-tree cell does not map to persisted storage"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  auto page = BtreePageView::Parse(*bytes, page_number_, geometry_);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  auto offset = page->cell_offset(persisted_index);
  auto cell = page->cell(persisted_index);
  if (!offset.has_value()) {
    return std::unexpected(std::move(offset.error()));
  }
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  return bytes->subspan(offset->value(), cell->encoded_size().value());
}

Status MutableBtreePage::DropLogicalCell(std::size_t index) {
  if (index >= cell_count_ + staged_count_) {
    return std::unexpected(Misuse("dropped logical B-tree cell index is out of range"));
  }
  std::size_t persisted_index = index;
  for (std::size_t staged_index = 0U; staged_index < staged_count_; ++staged_index) {
    std::optional<StagedCell>& staged = staged_cells_[staged_index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
    }
    if (staged->index == index) {
      for (std::size_t move = staged_index; move + 1U < staged_count_; ++move) {
        staged_cells_[move] = staged_cells_[move + 1U];
        std::optional<StagedCell>& moved = staged_cells_[move];
        if (moved.has_value()) {
          --moved->index;
        }
      }
      staged_cells_[staged_count_ - 1U] = std::nullopt;
      --staged_count_;
      return {};
    }
    if (staged->index < index) {
      if (persisted_index == 0U) {
        return std::unexpected(Corruption("staged B-tree cell indexes are inconsistent"));
      }
      --persisted_index;
    }
  }
  auto dropped = DropCell(persisted_index);
  if (!dropped.has_value()) {
    return dropped;
  }
  for (std::size_t staged_index = 0U; staged_index < staged_count_; ++staged_index) {
    std::optional<StagedCell>& staged = staged_cells_[staged_index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
    }
    if (staged->index > index) {
      --staged->index;
    }
  }
  return {};
}

Status MutableBtreePage::SetChildAt(std::size_t child_index, PageNumber child) {
  if (is_leaf()) {
    return std::unexpected(Misuse("leaf B-tree pages have no child slots"));
  }
  if (!IsValidPageReference(child, geometry_)) {
    return std::unexpected(Corruption("B-tree page has an invalid child reference"));
  }
  const std::size_t logical_count = cell_count_ + staged_count_;
  if (child_index > logical_count) {
    return std::unexpected(Misuse("B-tree child slot is out of range"));
  }
  if (child_index == logical_count) {
    return SetRightmostChild(child);
  }

  std::size_t persisted_index = child_index;
  for (std::size_t staged_index = 0U; staged_index < staged_count_; ++staged_index) {
    std::optional<StagedCell>& staged = staged_cells_[staged_index];
    if (!staged.has_value()) {
      return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
    }
    if (staged->index == child_index) {
      if (staged->writable_bytes.size() < sizeof(std::uint32_t) ||
          staged->bytes.size() != staged->writable_bytes.size()) {
        return std::unexpected(
            Corruption("staged interior B-tree cell has no writable child prefix"));
      }
      Store32(staged->writable_bytes, 0U, child.value());
      return {};
    }
    if (staged->index < child_index) {
      if (persisted_index == 0U) {
        return std::unexpected(Corruption("staged B-tree cell indexes are inconsistent"));
      }
      --persisted_index;
    }
  }
  if (persisted_index >= cell_count_) {
    return std::unexpected(Corruption("B-tree child slot does not map to persisted storage"));
  }
  auto bytes = Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  auto page = BtreePageView::Parse(*bytes, page_number_, geometry_);
  if (!page.has_value()) {
    return std::unexpected(std::move(page.error()));
  }
  auto offset = page->cell_offset(persisted_index);
  if (!offset.has_value()) {
    return std::unexpected(std::move(offset.error()));
  }
  owner_->NoteMutation();
  Store32(*bytes, offset->value(), child.value());
  return {};
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
        owner_->NoteMutation();
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
      owner_->NoteMutation();
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

Result<CellArray> CellArray::Create(BtreePageGeometry geometry) {
  const std::size_t maximum_page_cells = (geometry.page_size().value() - 8U) / 6U;
  const std::size_t unaligned_maximum = 3U * (maximum_page_cells + kStagedCellSlots);
  const std::size_t maximum_cells = (unaligned_maximum + 3U) & ~std::size_t{3U};
  const std::size_t scratch_bytes =
      maximum_cells * sizeof(CellLocator) + geometry.page_size().value();
  if (scratch_bytes > 7U * geometry.page_size().value()) {
    return std::unexpected(Corruption("CellArray exceeds SQLite's seven-page scratch bound"));
  }
  try {
    std::vector<CellLocator> cells;
    cells.reserve(maximum_cells);
    ByteBuffer copied{geometry.page_size()};
    return CellArray{geometry, maximum_cells, std::move(cells), std::move(copied)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

CellArray::CellArray(BtreePageGeometry geometry, std::size_t maximum_cells,
                     std::vector<CellLocator> cells, ByteBuffer copied_cells) noexcept
    : geometry_(geometry),
      maximum_cells_(maximum_cells),
      cells_(std::move(cells)),
      copied_cells_(std::move(copied_cells)) {
  sources_[0] = copied_cells_.view();
  source_count_ = 1U;
}

Status CellArray::AppendPage(const MutableBtreePage& page) {
  if (page.geometry_.page_size() != geometry_.page_size() ||
      page.geometry_.usable_size() != geometry_.usable_size()) {
    return std::unexpected(Misuse("CellArray page uses different page geometry"));
  }
  auto bytes = page.Bytes();
  if (!bytes.has_value()) {
    return std::unexpected(std::move(bytes.error()));
  }
  auto view = BtreePageView::Parse(*bytes, page.page_number_, geometry_);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  auto source_slot = SourceSlot(*bytes);
  if (!source_slot.has_value()) {
    return std::unexpected(std::move(source_slot.error()));
  }
  std::size_t persisted = 0U;
  std::size_t staged = 0U;
  const std::size_t total = page.cell_count_ + page.staged_count_;
  for (std::size_t logical = 0U; logical < total; ++logical) {
    if (staged < page.staged_count_) {
      const std::optional<StagedCell>& candidate = page.staged_cells_[staged];
      if (!candidate.has_value()) {
        return std::unexpected(Corruption("staged B-tree cell state is inconsistent"));
      }
      if (candidate->index == logical) {
        auto appended = AppendStaged(candidate->bytes);
        if (!appended.has_value()) {
          return appended;
        }
        ++staged;
        continue;
      }
      if (candidate->index < logical) {
        return std::unexpected(Corruption("staged B-tree cells are unordered"));
      }
    }
    if (persisted >= page.cell_count_) {
      return std::unexpected(Corruption("staged B-tree cell indexes exceed page contents"));
    }
    auto offset = view->cell_offset(persisted);
    auto cell = view->cell(persisted);
    if (!offset.has_value()) {
      return std::unexpected(std::move(offset.error()));
    }
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    auto appended = AppendLocator(*source_slot, offset->value(), cell->encoded_size().value(),
                                  CellLocatorFlags::kBorrowed);
    if (!appended.has_value()) {
      return appended;
    }
    ++persisted;
  }
  if (persisted != page.cell_count_ || staged != page.staged_count_) {
    return std::unexpected(Corruption("B-tree page cells were not assembled completely"));
  }
  return {};
}

Status CellArray::AppendPage(ByteView page, PageNumber page_number) {
  if (page.size() != geometry_.page_size().value()) {
    return std::unexpected(Misuse("CellArray page image uses different page geometry"));
  }
  auto view = BtreePageView::Parse(page, page_number, geometry_);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  auto source_slot = SourceSlot(page);
  if (!source_slot.has_value()) {
    return std::unexpected(std::move(source_slot.error()));
  }
  for (std::size_t index = 0U; index < view->cell_count(); ++index) {
    auto offset = view->cell_offset(index);
    auto cell = view->cell(index);
    if (!offset.has_value()) {
      return std::unexpected(std::move(offset.error()));
    }
    if (!cell.has_value()) {
      return std::unexpected(std::move(cell.error()));
    }
    auto appended = AppendLocator(*source_slot, offset->value(), cell->encoded_size().value(),
                                  CellLocatorFlags::kBorrowed);
    if (!appended.has_value()) {
      return appended;
    }
  }
  return {};
}

Status CellArray::AppendBorrowed(ByteView source, std::size_t offset, std::size_t size) {
  auto slot = SourceSlot(source);
  if (!slot.has_value()) {
    return std::unexpected(std::move(slot.error()));
  }
  return AppendLocator(*slot, offset, size, CellLocatorFlags::kBorrowed);
}

Status CellArray::AppendCopied(ByteView cell) {
  if (cell.empty() || cell.size() > copied_cells_.size().value() - copied_size_) {
    return std::unexpected(TooLarge("copied B-tree cells exceed divider scratch"));
  }
  std::memmove(copied_cells_.mutable_view().data() + static_cast<std::ptrdiff_t>(copied_size_),
               cell.data(), cell.size());
  auto appended = AppendLocator(0U, copied_size_, cell.size(), CellLocatorFlags::kCopied);
  if (!appended.has_value()) {
    return appended;
  }
  copied_size_ += cell.size();
  return {};
}

Result<ByteView> CellArray::Cell(std::size_t index) const {
  if (index >= cells_.size()) {
    return std::unexpected(Misuse("CellArray index is out of range"));
  }
  const CellLocator& cell = cells_[index];
  ByteView source;
  if (cell.flags == static_cast<std::uint8_t>(CellLocatorFlags::kStaged)) {
    if (cell.source_slot >= staged_source_count_) {
      return std::unexpected(Corruption("CellArray staged source slot is invalid"));
    }
    source = staged_sources_[cell.source_slot];
  } else {
    if (cell.source_slot >= source_count_) {
      return std::unexpected(Corruption("CellArray source slot is invalid"));
    }
    source = sources_[cell.source_slot];
  }
  if (cell.offset > source.size() || cell.size > source.size() - cell.offset) {
    return std::unexpected(Corruption("CellArray cell exceeds its source"));
  }
  return source.subspan(cell.offset, cell.size);
}

Result<std::optional<std::size_t>> CellArray::OffsetWithinTarget(std::size_t index,
                                                                 ByteView target) const {
  auto cell = Cell(index);
  if (!cell.has_value()) {
    return std::unexpected(std::move(cell.error()));
  }
  const CellLocator& locator = cells_[index];
  const ByteView source = locator.flags == static_cast<std::uint8_t>(CellLocatorFlags::kStaged)
                              ? staged_sources_[locator.source_slot]
                              : sources_[locator.source_slot];
  if (source.data() == target.data() && source.size() == target.size()) {
    return std::optional<std::size_t>{locator.offset};
  }
  if (!cell->empty()) {
    const std::less<> before;
    const bool overlaps_target = before(cell->data(), target.data() + target.size()) &&
                                 before(target.data(), cell->data() + cell->size());
    if (overlaps_target) {
      return std::unexpected(Corruption("CellArray source partially overlaps the edited page"));
    }
  }
  return std::optional<std::size_t>{};
}

Result<std::uint8_t> CellArray::SourceSlot(ByteView source) {
  for (std::size_t index = 0U; index < source_count_; ++index) {
    if (sources_[index].data() == source.data() && sources_[index].size() == source.size()) {
      return static_cast<std::uint8_t>(index);
    }
  }
  if (source_count_ >= sources_.size()) {
    return std::unexpected(TooLarge("CellArray exceeded its bounded source count"));
  }
  sources_[source_count_] = source;
  return static_cast<std::uint8_t>(source_count_++);
}

Status CellArray::AppendStaged(ByteView cell) {
  if (cell.empty() || cell.size() > (std::numeric_limits<std::uint16_t>::max)()) {
    return std::unexpected(Misuse("CellArray staged cell is not representable"));
  }
  if (staged_source_count_ >= staged_sources_.size()) {
    return std::unexpected(TooLarge("CellArray exceeded SQLite's staged-cell bound"));
  }
  if (cells_.size() >= maximum_cells_) {
    return std::unexpected(TooLarge("CellArray exceeded SQLite's bounded cell count"));
  }
  const auto source_slot = static_cast<std::uint8_t>(staged_source_count_);
  staged_sources_[staged_source_count_++] = cell;
  try {
    cells_.push_back(CellLocator{
        .offset = 0U,
        .size = static_cast<std::uint16_t>(cell.size()),
        .source_slot = source_slot,
        .flags = static_cast<std::uint8_t>(CellLocatorFlags::kStaged),
    });
    return {};
  } catch (const std::bad_alloc&) {
    --staged_source_count_;
    staged_sources_[staged_source_count_] = {};
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    --staged_source_count_;
    staged_sources_[staged_source_count_] = {};
    return std::unexpected(Error::OutOfMemory());
  }
}

Status CellArray::AppendLocator(std::uint8_t source_slot, std::size_t offset, std::size_t size,
                                CellLocatorFlags flags) {
  if (flags == CellLocatorFlags::kStaged) {
    return std::unexpected(Misuse("staged CellArray locators use dedicated bounded sources"));
  }
  if (source_slot >= source_count_ || size == 0U ||
      size > (std::numeric_limits<std::uint16_t>::max)() ||
      offset > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(Misuse("CellArray locator is not representable"));
  }
  if (cells_.size() >= maximum_cells_) {
    return std::unexpected(TooLarge("CellArray exceeded SQLite's bounded cell count"));
  }
  const ByteView source = sources_[source_slot];
  if (offset > source.size() || size > source.size() - offset) {
    return std::unexpected(Corruption("CellArray locator exceeds its source"));
  }
  try {
    cells_.push_back(CellLocator{
        .offset = static_cast<std::uint32_t>(offset),
        .size = static_cast<std::uint16_t>(size),
        .source_slot = source_slot,
        .flags = static_cast<std::uint8_t>(flags),
    });
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<ByteView> CellArray::ResolveForTarget(std::size_t index, ByteView target,
                                             ByteView target_copy) const {
  if (target.size() != target_copy.size()) {
    return std::unexpected(Misuse("CellArray target copy has the wrong size"));
  }
  auto offset = OffsetWithinTarget(index, target);
  if (!offset.has_value()) {
    return std::unexpected(std::move(offset.error()));
  }
  if (offset->has_value()) {
    return target_copy.subspan(**offset, cells_[index].size);
  }
  return Cell(index);
}

Result<WritableCursor> WritableCursor::Open(MutationPageOwner& owner, PageNumber root_page,
                                            bool table) {
  const DatabaseHeader* header = owner.pager().header();
  if (header == nullptr) {
    return std::unexpected(Misuse("writable cursor requires an initialized database"));
  }
  auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  return Open(owner, root_page, table, *geometry);
}

Result<WritableCursor> WritableCursor::Open(MutationPageOwner& owner, PageNumber root_page,
                                            bool table, BtreePageGeometry geometry) {
  auto active = owner.CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (root_page.value() == 0U) {
    return std::unexpected(Misuse("writable cursor root page zero is invalid"));
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
  auto root = BtreePageView::Parse(frame->get().bytes(), root_page, geometry);
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
  return WritableCursor{owner, root_page, table, geometry, *root_slot};
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

Result<bool> WritableCursor::FirstTable() {
  if (!table_) {
    return std::unexpected(Misuse("table first requires a table B-tree cursor"));
  }
  auto reset = ResetToRoot();
  if (!reset.has_value()) {
    EnterFault();
    return std::unexpected(std::move(reset.error()));
  }
  if (state_ == WritableCursorState::kInvalid) {
    return false;
  }
  auto descended = DescendLeftmost();
  if (!descended.has_value()) {
    EnterFault();
    return std::unexpected(std::move(descended.error()));
  }
  return true;
}

Result<bool> WritableCursor::NextTable() {
  if (!table_) {
    return std::unexpected(Misuse("table next requires a table B-tree cursor"));
  }
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (state_ != WritableCursorState::kValid) {
    return std::unexpected(Misuse("table next requires a valid writable cursor"));
  }
  auto page = CurrentPage();
  if (!page.has_value()) {
    EnterFault();
    return std::unexpected(std::move(page.error()));
  }
  if (!page->is_leaf()) {
    EnterFault();
    return std::unexpected(Corruption("positioned table cursor is not on a leaf page"));
  }
  if (current_index_ + 1U < page->cell_count()) {
    ++current_index_;
    frames_[frame_count_ - 1U].child_index = current_index_;
    return true;
  }

  while (frame_count_ > 1U) {
    auto moved = MoveToParent();
    if (!moved.has_value()) {
      EnterFault();
      return std::unexpected(std::move(moved.error()));
    }
    auto parent = CurrentPage();
    if (!parent.has_value()) {
      EnterFault();
      return std::unexpected(std::move(parent.error()));
    }
    const std::size_t child_slot = current_index_;
    if (child_slot >= parent->cell_count()) {
      continue;
    }
    auto descended = Descend(child_slot + 1U, *parent);
    if (!descended.has_value()) {
      EnterFault();
      return std::unexpected(std::move(descended.error()));
    }
    auto leftmost = DescendLeftmost();
    if (!leftmost.has_value()) {
      EnterFault();
      return std::unexpected(std::move(leftmost.error()));
    }
    return true;
  }
  InvalidatePosition();
  return false;
}

Result<WritableTableRow> WritableCursor::CurrentTableRow(std::vector<std::byte>& scratch) {
  if (!table_) {
    return std::unexpected(Misuse("current table row requires a table B-tree cursor"));
  }
  auto active = CheckActive();
  if (!active.has_value()) {
    return std::unexpected(std::move(active.error()));
  }
  if (state_ != WritableCursorState::kValid) {
    return std::unexpected(Misuse("current table row requires a valid writable cursor"));
  }
  auto page = CurrentPage();
  if (!page.has_value()) {
    EnterFault();
    return std::unexpected(std::move(page.error()));
  }
  if (!page->is_leaf() || current_index_ >= page->cell_count()) {
    EnterFault();
    return std::unexpected(Corruption("current table row position is invalid"));
  }
  auto cell = page->cell(current_index_);
  if (!cell.has_value()) {
    EnterFault();
    return std::unexpected(std::move(cell.error()));
  }
  if (!cell->rowid().has_value()) {
    EnterFault();
    return std::unexpected(Corruption("current table row has no rowid"));
  }
  auto payload = ReadCellPayload(*cell, scratch);
  if (!payload.has_value()) {
    EnterFault();
    return std::unexpected(std::move(payload.error()));
  }
  return WritableTableRow{
      .rowid = *cell->rowid(),
      .payload = *payload,
  };
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

Status WritableCursor::Balance(MutableBtreePage page, BtreeWriteWorkspace& workspace) {
  auto active = CheckActive();
  if (!active.has_value()) {
    return active;
  }
  if (frame_count_ == 0U || page.owner_ != owner_ ||
      page.owner_slot_ != frames_[frame_count_ - 1U].owner_slot ||
      page.geometry_.page_size() != geometry_.page_size() ||
      page.geometry_.usable_size() != geometry_.usable_size() || page.is_table() != table_) {
    return std::unexpected(Misuse("balanced page does not match the writable cursor"));
  }
  auto current_frame = owner_->Frame(page.owner_slot_);
  if (!current_frame.has_value()) {
    return std::unexpected(std::move(current_frame.error()));
  }
  if (current_frame->get().page_number() != page.page_number_ ||
      !owner_->IsWritable(page.owner_slot_)) {
    return std::unexpected(Misuse("balanced page is not the cursor's writable page"));
  }

  std::optional<ByteBuffer> retained_parent_overflow;
  std::optional<MutableBtreePage> current;
  current.emplace(std::move(page));
  bool quick_called = false;
  bool deeper_called = false;
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &current, checkpoint](Error error) -> Status {
    if (current.has_value()) {
      current->ClearStagedCells();
    }
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  while (true) {
    if (!current.has_value()) {
      return fail(Internal("writable cursor balance lost its current page"));
    }
    if (current->staged_count_ == 0U &&
        current->free_bytes_ * 3U <= geometry_.usable_size().value() * 2U) {
      break;
    }

    if (frame_count_ == 1U) {
      if (current->staged_count_ == 0U) {
        break;
      }
      if (deeper_called) {
        return fail(Corruption("writable cursor deepened its root more than once"));
      }
      auto child = MutableBtreePage::BalanceDeeper(*current);
      if (!child.has_value()) {
        return fail(std::move(child.error()));
      }
      deeper_called = true;
      frames_[0].child_index = 0U;
      frames_[1] = Frame{
          .owner_slot = child->owner_slot_,
          .child_index = 0U,
      };
      frame_count_ = 2U;
      current_index_ = 0U;
      current.reset();
      current.emplace(std::move(*child));
      continue;
    }

    const std::size_t parent_frame_index = frame_count_ - 2U;
    const std::size_t parent_slot = frames_[parent_frame_index].owner_slot;
    const std::size_t child_index = frames_[parent_frame_index].child_index;
    auto promoted = owner_->Promote(parent_slot);
    if (!promoted.has_value()) {
      return fail(std::move(promoted.error()));
    }
    auto opened_parent = MutableBtreePage::Open(*owner_, parent_slot, geometry_);
    if (!opened_parent.has_value()) {
      return fail(std::move(opened_parent.error()));
    }
    MutableBtreePage parent = std::move(*opened_parent);

    const std::optional<StagedCell>& first_staged = current->staged_cells_[0];
    const bool quick = current->type_ == BtreePageType::kLeafTable &&
                       current->staged_count_ == 1U && first_staged.has_value() &&
                       first_staged->index == current->cell_count_ &&
                       parent.page_number_ != PageNumber{1} && parent.staged_count_ == 0U &&
                       parent.cell_count_ == child_index;
    Status balanced;
    std::optional<ByteBuffer> next_parent_overflow;
    if (quick) {
      if (quick_called) {
        parent.ClearStagedCells();
        return fail(Corruption("writable cursor used quick balance more than once"));
      }
      quick_called = true;
      balanced = MutableBtreePage::BalanceQuick(parent, *current, workspace);
    } else {
      try {
        next_parent_overflow.emplace(geometry_.page_size());
      } catch (const std::bad_alloc&) {
        parent.ClearStagedCells();
        return fail(Error::OutOfMemory());
      } catch (const std::length_error&) {
        parent.ClearStagedCells();
        return fail(Error::OutOfMemory());
      }
      balanced = MutableBtreePage::BalanceNonroot(parent, *current, child_index,
                                                  next_parent_overflow->mutable_view(), workspace,
                                                  frame_count_ == 2U);
    }
    if (!balanced.has_value()) {
      parent.ClearStagedCells();
      return fail(std::move(balanced.error()));
    }

    current->ClearStagedCells();
    current.reset();
    owner_->Release(frames_[frame_count_ - 1U].owner_slot);
    --frame_count_;
    current_index_ = frames_[frame_count_ - 1U].child_index;
    current.emplace(std::move(parent));

    if (next_parent_overflow.has_value()) {
      retained_parent_overflow.reset();
      retained_parent_overflow.emplace(std::move(*next_parent_overflow));
    }
  }

  current->ClearStagedCells();
  current_index_ = 0U;
  state_ = WritableCursorState::kInvalid;
  return {};
}

Status WritableCursor::InsertTable(std::int64_t rowid, ByteView payload, BtreeInsertMode mode,
                                   BtreeWriteWorkspace& workspace) {
  if (!table_) {
    return std::unexpected(Misuse("table insertion requires a table B-tree cursor"));
  }
  auto seek = SeekTable(rowid);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  if (seek->exact && mode == BtreeInsertMode::kInsertOnly) {
    return std::unexpected(Constraint("table rowid already exists"));
  }

  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return promoted;
  }
  auto opened = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage page = std::move(*opened);
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Status {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  if (seek->exact) {
    auto view = CurrentPage();
    if (!view.has_value()) {
      return fail(std::move(view.error()));
    }
    auto old_cell = view->cell(seek->insertion_index);
    if (!old_cell.has_value()) {
      return fail(std::move(old_cell.error()));
    }
    if (old_cell->payload_size().value() == payload.size()) {
      auto overwritten = page.OverwritePayload(seek->insertion_index, payload, workspace);
      if (!overwritten.has_value()) {
        return fail(std::move(overwritten.error()));
      }
      current_index_ = 0U;
      state_ = WritableCursorState::kInvalid;
      return {};
    }
  }

  auto formatted = FillTableLeafCell(*owner_, geometry_, workspace, rowid, payload);
  if (!formatted.has_value()) {
    return fail(std::move(formatted.error()));
  }
  return InsertFormattedCell(std::move(page), seek->insertion_index, seek->exact, formatted->bytes,
                             std::nullopt, workspace);
}

Status WritableCursor::ReplaceCurrentTable(ByteView payload, BtreeWriteWorkspace& workspace) {
  if (!table_) {
    return std::unexpected(Misuse("current table replacement requires a table B-tree cursor"));
  }
  auto current_page = CurrentPage();
  if (!current_page.has_value()) {
    EnterFault();
    return std::unexpected(std::move(current_page.error()));
  }
  if (state_ != WritableCursorState::kValid || !current_page->is_leaf() ||
      current_index_ >= current_page->cell_count()) {
    return std::unexpected(Misuse("current table replacement requires a valid row"));
  }
  auto current_cell = current_page->cell(current_index_);
  if (!current_cell.has_value()) {
    EnterFault();
    return std::unexpected(std::move(current_cell.error()));
  }
  if (!current_cell->rowid().has_value()) {
    EnterFault();
    return std::unexpected(Corruption("current table replacement row has no rowid"));
  }
  const std::int64_t rowid = *current_cell->rowid();
  const std::size_t index = current_index_;
  const std::size_t old_payload_size = current_cell->payload_size().value();

  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return promoted;
  }
  auto opened = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage page = std::move(*opened);
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Status {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  if (old_payload_size == payload.size()) {
    auto overwritten = page.OverwritePayload(index, payload, workspace);
    if (!overwritten.has_value()) {
      return fail(std::move(overwritten.error()));
    }
    return {};
  }

  auto formatted = FillTableLeafCell(*owner_, geometry_, workspace, rowid, payload);
  if (!formatted.has_value()) {
    return fail(std::move(formatted.error()));
  }
  auto inserted =
      InsertFormattedCell(std::move(page), index, true, formatted->bytes, std::nullopt, workspace);
  if (!inserted.has_value()) {
    return inserted;
  }
  auto sought = SeekTable(rowid);
  if (!sought.has_value()) {
    owner_->MarkRollbackRequiredAfter(sought.error().code(), checkpoint);
    return std::unexpected(std::move(sought.error()));
  }
  if (!sought->exact) {
    Error error = Corruption("replaced table rowid is missing");
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  }
  return {};
}

Status WritableCursor::InsertIndex(ByteView record, std::span<const SqlValue> key,
                                   std::span<const IndexColumnOrder> columns,
                                   RecordCodecOptions options, BtreeInsertMode mode,
                                   std::vector<std::byte>& seek_scratch,
                                   BtreeWriteWorkspace& workspace) {
  if (table_) {
    return std::unexpected(Misuse("index insertion requires an index B-tree cursor"));
  }
  if (key.empty() || key.size() != columns.size()) {
    return std::unexpected(Misuse("index insertion requires a complete comparison key"));
  }
  auto record_view = RecordView::Parse(record, options);
  if (!record_view.has_value()) {
    return std::unexpected(std::move(record_view.error()));
  }
  if (record_view->field_count() != key.size()) {
    return std::unexpected(Misuse("index record field count does not match its complete key"));
  }
  auto stored_comparison = CompareIndexRecord(*record_view, key, columns);
  if (!stored_comparison.has_value()) {
    return std::unexpected(std::move(stored_comparison.error()));
  }
  if (stored_comparison->ordering != std::weak_ordering::equivalent) {
    return std::unexpected(Misuse("index record does not match its comparison key"));
  }

  auto seek = SeekIndex(key, columns, options, seek_scratch);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  if (seek->exact && mode == BtreeInsertMode::kInsertOnly) {
    return std::unexpected(Constraint("index key already exists"));
  }

  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return promoted;
  }
  auto opened = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage page = std::move(*opened);
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Status {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  std::optional<PageNumber> left_child;
  if (seek->exact) {
    auto view = CurrentPage();
    if (!view.has_value()) {
      return fail(std::move(view.error()));
    }
    auto old_cell = view->cell(seek->insertion_index);
    if (!old_cell.has_value()) {
      return fail(std::move(old_cell.error()));
    }
    left_child = old_cell->left_child();
    if (old_cell->payload_size().value() == record.size()) {
      auto overwritten = page.OverwritePayload(seek->insertion_index, record, workspace);
      if (!overwritten.has_value()) {
        return fail(std::move(overwritten.error()));
      }
      current_index_ = 0U;
      state_ = WritableCursorState::kInvalid;
      return {};
    }
  } else if (!page.is_leaf()) {
    return fail(Corruption("new index entry did not resolve to a leaf page"));
  }

  auto formatted = FillIndexCell(*owner_, geometry_, workspace, record, page.type_, left_child);
  if (!formatted.has_value()) {
    return fail(std::move(formatted.error()));
  }
  return InsertFormattedCell(std::move(page), seek->insertion_index, seek->exact, formatted->bytes,
                             left_child, workspace);
}

Status WritableCursor::DeleteTable(std::int64_t rowid, BtreeWriteWorkspace& workspace) {
  if (!table_) {
    return std::unexpected(Misuse("table deletion requires a table B-tree cursor"));
  }
  auto seek = SeekTable(rowid);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  if (!seek->exact) {
    return std::unexpected(NotFound("table rowid does not exist"));
  }

  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return promoted;
  }
  auto opened = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage page = std::move(*opened);
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Status {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  auto view = CurrentPage();
  if (!view.has_value()) {
    return fail(std::move(view.error()));
  }
  auto cell = view->cell(seek->insertion_index);
  if (!cell.has_value()) {
    return fail(std::move(cell.error()));
  }
  auto cleared = ClearCellOverflow(*owner_, geometry_, *cell);
  if (!cleared.has_value()) {
    return fail(std::move(cleared.error()));
  }
  auto dropped = page.DropCell(seek->insertion_index);
  if (!dropped.has_value()) {
    return fail(std::move(dropped.error()));
  }
  return Balance(std::move(page), workspace);
}

Result<bool> WritableCursor::DeleteCurrentTableAndNext(BtreeWriteWorkspace& workspace) {
  if (!table_) {
    return std::unexpected(Misuse("current table deletion requires a table B-tree cursor"));
  }
  auto current_page = CurrentPage();
  if (!current_page.has_value()) {
    EnterFault();
    return std::unexpected(std::move(current_page.error()));
  }
  if (state_ != WritableCursorState::kValid || !current_page->is_leaf() ||
      current_index_ >= current_page->cell_count()) {
    return std::unexpected(Misuse("current table deletion requires a valid row"));
  }
  auto current_cell = current_page->cell(current_index_);
  if (!current_cell.has_value()) {
    EnterFault();
    return std::unexpected(std::move(current_cell.error()));
  }
  if (!current_cell->rowid().has_value()) {
    EnterFault();
    return std::unexpected(Corruption("current table deletion row has no rowid"));
  }
  const std::int64_t deleted_rowid = *current_cell->rowid();

  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return std::unexpected(std::move(promoted.error()));
  }
  auto opened = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage page = std::move(*opened);
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Result<bool> {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  auto view = CurrentPage();
  if (!view.has_value()) {
    return fail(std::move(view.error()));
  }
  auto cell = view->cell(current_index_);
  if (!cell.has_value()) {
    return fail(std::move(cell.error()));
  }
  auto cleared = ClearCellOverflow(*owner_, geometry_, *cell);
  if (!cleared.has_value()) {
    return fail(std::move(cleared.error()));
  }
  auto dropped = page.DropCell(current_index_);
  if (!dropped.has_value()) {
    return fail(std::move(dropped.error()));
  }
  auto balanced = Balance(std::move(page), workspace);
  if (!balanced.has_value()) {
    return std::unexpected(std::move(balanced.error()));
  }

  auto sought = SeekTable(deleted_rowid);
  if (!sought.has_value()) {
    return std::unexpected(std::move(sought.error()));
  }
  if (sought->exact) {
    EnterFault();
    return std::unexpected(Corruption("deleted table rowid remains present"));
  }
  if (state_ == WritableCursorState::kInvalid) {
    return false;
  }
  if (sought->comparison > 0) {
    return true;
  }
  return NextTable();
}

Status WritableCursor::DeleteIndex(std::span<const SqlValue> key,
                                   std::span<const IndexColumnOrder> columns,
                                   RecordCodecOptions options, std::vector<std::byte>& seek_scratch,
                                   BtreeWriteWorkspace& workspace) {
  if (table_) {
    return std::unexpected(Misuse("index deletion requires an index B-tree cursor"));
  }
  if (key.empty() || key.size() != columns.size()) {
    return std::unexpected(Misuse("index deletion requires a complete comparison key"));
  }
  auto seek = SeekIndex(key, columns, options, seek_scratch);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  if (!seek->exact) {
    return std::unexpected(NotFound("index key does not exist"));
  }
  auto matched_page = CurrentPage();
  if (!matched_page.has_value()) {
    EnterFault();
    return std::unexpected(std::move(matched_page.error()));
  }
  auto matched_cell = matched_page->cell(seek->insertion_index);
  if (!matched_cell.has_value()) {
    EnterFault();
    return std::unexpected(std::move(matched_cell.error()));
  }
  auto matched_payload = ReadCellPayload(*matched_cell, seek_scratch);
  if (!matched_payload.has_value()) {
    EnterFault();
    return std::unexpected(std::move(matched_payload.error()));
  }
  auto matched_record = RecordView::Parse(*matched_payload, options);
  if (!matched_record.has_value()) {
    EnterFault();
    return std::unexpected(std::move(matched_record.error()));
  }
  if (matched_record->field_count() != key.size()) {
    return std::unexpected(Misuse("index deletion key does not identify a complete record"));
  }

  const std::size_t original_depth = frame_count_;
  const std::size_t original_index = seek->insertion_index;
  const std::size_t original_slot = current_owner_slot();
  auto promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return promoted;
  }
  auto opened = MutableBtreePage::Open(*owner_, original_slot, geometry_);
  if (!opened.has_value()) {
    EnterFault();
    return std::unexpected(std::move(opened.error()));
  }
  MutableBtreePage original_page = std::move(*opened);
  std::optional<MutableBtreePage> leaf_page;
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &original_page, &leaf_page, original_slot,
                     checkpoint](Error error) -> Status {
    auto original_frame = owner_->Frame(original_slot);
    if (original_frame.has_value() &&
        original_frame->get().page_number() == original_page.page_number_) {
      original_page.ClearStagedCells();
    }
    if (leaf_page.has_value()) {
      leaf_page->ClearStagedCells();
    }
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  auto original_view = CurrentPage();
  if (!original_view.has_value()) {
    return fail(std::move(original_view.error()));
  }
  auto original_cell = original_view->cell(original_index);
  if (!original_cell.has_value()) {
    return fail(std::move(original_cell.error()));
  }
  if (original_page.is_leaf()) {
    auto cleared = ClearCellOverflow(*owner_, geometry_, *original_cell);
    if (!cleared.has_value()) {
      return fail(std::move(cleared.error()));
    }
    auto dropped = original_page.DropCell(original_index);
    if (!dropped.has_value()) {
      return fail(std::move(dropped.error()));
    }
    return Balance(std::move(original_page), workspace);
  }

  const std::optional<PageNumber> preserved_child = original_cell->left_child();
  if (!preserved_child.has_value()) {
    return fail(Corruption("interior index cell has no left child"));
  }
  auto descended = Descend(original_index, *original_view);
  if (!descended.has_value()) {
    return fail(std::move(descended.error()));
  }
  while (true) {
    auto page = CurrentPage();
    if (!page.has_value()) {
      return fail(std::move(page.error()));
    }
    if (page->is_leaf()) {
      break;
    }
    descended = Descend(page->cell_count(), *page);
    if (!descended.has_value()) {
      return fail(std::move(descended.error()));
    }
  }

  auto leaf_view = CurrentPage();
  if (!leaf_view.has_value()) {
    return fail(std::move(leaf_view.error()));
  }
  if (leaf_view->cell_count() == 0U) {
    return fail(Corruption("index predecessor leaf is empty"));
  }
  const std::size_t predecessor_index = leaf_view->cell_count() - 1U;
  auto predecessor_offset = leaf_view->cell_offset(predecessor_index);
  auto predecessor_cell = leaf_view->cell(predecessor_index);
  if (!predecessor_offset.has_value()) {
    return fail(std::move(predecessor_offset.error()));
  }
  if (!predecessor_cell.has_value()) {
    return fail(std::move(predecessor_cell.error()));
  }
  auto predecessor_frame = owner_->Frame(current_owner_slot());
  if (!predecessor_frame.has_value()) {
    return fail(std::move(predecessor_frame.error()));
  }
  const ByteView predecessor_bytes = predecessor_frame->get().bytes().subspan(
      predecessor_offset->value(), predecessor_cell->encoded_size().value());
  auto predecessor_size = IndexLeafUnpaddedCellSize(predecessor_bytes, geometry_);
  if (!predecessor_size.has_value()) {
    return fail(std::move(predecessor_size.error()));
  }
  const MutableByteView replacement_scratch = workspace.cell_scratch_with_prefix();
  const std::size_t replacement_size = sizeof(std::uint32_t) + *predecessor_size;
  if (replacement_size > replacement_scratch.size()) {
    return fail(Corruption("index predecessor cell exceeds retained scratch"));
  }
  Store32(replacement_scratch, 0U, preserved_child->value());
  std::memmove(replacement_scratch.data() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)),
               predecessor_bytes.data(), *predecessor_size);
  const MutableByteView replacement = replacement_scratch.first(replacement_size);

  auto cleared = ClearCellOverflow(*owner_, geometry_, *original_cell);
  if (!cleared.has_value()) {
    return fail(std::move(cleared.error()));
  }
  auto dropped = original_page.DropCell(original_index);
  if (!dropped.has_value()) {
    return fail(std::move(dropped.error()));
  }
  auto inserted = original_page.InsertCell(original_index, ByteView{replacement}, preserved_child,
                                           replacement, workspace);
  if (!inserted.has_value()) {
    return fail(std::move(inserted.error()));
  }

  promoted = PromoteCurrent();
  if (!promoted.has_value()) {
    return fail(std::move(promoted.error()));
  }
  auto opened_leaf = MutableBtreePage::Open(*owner_, current_owner_slot(), geometry_);
  if (!opened_leaf.has_value()) {
    return fail(std::move(opened_leaf.error()));
  }
  leaf_page.emplace(std::move(*opened_leaf));
  auto leaf_dropped = leaf_page->DropCell(predecessor_index);
  if (!leaf_dropped.has_value()) {
    return fail(std::move(leaf_dropped.error()));
  }
  current_index_ = predecessor_index;
  frames_[frame_count_ - 1U].child_index = predecessor_index;

  auto balanced = Balance(std::move(*leaf_page), workspace);
  if (!balanced.has_value()) {
    return fail(std::move(balanced.error()));
  }
  if (frame_count_ > original_depth) {
    while (frame_count_ > original_depth) {
      auto moved = MoveToParent();
      if (!moved.has_value()) {
        return fail(std::move(moved.error()));
      }
    }
    if (current_owner_slot() != original_slot) {
      return fail(Corruption("index delete lost the original cursor level"));
    }
    balanced = Balance(std::move(original_page), workspace);
    if (!balanced.has_value()) {
      return fail(std::move(balanced.error()));
    }
  }
  return {};
}

Status WritableCursor::InsertFormattedCell(MutableBtreePage page, std::size_t insertion_index,
                                           bool replacing, ByteView cell,
                                           std::optional<PageNumber> left_child,
                                           BtreeWriteWorkspace& workspace) {
  const std::uint64_t checkpoint = owner_->operation_checkpoint();
  const auto fail = [this, &page, checkpoint](Error error) -> Status {
    page.ClearStagedCells();
    owner_->MarkRollbackRequiredAfter(error.code(), checkpoint);
    EnterFault();
    return std::unexpected(std::move(error));
  };

  if (replacing) {
    auto bytes = page.Bytes();
    if (!bytes.has_value()) {
      return fail(std::move(bytes.error()));
    }
    auto view = BtreePageView::Parse(*bytes, page.page_number_, geometry_);
    if (!view.has_value()) {
      return fail(std::move(view.error()));
    }
    auto old_offset = view->cell_offset(insertion_index);
    auto old_cell = view->cell(insertion_index);
    if (!old_offset.has_value()) {
      return fail(std::move(old_offset.error()));
    }
    if (!old_cell.has_value()) {
      return fail(std::move(old_cell.error()));
    }

    if (old_cell->encoded_size().value() == cell.size() &&
        old_cell->local_payload().size() == old_cell->payload_size().value()) {
      auto valid = page.ValidateCellImage(cell, left_child);
      if (!valid.has_value()) {
        return fail(std::move(valid.error()));
      }
      owner_->NoteMutation();
      std::memmove(bytes->data() + static_cast<std::ptrdiff_t>(old_offset->value()), cell.data(),
                   cell.size());
      current_index_ = 0U;
      state_ = WritableCursorState::kInvalid;
      return {};
    }

    auto cleared = ClearCellOverflow(*owner_, geometry_, *old_cell);
    if (!cleared.has_value()) {
      return fail(std::move(cleared.error()));
    }
    auto dropped = page.DropCell(insertion_index);
    if (!dropped.has_value()) {
      return fail(std::move(dropped.error()));
    }
  }

  MutableByteView staged_copy;
  if (left_child.has_value()) {
    const MutableByteView scratch = workspace.cell_scratch_with_prefix();
    if (cell.data() != scratch.data() || cell.size() > scratch.size()) {
      return fail(Misuse("staged interior index cell is not retained in writable scratch"));
    }
    staged_copy = scratch.first(cell.size());
  }
  auto inserted = page.InsertCell(insertion_index, cell, left_child, staged_copy, workspace);
  if (!inserted.has_value()) {
    return fail(std::move(inserted.error()));
  }
  if (page.staged_count_ != 0U) {
    return Balance(std::move(page), workspace);
  }

  current_index_ = 0U;
  state_ = WritableCursorState::kInvalid;
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

Status WritableCursor::DescendLeftmost() {
  while (true) {
    auto page = CurrentPage();
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    if (page->is_leaf()) {
      if (page->cell_count() == 0U) {
        return std::unexpected(Corruption("nonempty table cursor reached an empty leaf"));
      }
      current_index_ = 0U;
      frames_[frame_count_ - 1U].child_index = 0U;
      state_ = WritableCursorState::kValid;
      return {};
    }
    auto descended = Descend(0U, *page);
    if (!descended.has_value()) {
      return descended;
    }
  }
}

void WritableCursor::InvalidatePosition() noexcept {
  ReleaseDescendants();
  current_index_ = 0U;
  state_ = WritableCursorState::kInvalid;
}

void WritableCursor::ReleaseDescendants() noexcept {
  while (frame_count_ > 1U) {
    owner_->Release(frames_[frame_count_ - 1U].owner_slot);
    --frame_count_;
  }
}

void WritableCursor::EnterFault() noexcept { state_ = WritableCursorState::kFault; }

}  // namespace modern_sqlite::btree_internal
