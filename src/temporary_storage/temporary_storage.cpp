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

#include "modern_sqlite/pager/pager.hpp"

namespace modern_sqlite {
namespace {

constexpr std::size_t kMinimumPmaPageCount = 250;
constexpr std::size_t kMaximumPmaSize = 1U << 29U;
constexpr std::size_t kMergeRunCount = std::numeric_limits<std::size_t>::digits + 1U;

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

struct RecordSorter::Impl {
  struct Entry {
    Entry(ByteBuffer owned_record, RecordView record_view) noexcept
        : record(std::move(owned_record)), view(record_view) {}

    ByteBuffer record;
    RecordView view;
    std::unique_ptr<Entry> next;
  };

  using Run = std::unique_ptr<Entry>;

  Impl(RecordSorterDescriptor owned_descriptor, ByteCount threshold) noexcept
      : descriptor(std::move(owned_descriptor)), memory_threshold(threshold) {}

  ~Impl() { Clear(); }

  void Clear() noexcept {
    while (head != nullptr) {
      Run next = std::move(head->next);
      head.reset();
      head = std::move(next);
    }
    tail = nullptr;
    current = nullptr;
    count = 0;
    memory_bytes = 0;
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
    std::array<Run, kMergeRunCount> runs;
    while (head != nullptr) {
      Run run = std::move(head);
      head = std::move(run->next);
      run->next.reset();

      std::size_t level = 0;
      while (runs[level] != nullptr) {
        run = MergeRuns(std::move(runs[level]), std::move(run));
        ++level;
        if (level == runs.size()) {
          std::terminate();
        }
      }
      runs[level] = std::move(run);
    }

    Run sorted;
    for (Run& run : runs) {
      if (run == nullptr) {
        continue;
      }
      sorted = sorted == nullptr ? std::move(run) : MergeRuns(std::move(run), std::move(sorted));
    }
    head = std::move(sorted);
    tail = nullptr;
  }

  RecordSorterDescriptor descriptor;
  ByteCount memory_threshold;
  Run head;
  // Non-owning positions into the list rooted at head.
  Entry* tail = nullptr;
  Entry* current = nullptr;
  std::size_t count = 0;
  std::size_t memory_bytes = 0;
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
                                          ByteCount memory_threshold) {
  auto validated = ValidateRecordSorterDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    auto impl = std::make_unique<Impl>(descriptor, memory_threshold);
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
  return ByteCount{impl_ == nullptr ? 0U : impl_->memory_bytes};
}

Status RecordSorter::Insert(ByteBuffer record) {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kWriting) {
    return std::unexpected(Misuse("records can only be inserted while the sorter is writing"));
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
  if (entry_bytes > impl_->memory_threshold.value() ||
      impl_->memory_bytes > impl_->memory_threshold.value() - entry_bytes) {
    return std::unexpected(TooLarge("sorter memory threshold exceeded before spill support"));
  }

  try {
    auto entry = std::make_unique<Impl::Entry>(std::move(record), *view);
    Impl::Entry* const inserted = entry.get();
    if (impl_->tail == nullptr) {
      impl_->head = std::move(entry);
    } else {
      impl_->tail->next = std::move(entry);
    }
    impl_->tail = inserted;
    ++impl_->count;
    impl_->memory_bytes += entry_bytes;
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status RecordSorter::Rewind() {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kWriting) {
    return std::unexpected(Misuse("sorter rewind requires the writing state"));
  }
  impl_->Sort();
  impl_->current = impl_->head.get();
  impl_->state =
      impl_->current == nullptr ? RecordSorterState::kExhausted : RecordSorterState::kPositioned;
  return {};
}

Result<RecordView> RecordSorter::current_record() const {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned ||
      impl_->current == nullptr) {
    return std::unexpected(Misuse("sorter has no current record"));
  }
  return impl_->current->view;
}

Result<bool> RecordSorter::Next() {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned ||
      impl_->current == nullptr) {
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
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned ||
      impl_->current == nullptr) {
    return std::unexpected(Misuse("sorter comparison requires a current record"));
  }
  return CompareRecordPrefixes(impl_->current->view, record, impl_->descriptor.key_columns);
}

Status RecordSorter::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot reset a closed sorter"));
  }
  impl_->Clear();
  impl_->state = RecordSorterState::kWriting;
  return {};
}

void RecordSorter::Close() noexcept { impl_.reset(); }

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

Result<RecordSorter> TemporaryStorageFactory::CreateRecordSorter(
    const RecordSorterDescriptor& descriptor) const {
  return RecordSorter::Create(descriptor, sorter_memory_threshold());
}

Result<std::unique_ptr<File>> TemporaryStorageFactory::CreateTemporaryFile() const {
  if (!file_spill_enabled()) {
    return std::unexpected(Misuse("temporary file spill is disabled in memory mode"));
  }
  try {
    auto opened = vfs_->Open(std::nullopt, FileOpenOptions{
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

}  // namespace modern_sqlite
