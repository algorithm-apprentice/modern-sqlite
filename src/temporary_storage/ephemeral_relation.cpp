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

struct EphemeralRelation::Impl {
  struct FileBackend {
    FileBackend(std::unique_ptr<Pager> owned_pager, std::vector<IndexColumnOrder> key_columns,
                IndexBtreeWriter index_writer) noexcept
        : pager(std::move(owned_pager)),
          columns(std::move(key_columns)),
          index(std::move(index_writer)),
          root(index.root_page()) {}

    std::unique_ptr<Pager> pager;
    std::vector<IndexColumnOrder> columns;
    IndexBtreeWriter index;
    PageNumber root;
    std::optional<IndexBtreeCursor> cursor;
    ByteBuffer current_record;
  };

  explicit Impl(EphemeralRelationDescriptor owned_descriptor) noexcept
      : descriptor(std::move(owned_descriptor)) {}

  Impl(EphemeralRelationDescriptor owned_descriptor, FileBackend file_backend) noexcept
      : descriptor(std::move(owned_descriptor)), file(std::move(file_backend)) {}

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

  void ClearPosition() noexcept {
    current_view.reset();
    current_index = 0;
    if (file.has_value()) {
      file->cursor.reset();
      file->current_record = ByteBuffer{};
    }
  }

  [[nodiscard]] Result<void> LoadMemoryCurrent() {
    current_view.reset();
    if (current_index >= records.size()) {
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
    auto copied = backend.cursor->CopyPayload();
    if (!copied.has_value()) {
      return std::unexpected(std::move(copied.error()));
    }
    backend.current_record = std::move(*copied);
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
    std::size_t upper = records.size();
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

  EphemeralRelationDescriptor descriptor;
  std::vector<ByteBuffer> records;
  std::optional<FileBackend> file;
  std::optional<RecordView> current_view;
  std::size_t current_index = 0;
  std::size_t count = 0;
  EphemeralRelationState state = EphemeralRelationState::kWriting;
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
                                                    const TemporaryStorageFactory& factory) {
  auto validated = ValidateDescriptor(descriptor);
  if (!validated.has_value()) {
    return std::unexpected(std::move(validated.error()));
  }
  try {
    if (factory.file_spill_enabled()) {
      std::vector<IndexColumnOrder> columns = descriptor.key_columns;
      const std::size_t page_size = factory.pager_->page_size().value();
      const std::size_t cache_pages =
          std::max<std::size_t>(1U, factory.pager_->cache_capacity_pages());
      auto pager =
          Pager::OpenEphemeral(*factory.vfs_, PagerOptions{
                                                  .empty_database_page_size = ByteCount{page_size},
                                                  .cache_capacity_pages = cache_pages,
                                              });
      if (!pager.has_value()) {
        return std::unexpected(std::move(pager.error()));
      }
      auto begun_read = (*pager)->BeginRead();
      if (!begun_read.has_value()) {
        return std::unexpected(std::move(begun_read.error()));
      }
      auto begun_write = (*pager)->BeginWrite();
      if (!begun_write.has_value()) {
        return std::unexpected(std::move(begun_write.error()));
      }
      auto session = BtreeWriteSession::Open(**pager);
      if (!session.has_value()) {
        return std::unexpected(std::move(session.error()));
      }
      auto initialized = session->InitializeDatabase();
      if (!initialized.has_value()) {
        return std::unexpected(std::move(initialized.error()));
      }
      auto index = session->CreateIndexBtree(columns);
      if (!index.has_value()) {
        return std::unexpected(std::move(index.error()));
      }
      if (index->root_page() != PageNumber{2}) {
        return std::unexpected(Internal("ephemeral relation index root is not page two"));
      }
      auto impl = std::make_unique<Impl>(
          descriptor, Impl::FileBackend{std::move(*pager), std::move(columns), std::move(*index)});
      return EphemeralRelation{std::move(impl)};
    }
    return EphemeralRelation{std::make_unique<Impl>(descriptor)};
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

bool EphemeralRelation::file_backed() const noexcept {
  return impl_ != nullptr && impl_->file_backed();
}

Result<EphemeralInsertResult> EphemeralRelation::Insert(ByteBuffer record,
                                                        EphemeralInsertMode mode) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation insertion requires the writing state"));
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
    auto contained = impl_->File().index.ContainsEncoded(record.view());
    if (!contained.has_value()) {
      Error error = std::move(contained.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (*contained) {
      if (mode == EphemeralInsertMode::kKeepExisting) {
        return EphemeralInsertResult::kDuplicate;
      }
      auto replaced = impl_->File().index.ReplaceEncoded(record.view());
      if (!replaced.has_value()) {
        Error error = std::move(replaced.error());
        Close();
        return std::unexpected(std::move(error));
      }
      return EphemeralInsertResult::kReplaced;
    }
    if (impl_->count == std::numeric_limits<std::size_t>::max()) {
      Error error = TooLarge("ephemeral relation record count is exhausted");
      Close();
      return std::unexpected(std::move(error));
    }
    auto inserted = impl_->File().index.InsertEncoded(record.view());
    if (!inserted.has_value()) {
      Error error = std::move(inserted.error());
      Close();
      return std::unexpected(std::move(error));
    }
    ++impl_->count;
    return EphemeralInsertResult::kInserted;
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
    impl_->records[found->first] = std::move(record);
    return EphemeralInsertResult::kReplaced;
  }
  if (impl_->count == std::numeric_limits<std::size_t>::max()) {
    Error error = TooLarge("ephemeral relation record count is exhausted");
    Close();
    return std::unexpected(std::move(error));
  }
  try {
    impl_->records.insert(impl_->records.begin() + static_cast<std::ptrdiff_t>(found->first),
                          std::move(record));
    ++impl_->count;
    return EphemeralInsertResult::kInserted;
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

Result<bool> EphemeralRelation::Contains(ByteView key) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation membership requires the writing state"));
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
    auto contained = impl_->File().index.ContainsEncoded(key);
    if (!contained.has_value()) {
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
    auto erased = impl_->File().index.DeleteEncoded(key);
    if (!erased.has_value()) {
      Error error = std::move(erased.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (*erased) {
      if (impl_->count == 0U) {
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
  impl_->records.erase(impl_->records.begin() + static_cast<std::ptrdiff_t>(found->first));
  --impl_->count;
  return true;
}

Status EphemeralRelation::Rewind() {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("ephemeral relation rewind requires the writing state"));
  }
  impl_->ClearPosition();
  if (impl_->file_backed()) {
    try {
      Impl::FileBackend& file = impl_->File();
      auto cursor = IndexBtreeCursor::Open(*file.pager, file.root, file.columns);
      if (!cursor.has_value()) {
        Error error = std::move(cursor.error());
        Close();
        return std::unexpected(std::move(error));
      }
      file.cursor.emplace(std::move(*cursor));
      auto positioned = file.cursor->First();
      if (!positioned.has_value()) {
        Error error = std::move(positioned.error());
        Close();
        return std::unexpected(std::move(error));
      }
      if (!*positioned) {
        if (impl_->count != 0U) {
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
        Error error = std::move(loaded.error());
        Close();
        return std::unexpected(std::move(error));
      }
      impl_->state = EphemeralRelationState::kPositioned;
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
  if (impl_->records.empty()) {
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
  if (impl_->file_backed()) {
    try {
      Impl::FileBackend& file = impl_->File();
      if (!file.cursor.has_value()) {
        Error error = Internal("ephemeral relation file cursor is unavailable");
        Close();
        return std::unexpected(std::move(error));
      }
      auto advanced = file.cursor->Next();
      if (!advanced.has_value()) {
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
        Error error = std::move(loaded.error());
        Close();
        return std::unexpected(std::move(error));
      }
      return true;
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
  ++impl_->current_index;
  if (impl_->current_index == impl_->records.size()) {
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
  impl_->ClearPosition();
  if (impl_->file_backed()) {
    auto cleared = impl_->File().index.Clear();
    if (!cleared.has_value()) {
      Error error = std::move(cleared.error());
      Close();
      return std::unexpected(std::move(error));
    }
  } else {
    std::vector<ByteBuffer>{}.swap(impl_->records);
  }
  impl_->count = 0;
  impl_->state = EphemeralRelationState::kWriting;
  return {};
}

void EphemeralRelation::Close() noexcept { impl_.reset(); }

Result<EphemeralRelation> TemporaryStorageFactory::CreateEphemeralRelation(
    const EphemeralRelationDescriptor& descriptor) const {
  return EphemeralRelation::Create(descriptor, *this);
}

}  // namespace modern_sqlite
