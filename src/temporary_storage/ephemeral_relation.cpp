#include <array>
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
  if (descriptor.purpose != EphemeralRelationPurpose::kMembership &&
      descriptor.purpose != EphemeralRelationPurpose::kLookup &&
      descriptor.purpose != EphemeralRelationPurpose::ReusableMembership) {
    return std::unexpected(Misuse("ephemeral relation descriptor has an invalid purpose"));
  }
  for (const IndexColumnOrder& column : descriptor.key_columns) {
    if (!IsValid(column.direction()) || !IsValid(column.null_placement())) {
      return std::unexpected(Misuse("ephemeral relation descriptor has invalid ordering metadata"));
    }
  }
  const bool reusable = descriptor.purpose == EphemeralRelationPurpose::ReusableMembership;
  if ((descriptor.track_nulls && !reusable) ||
      (reusable &&
       (descriptor.field_count != 1U ||
        descriptor.key_columns.front().direction() != IndexSortDirection::kAscending ||
        descriptor.key_columns.front().null_placement() != IndexNullPlacement::kFirst))) {
    return std::unexpected(Misuse("reusable membership requires one ascending NULL-first key"));
  }
  return {};
}

}  // namespace

struct EphemeralRelation::Impl final {
  struct BtreeBackend {
    explicit BtreeBackend(temporary_storage_detail::RelationRootLease owned_root) noexcept
        : root(std::move(owned_root)) {}

    temporary_storage_detail::RelationRootLease root;
    std::optional<IndexBtreeCursor> cursor;
    ByteBuffer current_record;
    std::size_t current_bytes = 0;
  };

  Impl(EphemeralRelationDescriptor owned_descriptor,
       StatementTemporaryStorage& statement_storage) noexcept
      : descriptor(std::move(owned_descriptor)),
        statement_state(
            &temporary_storage_detail::StatementStorageAccess::State(statement_storage)),
        physical_file_mode(statement_storage.file_spill_enabled()) {}

  ~Impl() { ClearPosition(); }

  [[nodiscard]] bool has_btree() const noexcept { return btree.has_value(); }

  [[nodiscard]] bool file_backed() const noexcept { return physical_file_mode && has_btree(); }

  [[nodiscard]] BtreeBackend& Btree() noexcept {
    if (!btree.has_value()) {
      std::terminate();
    }
    return *btree;
  }

  [[nodiscard]] const BtreeBackend& Btree() const noexcept {
    if (!btree.has_value()) {
      std::terminate();
    }
    return *btree;
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
    if (temporary_storage_detail::StatementStorageAccess::Failed(*statement_state)) {
      return std::unexpected(Misuse("statement temporary storage is failed"));
    }
    return {};
  }

  void MarkStatementFailed() noexcept {
    temporary_storage_detail::StatementStorageAccess::MarkFailed(*statement_state);
  }

  void MarkBtreeFailure() noexcept {
    if (btree.has_value()) {
      btree->root.MarkStatementFailed();
    } else {
      MarkStatementFailed();
    }
  }

  [[nodiscard]] Status EnsureBtree() {
    if (btree.has_value()) {
      return {};
    }
    auto comparison_info = IndexComparisonInfo::CopyOf(
        descriptor.key_columns,
        IndexKeyShape{
            .key_field_count = descriptor.key_field_count,
            .all_field_count = descriptor.field_count +
                               (descriptor.purpose != EphemeralRelationPurpose::kLookup ? 1U : 0U),
        });
    if (!comparison_info.has_value()) {
      MarkStatementFailed();
      return std::unexpected(std::move(comparison_info.error()));
    }
    auto root = temporary_storage_detail::RelationArenaAccess::Acquire(*statement_state,
                                                                       std::move(*comparison_info));
    if (!root.has_value()) {
      MarkStatementFailed();
      return std::unexpected(std::move(root.error()));
    }
    btree.emplace(std::move(*root));
    return {};
  }

  void ClearBtreeCurrent() noexcept {
    current_view.reset();
    if (!btree.has_value()) {
      return;
    }
    BtreeBackend& backend = Btree();
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

  void ClearPosition() noexcept {
    ClearBtreeCurrent();
    if (btree.has_value()) {
      btree->cursor.reset();
    }
  }

  [[nodiscard]] Result<void> LoadBtreeCurrent() {
    current_view.reset();
    BtreeBackend& backend = Btree();
    if (!backend.cursor.has_value()) {
      return std::unexpected(Internal("ephemeral relation B-tree cursor is unavailable"));
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
            Corruption("ephemeral relation B-tree record has an invalid field count"));
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
    if (!reserved.has_value() && physical_file_mode &&
        reserved.error().code() == ErrorCode::kTooLarge) {
      reserved = Account().ReserveWorkingSet(ByteCount{copied_bytes});
    }
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
          Corruption("ephemeral relation B-tree record has an invalid field count"));
    }
    current_view = *parsed;
    return {};
  }

  [[nodiscard]] Result<IndexBtreeCursor*> OpenBtreeCursor() {
    BtreeBackend& backend = Btree();
    if (backend.cursor.has_value()) {
      return &*backend.cursor;
    }
    auto cursor = IndexBtreeCursor::Open(backend.root.database(), backend.root.root_page(),
                                         backend.root.writer().comparison_info());
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    return &backend.cursor.emplace(std::move(*cursor));
  }

  [[nodiscard]] Result<bool> AdvancePosition() {
    BtreeBackend& backend = Btree();
    if (!backend.cursor.has_value()) {
      return std::unexpected(Internal("ephemeral relation B-tree cursor is unavailable"));
    }
    ClearBtreeCurrent();
    auto advanced = backend.cursor->Next();
    if (!advanced.has_value()) {
      return std::unexpected(std::move(advanced.error()));
    }
    if (*advanced) {
      return true;
    }
    ClearPosition();
    return false;
  }

  [[nodiscard]] Result<std::weak_ordering> CompareCurrentPrefix(std::span<const SqlValue> key) {
    BtreeBackend& backend = Btree();
    if (!backend.cursor.has_value()) {
      return std::unexpected(Internal("ephemeral relation B-tree cursor is unavailable"));
    }
    return backend.cursor->CompareCurrent(key);
  }

  [[nodiscard]] Result<bool> SeekBtreeKey(std::span<const SqlValue> key) {
    ClearBtreeCurrent();
    if (count == 0U) {
      return false;
    }
    if (!has_btree()) {
      return std::unexpected(Internal("nonempty ephemeral relation has no B-tree"));
    }
    auto opened = OpenBtreeCursor();
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    auto positioned = (*opened)->Seek(key, BtreeSeekMode::kGreaterOrEqual);
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if (!*positioned) {
      return false;
    }
    auto comparison = CompareCurrentPrefix(key);
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    return *comparison == std::weak_ordering::equivalent;
  }

  [[nodiscard]] Status LoadNullPresence() {
    has_null_key = false;
    if (!descriptor.track_nulls || count == 0U) {
      return {};
    }
    if (!has_btree()) {
      return std::unexpected(Internal("nonempty ephemeral relation has no B-tree"));
    }
    auto opened = OpenBtreeCursor();
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    auto positioned = (*opened)->First();
    if (!positioned.has_value()) {
      return std::unexpected(std::move(positioned.error()));
    }
    if (!*positioned) {
      return std::unexpected(Corruption("nonempty ephemeral relation has no first record"));
    }
    const std::array<SqlValue, 1> null_key{SqlValue{}};
    auto comparison = CompareCurrentPrefix(null_key);
    if (!comparison.has_value()) {
      return std::unexpected(std::move(comparison.error()));
    }
    has_null_key = *comparison == std::weak_ordering::equivalent;
    return {};
  }

  [[nodiscard]] Result<EphemeralInsertResult> InsertBtree(ByteBuffer record,
                                                          EphemeralInsertMode mode) {
    BtreeBackend& backend = Btree();
    auto contained = backend.root.writer().ContainsEncoded(record.view());
    if (!contained.has_value()) {
      MarkBtreeFailure();
      return std::unexpected(std::move(contained.error()));
    }
    if (*contained) {
      if (mode == EphemeralInsertMode::kKeepExisting) {
        return EphemeralInsertResult::kDuplicate;
      }
      auto replaced = backend.root.writer().ReplaceEncoded(record.view());
      if (!replaced.has_value()) {
        MarkBtreeFailure();
        return std::unexpected(std::move(replaced.error()));
      }
      return EphemeralInsertResult::kReplaced;
    }
    if (count == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("ephemeral relation record count is exhausted"));
    }
    auto inserted = backend.root.writer().InsertEncoded(record.view());
    if (!inserted.has_value()) {
      MarkBtreeFailure();
      return std::unexpected(std::move(inserted.error()));
    }
    ++count;
    return EphemeralInsertResult::kInserted;
  }

  EphemeralRelationDescriptor descriptor;
  temporary_storage_detail::StatementState* statement_state;
  bool physical_file_mode;
  std::optional<BtreeBackend> btree;
  std::optional<RecordView> current_view;
  std::size_t count = 0;
  bool has_null_key = false;
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
    case EphemeralRelationState::kLookupReady:
      return "lookup-ready";
    case EphemeralRelationState::kLookupPositioned:
      return "lookup-positioned";
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
    auto account = storage.RegisterMemoryOwner();
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

  auto opened = impl_->EnsureBtree();
  if (!opened.has_value()) {
    Error error = std::move(opened.error());
    Close();
    return std::unexpected(std::move(error));
  }
  auto inserted = impl_->InsertBtree(std::move(record), mode);
  if (!inserted.has_value()) {
    Error error = std::move(inserted.error());
    Close();
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
  if (!impl_->has_btree()) {
    return false;
  }
  auto contained = impl_->Btree().root.writer().ContainsEncoded(key);
  if (!contained.has_value()) {
    impl_->MarkBtreeFailure();
    Error error = std::move(contained.error());
    Close();
    return std::unexpected(std::move(error));
  }
  return *contained;
}

Result<bool> EphemeralRelation::Contains(std::span<const SqlValue> key) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kLookupReady ||
      impl_->descriptor.purpose != EphemeralRelationPurpose::ReusableMembership) {
    return std::unexpected(Misuse("borrowed membership requires a sealed reusable relation"));
  }
  if (key.size() != impl_->descriptor.key_field_count) {
    return std::unexpected(Misuse("ephemeral membership key has an invalid width"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  try {
    auto contained = impl_->SeekBtreeKey(key);
    if (!contained.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(contained.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return *contained;
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> EphemeralRelation::HasNullKey() const {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kLookupReady ||
      impl_->descriptor.purpose != EphemeralRelationPurpose::ReusableMembership ||
      !impl_->descriptor.track_nulls) {
    return std::unexpected(Misuse("null presence requires a tracked sealed membership relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    return std::unexpected(std::move(usable.error()));
  }
  return impl_->has_null_key;
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
  if (!impl_->has_btree()) {
    return false;
  }
  auto erased = impl_->Btree().root.writer().DeleteEncoded(key);
  if (!erased.has_value()) {
    impl_->MarkBtreeFailure();
    Error error = std::move(erased.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (*erased) {
    if (impl_->count == 0U) {
      impl_->MarkBtreeFailure();
      Error error = Internal("ephemeral relation record count underflowed");
      Close();
      return std::unexpected(std::move(error));
    }
    --impl_->count;
  }
  return *erased;
}

Status EphemeralRelation::SealForLookup() {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting ||
      impl_->descriptor.purpose == EphemeralRelationPurpose::kMembership) {
    return std::unexpected(Misuse("ephemeral lookup sealing requires a writing lookup relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  try {
    auto loaded = impl_->LoadNullPresence();
    if (!loaded.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(loaded.error());
      Close();
      return std::unexpected(std::move(error));
    }
    impl_->state = EphemeralRelationState::kLookupReady;
    return {};
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> EphemeralRelation::SeekPrefix(std::span<const SqlValue> key) {
  if (impl_ == nullptr || impl_->descriptor.purpose != EphemeralRelationPurpose::kLookup ||
      (impl_->state != EphemeralRelationState::kLookupReady &&
       impl_->state != EphemeralRelationState::kLookupPositioned)) {
    return std::unexpected(Misuse("ephemeral prefix seek requires a sealed lookup relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  impl_->state = EphemeralRelationState::kLookupReady;
  if (key.empty() || key.size() > impl_->descriptor.key_field_count) {
    return std::unexpected(Misuse("ephemeral prefix key has an invalid width"));
  }
  try {
    auto contained = impl_->SeekBtreeKey(key);
    if (!contained.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(contained.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*contained) {
      impl_->ClearPosition();
      return false;
    }
    auto loaded = impl_->LoadBtreeCurrent();
    if (!loaded.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(loaded.error());
      Close();
      return std::unexpected(std::move(error));
    }
    impl_->state = EphemeralRelationState::kLookupPositioned;
    return true;
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<bool> EphemeralRelation::NextPrefix(std::span<const SqlValue> key) {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kLookupPositioned) {
    return std::unexpected(Misuse("ephemeral prefix next requires a current lookup record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  if (key.empty() || key.size() > impl_->descriptor.key_field_count) {
    impl_->ClearPosition();
    impl_->state = EphemeralRelationState::kLookupReady;
    return std::unexpected(Misuse("ephemeral prefix key has an invalid width"));
  }
  try {
    auto advanced = impl_->AdvancePosition();
    if (!advanced.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(advanced.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*advanced) {
      impl_->state = EphemeralRelationState::kLookupReady;
      return false;
    }
    auto comparison = impl_->CompareCurrentPrefix(key);
    if (!comparison.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(comparison.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (*comparison != std::weak_ordering::equivalent) {
      impl_->ClearPosition();
      impl_->state = EphemeralRelationState::kLookupReady;
      return false;
    }
    auto loaded = impl_->LoadBtreeCurrent();
    if (!loaded.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(loaded.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return true;
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Status EphemeralRelation::ClearPosition() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot clear a closed ephemeral relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  if (impl_->state == EphemeralRelationState::kLookupPositioned) {
    impl_->state = EphemeralRelationState::kLookupReady;
  } else if (impl_->state == EphemeralRelationState::kPositioned) {
    impl_->state = EphemeralRelationState::kExhausted;
  }
  return {};
}

Status EphemeralRelation::Rewind() {
  if (impl_ == nullptr || impl_->state != EphemeralRelationState::kWriting ||
      impl_->descriptor.purpose != EphemeralRelationPurpose::kMembership) {
    return std::unexpected(Misuse("ephemeral relation rewind requires the writing state"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  if (impl_->count == 0U) {
    impl_->state = EphemeralRelationState::kExhausted;
    return {};
  }
  if (!impl_->has_btree()) {
    impl_->MarkStatementFailed();
    Error error = Internal("nonempty ephemeral relation has no B-tree");
    Close();
    return std::unexpected(std::move(error));
  }
  try {
    auto opened = impl_->OpenBtreeCursor();
    if (!opened.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(opened.error());
      Close();
      return std::unexpected(std::move(error));
    }
    auto positioned = (*opened)->First();
    if (!positioned.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(positioned.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*positioned) {
      impl_->MarkBtreeFailure();
      Error error = Corruption("nonempty ephemeral relation has no first record");
      Close();
      return std::unexpected(std::move(error));
    }
    auto loaded = impl_->LoadBtreeCurrent();
    if (!loaded.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(loaded.error());
      Close();
      return std::unexpected(std::move(error));
    }
    impl_->state = EphemeralRelationState::kPositioned;
    return {};
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<RecordView> EphemeralRelation::current_record() const {
  if (impl_ == nullptr ||
      (impl_->state != EphemeralRelationState::kPositioned &&
       impl_->state != EphemeralRelationState::kLookupPositioned) ||
      !impl_->current_view.has_value()) {
    return std::unexpected(Misuse("ephemeral relation has no current record"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    return std::unexpected(std::move(usable.error()));
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
  try {
    auto advanced = impl_->AdvancePosition();
    if (!advanced.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(advanced.error());
      Close();
      return std::unexpected(std::move(error));
    }
    if (!*advanced) {
      impl_->state = EphemeralRelationState::kExhausted;
      return false;
    }
    auto loaded = impl_->LoadBtreeCurrent();
    if (!loaded.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(loaded.error());
      Close();
      return std::unexpected(std::move(error));
    }
    return true;
  } catch (const std::bad_alloc&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    impl_->MarkBtreeFailure();
    Close();
    return std::unexpected(Error::OutOfMemory());
  }
}

Status EphemeralRelation::Reset() {
  if (impl_ == nullptr) {
    return std::unexpected(Misuse("cannot reset a closed ephemeral relation"));
  }
  if (impl_->descriptor.purpose == EphemeralRelationPurpose::ReusableMembership &&
      impl_->state != EphemeralRelationState::kWriting) {
    return std::unexpected(Misuse("cannot reset a sealed reusable membership relation"));
  }
  auto usable = impl_->CheckStorage();
  if (!usable.has_value()) {
    Error error = std::move(usable.error());
    Close();
    return std::unexpected(std::move(error));
  }
  impl_->ClearPosition();
  if (impl_->has_btree()) {
    auto cleared = impl_->Btree().root.writer().Clear();
    if (!cleared.has_value()) {
      impl_->MarkBtreeFailure();
      Error error = std::move(cleared.error());
      Close();
      return std::unexpected(std::move(error));
    }
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
