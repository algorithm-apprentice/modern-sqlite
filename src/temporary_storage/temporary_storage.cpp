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
#include "modern_sqlite/pager/pager.hpp"

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

  struct PmaRun {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::size_t ordinal = 0;
  };

  struct PmaReader {
    PmaReader(File& input, PmaRun input_run) noexcept
        : file(&input), run(input_run), offset(input_run.begin) {}

    [[nodiscard]] Result<void> Advance(RecordCodecOptions options) {
      view.reset();
      record = ByteBuffer{};
      if (offset == run.end) {
        exhausted = true;
        return {};
      }
      if (offset > run.end) {
        return std::unexpected(Corruption("PMA reader advanced beyond its run"));
      }

      try {
        std::array<std::byte, 9> header{};
        const std::size_t header_size =
            static_cast<std::size_t>(std::min<std::uint64_t>(header.size(), run.end - offset));
        auto header_read =
            file->ReadAt(MutableByteView{header}.first(header_size), FileOffset{offset});
        if (!header_read.has_value()) {
          return std::unexpected(std::move(header_read.error()));
        }
        if (!header_read->complete()) {
          return std::unexpected(Corruption("PMA record-size varint is truncated"));
        }
        auto decoded = DecodeSqliteVarint(ByteView{header}.first(header_size));
        if (!decoded.has_value()) {
          return std::unexpected(Corruption("PMA record-size varint is malformed"));
        }

        const std::uint64_t size = decoded->value;
        const std::uint64_t header_bytes = decoded->bytes_consumed.value();
        if (header_bytes > run.end - offset) {
          return std::unexpected(Corruption("PMA record-size varint crosses the run boundary"));
        }
        const std::uint64_t payload_offset = offset + header_bytes;
        if (size > run.end - payload_offset || size > std::numeric_limits<std::size_t>::max()) {
          return std::unexpected(Corruption("PMA record payload crosses the run boundary"));
        }

        ByteBuffer next_record{ByteCount{static_cast<std::size_t>(size)}};
        auto payload_read = file->ReadAt(next_record.mutable_view(), FileOffset{payload_offset});
        if (!payload_read.has_value()) {
          return std::unexpected(std::move(payload_read.error()));
        }
        if (!payload_read->complete()) {
          return std::unexpected(Corruption("PMA record payload is truncated"));
        }
        offset = payload_offset + size;
        record = std::move(next_record);
        auto parsed = RecordView::Parse(record.view(), options);
        if (!parsed.has_value()) {
          return std::unexpected(std::move(parsed.error()));
        }
        view = *parsed;
        exhausted = false;
        return {};
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      } catch (const std::length_error&) {
        return std::unexpected(Error::OutOfMemory());
      }
    }

    File* file;
    PmaRun run;
    std::uint64_t offset;
    ByteBuffer record;
    std::optional<RecordView> view;
    bool exhausted = false;
  };

  struct MergeCursor {
    explicit MergeCursor(const RecordSorterDescriptor& sorter_descriptor) noexcept
        : descriptor(&sorter_descriptor) {}

    [[nodiscard]] static Result<std::unique_ptr<MergeCursor>> Create(
        File& file, std::span<const PmaRun> runs, const RecordSorterDescriptor& descriptor) {
      try {
        auto cursor = std::make_unique<MergeCursor>(descriptor);
        cursor->readers.reserve(runs.size());
        cursor->heap.reserve(runs.size());
        for (const PmaRun& run : runs) {
          cursor->readers.emplace_back(file, run);
          auto advanced = cursor->readers.back().Advance(descriptor.record_options);
          if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
          }
          if (!cursor->readers.back().exhausted) {
            cursor->heap.push_back(cursor->readers.size() - 1U);
          }
        }
        if (!cursor->heap.empty()) {
          for (std::size_t index = cursor->heap.size() / 2U; index > 0U; --index) {
            cursor->SiftDown(index - 1U);
          }
        }
        return cursor;
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      } catch (const std::length_error&) {
        return std::unexpected(Error::OutOfMemory());
      }
    }

    [[nodiscard]] bool empty() const noexcept { return heap.empty(); }

    [[nodiscard]] const RecordView& current() const noexcept {
      if (heap.empty()) {
        std::terminate();
      }
      const std::optional<RecordView>& record_view = readers[heap.front()].view;
      if (!record_view.has_value()) {
        std::terminate();
      }
      return *record_view;  // NOLINT(bugprone-unchecked-optional-access)
    }

    [[nodiscard]] Result<bool> Next() {
      if (heap.empty()) {
        return std::unexpected(Misuse("PMA merge cursor is exhausted"));
      }
      PmaReader& reader = readers[heap.front()];
      auto advanced = reader.Advance(descriptor->record_options);
      if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
      }
      if (reader.exhausted) {
        heap.front() = heap.back();
        heap.pop_back();
      }
      if (!heap.empty()) {
        SiftDown(0);
      }
      return !heap.empty();
    }

   private:
    [[nodiscard]] bool ComesBefore(std::size_t left_index, std::size_t right_index) const noexcept {
      const PmaReader& left = readers[left_index];
      const PmaReader& right = readers[right_index];
      if (!left.view.has_value() || !right.view.has_value()) {
        std::terminate();
      }
      const auto comparison =
          CompareRecordPrefixes(*left.view, *right.view, descriptor->key_columns);
      if (!comparison.has_value()) {
        std::terminate();
      }
      if (*comparison == std::weak_ordering::equivalent) {
        return left.run.ordinal < right.run.ordinal;
      }
      return *comparison == std::weak_ordering::less;
    }

    void SiftDown(std::size_t parent) noexcept {
      while (true) {
        const std::size_t left = (parent * 2U) + 1U;
        if (left >= heap.size()) {
          return;
        }
        const std::size_t right = left + 1U;
        std::size_t selected = left;
        if (right < heap.size() && ComesBefore(heap[right], heap[left])) {
          selected = right;
        }
        if (!ComesBefore(heap[selected], heap[parent])) {
          return;
        }
        std::swap(heap[parent], heap[selected]);
        parent = selected;
      }
    }

    const RecordSorterDescriptor* descriptor;
    std::vector<PmaReader> readers;
    std::vector<std::size_t> heap;
  };

  Impl(RecordSorterDescriptor owned_descriptor, ByteCount threshold,
       const TemporaryStorageFactory& owning_factory) noexcept
      : descriptor(std::move(owned_descriptor)),
        memory_threshold(threshold),
        factory(&owning_factory) {}

  ~Impl() { ClearAll(); }

  void ClearList() noexcept {
    while (head != nullptr) {
      Run next = std::move(head->next);
      head.reset();
      head = std::move(next);
    }
    tail = nullptr;
    current = nullptr;
    memory_bytes = 0;
  }

  void ClearAll() noexcept {
    merge.reset();
    ClearList();
    spill_file.reset();
    std::vector<PmaRun>{}.swap(runs);
    count = 0;
    spill_size = 0;
    next_run_ordinal = 0;
    merge_levels = 0;
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

  [[nodiscard]] static Result<std::uint64_t> WriteRecord(File& file, std::uint64_t offset,
                                                         ByteView record) {
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
    if (header_size > std::numeric_limits<std::uint64_t>::max() - offset ||
        record_size > std::numeric_limits<std::uint64_t>::max() - offset - header_size) {
      return std::unexpected(TooLarge("PMA file offset is not representable"));
    }
    auto header_write = file.WriteAt(ByteView{header}.first(encoded->value()), FileOffset{offset});
    if (!header_write.has_value()) {
      return std::unexpected(std::move(header_write.error()));
    }
    auto record_write = file.WriteAt(record, FileOffset{offset + header_size});
    if (!record_write.has_value()) {
      return std::unexpected(std::move(record_write.error()));
    }
    return offset + header_size + record_size;
  }

  [[nodiscard]] Result<void> EnsureSpillFile() {
    if (spill_file != nullptr) {
      return {};
    }
    auto opened = factory->CreateTemporaryFile();
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    spill_file = std::move(*opened);
    spill_size = 0;
    return {};
  }

  [[nodiscard]] Result<void> SpillCurrentRun() {
    if (head == nullptr) {
      return std::unexpected(Misuse("cannot spill an empty sorter run"));
    }
    Sort();
    try {
      runs.reserve(runs.size() + 1U);
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      return std::unexpected(Error::OutOfMemory());
    }
    auto file_ready = EnsureSpillFile();
    if (!file_ready.has_value()) {
      return file_ready;
    }

    const std::uint64_t begin = spill_size;
    std::uint64_t offset = begin;
    for (const Entry* entry = head.get(); entry != nullptr; entry = entry->next.get()) {
      auto written = WriteRecord(*spill_file, offset, entry->record.view());
      if (!written.has_value()) {
        return std::unexpected(std::move(written.error()));
      }
      offset = *written;
    }
    runs.push_back(PmaRun{
        .begin = begin,
        .end = offset,
        .ordinal = next_run_ordinal++,
    });
    spill_size = offset;
    ClearList();
    return {};
  }

  [[nodiscard]] Result<PmaRun> MergeGroupToFile(std::span<const PmaRun> group, File& output,
                                                std::uint64_t begin) const {
    auto cursor = MergeCursor::Create(*spill_file, group, descriptor);
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    std::uint64_t offset = begin;
    while (!(*cursor)->empty()) {
      auto written = WriteRecord(output, offset, (*cursor)->current().encoded());
      if (!written.has_value()) {
        return std::unexpected(std::move(written.error()));
      }
      offset = *written;
      auto advanced = (*cursor)->Next();
      if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
      }
    }
    return PmaRun{
        .begin = begin,
        .end = offset,
        .ordinal = group.front().ordinal,
    };
  }

  [[nodiscard]] Result<void> BuildMergeLevels() {
    while (runs.size() > kMergeFanIn) {
      auto next_file = factory->CreateTemporaryFile();
      if (!next_file.has_value()) {
        return std::unexpected(std::move(next_file.error()));
      }
      std::vector<PmaRun> next_runs;
      try {
        next_runs.reserve((runs.size() + kMergeFanIn - 1U) / kMergeFanIn);
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      } catch (const std::length_error&) {
        return std::unexpected(Error::OutOfMemory());
      }

      std::uint64_t next_size = 0;
      for (std::size_t begin = 0; begin < runs.size(); begin += kMergeFanIn) {
        const std::size_t group_count = std::min(kMergeFanIn, runs.size() - begin);
        auto merged = MergeGroupToFile(std::span<const PmaRun>{runs}.subspan(begin, group_count),
                                       **next_file, next_size);
        if (!merged.has_value()) {
          return std::unexpected(std::move(merged.error()));
        }
        next_size = merged->end;
        next_runs.push_back(*merged);
      }
      spill_file = std::move(*next_file);
      spill_size = next_size;
      runs = std::move(next_runs);
      ++merge_levels;
    }
    return {};
  }

  RecordSorterDescriptor descriptor;
  ByteCount memory_threshold;
  const TemporaryStorageFactory* factory;
  Run head;
  // Non-owning positions into the list rooted at head.
  Entry* tail = nullptr;
  Entry* current = nullptr;
  std::size_t count = 0;
  std::size_t memory_bytes = 0;
  std::unique_ptr<File> spill_file;
  std::vector<PmaRun> runs;
  std::uint64_t spill_size = 0;
  std::size_t next_run_ordinal = 0;
  std::size_t merge_levels = 0;
  std::unique_ptr<MergeCursor> merge;
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
                                          ByteCount memory_threshold,
                                          const TemporaryStorageFactory& factory) {
  auto validated = ValidateRecordSorterDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    auto impl = std::make_unique<Impl>(descriptor, memory_threshold, factory);
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

bool RecordSorter::has_spilled() const noexcept {
  return impl_ != nullptr && impl_->spill_file != nullptr;
}

std::size_t RecordSorter::spilled_run_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->runs.size();
}

std::size_t RecordSorter::merge_level_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->merge_levels;
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
  const bool exceeds_threshold =
      entry_bytes > impl_->memory_threshold.value() ||
      impl_->memory_bytes > impl_->memory_threshold.value() - entry_bytes;
  if (exceeds_threshold && impl_->head != nullptr) {
    if (!impl_->factory->file_spill_enabled()) {
      return std::unexpected(TooLarge("sorter memory threshold exceeded in memory mode"));
    }
    auto spilled = impl_->SpillCurrentRun();
    if (!spilled.has_value()) {
      Error error = std::move(spilled.error());
      Close();
      return std::unexpected(std::move(error));
    }
  } else if (exceeds_threshold && !impl_->factory->file_spill_enabled()) {
    return std::unexpected(TooLarge("sorter memory threshold exceeded in memory mode"));
  }
  if (impl_->count == std::numeric_limits<std::size_t>::max()) {
    return std::unexpected(TooLarge("sorter record count is exhausted"));
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
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  } catch (const std::length_error&) {
    Error error = Error::OutOfMemory();
    Close();
    return std::unexpected(std::move(error));
  }
}

Status RecordSorter::Rewind() {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kWriting) {
    return std::unexpected(Misuse("sorter rewind requires the writing state"));
  }
  if (impl_->runs.empty()) {
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
  auto merge = Impl::MergeCursor::Create(*impl_->spill_file, impl_->runs, impl_->descriptor);
  if (!merge.has_value()) {
    Error error = std::move(merge.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->merge = std::move(*merge);
  impl_->state =
      impl_->merge->empty() ? RecordSorterState::kExhausted : RecordSorterState::kPositioned;
  return {};
}

Result<RecordView> RecordSorter::current_record() const {
  if (impl_ == nullptr || impl_->state != RecordSorterState::kPositioned) {
    return std::unexpected(Misuse("sorter has no current record"));
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
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*advanced) {
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
  impl_->ClearAll();
  impl_->state = RecordSorterState::kWriting;
  return {};
}

void RecordSorter::Close() noexcept { impl_.reset(); }

struct BoundedTopN::Impl {
  struct Entry {
    Entry(ByteBuffer owned_record, RecordView record_view, std::size_t owned_bytes) noexcept
        : record(std::move(owned_record)), view(record_view), memory_bytes(owned_bytes) {}

    ByteBuffer record;
    RecordView view;
    std::size_t memory_bytes;
    std::unique_ptr<Entry> next;
  };

  Impl(RecordSorterDescriptor owned_descriptor, std::size_t retained_limit,
       ByteCount threshold) noexcept
      : descriptor(std::move(owned_descriptor)),
        maximum_records(retained_limit),
        memory_threshold(threshold) {}

  ~Impl() { Clear(); }

  void Clear() noexcept {
    while (head != nullptr) {
      std::unique_ptr<Entry> next = std::move(head->next);
      head.reset();
      head = std::move(next);
    }
    current = nullptr;
    pending_record.reset();
    pending_view.reset();
    pending_bytes = 0;
    count = 0;
    memory_bytes = 0;
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
    memory_bytes -= removed_bytes;
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

  RecordSorterDescriptor descriptor;
  std::size_t maximum_records;
  ByteCount memory_threshold;
  std::unique_ptr<Entry> head;
  Entry* current = nullptr;
  std::optional<ByteBuffer> pending_record;
  std::optional<RecordView> pending_view;
  std::size_t pending_bytes = 0;
  std::size_t count = 0;
  std::size_t memory_bytes = 0;
  BoundedTopNState state = BoundedTopNState::kWriting;
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
                                        ByteCount memory_threshold) {
  auto validated = ValidateRecordSorterDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    auto impl = std::make_unique<Impl>(descriptor, bound, memory_threshold);
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
  return ByteCount{impl_ == nullptr ? 0U : impl_->memory_bytes};
}

bool BoundedTopN::has_pending_candidate() const noexcept {
  return impl_ != nullptr && impl_->state == BoundedTopNState::kCandidatePending;
}

Result<TopNCheckResult> BoundedTopN::CheckCandidate(ByteBuffer key) {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kWriting) {
    return std::unexpected(Misuse("top-N admission requires the writing state"));
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

  std::size_t removed_bytes = 0;
  const bool full = impl_->count == impl_->maximum_records;
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

  const std::size_t key_bytes = key.capacity().value();
  if (impl_->memory_bytes < removed_bytes ||
      key_bytes > std::numeric_limits<std::size_t>::max() - (impl_->memory_bytes - removed_bytes)) {
    return std::unexpected(TooLarge("top-N candidate memory accounting overflowed"));
  }
  const std::size_t projected = impl_->memory_bytes - removed_bytes + key_bytes;
  if (projected > impl_->memory_threshold.value()) {
    return std::unexpected(TooLarge("top-N memory threshold exceeded before spill support"));
  }

  if (full) {
    static_cast<void>(impl_->RemoveLargest());
  }
  impl_->pending_record.emplace(std::move(key));
  impl_->pending_view = *view;
  impl_->pending_bytes = key_bytes;
  impl_->memory_bytes += key_bytes;
  impl_->state = BoundedTopNState::kCandidatePending;
  return TopNCheckResult::kAccepted;
}

Status BoundedTopN::Insert(ByteBuffer record) {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kCandidatePending ||
      !impl_->pending_view.has_value()) {
    return std::unexpected(Misuse("top-N insertion requires a pending candidate"));
  }
  auto view = RecordView::Parse(record.view(), impl_->descriptor.record_options);
  if (!view.has_value()) {
    return std::unexpected(std::move(view.error()));
  }
  if (view->field_count() != impl_->descriptor.field_count) {
    return std::unexpected(Misuse("top-N record shape does not match the descriptor"));
  }
  auto key_comparison =
      CompareRecordPrefixes(*view, *impl_->pending_view, impl_->descriptor.key_columns);
  if (!key_comparison.has_value()) {
    return std::unexpected(std::move(key_comparison.error()));
  }
  if (*key_comparison != std::weak_ordering::equivalent) {
    return std::unexpected(Misuse("top-N record key does not match the pending candidate"));
  }

  const std::size_t record_bytes = record.capacity().value();
  if (record_bytes > std::numeric_limits<std::size_t>::max() - sizeof(Impl::Entry)) {
    Error error = TooLarge("top-N record memory accounting overflowed");
    Close();
    return std::unexpected(std::move(error));
  }
  const std::size_t entry_bytes = record_bytes + sizeof(Impl::Entry);
  const std::size_t retained_bytes = impl_->memory_bytes - impl_->pending_bytes;
  if (entry_bytes > std::numeric_limits<std::size_t>::max() - retained_bytes ||
      retained_bytes + entry_bytes > impl_->memory_threshold.value()) {
    Error error = TooLarge("top-N memory threshold exceeded before spill support");
    Close();
    return std::unexpected(std::move(error));
  }

  try {
    auto entry = std::make_unique<Impl::Entry>(std::move(record), *view, entry_bytes);
    impl_->InsertSorted(std::move(entry));
    impl_->memory_bytes = retained_bytes + entry_bytes;
    ++impl_->count;
    impl_->pending_record.reset();
    impl_->pending_view.reset();
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
  impl_->current = impl_->head.get();
  impl_->state =
      impl_->current == nullptr ? BoundedTopNState::kExhausted : BoundedTopNState::kPositioned;
  return {};
}

Result<RecordView> BoundedTopN::current_record() const {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kPositioned ||
      impl_->current == nullptr) {
    return std::unexpected(Misuse("top-N has no current record"));
  }
  return impl_->current->view;
}

Result<bool> BoundedTopN::Next() {
  if (impl_ == nullptr || impl_->state != BoundedTopNState::kPositioned ||
      impl_->current == nullptr) {
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
  impl_->Clear();
  impl_->state = BoundedTopNState::kWriting;
  return {};
}

void BoundedTopN::Close() noexcept { impl_.reset(); }

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
  return RecordSorter::Create(descriptor, sorter_memory_threshold(), *this);
}

Result<BoundedTopN> TemporaryStorageFactory::CreateTopN(const RecordSorterDescriptor& descriptor,
                                                        std::size_t bound) const {
  return BoundedTopN::Create(descriptor, bound, sorter_memory_threshold());
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
