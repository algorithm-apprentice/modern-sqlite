#include "modern_sqlite/temporary_storage/temporary_storage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "temporary_storage_internal.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kMinimumPmaPageCount = 250;
constexpr std::size_t kMaximumPmaSize = 1U << 29U;
constexpr std::size_t kMergeRunCount = std::numeric_limits<std::size_t>::digits + 1U;
constexpr std::size_t kMergeFanIn = 16;

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

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Error Corruption(std::string_view message) noexcept {
  return MakeError(ErrorCode::kCorruption, message);
}

[[nodiscard]] Error Internal(std::string_view message) noexcept {
  return MakeError(ErrorCode::kInternal, message);
}

[[nodiscard]] bool IsValid(TemporaryStoreMode mode) noexcept {
  switch (mode) {
    case TemporaryStoreMode::kFile:
    case TemporaryStoreMode::kMemory:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(RecordSchemaFormat format) noexcept {
  switch (format) {
    case RecordSchemaFormat::kOne:
    case RecordSchemaFormat::kTwo:
    case RecordSchemaFormat::kThree:
    case RecordSchemaFormat::kFour:
      return true;
  }
  return false;
}

[[nodiscard]] bool IsValid(IndexSortDirection direction) noexcept {
  return direction == IndexSortDirection::kAscending ||
         direction == IndexSortDirection::kDescending;
}

[[nodiscard]] bool IsValid(IndexNullPlacement placement) noexcept {
  return placement == IndexNullPlacement::kFirst || placement == IndexNullPlacement::kLast;
}

[[nodiscard]] Result<void> ValidateRecordSorterDescriptor(
    const RecordSorterDescriptor& descriptor) {
  if (descriptor.field_count == 0 || descriptor.field_count > kMaximumRecordFieldCount ||
      descriptor.key_field_count == 0 || descriptor.key_field_count > descriptor.field_count ||
      descriptor.key_columns.size() != descriptor.key_field_count) {
    return std::unexpected(Misuse("record sorter descriptor has an invalid field shape"));
  }
  if (!IsValid(descriptor.record_options.schema_format)) {
    return std::unexpected(Misuse("record sorter descriptor has an invalid record format"));
  }
  for (const IndexColumnOrder& column : descriptor.key_columns) {
    if (!IsValid(column.direction()) || !IsValid(column.null_placement())) {
      return std::unexpected(Misuse("record sorter descriptor has invalid ordering metadata"));
    }
  }
  return {};
}

}  // namespace

namespace temporary_storage_detail {

struct StatementState;

struct RelationRoot {
  // A retired writer's comparison references may have expired. Reuse only
  // calls root_page() and Clear(), which do not inspect comparison metadata,
  // before replacing the writer with freshly borrowed columns.
  std::optional<IndexBtreeWriter> writer;
  std::unique_ptr<RelationRoot> next;
};

struct RelationArena {
  RelationArena(TemporaryFileLease owned_file_lease, std::unique_ptr<Pager> owned_pager,
                BtreeWriteSession owned_session) noexcept
      : file_lease(std::move(owned_file_lease)),
        pager(std::move(owned_pager)),
        session(std::move(owned_session)) {}

  ~RelationArena() {
    while (retired_roots != nullptr) {
      std::unique_ptr<RelationRoot> next = std::move(retired_roots->next);
      retired_roots = std::move(next);
    }
  }

  TemporaryFileLease file_lease;
  std::unique_ptr<Pager> pager;
  BtreeWriteSession session;
  std::unique_ptr<RelationRoot> retired_roots;
  std::size_t root_count = 0;
};

struct SorterExtentNode {
  std::uint64_t begin = 0;
  std::uint64_t size = 0;
  std::uint64_t sorter_ordinal = 0;
  std::uint64_t run_ordinal = 0;
  std::unique_ptr<SorterExtentNode> next;
};

struct SorterArena;

class SorterExtentLease final {
 public:
  SorterExtentLease(const SorterExtentLease&) = delete;
  SorterExtentLease& operator=(const SorterExtentLease&) = delete;
  SorterExtentLease(SorterExtentLease&& other) noexcept
      : arena_(std::exchange(other.arena_, nullptr)), extent_(std::move(other.extent_)) {}
  SorterExtentLease& operator=(SorterExtentLease&& other) noexcept;
  ~SorterExtentLease();

  [[nodiscard]] bool valid() const noexcept { return arena_ != nullptr && extent_ != nullptr; }
  [[nodiscard]] File& file() const noexcept;
  [[nodiscard]] std::uint64_t begin() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;

 private:
  friend struct SorterArena;

  SorterExtentLease(SorterArena& arena, std::unique_ptr<SorterExtentNode> extent) noexcept
      : arena_(&arena), extent_(std::move(extent)) {}

  void Release() noexcept;

  SorterArena* arena_ = nullptr;
  std::unique_ptr<SorterExtentNode> extent_;
};

struct SorterArena {
  SorterArena(TemporaryFileLease owned_file_lease, std::unique_ptr<File> owned_file) noexcept
      : file_lease(std::move(owned_file_lease)), file(std::move(owned_file)) {}

  ~SorterArena() {
    if (live_extents != 0U) {
      std::terminate();
    }
    while (free_extents != nullptr) {
      std::unique_ptr<SorterExtentNode> next = std::move(free_extents->next);
      free_extents = std::move(next);
    }
  }

  [[nodiscard]] Result<SorterExtentLease> Allocate(std::uint64_t size, std::uint64_t sorter_ordinal,
                                                   std::uint64_t run_ordinal) {
    if (size == 0U) {
      return std::unexpected(Misuse("temporary sorter extent cannot be empty"));
    }
    std::unique_ptr<SorterExtentNode>* link = &free_extents;
    while (*link != nullptr && (*link)->size < size) {
      link = &((*link)->next);
    }

    std::unique_ptr<SorterExtentNode> extent;
    if (*link != nullptr && (*link)->size == size) {
      extent = std::move(*link);
      *link = std::move(extent->next);
      extent->next.reset();
      extent->sorter_ordinal = sorter_ordinal;
      extent->run_ordinal = run_ordinal;
    } else if (*link != nullptr) {
      try {
        extent = std::make_unique<SorterExtentNode>(SorterExtentNode{
            .begin = (*link)->begin,
            .size = size,
            .sorter_ordinal = sorter_ordinal,
            .run_ordinal = run_ordinal,
            .next = nullptr,
        });
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      }
      (*link)->begin += size;
      (*link)->size -= size;
      ++extent_node_count;
    } else {
      if (size > std::numeric_limits<std::uint64_t>::max() - high_water) {
        return std::unexpected(TooLarge("temporary sorter file extent overflows"));
      }
      try {
        extent = std::make_unique<SorterExtentNode>(SorterExtentNode{
            .begin = high_water,
            .size = size,
            .sorter_ordinal = sorter_ordinal,
            .run_ordinal = run_ordinal,
            .next = nullptr,
        });
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      }
      high_water += size;
      ++extent_node_count;
    }
    ++live_extents;
    return SorterExtentLease{*this, std::move(extent)};
  }

  void Release(std::unique_ptr<SorterExtentNode> extent) noexcept {
    if (extent == nullptr || live_extents == 0U) {
      std::terminate();
    }
    extent->sorter_ordinal = 0;
    extent->run_ordinal = 0;
    SorterExtentNode* previous = nullptr;
    std::unique_ptr<SorterExtentNode>* link = &free_extents;
    while (*link != nullptr && (*link)->begin < extent->begin) {
      previous = link->get();
      link = &((*link)->next);
    }
    extent->next = std::move(*link);
    *link = std::move(extent);
    SorterExtentNode* current = link->get();

    if (current->next != nullptr && current->begin + current->size == current->next->begin) {
      std::unique_ptr<SorterExtentNode> next = std::move(current->next);
      current->size += next->size;
      current->next = std::move(next->next);
      --extent_node_count;
    }
    if (previous != nullptr && previous->begin + previous->size == current->begin) {
      previous->size += current->size;
      previous->next = std::move(current->next);
      --extent_node_count;
    }
    --live_extents;

    while (free_extents != nullptr) {
      std::unique_ptr<SorterExtentNode>* tail_link = &free_extents;
      while ((*tail_link)->next != nullptr) {
        tail_link = &((*tail_link)->next);
      }
      const SorterExtentNode* tail = tail_link->get();
      if (tail->begin + tail->size != high_water) {
        break;
      }
      high_water = tail->begin;
      tail_link->reset();
      --extent_node_count;
    }
  }

  TemporaryFileLease file_lease;
  std::unique_ptr<File> file;
  std::unique_ptr<SorterExtentNode> free_extents;
  std::uint64_t high_water = 0;
  std::size_t live_extents = 0;
  std::size_t extent_node_count = 0;
};

SorterExtentLease& SorterExtentLease::operator=(SorterExtentLease&& other) noexcept {
  if (this != &other) {
    Release();
    arena_ = std::exchange(other.arena_, nullptr);
    extent_ = std::move(other.extent_);
  }
  return *this;
}

SorterExtentLease::~SorterExtentLease() { Release(); }

File& SorterExtentLease::file() const noexcept {
  if (!valid() || arena_->file == nullptr) {
    std::terminate();
  }
  return *arena_->file;
}

std::uint64_t SorterExtentLease::begin() const noexcept {
  if (!valid()) {
    std::terminate();
  }
  return extent_->begin;
}

std::uint64_t SorterExtentLease::size() const noexcept {
  if (!valid()) {
    std::terminate();
  }
  return extent_->size;
}

void SorterExtentLease::Release() noexcept {
  if (arena_ != nullptr) {
    arena_->Release(std::move(extent_));
    arena_ = nullptr;
  }
}

struct MemoryRegistration {
  ~MemoryRegistration();

  // All links are borrowed; the account owns this registration.
  StatementState* state = nullptr;
  TemporaryStorageSpillTarget* target = nullptr;
  MemoryRegistration* previous = nullptr;
  MemoryRegistration* next = nullptr;
  std::size_t retained_bytes = 0;
  std::uint64_t ordinal = 0;
  bool spilling = false;
};

struct StatementState {
  StatementState(const TemporaryStorageFactory& owning_factory, ByteCount memory_limit) noexcept
      : factory(&owning_factory),
        budget(memory_limit.value()),
        file_spill(owning_factory.file_spill_enabled()) {}

  [[nodiscard]] ByteCount SorterPageSize() const noexcept { return factory->pager_->page_size(); }

  ~StatementState() {
    if (owners != nullptr || owner_count != 0U || live_relation_roots != 0U ||
        (sorter_arena != nullptr && sorter_arena->live_extents != 0U)) {
      std::terminate();
    }
    sorter_arena.reset();
    relation_arena.reset();
    if (live_files != 0U) {
      std::terminate();
    }
  }

  [[nodiscard]] Result<TemporaryMemoryAccount> Register(TemporaryStorageSpillTarget* target) {
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (next_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("temporary memory owner ordinal is exhausted"));
    }
    if (owner_count == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("temporary memory owner registry is exhausted"));
    }
    try {
      auto registration = std::make_unique<MemoryRegistration>();
      registration->state = this;
      registration->target = target;
      registration->ordinal = next_ordinal++;
      registration->next = owners;
      if (owners != nullptr) {
        owners->previous = registration.get();
      }
      owners = registration.get();
      ++owner_count;
      return TemporaryMemoryAccount{std::move(registration)};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  void Unregister(MemoryRegistration& registration) noexcept {
    if (registration.state != this || registration.retained_bytes > retained) {
      std::terminate();
    }
    retained -= registration.retained_bytes;
    registration.retained_bytes = 0;
    if (registration.previous != nullptr) {
      registration.previous->next = registration.next;
    } else {
      owners = registration.next;
    }
    if (registration.next != nullptr) {
      registration.next->previous = registration.previous;
    }
    registration.previous = nullptr;
    registration.next = nullptr;
    registration.state = nullptr;
    if (owner_count == 0U) {
      std::terminate();
    }
    --owner_count;
  }

  [[nodiscard]] Status Reserve(MemoryRegistration& registration, std::size_t amount) {
    if (registration.state != this) {
      return std::unexpected(Misuse("temporary memory account is detached"));
    }
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (amount == 0U) {
      return {};
    }
    if (amount > std::numeric_limits<std::size_t>::max() - registration.retained_bytes ||
        amount > std::numeric_limits<std::size_t>::max() - retained) {
      return std::unexpected(TooLarge("temporary memory accounting overflowed"));
    }

    while (retained > budget || amount > budget - retained) {
      if (!file_spill) {
        return std::unexpected(TooLarge("statement temporary memory budget exceeded"));
      }
      MemoryRegistration* selected = nullptr;
      std::size_t selected_reclaimable = 0;
      for (MemoryRegistration* candidate = owners; candidate != nullptr;
           candidate = candidate->next) {
        if (candidate->target == nullptr || candidate->spilling) {
          continue;
        }
        const std::size_t reclaimable = candidate->target->reclaimable_bytes().value();
        if (reclaimable > candidate->retained_bytes) {
          failed = true;
          return std::unexpected(Internal("temporary spill target exceeds its retained memory"));
        }
        if (reclaimable == 0U) {
          continue;
        }
        if (selected == nullptr || reclaimable > selected_reclaimable ||
            (reclaimable == selected_reclaimable && candidate->ordinal < selected->ordinal)) {
          selected = candidate;
          selected_reclaimable = reclaimable;
        }
      }
      if (selected == nullptr) {
        return std::unexpected(TooLarge("statement temporary memory budget exceeded"));
      }

      const std::size_t before = retained;
      selected->spilling = true;
      Status spilled = selected->target->SpillForPressure();
      selected->spilling = false;
      if (!spilled.has_value()) {
        failed = true;
        return std::unexpected(std::move(spilled.error()));
      }
      if (failed) {
        return std::unexpected(Internal("statement temporary storage failed during spill"));
      }
      if (retained >= before) {
        failed = true;
        return std::unexpected(Internal("temporary pressure spill released no memory"));
      }
    }

    registration.retained_bytes += amount;
    retained += amount;
    peak_retained = std::max(peak_retained, retained);
    MODERN_SQLITE_RECORD_AGGREGATE_MAXIMUM(instrumentation::AggregateWorkMetric::RetainedBytesPeak,
                                           retained);
    return {};
  }

  [[nodiscard]] Status Release(MemoryRegistration& registration, std::size_t amount) noexcept {
    if (registration.state != this) {
      return std::unexpected(Misuse("temporary memory account is detached"));
    }
    if (amount > registration.retained_bytes || amount > retained) {
      return std::unexpected(Misuse("temporary memory release exceeds retained bytes"));
    }
    registration.retained_bytes -= amount;
    retained -= amount;
    return {};
  }

  [[nodiscard]] Status ReserveWorkingSet(MemoryRegistration& registration, std::size_t amount) {
    if (registration.state != this) {
      return std::unexpected(Misuse("temporary memory account is detached"));
    }
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (!file_spill) {
      return std::unexpected(TooLarge("temporary working set exceeds the memory-mode budget"));
    }
    if (amount > std::numeric_limits<std::size_t>::max() - registration.retained_bytes ||
        amount > std::numeric_limits<std::size_t>::max() - retained) {
      return std::unexpected(TooLarge("temporary working-set accounting overflowed"));
    }
    registration.retained_bytes += amount;
    retained += amount;
    peak_retained = std::max(peak_retained, retained);
    MODERN_SQLITE_RECORD_AGGREGATE_MAXIMUM(
        instrumentation::AggregateWorkMetric::WorkingSetBytesPeak, retained);
    return {};
  }

  [[nodiscard]] Result<TemporaryFileLease> AcquireFile() {
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (!file_spill) {
      return std::unexpected(Misuse("temporary files are disabled in memory mode"));
    }
    if (live_files >= 2U) {
      return std::unexpected(TooLarge("statement temporary file limit exceeded"));
    }
    ++live_files;
    peak_live_files = std::max(peak_live_files, live_files);
    MODERN_SQLITE_RECORD_AGGREGATE_MAXIMUM(instrumentation::AggregateWorkMetric::LiveFilesPeak,
                                           live_files);
    MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(
        instrumentation::AggregateWorkMetric::FileAcquisitions);
    return TemporaryFileLease{*this};
  }

  void ReleaseFile() noexcept {
    if (live_files == 0U) {
      std::terminate();
    }
    --live_files;
  }

  [[nodiscard]] Result<RelationArena*> EnsureRelationArena() {
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (relation_arena != nullptr) {
      return relation_arena.get();
    }

    auto file_lease = AcquireFile();
    if (!file_lease.has_value()) {
      failed = true;
      return std::unexpected(std::move(file_lease.error()));
    }
    const std::size_t page_size = factory->pager_->page_size().value();
    const std::size_t threshold_pages = 1U + (budget - 1U) / page_size;
    const std::size_t main_cache_pages =
        std::max<std::size_t>(1U, factory->pager_->cache_capacity_pages());
    const std::size_t cache_pages =
        std::max<std::size_t>(1U, std::min(main_cache_pages, threshold_pages));
    auto pager =
        Pager::OpenEphemeral(*factory->vfs_, PagerOptions{
                                                 .empty_database_page_size = ByteCount{page_size},
                                                 .cache_capacity_pages = cache_pages,
                                             });
    if (!pager.has_value()) {
      failed = true;
      return std::unexpected(std::move(pager.error()));
    }
    auto begun_read = (*pager)->BeginRead();
    if (!begun_read.has_value()) {
      failed = true;
      return std::unexpected(std::move(begun_read.error()));
    }
    auto begun_write = (*pager)->BeginWrite();
    if (!begun_write.has_value()) {
      failed = true;
      return std::unexpected(std::move(begun_write.error()));
    }
    auto session = BtreeWriteSession::Open(**pager);
    if (!session.has_value()) {
      failed = true;
      return std::unexpected(std::move(session.error()));
    }
    auto initialized = session->InitializeDatabase();
    if (!initialized.has_value()) {
      failed = true;
      return std::unexpected(std::move(initialized.error()));
    }
    try {
      relation_arena = std::make_unique<RelationArena>(std::move(*file_lease), std::move(*pager),
                                                       std::move(*session));
      return relation_arena.get();
    } catch (const std::bad_alloc&) {
      failed = true;
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      failed = true;
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<std::unique_ptr<RelationRoot>> AcquireRelationRoot(
      std::span<const IndexColumnOrder> columns) {
    auto arena = EnsureRelationArena();
    if (!arena.has_value()) {
      return std::unexpected(std::move(arena.error()));
    }
    if (live_relation_roots == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("live temporary relation root count is exhausted"));
    }

    std::unique_ptr<RelationRoot> root;
    if ((*arena)->retired_roots != nullptr) {
      root = std::move((*arena)->retired_roots);
      (*arena)->retired_roots = std::move(root->next);
      if (!root->writer.has_value()) {
        std::terminate();
      }
      auto cleared = root->writer->Clear();
      if (!cleared.has_value()) {
        failed = true;
        root->next = std::move((*arena)->retired_roots);
        (*arena)->retired_roots = std::move(root);
        return std::unexpected(std::move(cleared.error()));
      }
      const PageNumber root_page = root->writer->root_page();
      auto reopened = (*arena)->session.OpenIndexBtree(root_page, columns);
      if (!reopened.has_value()) {
        failed = true;
        root->next = std::move((*arena)->retired_roots);
        (*arena)->retired_roots = std::move(root);
        return std::unexpected(std::move(reopened.error()));
      }
      root->writer.emplace(std::move(*reopened));
    } else {
      if ((*arena)->root_count == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(TooLarge("temporary relation root count is exhausted"));
      }
      try {
        root = std::make_unique<RelationRoot>();
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      }
      auto created = (*arena)->session.CreateIndexBtree(columns);
      if (!created.has_value()) {
        failed = true;
        return std::unexpected(std::move(created.error()));
      }
      if ((*arena)->root_count == 0U && created->root_page() != PageNumber{2}) {
        failed = true;
        return std::unexpected(Internal("first temporary relation root is not page two"));
      }
      root->writer.emplace(std::move(*created));
      ++(*arena)->root_count;
    }
    ++live_relation_roots;
    return root;
  }

  void RetireRelationRoot(std::unique_ptr<RelationRoot> root) noexcept {
    if (root == nullptr || relation_arena == nullptr || live_relation_roots == 0U) {
      std::terminate();
    }
    root->next = std::move(relation_arena->retired_roots);
    relation_arena->retired_roots = std::move(root);
    --live_relation_roots;
  }

  [[nodiscard]] Result<std::uint64_t> AllocateSorterOrdinal() {
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (next_sorter_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("temporary sorter ordinal is exhausted"));
    }
    return next_sorter_ordinal++;
  }

  [[nodiscard]] Result<std::unique_ptr<File>> OpenSorterFile() const {
    try {
      auto opened = factory->vfs_->Open(std::nullopt, FileOpenOptions{
                                                          .kind = FileKind::kTemporaryJournal,
                                                          .access = FileAccessMode::kReadWrite,
                                                          .create = true,
                                                          .exclusive_create = true,
                                                          .delete_on_close = true,
                                                      });
      if (!opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      return std::move(opened->file);
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<SorterArena*> EnsureSorterArena() {
    if (failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    if (sorter_arena != nullptr) {
      return sorter_arena.get();
    }
    auto file_lease = AcquireFile();
    if (!file_lease.has_value()) {
      failed = true;
      return std::unexpected(std::move(file_lease.error()));
    }
    auto file = OpenSorterFile();
    if (!file.has_value()) {
      failed = true;
      return std::unexpected(std::move(file.error()));
    }
    try {
      sorter_arena = std::make_unique<SorterArena>(std::move(*file_lease), std::move(*file));
      return sorter_arena.get();
    } catch (const std::bad_alloc&) {
      failed = true;
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      failed = true;
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<SorterExtentLease> AllocateSorterExtent(std::uint64_t size,
                                                               std::uint64_t sorter_ordinal,
                                                               std::uint64_t run_ordinal) {
    auto arena = EnsureSorterArena();
    if (!arena.has_value()) {
      return std::unexpected(std::move(arena.error()));
    }
    auto extent = (*arena)->Allocate(size, sorter_ordinal, run_ordinal);
    if (!extent.has_value()) {
      failed = true;
      return std::unexpected(std::move(extent.error()));
    }
    return extent;
  }

  [[nodiscard]] std::size_t OverheadBytes() const noexcept {
    std::size_t overhead = sizeof(StatementState);
    const auto add = [&overhead](std::size_t count, std::size_t size) {
      if (count > (std::numeric_limits<std::size_t>::max() - overhead) / size) {
        overhead = std::numeric_limits<std::size_t>::max();
        return;
      }
      overhead += count * size;
    };
    add(owner_count, sizeof(MemoryRegistration));
    if (relation_arena != nullptr) {
      add(1U, sizeof(RelationArena));
      add(relation_arena->root_count, sizeof(RelationRoot));
    }
    if (sorter_arena != nullptr) {
      add(1U, sizeof(SorterArena));
      add(sorter_arena->extent_node_count, sizeof(SorterExtentNode));
    }
    return overhead;
  }

  // The session-owned factory outlives statement storage.
  const TemporaryStorageFactory* factory;
  std::size_t budget;
  bool file_spill;
  bool failed = false;
  std::size_t retained = 0;
  std::size_t peak_retained = 0;
  std::size_t live_files = 0;
  std::size_t peak_live_files = 0;
  std::size_t owner_count = 0;
  std::size_t live_relation_roots = 0;
  std::uint64_t next_ordinal = 0;
  std::uint64_t next_sorter_ordinal = 0;
  MemoryRegistration* owners = nullptr;
  std::unique_ptr<RelationArena> relation_arena;
  std::unique_ptr<SorterArena> sorter_arena;
};

MemoryRegistration::~MemoryRegistration() {
  if (state != nullptr) {
    state->Unregister(*this);
  }
}

struct RelationRootLease::Impl {
  ~Impl() {
    if (root != nullptr) {
      state->RetireRelationRoot(std::move(root));
    }
  }

  // The statement state owns the arena and outlives every root lease.
  StatementState* state = nullptr;
  std::unique_ptr<RelationRoot> root;
};

}  // namespace temporary_storage_detail

TemporaryMemoryAccount::TemporaryMemoryAccount(
    std::unique_ptr<temporary_storage_detail::MemoryRegistration> registration) noexcept
    : registration_(std::move(registration)) {}

TemporaryMemoryAccount::TemporaryMemoryAccount(TemporaryMemoryAccount&&) noexcept = default;

TemporaryMemoryAccount& TemporaryMemoryAccount::operator=(TemporaryMemoryAccount&&) noexcept =
    default;

TemporaryMemoryAccount::~TemporaryMemoryAccount() = default;

bool TemporaryMemoryAccount::valid() const noexcept {
  return registration_ != nullptr && registration_->state != nullptr;
}

ByteCount TemporaryMemoryAccount::retained_bytes() const noexcept {
  return ByteCount{valid() ? registration_->retained_bytes : 0U};
}

Status TemporaryMemoryAccount::Reserve(ByteCount amount) {
  if (!valid()) {
    return std::unexpected(Misuse("temporary memory account is invalid"));
  }
  return registration_->state->Reserve(*registration_, amount.value());
}

Status TemporaryMemoryAccount::ReserveWorkingSet(ByteCount amount) {
  if (!valid()) {
    return std::unexpected(Misuse("temporary memory account is invalid"));
  }
  return registration_->state->ReserveWorkingSet(*registration_, amount.value());
}

Status TemporaryMemoryAccount::Release(ByteCount amount) {
  if (!valid()) {
    return std::unexpected(Misuse("temporary memory account is invalid"));
  }
  return registration_->state->Release(*registration_, amount.value());
}

TemporaryFileLease::TemporaryFileLease(temporary_storage_detail::StatementState& state) noexcept
    : state_(&state) {}

TemporaryFileLease::TemporaryFileLease(TemporaryFileLease&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

TemporaryFileLease& TemporaryFileLease::operator=(TemporaryFileLease&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::exchange(other.state_, nullptr);
  }
  return *this;
}

TemporaryFileLease::~TemporaryFileLease() { Release(); }

bool TemporaryFileLease::valid() const noexcept { return state_ != nullptr; }

void TemporaryFileLease::Release() noexcept {
  if (state_ != nullptr) {
    state_->ReleaseFile();
    state_ = nullptr;
  }
}

temporary_storage_detail::RelationRootLease::RelationRootLease(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

temporary_storage_detail::RelationRootLease::RelationRootLease(RelationRootLease&&) noexcept =
    default;

temporary_storage_detail::RelationRootLease& temporary_storage_detail::RelationRootLease::operator=(
    RelationRootLease&&) noexcept = default;

temporary_storage_detail::RelationRootLease::~RelationRootLease() = default;

bool temporary_storage_detail::RelationRootLease::valid() const noexcept {
  return impl_ != nullptr && impl_->root != nullptr;
}

Pager& temporary_storage_detail::RelationRootLease::pager() const noexcept {
  if (!valid() || impl_->state->relation_arena == nullptr) {
    std::terminate();
  }
  return *impl_->state->relation_arena->pager;
}

IndexBtreeWriter& temporary_storage_detail::RelationRootLease::writer() noexcept {
  if (!valid() || !impl_->root->writer.has_value()) {
    std::terminate();
  }
  return *impl_->root->writer;
}

PageNumber temporary_storage_detail::RelationRootLease::root_page() const noexcept {
  if (!valid() || !impl_->root->writer.has_value()) {
    std::terminate();
  }
  return impl_->root->writer->root_page();
}

void temporary_storage_detail::RelationRootLease::MarkStatementFailed() noexcept {
  if (!valid()) {
    std::terminate();
  }
  impl_->state->failed = true;
}

Result<temporary_storage_detail::RelationRootLease>
temporary_storage_detail::RelationArenaAccess::Acquire(StatementState& state,
                                                       std::span<const IndexColumnOrder> columns) {
  try {
    auto impl = std::make_unique<RelationRootLease::Impl>();
    impl->state = &state;
    auto root = state.AcquireRelationRoot(columns);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    impl->root = std::move(*root);
    return RelationRootLease{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

temporary_storage_detail::StatementState& temporary_storage_detail::StatementStorageAccess::State(
    StatementTemporaryStorage& storage) noexcept {
  if (storage.state_ == nullptr) {
    std::terminate();
  }
  return *storage.state_;
}

bool temporary_storage_detail::StatementStorageAccess::Failed(
    const StatementState& state) noexcept {
  return state.failed;
}

StatementTemporaryStorage::StatementTemporaryStorage(
    std::unique_ptr<temporary_storage_detail::StatementState> state) noexcept
    : state_(std::move(state)) {}

StatementTemporaryStorage::StatementTemporaryStorage(StatementTemporaryStorage&&) noexcept =
    default;

StatementTemporaryStorage::~StatementTemporaryStorage() = default;

bool StatementTemporaryStorage::valid() const noexcept { return state_ != nullptr; }

bool StatementTemporaryStorage::failed() const noexcept {
  return state_ != nullptr && state_->failed;
}

bool StatementTemporaryStorage::file_spill_enabled() const noexcept {
  return state_ != nullptr && state_->file_spill;
}

ByteCount StatementTemporaryStorage::memory_budget() const noexcept {
  return ByteCount{state_ == nullptr ? 0U : state_->budget};
}

ByteCount StatementTemporaryStorage::retained_bytes() const noexcept {
  return ByteCount{state_ == nullptr ? 0U : state_->retained};
}

ByteCount StatementTemporaryStorage::peak_retained_bytes() const noexcept {
  return ByteCount{state_ == nullptr ? 0U : state_->peak_retained};
}

ByteCount StatementTemporaryStorage::overhead_bytes() const noexcept {
  return ByteCount{state_ == nullptr ? 0U : state_->OverheadBytes()};
}

std::size_t StatementTemporaryStorage::live_file_count() const noexcept {
  return state_ == nullptr ? 0U : state_->live_files;
}

std::size_t StatementTemporaryStorage::peak_live_file_count() const noexcept {
  return state_ == nullptr ? 0U : state_->peak_live_files;
}

Result<TemporaryMemoryAccount> StatementTemporaryStorage::RegisterMemoryOwner() {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return state_->Register(nullptr);
}

Result<TemporaryMemoryAccount> StatementTemporaryStorage::RegisterMemoryOwner(
    TemporaryStorageSpillTarget& target) {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return state_->Register(&target);
}

Result<TemporaryFileLease> StatementTemporaryStorage::AcquireFileLease() {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return state_->AcquireFile();
}

struct RecordSorter::Impl final : TemporaryStorageSpillTarget {
  struct Entry {
    Entry(ByteBuffer owned_record, RecordView record_view, std::size_t owned_bytes) noexcept
        : record(std::move(owned_record)), view(record_view), memory_bytes(owned_bytes) {}

    ByteBuffer record;
    RecordView view;
    std::size_t memory_bytes;
    std::unique_ptr<Entry> next;
  };

  using Run = std::unique_ptr<Entry>;

  struct PmaRun {
    PmaRun(temporary_storage_detail::SorterExtentLease owned_extent,
           std::uint64_t run_ordinal) noexcept
        : extent(std::move(owned_extent)), ordinal(run_ordinal) {}

    temporary_storage_detail::SorterExtentLease extent;
    std::uint64_t ordinal;
    std::unique_ptr<PmaRun> next;
  };

  static_assert(sizeof(PmaRun) <= sizeof(Entry));

  enum class PmaMemoryUse : std::uint8_t {
    Drain,
    Spill,
  };

  [[nodiscard]] static Status ReservePmaBytes(TemporaryMemoryAccount& account, ByteCount amount,
                                              PmaMemoryUse use) {
    if (use == PmaMemoryUse::Spill) {
      return account.ReserveWorkingSet(amount);
    }
    auto reserved = account.Reserve(amount);
    if (reserved.has_value() || reserved.error().code() != ErrorCode::kTooLarge) {
      return reserved;
    }
    return account.ReserveWorkingSet(amount);
  }

  static void ReleasePmaBytes(TemporaryMemoryAccount& account, std::size_t amount) noexcept {
    const auto released = account.Release(ByteCount{amount});
    if (!released.has_value()) {
      std::terminate();
    }
  }

  [[nodiscard]] static Result<ByteBuffer> AllocatePmaBuffer(TemporaryMemoryAccount& account,
                                                            ByteCount size, PmaMemoryUse use) {
    auto reserved = ReservePmaBytes(account, size, use);
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }
    ByteBuffer buffer;
    try {
      buffer = ByteBuffer{size};
    } catch (const std::bad_alloc&) {
      ReleasePmaBytes(account, size.value());
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      ReleasePmaBytes(account, size.value());
      return std::unexpected(Error::OutOfMemory());
    }
    const std::size_t actual_bytes = buffer.capacity().value();
    if (actual_bytes > size.value()) {
      auto additional = ReservePmaBytes(account, ByteCount{actual_bytes - size.value()}, use);
      if (!additional.has_value()) {
        buffer = ByteBuffer{};
        ReleasePmaBytes(account, size.value());
        return std::unexpected(std::move(additional.error()));
      }
    }
    return buffer;
  }

  class PmaWriter final {
   public:
    PmaWriter(const PmaWriter&) = delete;
    PmaWriter& operator=(const PmaWriter&) = delete;
    PmaWriter(PmaWriter&& other) noexcept
        : file_(other.file_),
          account_(std::exchange(other.account_, nullptr)),
          buffer_(std::move(other.buffer_)),
          offset_(other.offset_),
          limit_(other.limit_),
          buffer_begin_(other.buffer_begin_),
          buffer_end_(other.buffer_end_) {}
    PmaWriter& operator=(PmaWriter&&) = delete;
    ~PmaWriter() { ReleaseBuffer(); }

    [[nodiscard]] static Result<PmaWriter> Create(
        const temporary_storage_detail::SorterExtentLease& extent, TemporaryMemoryAccount& account,
        ByteCount page_size) {
      auto buffer = AllocatePmaBuffer(account, page_size, PmaMemoryUse::Spill);
      if (!buffer.has_value()) {
        return std::unexpected(std::move(buffer.error()));
      }
      return PmaWriter{extent, account, std::move(*buffer)};
    }

    [[nodiscard]] Status WriteRecord(ByteView record) {
      auto storage_size = RecordStorageSize(record);
      if (!storage_size.has_value()) {
        return std::unexpected(std::move(storage_size.error()));
      }
      if (offset_ > limit_ || *storage_size > limit_ - offset_) {
        return std::unexpected(Internal("PMA record exceeds its allocated extent"));
      }
      std::array<std::byte, 9> header{};
      auto encoded = EncodeSqliteVarint(static_cast<std::uint64_t>(record.size()), header);
      if (!encoded.has_value()) {
        std::terminate();
      }
      auto appended = Append(ByteView{header}.first(encoded->value()));
      if (!appended.has_value()) {
        return std::unexpected(std::move(appended.error()));
      }
      return Append(record);
    }

    [[nodiscard]] Status Finish() {
      if (offset_ != limit_) {
        return std::unexpected(Internal("PMA run did not fill its allocated extent"));
      }
      auto flushed = Flush();
      if (!flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
      }
      ReleaseBuffer();
      return {};
    }

   private:
    PmaWriter(const temporary_storage_detail::SorterExtentLease& extent,
              TemporaryMemoryAccount& account, ByteBuffer buffer) noexcept
        : file_(&extent.file()),
          account_(&account),
          buffer_(std::move(buffer)),
          offset_(extent.begin()),
          limit_(offset_ + extent.size()),
          buffer_begin_(static_cast<std::size_t>(offset_ % buffer_.size().value())),
          buffer_end_(buffer_begin_) {}

    [[nodiscard]] Status Append(ByteView source) {
      if (buffer_.empty()) {
        return std::unexpected(Misuse("PMA writer has already finished"));
      }
      if (offset_ > limit_ || source.size() > limit_ - offset_) {
        return std::unexpected(Internal("PMA output crosses its allocated extent"));
      }
      while (!source.empty()) {
        const std::size_t amount = std::min(source.size(), buffer_.size().value() - buffer_end_);
        std::ranges::copy(source.first(amount),
                          buffer_.mutable_view().subspan(buffer_end_).begin());
        buffer_end_ += amount;
        offset_ += amount;
        source = source.subspan(amount);
        if (buffer_end_ == buffer_.size().value()) {
          auto flushed = Flush();
          if (!flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
          }
        }
      }
      return {};
    }

    [[nodiscard]] Status Flush() {
      if (buffer_begin_ == buffer_end_) {
        return {};
      }
      const std::size_t amount = buffer_end_ - buffer_begin_;
      auto written = file_->WriteAt(buffer_.view().subspan(buffer_begin_, amount),
                                    FileOffset{offset_ - amount});
      if (!written.has_value()) {
        return std::unexpected(std::move(written.error()));
      }
      MODERN_SQLITE_RECORD_AGGREGATE_OPERATION(
          instrumentation::AggregateWorkMetric::SorterSpillWrites);
      buffer_begin_ = 0;
      buffer_end_ = 0;
      return {};
    }

    void ReleaseBuffer() noexcept {
      const std::size_t bytes = buffer_.capacity().value();
      buffer_ = ByteBuffer{};
      if (account_ != nullptr && bytes != 0U) {
        ReleasePmaBytes(*account_, bytes);
      }
    }

    File* file_;
    TemporaryMemoryAccount* account_;
    ByteBuffer buffer_;
    std::uint64_t offset_;
    std::uint64_t limit_;
    std::size_t buffer_begin_;
    std::size_t buffer_end_;
  };

  struct PmaReader {
    PmaReader(File& input, const PmaRun& input_run, ByteBuffer page_buffer) noexcept
        : file(&input),
          begin(input_run.extent.begin()),
          end(begin + input_run.extent.size()),
          offset(begin),
          ordinal(input_run.ordinal),
          buffer(std::move(page_buffer)) {}

    File* file;
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t offset;
    std::uint64_t ordinal;
    ByteBuffer buffer;
    std::size_t buffer_position = 0;
    std::size_t buffer_limit = 0;
    ByteBuffer record;
    std::optional<RecordView> view;
    bool exhausted = false;
  };

  struct MergeCursor {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    using ReaderArray = std::unique_ptr<std::optional<PmaReader>[]>;
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    using HeapArray = std::unique_ptr<std::size_t[]>;

    MergeCursor(const RecordSorterDescriptor& sorter_descriptor,
                TemporaryMemoryAccount& memory_account, PmaMemoryUse memory_use,
                ReaderArray owned_readers, HeapArray owned_heap, std::size_t input_count) noexcept
        : descriptor(&sorter_descriptor),
          account(memory_account),
          use(memory_use),
          readers(std::move(owned_readers)),
          heap(std::move(owned_heap)),
          reader_count(input_count) {}

    ~MergeCursor() {
      for (std::size_t index = 0; index < reader_count; ++index) {
        std::optional<PmaReader>& reader = readers[index];
        if (reader.has_value()) {
          reader->view.reset();
          reader->record = ByteBuffer{};
          reader->buffer = ByteBuffer{};
        }
      }
      if (charged_bytes != 0U) {
        ReleasePmaBytes(account, charged_bytes);
      }
    }

    [[nodiscard]] static Result<std::unique_ptr<MergeCursor>> Create(
        File& file, std::span<PmaRun* const> runs, const RecordSorterDescriptor& descriptor,
        TemporaryMemoryAccount& account, ByteCount page_size, PmaMemoryUse use) {
      if (runs.empty() || runs.size() > kMergeFanIn) {
        return std::unexpected(Misuse("PMA merge fan-in is invalid"));
      }
      if (runs.size() >
              std::numeric_limits<std::size_t>::max() / sizeof(std::optional<PmaReader>) ||
          runs.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::size_t)) {
        return std::unexpected(TooLarge("PMA merge allocation size overflows"));
      }
      const std::size_t reader_bytes = runs.size() * sizeof(std::optional<PmaReader>);
      const std::size_t heap_bytes = runs.size() * sizeof(std::size_t);
      if (reader_bytes > std::numeric_limits<std::size_t>::max() - sizeof(MergeCursor) ||
          heap_bytes >
              std::numeric_limits<std::size_t>::max() - sizeof(MergeCursor) - reader_bytes) {
        return std::unexpected(TooLarge("PMA merge memory accounting overflows"));
      }
      const std::size_t base_bytes = sizeof(MergeCursor) + reader_bytes + heap_bytes;
      auto reserved = ReservePmaBytes(account, ByteCount{base_bytes}, use);
      if (!reserved.has_value()) {
        return std::unexpected(std::move(reserved.error()));
      }
      std::unique_ptr<MergeCursor> cursor;
      try {
        // Exact-size arrays avoid charging the unused portion of the 16-way fan-in.
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        auto readers = std::make_unique<std::optional<PmaReader>[]>(runs.size());
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        auto heap = std::make_unique<std::size_t[]>(runs.size());
        cursor = std::make_unique<MergeCursor>(descriptor, account, use, std::move(readers),
                                               std::move(heap), runs.size());
      } catch (const std::bad_alloc&) {
        ReleasePmaBytes(account, base_bytes);
        return std::unexpected(Error::OutOfMemory());
      } catch (const std::length_error&) {
        ReleasePmaBytes(account, base_bytes);
        return std::unexpected(Error::OutOfMemory());
      }
      cursor->charged_bytes = base_bytes;
      for (std::size_t index = 0; index < runs.size(); ++index) {
        auto buffer = AllocatePmaBuffer(account, page_size, use);
        if (!buffer.has_value()) {
          return std::unexpected(std::move(buffer.error()));
        }
        cursor->charged_bytes += buffer->capacity().value();
        cursor->readers[index].emplace(file, *runs[index], std::move(*buffer));
        auto advanced = cursor->AdvanceReader(index);
        if (!advanced.has_value()) {
          return std::unexpected(std::move(advanced.error()));
        }
        if (!cursor->Reader(index).exhausted) {
          cursor->heap[cursor->heap_size++] = index;
        }
      }
      for (std::size_t index = cursor->heap_size / 2U; index > 0U; --index) {
        cursor->SiftDown(index - 1U);
      }
      return cursor;
    }

    [[nodiscard]] bool empty() const noexcept { return heap_size == 0U; }

    [[nodiscard]] const RecordView& current() const noexcept {
      if (empty()) {
        std::terminate();
      }
      const std::optional<PmaReader>& reader = readers[heap[0]];
      if (!reader.has_value() || !reader->view.has_value()) {
        std::terminate();
      }
      return *reader->view;  // NOLINT(bugprone-unchecked-optional-access)
    }

    [[nodiscard]] Result<bool> Next() {
      if (empty()) {
        return std::unexpected(Misuse("PMA merge cursor is exhausted"));
      }
      auto advanced = AdvanceReader(heap[0]);
      if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
      }
      if (Reader(heap[0]).exhausted) {
        heap[0] = heap[heap_size - 1U];
        --heap_size;
      }
      if (!empty()) {
        SiftDown(0);
      }
      return !empty();
    }

   private:
    [[nodiscard]] PmaReader& Reader(std::size_t index) noexcept {
      if (index >= reader_count || !readers[index].has_value()) {
        std::terminate();
      }
      return *readers[index];  // NOLINT(bugprone-unchecked-optional-access)
    }

    [[nodiscard]] const PmaReader& Reader(std::size_t index) const noexcept {
      if (index >= reader_count || !readers[index].has_value()) {
        std::terminate();
      }
      return *readers[index];  // NOLINT(bugprone-unchecked-optional-access)
    }

    [[nodiscard]] Result<void> AdvanceReader(std::size_t index) {
      PmaReader& reader = Reader(index);
      reader.view.reset();
      ReleaseBuffer(reader.record);
      if (reader.offset == reader.end) {
        ReleaseBuffer(reader.buffer);
        reader.exhausted = true;
        return {};
      }
      if (reader.offset < reader.begin || reader.offset > reader.end) {
        return std::unexpected(Corruption("PMA reader advanced beyond its extent"));
      }

      std::array<std::byte, 9> header{};
      const std::uint64_t record_offset = reader.offset;
      std::size_t header_size = 0;
      while (header_size < header.size() && reader.offset < reader.end) {
        auto read = ReadBuffered(reader, MutableByteView{header}.subspan(header_size, 1U));
        if (!read.has_value()) {
          return std::unexpected(std::move(read.error()));
        }
        const std::byte byte = header[header_size++];
        if ((std::to_integer<std::uint8_t>(byte) & 0x80U) == 0U) {
          break;
        }
      }
      auto decoded = DecodeSqliteVarint(ByteView{header}.first(header_size));
      if (!decoded.has_value()) {
        return std::unexpected(Corruption("PMA record-size varint is malformed"));
      }
      const std::uint64_t size = decoded->value;
      const std::uint64_t header_bytes = decoded->bytes_consumed.value();
      if (header_bytes > reader.end - record_offset) {
        return std::unexpected(Corruption("PMA record-size varint crosses its extent"));
      }
      const std::uint64_t payload_offset = reader.offset;
      if (size > reader.end - payload_offset || size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(Corruption("PMA record payload crosses its extent"));
      }
      const auto allocation_size = static_cast<std::size_t>(size);
      auto next_record = AllocatePmaBuffer(account, ByteCount{allocation_size}, use);
      if (!next_record.has_value()) {
        return std::unexpected(std::move(next_record.error()));
      }
      charged_bytes += next_record->capacity().value();
      reader.record = std::move(*next_record);
      auto payload_read = ReadBuffered(reader, reader.record.mutable_view());
      if (!payload_read.has_value()) {
        return std::unexpected(std::move(payload_read.error()));
      }
      auto parsed = RecordView::Parse(reader.record.view(), descriptor->record_options);
      if (!parsed.has_value()) {
        return std::unexpected(std::move(parsed.error()));
      }
      if (parsed->field_count() != descriptor->field_count) {
        return std::unexpected(Corruption("PMA record field count does not match the descriptor"));
      }
      reader.view = *parsed;
      reader.exhausted = false;
      return {};
    }

    [[nodiscard]] static Status ReadBuffered(PmaReader& reader, MutableByteView destination) {
      if (reader.offset > reader.end || destination.size() > reader.end - reader.offset) {
        return std::unexpected(Corruption("PMA read crosses its extent"));
      }
      while (!destination.empty()) {
        if (reader.buffer_position == reader.buffer_limit) {
          const std::size_t page_size = reader.buffer.size().value();
          const auto page_remaining =
              page_size - static_cast<std::size_t>(reader.offset % page_size);
          const auto amount = static_cast<std::size_t>(
              std::min<std::uint64_t>(page_remaining, reader.end - reader.offset));
          auto read = reader.file->ReadAt(reader.buffer.mutable_view().first(amount),
                                          FileOffset{reader.offset});
          if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
          }
          if (!read->complete()) {
            return std::unexpected(Corruption("PMA input refill is truncated"));
          }
          reader.buffer_position = 0;
          reader.buffer_limit = amount;
        }
        const std::size_t amount =
            std::min(destination.size(), reader.buffer_limit - reader.buffer_position);
        std::ranges::copy(reader.buffer.view().subspan(reader.buffer_position, amount),
                          destination.begin());
        reader.buffer_position += amount;
        reader.offset += amount;
        destination = destination.subspan(amount);
      }
      return {};
    }

    void ReleaseBuffer(ByteBuffer& buffer) noexcept {
      const std::size_t bytes = buffer.capacity().value();
      buffer = ByteBuffer{};
      if (bytes > charged_bytes) {
        std::terminate();
      }
      if (bytes != 0U) {
        ReleasePmaBytes(account, bytes);
        charged_bytes -= bytes;
      }
    }

    [[nodiscard]] bool ComesBefore(std::size_t left_index, std::size_t right_index) const noexcept {
      const PmaReader& left = Reader(left_index);
      const PmaReader& right = Reader(right_index);
      if (!left.view.has_value() || !right.view.has_value()) {
        std::terminate();
      }
      const auto comparison =
          CompareRecordPrefixes(*left.view, *right.view, descriptor->key_columns);
      if (!comparison.has_value()) {
        std::terminate();
      }
      if (*comparison == std::weak_ordering::equivalent) {
        return left.ordinal < right.ordinal;
      }
      return *comparison == std::weak_ordering::less;
    }

    void SiftDown(std::size_t parent) noexcept {
      while (true) {
        const std::size_t left = parent * 2U + 1U;
        if (left >= heap_size) {
          return;
        }
        const std::size_t right = left + 1U;
        std::size_t selected = left;
        if (right < heap_size && ComesBefore(heap[right], heap[left])) {
          selected = right;
        }
        if (!ComesBefore(heap[selected], heap[parent])) {
          return;
        }
        std::swap(heap[selected], heap[parent]);
        parent = selected;
      }
    }

    const RecordSorterDescriptor* descriptor;
    TemporaryMemoryAccount& account;
    PmaMemoryUse use;
    ReaderArray readers;
    HeapArray heap;
    std::size_t reader_count = 0;
    std::size_t heap_size = 0;
    std::size_t charged_bytes = 0;
  };

  Impl(RecordSorterDescriptor owned_descriptor,
       temporary_storage_detail::StatementState& owning_state, ByteCount threshold,
       std::uint64_t owned_sorter_ordinal) noexcept
      : descriptor(std::move(owned_descriptor)),
        statement_state(&owning_state),
        memory_threshold(threshold),
        sorter_ordinal(owned_sorter_ordinal) {}

  ~Impl() override { ClearAll(); }

  [[nodiscard]] TemporaryMemoryAccount& Account() noexcept {
    if (!memory_account.has_value()) {
      std::terminate();
    }
    return *memory_account;
  }

  [[nodiscard]] Result<void> CheckStorage() const {
    if (statement_state->failed) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    return {};
  }

  void MarkFailed() noexcept { statement_state->failed = true; }

  [[nodiscard]] ByteCount reclaimable_bytes() const noexcept override {
    if (state != RecordSorterState::kWriting) {
      return ByteCount{0};
    }
    if (head != nullptr) {
      return ByteCount{entry_memory_bytes > sizeof(PmaRun) ? entry_memory_bytes - sizeof(PmaRun)
                                                           : 0U};
    }
    return ByteCount{run_count > 1U && run_memory_bytes > sizeof(PmaRun)
                         ? run_memory_bytes - sizeof(PmaRun)
                         : 0U};
  }

  [[nodiscard]] Status SpillForPressure() override {
    if (state != RecordSorterState::kWriting) {
      return std::unexpected(Misuse("temporary sorter cannot spill outside the writing state"));
    }
    if (head != nullptr) {
      return SpillCurrentRun();
    }
    if (run_count > 1U) {
      return BuildMergeLevels(1U);
    }
    return {};
  }

  void DestroyListWithoutAccounting() noexcept {
    while (head != nullptr) {
      Run next = std::move(head->next);
      head.reset();
      head = std::move(next);
    }
    tail = nullptr;
    current = nullptr;
    entry_memory_bytes = 0;
  }

  void ClearList() noexcept {
    const std::size_t released_bytes = entry_memory_bytes;
    DestroyListWithoutAccounting();
    if (released_bytes != 0U) {
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
  }

  void ClearRuns() noexcept {
    while (runs != nullptr) {
      std::unique_ptr<PmaRun> next = std::move(runs->next);
      runs.reset();
      runs = std::move(next);
    }
    run_tail = nullptr;
    run_count = 0;
    const std::size_t released_bytes = run_memory_bytes;
    run_memory_bytes = 0;
    if (released_bytes != 0U) {
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
  }

  void ClearAll() noexcept {
    merge.reset();
    ClearList();
    ClearRuns();
    count = 0;
    next_run_ordinal = 0;
    merge_levels = 0;
    spilled = false;
  }

  [[nodiscard]] Run MergeRuns(Run left, Run right) const noexcept {
    Run merged;
    Run* output = &merged;
    while (left != nullptr && right != nullptr) {
      const auto comparison =
          CompareRecordPrefixes(left->view, right->view, descriptor.key_columns);
      if (!comparison.has_value()) {
        std::terminate();
      }
      Run selected;
      if (*comparison != std::weak_ordering::greater) {
        selected = std::move(left);
        left = std::move(selected->next);
      } else {
        selected = std::move(right);
        right = std::move(selected->next);
      }
      selected->next.reset();
      *output = std::move(selected);
      output = &((*output)->next);
    }
    *output = left != nullptr ? std::move(left) : std::move(right);
    return merged;
  }

  void Sort() noexcept {
    std::array<Run, kMergeRunCount> sort_runs;
    while (head != nullptr) {
      Run run = std::move(head);
      head = std::move(run->next);
      run->next.reset();

      std::size_t level = 0;
      while (sort_runs[level] != nullptr) {
        run = MergeRuns(std::move(sort_runs[level]), std::move(run));
        ++level;
        if (level == sort_runs.size()) {
          std::terminate();
        }
      }
      sort_runs[level] = std::move(run);
    }

    Run sorted;
    for (Run& run : sort_runs) {
      if (run == nullptr) {
        continue;
      }
      sorted = sorted == nullptr ? std::move(run) : MergeRuns(std::move(run), std::move(sorted));
    }
    head = std::move(sorted);
    tail = nullptr;
  }

  [[nodiscard]] static Result<std::uint64_t> RecordStorageSize(ByteView record) {
    if (record.size() > std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("PMA record size is not representable"));
    }
    std::array<std::byte, 9> header{};
    auto encoded = EncodeSqliteVarint(static_cast<std::uint64_t>(record.size()), header);
    if (!encoded.has_value()) {
      std::terminate();
    }
    const std::uint64_t header_size = encoded->value();
    const auto record_size = static_cast<std::uint64_t>(record.size());
    if (record_size > std::numeric_limits<std::uint64_t>::max() - header_size) {
      return std::unexpected(TooLarge("PMA record size overflows"));
    }
    return header_size + record_size;
  }

  [[nodiscard]] Result<std::uint64_t> CurrentRunStorageSize() const {
    std::uint64_t total = 0;
    for (const Entry* entry = head.get(); entry != nullptr; entry = entry->next.get()) {
      auto size = RecordStorageSize(entry->record.view());
      if (!size.has_value()) {
        return std::unexpected(std::move(size.error()));
      }
      if (*size > std::numeric_limits<std::uint64_t>::max() - total) {
        return std::unexpected(TooLarge("PMA run size overflows"));
      }
      total += *size;
    }
    return total;
  }

  void AppendRun(std::unique_ptr<PmaRun> run) noexcept {
    PmaRun* inserted = run.get();
    if (run_tail == nullptr) {
      runs = std::move(run);
    } else {
      run_tail->next = std::move(run);
    }
    run_tail = inserted;
    ++run_count;
  }

  [[nodiscard]] Result<void> SpillCurrentRun() {
    if (head == nullptr) {
      return std::unexpected(Misuse("cannot spill an empty sorter run"));
    }
    if (next_run_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("temporary sorter run ordinal is exhausted"));
    }
    Sort();
    auto size = CurrentRunStorageSize();
    if (!size.has_value()) {
      return std::unexpected(std::move(size.error()));
    }
    auto extent = statement_state->AllocateSorterExtent(*size, sorter_ordinal, next_run_ordinal);
    if (!extent.has_value()) {
      return std::unexpected(std::move(extent.error()));
    }
    auto writer = PmaWriter::Create(*extent, Account(), statement_state->SorterPageSize());
    if (!writer.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(writer.error()));
    }
    for (const Entry* entry = head.get(); entry != nullptr; entry = entry->next.get()) {
      auto written = writer->WriteRecord(entry->record.view());
      if (!written.has_value()) {
        MarkFailed();
        return std::unexpected(std::move(written.error()));
      }
    }
    auto finished = writer->Finish();
    if (!finished.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(finished.error()));
    }
    std::unique_ptr<PmaRun> run;
    try {
      run = std::make_unique<PmaRun>(std::move(*extent), next_run_ordinal);
    } catch (const std::bad_alloc&) {
      MarkFailed();
      return std::unexpected(Error::OutOfMemory());
    }
    if (entry_memory_bytes < sizeof(PmaRun)) {
      MarkFailed();
      return std::unexpected(Internal("sorter run metadata exceeds its reclaimed entries"));
    }
    const std::size_t released_bytes = entry_memory_bytes - sizeof(PmaRun);
    AppendRun(std::move(run));
    ++next_run_ordinal;
    DestroyListWithoutAccounting();
    run_memory_bytes += sizeof(PmaRun);
    if (released_bytes != 0U) {
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
    spilled = true;
    return {};
  }

  [[nodiscard]] Result<void> SpillDirect(ByteBuffer record) {
    if (next_run_ordinal == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("temporary sorter run ordinal is exhausted"));
    }
    auto size = RecordStorageSize(record.view());
    if (!size.has_value()) {
      return std::unexpected(std::move(size.error()));
    }
    auto extent = statement_state->AllocateSorterExtent(*size, sorter_ordinal, next_run_ordinal);
    if (!extent.has_value()) {
      return std::unexpected(std::move(extent.error()));
    }
    auto writer = PmaWriter::Create(*extent, Account(), statement_state->SorterPageSize());
    if (!writer.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(writer.error()));
    }
    auto written = writer->WriteRecord(record.view());
    if (!written.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(written.error()));
    }
    auto finished = writer->Finish();
    if (!finished.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(finished.error()));
    }
    std::unique_ptr<PmaRun> run;
    try {
      run = std::make_unique<PmaRun>(std::move(*extent), next_run_ordinal);
    } catch (const std::bad_alloc&) {
      MarkFailed();
      return std::unexpected(Error::OutOfMemory());
    }
    auto reserved = Account().Reserve(ByteCount{sizeof(PmaRun)});
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }
    AppendRun(std::move(run));
    run_memory_bytes += sizeof(PmaRun);
    ++next_run_ordinal;
    ++count;
    spilled = true;
    return {};
  }

  [[nodiscard]] Result<std::unique_ptr<PmaRun>> MergeGroup(std::span<PmaRun* const> group) {
    std::uint64_t size = 0;
    for (const PmaRun* run : group) {
      if (run->extent.size() > std::numeric_limits<std::uint64_t>::max() - size) {
        return std::unexpected(TooLarge("merged PMA extent size overflows"));
      }
      size += run->extent.size();
    }
    auto output =
        statement_state->AllocateSorterExtent(size, sorter_ordinal, group.front()->ordinal);
    if (!output.has_value()) {
      return std::unexpected(std::move(output.error()));
    }
    auto writer = PmaWriter::Create(*output, Account(), statement_state->SorterPageSize());
    if (!writer.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(writer.error()));
    }
    auto cursor = MergeCursor::Create(group.front()->extent.file(), group, descriptor, Account(),
                                      statement_state->SorterPageSize(), PmaMemoryUse::Spill);
    if (!cursor.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(cursor.error()));
    }
    while (!(*cursor)->empty()) {
      auto written = writer->WriteRecord((*cursor)->current().encoded());
      if (!written.has_value()) {
        MarkFailed();
        return std::unexpected(std::move(written.error()));
      }
      auto advanced = (*cursor)->Next();
      if (!advanced.has_value()) {
        MarkFailed();
        return std::unexpected(std::move(advanced.error()));
      }
    }
    auto finished = writer->Finish();
    if (!finished.has_value()) {
      MarkFailed();
      return std::unexpected(std::move(finished.error()));
    }
    try {
      return std::make_unique<PmaRun>(std::move(*output), group.front()->ordinal);
    } catch (const std::bad_alloc&) {
      MarkFailed();
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<void> BuildMergeLevels(std::size_t target_run_count = kMergeFanIn) {
    if (target_run_count == 0U || target_run_count > kMergeFanIn) {
      return std::unexpected(Misuse("temporary sorter merge target is invalid"));
    }
    while (run_count > target_run_count) {
      const std::size_t old_count = run_count;
      std::unique_ptr<PmaRun> input = std::move(runs);
      run_tail = nullptr;
      std::unique_ptr<PmaRun> output;
      PmaRun* output_tail = nullptr;
      std::size_t output_count = 0;

      while (input != nullptr) {
        std::array<PmaRun*, kMergeFanIn> group{};
        std::size_t group_count = 0;
        PmaRun* scan = input.get();
        while (scan != nullptr && group_count < kMergeFanIn) {
          group[group_count++] = scan;
          scan = scan->next.get();
        }
        auto merged = MergeGroup(std::span<PmaRun* const>{group}.first(group_count));
        if (!merged.has_value()) {
          return std::unexpected(std::move(merged.error()));
        }

        const std::unique_ptr<PmaRun> consumed = std::move(input);
        PmaRun* consumed_tail = consumed.get();
        for (std::size_t index = 1; index < group_count; ++index) {
          consumed_tail = consumed_tail->next.get();
        }
        input = std::move(consumed_tail->next);
        consumed_tail->next.reset();

        PmaRun* inserted = merged->get();
        if (output_tail == nullptr) {
          output = std::move(*merged);
        } else {
          output_tail->next = std::move(*merged);
        }
        output_tail = inserted;
        ++output_count;
      }

      runs = std::move(output);
      run_tail = output_tail;
      run_count = output_count;
      const std::size_t released_nodes = old_count - output_count;
      const std::size_t released_bytes = released_nodes * sizeof(PmaRun);
      if (released_bytes > run_memory_bytes) {
        std::terminate();
      }
      run_memory_bytes -= released_bytes;
      if (released_bytes != 0U) {
        const auto released = Account().Release(ByteCount{released_bytes});
        if (!released.has_value()) {
          std::terminate();
        }
      }
      ++merge_levels;
    }
    return {};
  }

  [[nodiscard]] Result<std::unique_ptr<MergeCursor>> CreateFinalMerge() {
    std::array<PmaRun*, kMergeFanIn> group{};
    std::size_t index = 0;
    for (PmaRun* run = runs.get(); run != nullptr; run = run->next.get()) {
      if (index == group.size()) {
        return std::unexpected(Internal("final PMA merge exceeds its fan-in"));
      }
      group[index++] = run;
    }
    if (index == 0U) {
      return std::unexpected(Internal("final PMA merge has no runs"));
    }
    return MergeCursor::Create(runs->extent.file(), std::span<PmaRun* const>{group}.first(index),
                               descriptor, Account(), statement_state->SorterPageSize(),
                               PmaMemoryUse::Drain);
  }

  RecordSorterDescriptor descriptor;
  temporary_storage_detail::StatementState* statement_state;
  ByteCount memory_threshold;
  std::uint64_t sorter_ordinal;
  std::optional<TemporaryMemoryAccount> memory_account;
  Run head;
  Entry* tail = nullptr;
  Entry* current = nullptr;
  std::size_t count = 0;
  std::size_t entry_memory_bytes = 0;
  std::unique_ptr<PmaRun> runs;
  PmaRun* run_tail = nullptr;
  std::size_t run_count = 0;
  std::size_t run_memory_bytes = 0;
  std::uint64_t next_run_ordinal = 0;
  std::size_t merge_levels = 0;
  std::unique_ptr<MergeCursor> merge;
  bool spilled = false;
  RecordSorterState state = RecordSorterState::kWriting;
};

std::string_view RecordSorterStateName(RecordSorterState state) noexcept {
  switch (state) {
    case RecordSorterState::kWriting:
      return "writing";
    case RecordSorterState::kPositioned:
      return "positioned";
    case RecordSorterState::kExhausted:
      return "exhausted";
    case RecordSorterState::kClosed:
      return "closed";
  }
  return "unknown";
}

RecordSorter::RecordSorter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RecordSorter::RecordSorter(RecordSorter&&) noexcept = default;

RecordSorter& RecordSorter::operator=(RecordSorter&&) noexcept = default;

RecordSorter::~RecordSorter() = default;

Result<RecordSorter> RecordSorter::Create(const RecordSorterDescriptor& descriptor,
                                          StatementTemporaryStorage& storage) {
  auto validated = ValidateRecordSorterDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    temporary_storage_detail::StatementState& state =
        temporary_storage_detail::StatementStorageAccess::State(storage);
    auto ordinal = state.AllocateSorterOrdinal();
    if (!ordinal.has_value()) {
      return std::unexpected(std::move(ordinal.error()));
    }
    auto impl = std::make_unique<Impl>(descriptor, state, storage.memory_budget(), *ordinal);
    Result<TemporaryMemoryAccount> account = storage.file_spill_enabled()
                                                 ? storage.RegisterMemoryOwner(*impl)
                                                 : storage.RegisterMemoryOwner();
    if (!account.has_value()) {
      return std::unexpected(std::move(account.error()));
    }
    impl->memory_account.emplace(std::move(*account));
    return RecordSorter{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

bool RecordSorter::valid() const noexcept { return impl_ != nullptr; }

RecordSorterState RecordSorter::state() const noexcept {
  return impl_ == nullptr ? RecordSorterState::kClosed : impl_->state;
}

std::size_t RecordSorter::record_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->count;
}

ByteCount RecordSorter::memory_usage() const noexcept {
  return impl_ == nullptr ? ByteCount{0} : impl_->Account().retained_bytes();
}

bool RecordSorter::has_spilled() const noexcept { return impl_ != nullptr && impl_->spilled; }

std::size_t RecordSorter::spilled_run_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->run_count;
}

std::size_t RecordSorter::merge_level_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->merge_levels;
}

Status RecordSorter::Insert(ByteBuffer record) {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kWriting) {
    return std::unexpected(Misuse("records can only be inserted while the sorter is writing"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto view = RecordView::Parse(record.view(), impl_->descriptor.record_options);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  if (view->field_count() != impl_->descriptor.field_count) {
    return std::unexpected(Misuse("sorter record field count does not match the descriptor"));
  }

  const std::size_t record_bytes = record.capacity().value();
  if (record_bytes > std::numeric_limits<std::size_t>::max() - sizeof(Impl::Entry)) {
    return std::unexpected(TooLarge("sorter record memory accounting overflowed"));
  }
  const std::size_t entry_bytes = record_bytes + sizeof(Impl::Entry);
  if (impl_->count == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(TooLarge("sorter record count is exhausted"));
  }
  if (entry_bytes > impl_->memory_threshold.value()) {
    if (!impl_->statement_state->file_spill) {
      return std::unexpected(TooLarge("sorter memory threshold exceeded in memory mode"));
    }
    if (impl_->head != nullptr) {
      auto current_spill = impl_->SpillCurrentRun();
      if (!current_spill.has_value()) {
        Error error = std::move(current_spill.error());
        Close();
        return std::unexpected(std::move(error));
      }
    }
    auto spilled = impl_->SpillDirect(std::move(record));
    if (!spilled.has_value()) {
      Error error = std::move(spilled.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return {};
  }
  const bool exceeds_threshold =
      entry_bytes > impl_->memory_threshold.value() ||
      impl_->entry_memory_bytes > impl_->memory_threshold.value() - entry_bytes;
  if (exceeds_threshold && impl_->head != nullptr) {
    if (!impl_->statement_state->file_spill) {
      return std::unexpected(TooLarge("sorter memory threshold exceeded in memory mode"));
    }
    auto spilled = impl_->SpillCurrentRun();
    if (!spilled.has_value()) {
      Error error = std::move(spilled.error());
      Close();
      return std::unexpected(std::move(error));
    }
  } else if (exceeds_threshold && !impl_->statement_state->file_spill) {
    return std::unexpected(TooLarge("sorter memory threshold exceeded in memory mode"));
  }
  auto reserved = impl_->Account().Reserve(ByteCount{entry_bytes});
  if (!reserved.has_value()) {
    Error error = std::move(reserved.error());
    if (impl_->statement_state->failed) {
      Close();
    }
    return std::unexpected(std::move(error));
  }
  try {
    auto entry = std::make_unique<Impl::Entry>(std::move(record), *view, entry_bytes);
    Impl::Entry* const inserted = entry.get();
    if (impl_->tail == nullptr) {
      impl_->head = std::move(entry);
    } else {
      impl_->tail->next = std::move(entry);
    }
    impl_->tail = inserted;
    ++impl_->count;
    impl_->entry_memory_bytes += entry_bytes;
    return {};
  } catch (const std::bad_alloc&) {
    const auto released = impl_->Account().Release(ByteCount{entry_bytes});
    if (!released.has_value()) {
      std::terminate();
    }
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  } catch (const std::length_error&) {
    const auto released = impl_->Account().Release(ByteCount{entry_bytes});
    if (!released.has_value()) {
      std::terminate();
    }
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  }
}

Status RecordSorter::Rewind() {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kWriting) {
    return std::unexpected(Misuse("sorter rewind requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->runs == nullptr) {
    impl_->Sort();
    impl_->current = impl_->head.get();
    impl_->state =
        impl_->current == nullptr ? RecordSorterState::kExhausted : RecordSorterState::kPositioned;
    return {};
  }

  if (impl_->head != nullptr) {
    auto spilled = impl_->SpillCurrentRun();
    if (!spilled.has_value()) {
      Error error = std::move(spilled.error());
      Close();
      return std::unexpected(std::move(error));
    }
  }
  auto levels = impl_->BuildMergeLevels();
  if (!levels.has_value()) {
    Error error = std::move(levels.error());
    Close();
    return std::unexpected(std::move(error));
  }
  // Final merge buffers may trigger statement pressure. The published runs
  // must not be rewritten while their pointers seed the merge cursor.
  impl_->state = RecordSorterState::kPositioned;
  auto merge = impl_->CreateFinalMerge();
  if (!merge.has_value()) {
    Error error = std::move(merge.error());
    if (error.code() != ErrorCode::kOutOfMemory && error.code() != ErrorCode::kTooLarge &&
        error.code() != ErrorCode::kMisuse) {
      impl_->MarkFailed();
    }
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->merge = std::move(*merge);
  if (impl_->merge->empty()) {
    impl_->merge.reset();
    impl_->ClearRuns();
    impl_->state = RecordSorterState::kExhausted;
  } else {
    impl_->state = RecordSorterState::kPositioned;
  }
  return {};
}

Result<RecordView> RecordSorter::current_record() const {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned) {
    return std::unexpected(Misuse("sorter has no current record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    return std::unexpected(std::move(usable.error()));
  }
  if (impl_->merge != nullptr) {
    return impl_->merge->current();
  }
  if (impl_->current == nullptr) {
    return std::unexpected(Misuse("sorter has no current record"));
  }
  return impl_->current->view;
}

Result<bool> RecordSorter::Next() {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned) {
    return std::unexpected(Misuse("sorter next requires a current record"));
  }
  if (impl_->merge != nullptr) {
    auto advanced = impl_->merge->Next();
    if (!advanced.has_value()) {
      Error error = std::move(advanced.error());
      if (error.code() != ErrorCode::kOutOfMemory && error.code() != ErrorCode::kTooLarge &&
          error.code() != ErrorCode::kMisuse) {
        impl_->MarkFailed();
      }
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*advanced) {
      impl_->merge.reset();
      impl_->ClearRuns();
      impl_->state = RecordSorterState::kExhausted;
    }
    return *advanced;
  }
  if (impl_->current == nullptr) {
    return std::unexpected(Misuse("sorter next requires a current record"));
  }
  impl_->current = impl_->current->next.get();
  if (impl_->current == nullptr) {
    impl_->state = RecordSorterState::kExhausted;
    return false;
  }
  return true;
}

Result<std::weak_ordering> RecordSorter::CompareCurrent(const RecordView& record) const {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned) {
    return std::unexpected(Misuse("sorter comparison requires a current record"));
  }
  auto current = current_record();
  if (!current.has_value()) {
    return std::unexpected(std::move(current.error()));
  }
  return CompareRecordPrefixes(*current, record, impl_->descriptor.key_columns);
}

Status RecordSorter::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot reset a closed sorter"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearAll();
  impl_->state = RecordSorterState::kWriting;
  return {};
}

void RecordSorter::Close() noexcept { impl_.reset(); }

Result<RecordSorter> StatementTemporaryStorage::CreateRecordSorter(
    const RecordSorterDescriptor& descriptor) {
  if (!valid()) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return RecordSorter::Create(descriptor, *this);
}

struct BoundedTopN::Impl {
  struct Entry {
    Entry(ByteBuffer owned_record, RecordView record_view, std::size_t owned_bytes) noexcept
        : record(std::move(owned_record)), view(record_view), memory_bytes(owned_bytes) {}

    ByteBuffer record;
    RecordView view;
    std::size_t memory_bytes;
    std::unique_ptr<Entry> next;
  };

  struct FileBackend {
    FileBackend(temporary_storage_detail::RelationRootLease owned_root,
                std::vector<IndexColumnOrder> key_columns) noexcept
        : root(std::move(owned_root)), columns(std::move(key_columns)) {}

    temporary_storage_detail::RelationRootLease root;
    std::vector<IndexColumnOrder> columns;
    std::optional<IndexBtreeCursor> cursor;
    ByteBuffer current_wrapper;
    std::optional<RecordView> current_record;
    std::size_t current_bytes = 0;
    std::uint64_t next_sequence = 0;
  };

  Impl(RecordSorterDescriptor owned_descriptor, std::size_t retained_limit,
       temporary_storage_detail::StatementState& owning_state) noexcept
      : descriptor(std::move(owned_descriptor)),
        maximum_records(retained_limit),
        statement_state(&owning_state) {}

  ~Impl() { Clear(); }

  [[nodiscard]] TemporaryMemoryAccount& Account() noexcept {
    if (!memory_account.has_value()) {
      std::terminate();
    }
    return *memory_account;
  }

  [[nodiscard]] const TemporaryMemoryAccount& Account() const noexcept {
    if (!memory_account.has_value()) {
      std::terminate();
    }
    return *memory_account;
  }

  [[nodiscard]] Result<void> CheckStorage() const {
    if (temporary_storage_detail::StatementStorageAccess::Failed(*statement_state)) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    return {};
  }

  void MarkFileFailure() noexcept {
    if (file.has_value()) {
      file->root.MarkStatementFailed();
    }
  }

  void ClearPending() noexcept {
    pending_view.reset();
    pending_record.reset();
    const std::size_t released_bytes = pending_bytes;
    pending_bytes = 0;
    if (released_bytes != 0U) {
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
  }

  void ClearFileCurrent() noexcept {
    if (!file.has_value()) {
      return;
    }
    FileBackend& backend = File();
    backend.current_record.reset();
    backend.current_wrapper = ByteBuffer{};
    const std::size_t released_bytes = backend.current_bytes;
    backend.current_bytes = 0;
    if (released_bytes != 0U) {
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
  }

  void Clear() noexcept {
    while (head != nullptr) {
      std::unique_ptr<Entry> next = std::move(head->next);
      head.reset();
      head = std::move(next);
    }
    current = nullptr;
    pending_view.reset();
    pending_record.reset();
    pending_bytes = 0;
    count = 0;
    if (file.has_value()) {
      file->cursor.reset();
      file->current_record.reset();
      file->current_wrapper = ByteBuffer{};
      file->current_bytes = 0;
    }
    if (memory_account.has_value()) {
      const ByteCount retained = memory_account->retained_bytes();
      if (retained.value() != 0U) {
        const auto released = memory_account->Release(retained);
        if (!released.has_value()) {
          std::terminate();
        }
      }
    }
  }

  [[nodiscard]] bool file_backed() const noexcept { return file.has_value(); }

  [[nodiscard]] FileBackend& File() noexcept {
    if (!file.has_value()) {
      std::terminate();
    }
    return *file;
  }

  [[nodiscard]] const FileBackend& File() const noexcept {
    if (!file.has_value()) {
      std::terminate();
    }
    return *file;
  }

  [[nodiscard]] RecordView PendingView() const noexcept {
    if (!pending_view.has_value()) {
      std::terminate();
    }
    return *pending_view;
  }

  [[nodiscard]] std::weak_ordering Compare(const RecordView& left,
                                           const RecordView& right) const noexcept {
    const auto comparison = CompareRecordPrefixes(left, right, descriptor.key_columns);
    if (!comparison.has_value()) {
      std::terminate();
    }
    return *comparison;
  }

  [[nodiscard]] Entry* Largest() const noexcept {
    Entry* result = head.get();
    if (result == nullptr) {
      return nullptr;
    }
    while (result->next != nullptr) {
      result = result->next.get();
    }
    return result;
  }

  [[nodiscard]] std::size_t RemoveLargest() noexcept {
    std::unique_ptr<Entry>* link = &head;
    while ((*link)->next != nullptr) {
      link = &((*link)->next);
    }
    std::unique_ptr<Entry> removed = std::move(*link);
    const std::size_t removed_bytes = removed->memory_bytes;
    --count;
    return removed_bytes;
  }

  void InsertSorted(std::unique_ptr<Entry> entry) noexcept {
    std::unique_ptr<Entry>* link = &head;
    while (*link != nullptr && Compare((*link)->view, entry->view) != std::weak_ordering::greater) {
      link = &((*link)->next);
    }
    entry->next = std::move(*link);
    *link = std::move(entry);
  }

  [[nodiscard]] Result<void> LoadFileCurrent() {
    if (!file.has_value() || !file->cursor.has_value()) {
      return std::unexpected(Internal("file-backed top-N cursor is unavailable"));
    }
    auto copied = file->cursor->CopyPayload();
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    const std::size_t copied_bytes = copied->capacity().value();
    auto reserved = Account().Reserve(ByteCount{copied_bytes});
    if (!reserved.has_value() && reserved.error().code() == ErrorCode::kTooLarge &&
        !temporary_storage_detail::StatementStorageAccess::Failed(*statement_state)) {
      reserved = Account().ReserveWorkingSet(ByteCount{copied_bytes});
    }
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }
    FileBackend& backend = File();
    backend.current_wrapper = std::move(*copied);
    backend.current_bytes = copied_bytes;
    auto wrapper = RecordView::Parse(backend.current_wrapper.view(), descriptor.record_options);
    if (!wrapper.has_value()) {
      return std::unexpected(std::move(wrapper.error()));
    }
    if (wrapper->field_count() != descriptor.key_field_count + 2U) {
      return std::unexpected(Corruption("top-N queue record has an invalid field count"));
    }
    auto sequence = wrapper->field(descriptor.key_field_count);
    if (!sequence.has_value()) {
      return std::unexpected(std::move(sequence.error()));
    }
    const std::optional<std::int64_t> sequence_value = sequence->integer_value();
    if (!sequence_value.has_value() || *sequence_value < 0) {
      return std::unexpected(Corruption("top-N queue sequence is not a nonnegative integer"));
    }
    auto packed = wrapper->field(descriptor.key_field_count + 1U);
    if (!packed.has_value()) {
      return std::unexpected(std::move(packed.error()));
    }
    const std::optional<ByteView> packed_bytes = packed->blob_value();
    if (!packed_bytes.has_value()) {
      return std::unexpected(Corruption("top-N queue payload is not a blob"));
    }
    auto record = RecordView::Parse(*packed_bytes, descriptor.record_options);
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    if (record->field_count() != descriptor.field_count) {
      return std::unexpected(Corruption("top-N packed record has an invalid field count"));
    }
    auto key_comparison = CompareRecordPrefixes(*record, *wrapper, descriptor.key_columns);
    if (!key_comparison.has_value()) {
      return std::unexpected(std::move(key_comparison.error()));
    }
    if (*key_comparison != std::weak_ordering::equivalent) {
      return std::unexpected(Corruption("top-N queue key does not match its packed record"));
    }
    backend.current_record = *record;
    return {};
  }

  [[nodiscard]] Result<ByteBuffer> CopyLargestFileRecord() {
    if (!file.has_value()) {
      return std::unexpected(Internal("file-backed top-N relation is unavailable"));
    }
    FileBackend& backend = File();
    auto cursor =
        IndexBtreeCursor::Open(backend.root.pager(), backend.root.root_page(), backend.columns);
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    auto positioned = cursor->Last();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if (!*positioned) {
      return std::unexpected(Internal("full file-backed top-N relation is empty"));
    }
    return cursor->CopyPayload();
  }

  RecordSorterDescriptor descriptor;
  std::size_t maximum_records;
  temporary_storage_detail::StatementState* statement_state;
  std::optional<TemporaryMemoryAccount> memory_account;
  std::unique_ptr<Entry> head;
  Entry* current = nullptr;
  std::optional<ByteBuffer> pending_record;
  std::optional<RecordView> pending_view;
  std::size_t pending_bytes = 0;
  std::size_t count = 0;
  BoundedTopNState state = BoundedTopNState::kWriting;
  std::optional<FileBackend> file;
};

std::string_view BoundedTopNStateName(BoundedTopNState state) noexcept {
  switch (state) {
    case BoundedTopNState::kWriting:
      return "writing";
    case BoundedTopNState::kCandidatePending:
      return "candidate_pending";
    case BoundedTopNState::kPositioned:
      return "positioned";
    case BoundedTopNState::kExhausted:
      return "exhausted";
    case BoundedTopNState::kClosed:
      return "closed";
  }
  return "unknown";
}

BoundedTopN::BoundedTopN(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

BoundedTopN::BoundedTopN(BoundedTopN&&) noexcept = default;

BoundedTopN& BoundedTopN::operator=(BoundedTopN&&) noexcept = default;

BoundedTopN::~BoundedTopN() = default;

Result<BoundedTopN> BoundedTopN::Create(const RecordSorterDescriptor& descriptor, std::size_t bound,
                                        StatementTemporaryStorage& storage) {
  auto validated = ValidateRecordSorterDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    temporary_storage_detail::StatementState& state =
        temporary_storage_detail::StatementStorageAccess::State(storage);
    auto impl = std::make_unique<Impl>(descriptor, bound, state);
    auto account = storage.RegisterMemoryOwner();
    if (!account.has_value()) {
      return std::unexpected(std::move(account.error()));
    }
    impl->memory_account.emplace(std::move(*account));
    if (storage.file_spill_enabled()) {
      std::vector<IndexColumnOrder> columns = descriptor.key_columns;
      columns.emplace_back(BinaryCollation(), IndexSortDirection::kAscending,
                           IndexNullPlacement::kFirst);
      auto root = temporary_storage_detail::RelationArenaAccess::Acquire(state, columns);
      if (!root.has_value()) {
        return std::unexpected(std::move(root.error()));
      }
      impl->file.emplace(std::move(*root), std::move(columns));
    }
    return BoundedTopN{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

bool BoundedTopN::valid() const noexcept { return impl_ != nullptr; }

BoundedTopNState BoundedTopN::state() const noexcept {
  return impl_ == nullptr ? BoundedTopNState::kClosed : impl_->state;
}

std::size_t BoundedTopN::bound() const noexcept {
  return impl_ == nullptr ? 0U : impl_->maximum_records;
}

std::size_t BoundedTopN::record_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->count;
}

ByteCount BoundedTopN::memory_usage() const noexcept {
  return impl_ == nullptr ? ByteCount{0} : impl_->Account().retained_bytes();
}

bool BoundedTopN::has_pending_candidate() const noexcept {
  return impl_ != nullptr && impl_->state == BoundedTopNState::kCandidatePending;
}

Result<TopNCheckResult> BoundedTopN::CheckCandidate(ByteBuffer key) {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kWriting) {
    return std::unexpected(Misuse("top-N admission requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto view = RecordView::Parse(key.view(), impl_->descriptor.record_options);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  if (view->field_count() != impl_->descriptor.key_field_count) {
    return std::unexpected(Misuse("top-N candidate key shape does not match the descriptor"));
  }
  if (impl_->maximum_records == 0U) {
    return TopNCheckResult::kRejected;
  }

  const bool full = impl_->count == impl_->maximum_records;
  const std::size_t key_bytes = key.capacity().value();
  if (impl_->file_backed()) {
    Impl::FileBackend& file = impl_->File();
    try {
      std::optional<ByteBuffer> largest_record;
      if (full) {
        auto copied = impl_->CopyLargestFileRecord();
        if (!copied.has_value()) {
          impl_->MarkFileFailure();
          Error error = std::move(copied.error());
          Close();
          return std::unexpected(std::move(error));
        }
        largest_record.emplace(std::move(*copied));
        auto largest = RecordView::Parse(largest_record->view(), impl_->descriptor.record_options);
        if (!largest.has_value()) {
          impl_->MarkFileFailure();
          Error error = std::move(largest.error());
          Close();
          return std::unexpected(std::move(error));
        }
        auto comparison = CompareRecordPrefixes(*view, *largest, impl_->descriptor.key_columns);
        if (!comparison.has_value()) {
          impl_->MarkFileFailure();
          Error error = std::move(comparison.error());
          Close();
          return std::unexpected(std::move(error));
        }
        if (*comparison != std::weak_ordering::less) {
          return TopNCheckResult::kRejected;
        }
      }
      auto reserved = impl_->Account().Reserve(ByteCount{key_bytes});
      if (!reserved.has_value()) {
        Error error = std::move(reserved.error());
        if (temporary_storage_detail::StatementStorageAccess::Failed(*impl_->statement_state)) {
          Close();
        }
        return std::unexpected(std::move(error));
      }
      if (full) {
        if (!largest_record.has_value()) {
          std::terminate();
        }
        auto deleted = file.root.writer().DeleteEncoded(largest_record->view());
        if (!deleted.has_value()) {
          impl_->MarkFileFailure();
          Error error = std::move(deleted.error());
          Close();
          return std::unexpected(std::move(error));
        }
        if (!*deleted) {
          impl_->MarkFileFailure();
          Error error = Internal("largest file-backed top-N record disappeared");
          Close();
          return std::unexpected(std::move(error));
        }
        --impl_->count;
      }
    } catch (const std::bad_alloc&) {
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    } catch (const std::length_error&) {
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    }
  } else {
    std::size_t removed_bytes = 0;
    if (full) {
      const Impl::Entry* largest = impl_->Largest();
      if (largest == nullptr) {
        std::terminate();
      }
      if (impl_->Compare(*view, largest->view) != std::weak_ordering::less) {
        return TopNCheckResult::kRejected;
      }
      removed_bytes = largest->memory_bytes;
    }
    const std::size_t additional_bytes = key_bytes > removed_bytes ? key_bytes - removed_bytes : 0U;
    auto reserved = impl_->Account().Reserve(ByteCount{additional_bytes});
    if (!reserved.has_value()) {
      Error error = std::move(reserved.error());
      if (temporary_storage_detail::StatementStorageAccess::Failed(*impl_->statement_state)) {
        Close();
      }
      return std::unexpected(std::move(error));
    }
    if (full) {
      const std::size_t actual_removed = impl_->RemoveLargest();
      if (actual_removed != removed_bytes) {
        std::terminate();
      }
    }
    if (removed_bytes > key_bytes) {
      const auto released = impl_->Account().Release(ByteCount{removed_bytes - key_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
  }
  impl_->pending_record.emplace(std::move(key));
  impl_->pending_view = *view;
  impl_->pending_bytes = key_bytes;
  impl_->state = BoundedTopNState::kCandidatePending;
  return TopNCheckResult::kAccepted;
}

Status BoundedTopN::Insert(ByteBuffer record) {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kCandidatePending ||
      !impl_->pending_view.has_value()) {
    return std::unexpected(Misuse("top-N insertion requires a pending candidate"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto view = RecordView::Parse(record.view(), impl_->descriptor.record_options);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  if (view->field_count() != impl_->descriptor.field_count) {
    return std::unexpected(Misuse("top-N record shape does not match the descriptor"));
  }
  const RecordView pending_view = impl_->PendingView();
  auto key_comparison = CompareRecordPrefixes(*view, pending_view, impl_->descriptor.key_columns);
  if (!key_comparison.has_value()) {
    return std::unexpected(std::move(key_comparison.error()));
  }
  if (*key_comparison != std::weak_ordering::equivalent) {
    return std::unexpected(Misuse("top-N record key does not match the pending candidate"));
  }

  if (impl_->file_backed()) {
    Impl::FileBackend& file = impl_->File();
    if (file.next_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      Error error = TooLarge("top-N sequence is exhausted");
      Close();
      return std::unexpected(std::move(error));
    }
    try {
      std::vector<SqlValue> fields;
      fields.reserve(impl_->descriptor.key_field_count + 2U);
      RecordCursor cursor = pending_view.cursor();
      for (std::size_t index = 0; index < impl_->descriptor.key_field_count; ++index) {
        const std::optional<RecordFieldView> field = cursor.Next();
        if (!field.has_value()) {
          Error error = Corruption("top-N pending key ended before its descriptor");
          Close();
          return std::unexpected(std::move(error));
        }
        fields.push_back(field->ToOwned());
      }
      fields.push_back(SqlValue::Integer(static_cast<std::int64_t>(file.next_sequence)));
      fields.push_back(SqlValue::Blob(std::move(record)));
      auto queue_record = EncodeRecord(fields, impl_->descriptor.record_options);
      if (!queue_record.has_value()) {
        Error error = std::move(queue_record.error());
        Close();
        return std::unexpected(std::move(error));
      }
      auto inserted = file.root.writer().InsertEncoded(queue_record->view());
      if (!inserted.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(inserted.error());
        Close();
        return std::unexpected(std::move(error));
      }
      ++file.next_sequence;
      ++impl_->count;
      impl_->ClearPending();
      impl_->state = BoundedTopNState::kWriting;
      return {};
    } catch (const std::bad_alloc&) {
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    } catch (const std::length_error&) {
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    }
  }

  const std::size_t record_bytes = record.capacity().value();
  if (record_bytes > std::numeric_limits<std::size_t>::max() - sizeof(Impl::Entry)) {
    Error error = TooLarge("top-N record memory accounting overflowed");
    Close();
    return std::unexpected(std::move(error));
  }
  const std::size_t entry_bytes = record_bytes + sizeof(Impl::Entry);
  const std::size_t additional_bytes =
      entry_bytes > impl_->pending_bytes ? entry_bytes - impl_->pending_bytes : 0U;
  auto reserved = impl_->Account().Reserve(ByteCount{additional_bytes});
  if (!reserved.has_value()) {
    Error error = std::move(reserved.error());
    Close();
    return std::unexpected(std::move(error));
  }

  try {
    auto entry = std::make_unique<Impl::Entry>(std::move(record), *view, entry_bytes);
    impl_->InsertSorted(std::move(entry));
    if (impl_->pending_bytes > entry_bytes) {
      const auto released = impl_->Account().Release(ByteCount{impl_->pending_bytes - entry_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    }
    ++impl_->count;
    impl_->pending_view.reset();
    impl_->pending_record.reset();
    impl_->pending_bytes = 0;
    impl_->state = BoundedTopNState::kWriting;
    return {};
  } catch (const std::bad_alloc&) {
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  } catch (const std::length_error&) {
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  }
}

Status BoundedTopN::Rewind() {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kWriting) {
    return std::unexpected(Misuse("top-N rewind requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    Impl::FileBackend& file = impl_->File();
    try {
      auto cursor = IndexBtreeCursor::Open(file.root.pager(), file.root.root_page(), file.columns);
      if (!cursor.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(cursor.error());
        Close();
        return std::unexpected(std::move(error));
      }
      file.cursor.emplace(std::move(*cursor));
      auto positioned = file.cursor->First();
      if (!positioned.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(positioned.error());
        Close();
        return std::unexpected(std::move(error));
      }
      if (!*positioned) {
        if (impl_->count != 0U) {
          impl_->MarkFileFailure();
          Error error = Corruption("nonempty file-backed top-N relation has no first record");
          Close();
          return std::unexpected(std::move(error));
        }
        file.cursor.reset();
        impl_->state = BoundedTopNState::kExhausted;
        return {};
      }
      auto loaded = impl_->LoadFileCurrent();
      if (!loaded.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(loaded.error());
        Close();
        return std::unexpected(std::move(error));
      }
      impl_->state = BoundedTopNState::kPositioned;
      return {};
    } catch (const std::bad_alloc&) {
      impl_->MarkFileFailure();
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    } catch (const std::length_error&) {
      impl_->MarkFileFailure();
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    }
  }
  impl_->current = impl_->head.get();
  impl_->state =
      impl_->current == nullptr ? BoundedTopNState::kExhausted : BoundedTopNState::kPositioned;
  return {};
}

Result<RecordView> BoundedTopN::current_record() const {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kPositioned) {
    return std::unexpected(Misuse("top-N has no current record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    return std::unexpected(std::move(usable.error()));
  }
  if (impl_->file_backed()) {
    const Impl::FileBackend& file = impl_->File();
    if (!file.current_record.has_value()) {
      return std::unexpected(Misuse("file-backed top-N has no current record"));
    }
    return *file.current_record;
  }
  if (impl_->current == nullptr) {
    return std::unexpected(Misuse("top-N has no current record"));
  }
  return impl_->current->view;
}

Result<bool> BoundedTopN::Next() {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kPositioned) {
    return std::unexpected(Misuse("top-N next requires a current record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    Impl::FileBackend& file = impl_->File();
    try {
      if (!file.cursor.has_value()) {
        Error error = Internal("file-backed top-N cursor is unavailable");
        impl_->MarkFileFailure();
        Close();
        return std::unexpected(std::move(error));
      }
      impl_->ClearFileCurrent();
      auto advanced = file.cursor->Next();
      if (!advanced.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(advanced.error());
        Close();
        return std::unexpected(std::move(error));
      }
      if (!*advanced) {
        file.cursor.reset();
        impl_->state = BoundedTopNState::kExhausted;
        return false;
      }
      auto loaded = impl_->LoadFileCurrent();
      if (!loaded.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(loaded.error());
        Close();
        return std::unexpected(std::move(error));
      }
      return true;
    } catch (const std::bad_alloc&) {
      impl_->MarkFileFailure();
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    } catch (const std::length_error&) {
      impl_->MarkFileFailure();
      Error error = Error::OutOfMemory();
      Close();
      return std::unexpected(std::move(error));
    }
  }
  if (impl_->current == nullptr) {
    return std::unexpected(Misuse("top-N next requires a current record"));
  }
  impl_->current = impl_->current->next.get();
  if (impl_->current == nullptr) {
    impl_->state = BoundedTopNState::kExhausted;
    return false;
  }
  return true;
}

Status BoundedTopN::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot reset a closed top-N relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    Impl::FileBackend& file = impl_->File();
    file.cursor.reset();
    impl_->ClearFileCurrent();
    auto cleared = file.root.writer().Clear();
    if (!cleared.has_value()) {
      impl_->MarkFileFailure();
      Error error = std::move(cleared.error());
      Close();
      return std::unexpected(std::move(error));
    }
    file.next_sequence = 0;
  }
  impl_->Clear();
  impl_->state = BoundedTopNState::kWriting;
  return {};
}

void BoundedTopN::Close() noexcept { impl_.reset(); }

Result<BoundedTopN> StatementTemporaryStorage::CreateTopN(const RecordSorterDescriptor& descriptor,
                                                          std::size_t bound) {
  if (!valid()) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return BoundedTopN::Create(descriptor, bound, *this);
}

Result<void> ValidateTemporaryStorageOptions(TemporaryStorageOptions options) {
  if (!IsValid(options.mode)) {
    return std::unexpected(Misuse("temporary storage mode is invalid"));
  }
  if (options.sorter_memory_threshold.has_value() &&
      (options.sorter_memory_threshold->value() == 0 ||
       options.sorter_memory_threshold->value() > kMaximumPmaSize)) {
    return std::unexpected(Misuse("sorter memory threshold must be between 1 byte and 512 MiB"));
  }
  return {};
}

Result<TemporaryStorageFactory> TemporaryStorageFactory::Create(Vfs& vfs, const Pager& pager,
                                                                TemporaryStorageOptions options) {
  auto validated = ValidateTemporaryStorageOptions(options);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  if (!pager.uses_vfs(vfs)) {
    return std::unexpected(Misuse("temporary storage VFS does not match the attached pager"));
  }
  return TemporaryStorageFactory{vfs, pager, options};
}

bool TemporaryStorageFactory::file_spill_enabled() const noexcept {
  return options_.mode == TemporaryStoreMode::kFile;
}

ByteCount TemporaryStorageFactory::sorter_memory_threshold() const noexcept {
  if (options_.sorter_memory_threshold.has_value()) {
    return *options_.sorter_memory_threshold;
  }

  const std::size_t page_size = pager_->page_size().value();
  const std::size_t minimum = page_size * kMinimumPmaPageCount;
  const std::size_t cache_pages = pager_->cache_capacity_pages();
  const std::size_t cache_size =
      cache_pages > kMaximumPmaSize / page_size ? kMaximumPmaSize : cache_pages * page_size;
  return ByteCount{std::max(minimum, std::min(cache_size, kMaximumPmaSize))};
}

Result<StatementTemporaryStorage> TemporaryStorageFactory::CreateStatementStorage() const {
  try {
    return StatementTemporaryStorage{std::make_unique<temporary_storage_detail::StatementState>(
        *this, sorter_memory_threshold())};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace modern_sqlite
