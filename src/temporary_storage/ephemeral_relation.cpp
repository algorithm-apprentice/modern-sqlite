#include <algorithm>
#include <compare>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "temporary_storage_internal.hpp"

namespace modern_sqlite {
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

[[nodiscard]] Error Internal(std::string_view message) noexcept {
  return MakeError(ErrorCode::kInternal, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] bool IsValid(EphemeralInsertMode mode) noexcept {
  return mode == EphemeralInsertMode::kKeepExisting ||
         mode == EphemeralInsertMode::kReplaceExisting;
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

[[nodiscard]] Result<void> ValidateDescriptor(const EphemeralRelationDescriptor& descriptor) {
  if (descriptor.field_count == 0 || descriptor.field_count > kMaximumRecordFieldCount ||
      descriptor.key_field_count == 0 || descriptor.key_field_count > descriptor.field_count ||
      descriptor.key_columns.size() != descriptor.key_field_count) {
    return std::unexpected(Misuse("ephemeral relation descriptor has an invalid field shape"));
  }
  if (!IsValid(descriptor.record_options.schema_format)) {
    return std::unexpected(Misuse("ephemeral relation descriptor has an invalid record format"));
  }
  for (const IndexColumnOrder& column : descriptor.key_columns) {
    if (!IsValid(column.direction()) || !IsValid(column.null_placement())) {
      return std::unexpected(Misuse("ephemeral relation descriptor has invalid ordering metadata"));
    }
  }
  return {};
}

}  // namespace

struct EphemeralRelation::Impl final : TemporaryStorageSpillTarget {
  struct FileBackend {
    explicit FileBackend(temporary_storage_detail::RelationRootLease owned_root) noexcept
        : root(std::move(owned_root)) {}

    temporary_storage_detail::RelationRootLease root;
    std::optional<IndexBtreeCursor> cursor;
    ByteBuffer current_record;
    std::size_t current_bytes = 0;
  };

  Impl(EphemeralRelationDescriptor owned_descriptor,
       StatementTemporaryStorage& statement_storage) noexcept
      : descriptor(std::move(owned_descriptor)),
        statement_state(&temporary_storage_detail::RelationArenaAccess::State(statement_storage)),
        memory_threshold(statement_storage.memory_budget()),
        spill_enabled(statement_storage.file_spill_enabled()) {}

  ~Impl() override {
    ClearPosition();
    ReleaseMemoryStorage();
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

  void ClearFileCurrent() noexcept {
    current_view.reset();
    if (file.has_value()) {
      FileBackend& backend = File();
      if (backend.current_bytes != 0U) {
        const std::size_t released_bytes = backend.current_bytes;
        backend.current_record = ByteBuffer{};
        backend.current_bytes = 0;
        const auto released = Account().Release(ByteCount{released_bytes});
        if (!released.has_value()) {
          std::terminate();
        }
      } else {
        backend.current_record = ByteBuffer{};
      }
    }
  }

  void ClearPosition() noexcept {
    ClearFileCurrent();
    current_index = 0;
    if (file.has_value()) {
      file->cursor.reset();
    }
  }

  void ReleaseMemoryStorage() noexcept {
    if (memory_storage_bytes != 0U) {
      const std::size_t released_bytes = memory_storage_bytes;
      records.reset();
      record_capacity = 0;
      memory_storage_bytes = 0;
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
    } else {
      records.reset();
      record_capacity = 0;
    }
  }

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
    if (temporary_storage_detail::RelationArenaAccess::Failed(*statement_state)) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    return {};
  }

  void MarkFileFailure() noexcept {
    if (file.has_value()) {
      file->root.MarkStatementFailed();
    }
  }

  [[nodiscard]] ByteCount reclaimable_bytes() const noexcept override {
    if (!spill_enabled || file_backed() || state != EphemeralRelationState::kWriting) {
      return ByteCount{0};
    }
    return ByteCount{memory_storage_bytes};
  }

  [[nodiscard]] Status SpillForPressure() override { return MigrateToFile(true); }

  [[nodiscard]] Result<void> MigrateToFile(bool release_memory) {
    if (file_backed()) {
      return {};
    }
    if (!spill_enabled) {
      return std::unexpected(TooLarge("ephemeral relation cannot spill in memory mode"));
    }
    if (state != EphemeralRelationState::kWriting) {
      return std::unexpected(Misuse("ephemeral relation cannot spill outside the writing state"));
    }
    auto root = temporary_storage_detail::RelationArenaAccess::Acquire(*statement_state,
                                                                       descriptor.key_columns);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    for (std::size_t index = 0; index < count; ++index) {
      auto inserted = root->writer().InsertEncoded(records[index].view());
      if (!inserted.has_value()) {
        root->MarkStatementFailed();
        return std::unexpected(std::move(inserted.error()));
      }
    }
    file.emplace(std::move(*root));
    if (release_memory) {
      ReleaseMemoryStorage();
    }
    return {};
  }

  [[nodiscard]] Result<void> LoadMemoryCurrent() {
    current_view.reset();
    if (current_index >= count) {
      return std::unexpected(Internal("ephemeral relation memory cursor is out of range"));
    }
    auto parsed = RecordView::Parse(records[current_index].view(), descriptor.record_options);
    if (!parsed.has_value()) {
      return std::unexpected(std::move(parsed.error()));
    }
    if (parsed->field_count() != descriptor.field_count) {
      return std::unexpected(
          Corruption("ephemeral relation memory record has an invalid field count"));
    }
    current_view = *parsed;
    return {};
  }

  [[nodiscard]] Result<void> LoadFileCurrent() {
    current_view.reset();
    FileBackend& backend = File();
    if (!backend.cursor.has_value()) {
      return std::unexpected(Internal("ephemeral relation file cursor is unavailable"));
    }
    auto payload = backend.cursor->payload();
    if (!payload.has_value()) {
      return std::unexpected(std::move(payload.error()));
    }
    if (payload->is_fully_local()) {
      auto parsed = RecordView::Parse(payload->local_bytes(), descriptor.record_options);
      if (!parsed.has_value()) {
        return std::unexpected(std::move(parsed.error()));
      }
      if (parsed->field_count() != descriptor.field_count) {
        return std::unexpected(
            Corruption("ephemeral relation file record has an invalid field count"));
      }
      current_view = *parsed;
      return {};
    }
    auto copied = backend.cursor->CopyPayload();
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    const std::size_t copied_bytes = copied->capacity().value();
    auto reserved = Account().Reserve(ByteCount{copied_bytes});
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }
    backend.current_record = std::move(*copied);
    backend.current_bytes = copied_bytes;
    auto parsed = RecordView::Parse(backend.current_record.view(), descriptor.record_options);
    if (!parsed.has_value()) {
      return std::unexpected(std::move(parsed.error()));
    }
    if (parsed->field_count() != descriptor.field_count) {
      return std::unexpected(
          Corruption("ephemeral relation file record has an invalid field count"));
    }
    current_view = *parsed;
    return {};
  }

  [[nodiscard]] Result<std::pair<std::size_t, bool>> FindMemory(const RecordView& key) const {
    std::size_t lower = 0;
    std::size_t upper = count;
    while (lower < upper) {
      const std::size_t middle = lower + (upper - lower) / 2U;
      auto stored = RecordView::Parse(records[middle].view(), descriptor.record_options);
      if (!stored.has_value()) {
        return std::unexpected(std::move(stored.error()));
      }
      auto comparison = CompareRecordPrefixes(*stored, key, descriptor.key_columns);
      if (!comparison.has_value()) {
        return std::unexpected(std::move(comparison.error()));
      }
      if (*comparison == std::weak_ordering::equivalent) {
        return std::pair{middle, true};
      }
      if (*comparison == std::weak_ordering::less) {
        lower = middle + 1U;
      } else {
        upper = middle;
      }
    }
    return std::pair{lower, false};
  }

  [[nodiscard]] Result<std::size_t> GrowthCapacity() const {
    if (count < record_capacity) {
      return record_capacity;
    }
    if (record_capacity == 0U) {
      return 1U;
    }
    if (record_capacity > std::numeric_limits<std::size_t>::max() / 2U) {
      return std::unexpected(TooLarge("ephemeral relation capacity is exhausted"));
    }
    return record_capacity * 2U;
  }

  [[nodiscard]] static Result<std::size_t> ArrayBytes(std::size_t capacity) {
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(ByteBuffer)) {
      return std::unexpected(TooLarge("ephemeral relation array accounting overflowed"));
    }
    return capacity * sizeof(ByteBuffer);
  }

  [[nodiscard]] Result<EphemeralInsertResult> InsertFile(ByteBuffer record,
                                                         EphemeralInsertMode mode) {
    FileBackend& backend = File();
    auto contained = backend.root.writer().ContainsEncoded(record.view());
    if (!contained.has_value()) {
      MarkFileFailure();
      return std::unexpected(std::move(contained.error()));
    }
    if (*contained) {
      if (mode == EphemeralInsertMode::kKeepExisting) {
        return EphemeralInsertResult::kDuplicate;
      }
      auto replaced = backend.root.writer().ReplaceEncoded(record.view());
      if (!replaced.has_value()) {
        MarkFileFailure();
        return std::unexpected(std::move(replaced.error()));
      }
      return EphemeralInsertResult::kReplaced;
    }
    if (count == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("ephemeral relation record count is exhausted"));
    }
    auto inserted = backend.root.writer().InsertEncoded(record.view());
    if (!inserted.has_value()) {
      MarkFileFailure();
      return std::unexpected(std::move(inserted.error()));
    }
    ++count;
    return EphemeralInsertResult::kInserted;
  }

  [[nodiscard]] Result<EphemeralInsertResult> ReplaceMemory(std::size_t index, ByteBuffer record) {
    const std::size_t old_bytes = records[index].capacity().value();
    const std::size_t new_bytes = record.capacity().value();
    if (new_bytes > old_bytes) {
      const std::size_t additional = new_bytes - old_bytes;
      if (additional > memory_threshold.value() ||
          memory_storage_bytes > memory_threshold.value() - additional) {
        if (!spill_enabled) {
          return std::unexpected(
              TooLarge("ephemeral relation memory threshold exceeded in memory mode"));
        }
        auto spilled = MigrateToFile(false);
        if (!spilled.has_value()) {
          return std::unexpected(std::move(spilled.error()));
        }
        auto replaced = InsertFile(std::move(record), EphemeralInsertMode::kReplaceExisting);
        if (!replaced.has_value()) {
          return std::unexpected(std::move(replaced.error()));
        }
        if (*replaced != EphemeralInsertResult::kReplaced) {
          MarkFileFailure();
          return std::unexpected(Internal("migrated ephemeral relation replacement lost its key"));
        }
        ReleaseMemoryStorage();
        return *replaced;
      }
      auto reserved = Account().Reserve(ByteCount{additional});
      if (!reserved.has_value()) {
        return std::unexpected(std::move(reserved.error()));
      }
      if (file_backed()) {
        const auto released = Account().Release(ByteCount{additional});
        if (!released.has_value()) {
          std::terminate();
        }
        auto replaced = InsertFile(std::move(record), EphemeralInsertMode::kReplaceExisting);
        if (!replaced.has_value()) {
          return std::unexpected(std::move(replaced.error()));
        }
        if (*replaced != EphemeralInsertResult::kReplaced) {
          MarkFileFailure();
          return std::unexpected(Internal("pressure-spilled relation replacement lost its key"));
        }
        return *replaced;
      }
      memory_storage_bytes += additional;
    } else if (old_bytes > new_bytes) {
      const std::size_t released_bytes = old_bytes - new_bytes;
      if (released_bytes > memory_storage_bytes) {
        std::terminate();
      }
      records[index] = std::move(record);
      const auto released = Account().Release(ByteCount{released_bytes});
      if (!released.has_value()) {
        std::terminate();
      }
      memory_storage_bytes -= released_bytes;
      return EphemeralInsertResult::kReplaced;
    }
    records[index] = std::move(record);
    return EphemeralInsertResult::kReplaced;
  }

  [[nodiscard]] Result<EphemeralInsertResult> InsertMemory(std::size_t index, ByteBuffer record) {
    if (count == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("ephemeral relation record count is exhausted"));
    }
    auto next_capacity = GrowthCapacity();
    if (!next_capacity.has_value()) {
      return std::unexpected(std::move(next_capacity.error()));
    }
    auto old_array_bytes = ArrayBytes(record_capacity);
    if (!old_array_bytes.has_value()) {
      return std::unexpected(std::move(old_array_bytes.error()));
    }
    auto next_array_bytes = ArrayBytes(*next_capacity);
    if (!next_array_bytes.has_value()) {
      return std::unexpected(std::move(next_array_bytes.error()));
    }
    const std::size_t array_delta = *next_array_bytes - *old_array_bytes;
    const std::size_t record_bytes = record.capacity().value();
    if (record_bytes > std::numeric_limits<std::size_t>::max() - array_delta) {
      return std::unexpected(TooLarge("ephemeral relation memory accounting overflowed"));
    }
    const std::size_t additional = array_delta + record_bytes;
    if (additional > memory_threshold.value() ||
        memory_storage_bytes > memory_threshold.value() - additional) {
      if (!spill_enabled) {
        return std::unexpected(
            TooLarge("ephemeral relation memory threshold exceeded in memory mode"));
      }
      auto spilled = MigrateToFile(false);
      if (!spilled.has_value()) {
        return std::unexpected(std::move(spilled.error()));
      }
      auto inserted = InsertFile(std::move(record), EphemeralInsertMode::kKeepExisting);
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
      if (*inserted != EphemeralInsertResult::kInserted) {
        MarkFileFailure();
        return std::unexpected(Internal("migrated ephemeral relation gained a duplicate key"));
      }
      ReleaseMemoryStorage();
      return *inserted;
    }

    auto reserved = Account().Reserve(ByteCount{additional});
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }
    if (file_backed()) {
      const auto released = Account().Release(ByteCount{additional});
      if (!released.has_value()) {
        std::terminate();
      }
      auto inserted = InsertFile(std::move(record), EphemeralInsertMode::kKeepExisting);
      if (!inserted.has_value()) {
        return std::unexpected(std::move(inserted.error()));
      }
      if (*inserted != EphemeralInsertResult::kInserted) {
        MarkFileFailure();
        return std::unexpected(Internal("pressure-spilled relation gained a duplicate key"));
      }
      return *inserted;
    }

    try {
      if (*next_capacity != record_capacity) {
        // A fixed-capacity RAII array keeps allocation accounting exact.
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        auto grown = std::make_unique<ByteBuffer[]>(*next_capacity);
        for (std::size_t current = 0; current < index; ++current) {
          grown[current] = std::move(records[current]);
        }
        grown[index] = std::move(record);
        for (std::size_t current = index; current < count; ++current) {
          grown[current + 1U] = std::move(records[current]);
        }
        records = std::move(grown);
        record_capacity = *next_capacity;
      } else {
        for (std::size_t current = count; current > index; --current) {
          records[current] = std::move(records[current - 1U]);
        }
        records[index] = std::move(record);
      }
    } catch (const std::bad_alloc&) {
      const auto released = Account().Release(ByteCount{additional});
      if (!released.has_value()) {
        std::terminate();
      }
      return std::unexpected(Error::OutOfMemory());
    }
    ++count;
    memory_storage_bytes += additional;
    return EphemeralInsertResult::kInserted;
  }

  EphemeralRelationDescriptor descriptor;
  // Statement state is heap-stable across StatementTemporaryStorage moves.
  temporary_storage_detail::StatementState* statement_state;
  ByteCount memory_threshold;
  bool spill_enabled;
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  std::unique_ptr<ByteBuffer[]> records;
  std::size_t record_capacity = 0;
  std::size_t memory_storage_bytes = 0;
  std::optional<FileBackend> file;
  std::optional<RecordView> current_view;
  std::size_t current_index = 0;
  std::size_t count = 0;
  EphemeralRelationState state = EphemeralRelationState::kWriting;
  std::optional<TemporaryMemoryAccount> memory_account;
};

std::string_view EphemeralRelationStateName(EphemeralRelationState state) noexcept {
  switch (state) {
    case EphemeralRelationState::kWriting:
      return "writing";
    case EphemeralRelationState::kPositioned:
      return "positioned";
    case EphemeralRelationState::kExhausted:
      return "exhausted";
    case EphemeralRelationState::kClosed:
      return "closed";
  }
  return "unknown";
}

EphemeralRelation::EphemeralRelation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

EphemeralRelation::EphemeralRelation(EphemeralRelation&&) noexcept = default;

EphemeralRelation& EphemeralRelation::operator=(EphemeralRelation&&) noexcept = default;

EphemeralRelation::~EphemeralRelation() = default;

Result<EphemeralRelation> EphemeralRelation::Create(const EphemeralRelationDescriptor& descriptor,
                                                    StatementTemporaryStorage& storage) {
  auto validated = ValidateDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    auto impl = std::make_unique<Impl>(descriptor, storage);
    Result<TemporaryMemoryAccount> account = storage.file_spill_enabled()
                                                 ? storage.RegisterMemoryOwner(*impl)
                                                 : storage.RegisterMemoryOwner();
    if (!account.has_value()) {
      return std::unexpected(std::move(account.error()));
    }
    impl->memory_account.emplace(std::move(*account));
    return EphemeralRelation{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

bool EphemeralRelation::valid() const noexcept { return impl_ != nullptr; }

EphemeralRelationState EphemeralRelation::state() const noexcept {
  return impl_ == nullptr ? EphemeralRelationState::kClosed : impl_->state;
}

std::size_t EphemeralRelation::record_count() const noexcept {
  return impl_ == nullptr ? 0U : impl_->count;
}

ByteCount EphemeralRelation::memory_usage() const noexcept {
  return impl_ == nullptr ? ByteCount{0} : impl_->Account().retained_bytes();
}

bool EphemeralRelation::file_backed() const noexcept {
  return impl_ != nullptr && impl_->file_backed();
}

Result<EphemeralInsertResult> EphemeralRelation::Insert(ByteBuffer record,
                                                        EphemeralInsertMode mode) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation insertion requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (!IsValid(mode)) {
    return std::unexpected(Misuse("ephemeral relation insertion mode is invalid"));
  }
  auto view = RecordView::Parse(record.view(), impl_->descriptor.record_options);
  if (!view.has_value()) {
    Error error = std::move(view.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (view->field_count() != impl_->descriptor.field_count) {
    Error error = Misuse("ephemeral relation record field count does not match the descriptor");
    Close();
    return std::unexpected(std::move(error));
  }

  if (impl_->file_backed()) {
    auto inserted = impl_->InsertFile(std::move(record), mode);
    if (!inserted.has_value()) {
      Error error = std::move(inserted.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return *inserted;
  }

  auto found = impl_->FindMemory(*view);
  if (!found.has_value()) {
    Error error = std::move(found.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (found->second) {
    if (mode == EphemeralInsertMode::kKeepExisting) {
      return EphemeralInsertResult::kDuplicate;
    }
    auto replaced = impl_->ReplaceMemory(found->first, std::move(record));
    if (!replaced.has_value()) {
      Error error = std::move(replaced.error());
      if (error.code() != ErrorCode::kTooLarge ||
          temporary_storage_detail::RelationArenaAccess::Failed(*impl_->statement_state)) {
        Close();
      }
      return std::unexpected(std::move(error));
    }
    return *replaced;
  }
  auto inserted = impl_->InsertMemory(found->first, std::move(record));
  if (!inserted.has_value()) {
    Error error = std::move(inserted.error());
    if (error.code() != ErrorCode::kTooLarge ||
        temporary_storage_detail::RelationArenaAccess::Failed(*impl_->statement_state)) {
      Close();
    }
    return std::unexpected(std::move(error));
  }
  return *inserted;
}

Result<bool> EphemeralRelation::Contains(ByteView key) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation membership requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto view = RecordView::Parse(key, impl_->descriptor.record_options);
  if (!view.has_value()) {
    Error error = std::move(view.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (view->field_count() != impl_->descriptor.key_field_count) {
    Error error = Misuse("ephemeral relation key field count does not match the descriptor");
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    auto contained = impl_->File().root.writer().ContainsEncoded(key);
    if (!contained.has_value()) {
      impl_->MarkFileFailure();
      Error error = std::move(contained.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return *contained;
  }
  auto found = impl_->FindMemory(*view);
  if (!found.has_value()) {
    Error error = std::move(found.error());
    Close();
    return std::unexpected(std::move(error));
  }
  return found->second;
}

Result<bool> EphemeralRelation::Erase(ByteView key) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation erase requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto view = RecordView::Parse(key, impl_->descriptor.record_options);
  if (!view.has_value()) {
    Error error = std::move(view.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (view->field_count() != impl_->descriptor.key_field_count) {
    Error error = Misuse("ephemeral relation key field count does not match the descriptor");
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    auto erased = impl_->File().root.writer().DeleteEncoded(key);
    if (!erased.has_value()) {
      impl_->MarkFileFailure();
      Error error = std::move(erased.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (*erased) {
      if (impl_->count == 0U) {
        impl_->MarkFileFailure();
        Error error = Internal("ephemeral relation record count underflowed");
        Close();
        return std::unexpected(std::move(error));
      }
      --impl_->count;
    }
    return *erased;
  }
  auto found = impl_->FindMemory(*view);
  if (!found.has_value()) {
    Error error = std::move(found.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (!found->second) {
    return false;
  }
  const std::size_t removed_bytes = impl_->records[found->first].capacity().value();
  for (std::size_t index = found->first; index + 1U < impl_->count; ++index) {
    impl_->records[index] = std::move(impl_->records[index + 1U]);
  }
  impl_->records[impl_->count - 1U] = ByteBuffer{};
  --impl_->count;
  if (removed_bytes > impl_->memory_storage_bytes) {
    std::terminate();
  }
  const auto released = impl_->Account().Release(ByteCount{removed_bytes});
  if (!released.has_value()) {
    std::terminate();
  }
  impl_->memory_storage_bytes -= removed_bytes;
  return true;
}

Status EphemeralRelation::Rewind() {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation rewind requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  if (impl_->file_backed()) {
    try {
      Impl::FileBackend& file = impl_->File();
      auto cursor = IndexBtreeCursor::Open(file.root.pager(), file.root.root_page(),
                                           impl_->descriptor.key_columns);
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
          Error error = Corruption("nonempty ephemeral relation has no first record");
          Close();
          return std::unexpected(std::move(error));
        }
        file.cursor.reset();
        impl_->state = EphemeralRelationState::kExhausted;
        return {};
      }
      auto loaded = impl_->LoadFileCurrent();
      if (!loaded.has_value()) {
        impl_->MarkFileFailure();
        Error error = std::move(loaded.error());
        Close();
        return std::unexpected(std::move(error));
      }
      impl_->state = EphemeralRelationState::kPositioned;
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
  if (impl_->count == 0U) {
    impl_->state = EphemeralRelationState::kExhausted;
    return {};
  }
  auto loaded = impl_->LoadMemoryCurrent();
  if (!loaded.has_value()) {
    Error error = std::move(loaded.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->state = EphemeralRelationState::kPositioned;
  return {};
}

Result<RecordView> EphemeralRelation::current_record() const {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kPositioned ||
      !impl_->current_view.has_value()) {
    return std::unexpected(Misuse("ephemeral relation has no current record"));
  }
  return *impl_->current_view;
}

Result<bool> EphemeralRelation::Next() {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kPositioned) {
    return std::unexpected(Misuse("ephemeral relation next requires a current record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (impl_->file_backed()) {
    try {
      Impl::FileBackend& file = impl_->File();
      if (!file.cursor.has_value()) {
        impl_->MarkFileFailure();
        Error error = Internal("ephemeral relation file cursor is unavailable");
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
        impl_->ClearPosition();
        impl_->state = EphemeralRelationState::kExhausted;
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
  ++impl_->current_index;
  if (impl_->current_index == impl_->count) {
    impl_->current_view.reset();
    impl_->state = EphemeralRelationState::kExhausted;
    return false;
  }
  auto loaded = impl_->LoadMemoryCurrent();
  if (!loaded.has_value()) {
    Error error = std::move(loaded.error());
    Close();
    return std::unexpected(std::move(error));
  }
  return true;
}

Status EphemeralRelation::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot reset a closed ephemeral relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  if (impl_->file_backed()) {
    auto cleared = impl_->File().root.writer().Clear();
    if (!cleared.has_value()) {
      impl_->MarkFileFailure();
      Error error = std::move(cleared.error());
      Close();
      return std::unexpected(std::move(error));
    }
  } else {
    impl_->ReleaseMemoryStorage();
  }
  impl_->count = 0;
  impl_->state = EphemeralRelationState::kWriting;
  return {};
}

void EphemeralRelation::Close() noexcept { impl_.reset(); }

Result<EphemeralRelation> StatementTemporaryStorage::CreateEphemeralRelation(
    const EphemeralRelationDescriptor& descriptor) {
  if (!valid()) {
    return std::unexpected(Misuse("statement temporary storage is invalid"));
  }
  return EphemeralRelation::Create(descriptor, *this);
}

}  // namespace modern_sqlite
