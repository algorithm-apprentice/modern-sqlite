#include "modern_sqlite/storage/btree/writer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/database_format.hpp"
#include "writer_internal.hpp"

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

[[nodiscard]] Error NotFound(std::string_view message) noexcept {
  return MakeError(ErrorCode::kNotFound, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) noexcept {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] Error Protocol(std::string_view message) noexcept {
  return MakeError(ErrorCode::kProtocol, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Result<std::vector<IndexColumnOrder>> CopyColumns(
    std::span<const IndexColumnOrder> columns) {
  if (columns.empty()) {
    return std::unexpected(Misuse("index B-tree requires comparison metadata"));
  }
  for (const IndexColumnOrder& column : columns) {
    switch (column.direction()) {
      case IndexSortDirection::kAscending:
      case IndexSortDirection::kDescending:
        break;
      default:
        return std::unexpected(Misuse("index sort direction is invalid"));
    }
    switch (column.null_placement()) {
      case IndexNullPlacement::kFirst:
      case IndexNullPlacement::kLast:
        break;
      default:
        return std::unexpected(Misuse("index null placement is invalid"));
    }
  }
  try {
    return std::vector<IndexColumnOrder>{columns.begin(), columns.end()};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace

namespace btree_internal {

class BtreeWriterCore final {
 public:
  struct RootValidation {
    std::uint64_t incarnation;
    std::uint64_t statement_epoch;
  };

  BtreeWriterCore(Pager& pager, std::uint64_t generation, BtreeWriteWorkspace workspace,
                  std::optional<BtreePageGeometry> geometry, RecordCodecOptions record_options,
                  bool managed) noexcept
      : pager_(&pager),
        pager_lifetime_(pager.lifetime_token()),
        generation_(generation),
        workspace_(std::move(workspace)),
        geometry_(geometry),
        record_options_(record_options),
        managed_(managed),
        statement_active_(!managed) {}

  [[nodiscard]] Status CheckTransaction() const {
    const Pager* const pager = LivePager();
    if (pager == nullptr || !pager->in_write_transaction() ||
        pager->write_transaction_generation() != generation_) {
      return std::unexpected(
          SchemaChanged("B-tree writer belongs to an obsolete write transaction"));
    }
    if (const auto failure = pager->write_failure_code(); failure.has_value()) {
      return std::unexpected(MakeError(*failure, "B-tree writer requires transaction rollback"));
    }
    return {};
  }

  [[nodiscard]] Status CheckActive() const {
    auto transaction = CheckTransaction();
    if (!transaction.has_value()) {
      return transaction;
    }
    if (managed_ && !statement_active_) {
      return std::unexpected(SchemaChanged("B-tree writer statement scope has ended"));
    }
    return {};
  }

  [[nodiscard]] Status BeginManagedStatement() {
    auto transaction = CheckTransaction();
    if (!transaction.has_value()) {
      return transaction;
    }
    if (!managed_) {
      return std::unexpected(Misuse("B-tree writer session is not transaction-managed"));
    }
    if (statement_active_) {
      return std::unexpected(Misuse("B-tree writer statement scope is already active"));
    }
    if (statement_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("B-tree writer statement generation is exhausted"));
    }
    ++statement_epoch_;
    statement_active_ = true;
    return {};
  }

  void EndManagedStatement() noexcept {
    if (managed_) {
      statement_active_ = false;
    }
  }

  [[nodiscard]] std::uint64_t statement_epoch() const noexcept { return statement_epoch_; }

  [[nodiscard]] bool requires_rollback() const noexcept {
    const Pager* const pager = LivePager();
    return pager != nullptr && pager->write_failure_code().has_value();
  }

  [[nodiscard]] Result<BtreePageGeometry> geometry() const {
    auto active = CheckActive();
    if (!active.has_value()) {
      return std::unexpected(std::move(active.error()));
    }
    if (!geometry_.has_value()) {
      return std::unexpected(Misuse("database must be initialized before B-tree mutation"));
    }
    return *geometry_;
  }

  [[nodiscard]] Status InitializeDatabase(BtreeDatabaseOptions options) {
    auto active = CheckActive();
    if (!active.has_value()) {
      return active;
    }
    if (pager_->page_count() != 0U || pager_->header() != nullptr || geometry_.has_value()) {
      return std::unexpected(Misuse("database is already initialized"));
    }
    if (options.reserved_bytes.value() > 255U) {
      return std::unexpected(Misuse("database reserved-byte count exceeds SQLite's limit"));
    }
    const std::size_t page_size = pager_->page_size().value();
    if (page_size < 512U || page_size > 65536U || !std::has_single_bit(page_size) ||
        options.reserved_bytes.value() > page_size ||
        page_size - options.reserved_bytes.value() < 480U) {
      return std::unexpected(Misuse("database page geometry is invalid"));
    }
    switch (options.schema_format) {
      case DatabaseSchemaFormat::kOne:
      case DatabaseSchemaFormat::kTwo:
      case DatabaseSchemaFormat::kThree:
      case DatabaseSchemaFormat::kFour:
        break;
      default:
        return std::unexpected(Misuse("database schema format is invalid"));
    }
    if (options.text_encoding != DatabaseTextEncoding::kUtf8) {
      return std::unexpected(Misuse("initial writable database requires UTF-8 encoding"));
    }
    auto next_geometry = BtreePageGeometry::Create(
        pager_->page_size(), ByteCount{page_size - options.reserved_bytes.value()});
    if (!next_geometry.has_value()) {
      return std::unexpected(std::move(next_geometry.error()));
    }

    auto page = pager_->AllocatePage();
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    if (page->frame().page_number() != PageNumber{1}) {
      pager_->ReportWriteCoordinatorFailure(ErrorCode::kCorruption);
      return std::unexpected(Corruption("database initialization did not allocate page one"));
    }
    const MutableByteView bytes = page->mutable_bytes();
    std::ranges::fill(bytes, std::byte{0});
    static constexpr std::array<std::byte, 16> kMagic{
        std::byte{0x53}, std::byte{0x51}, std::byte{0x4c}, std::byte{0x69},
        std::byte{0x74}, std::byte{0x65}, std::byte{0x20}, std::byte{0x66},
        std::byte{0x6f}, std::byte{0x72}, std::byte{0x6d}, std::byte{0x61},
        std::byte{0x74}, std::byte{0x20}, std::byte{0x33}, std::byte{0x00},
    };
    std::ranges::copy(kMagic, bytes.begin());
    const auto encoded_page_size = static_cast<std::uint16_t>(page_size == 65536U ? 1U : page_size);
    StoreBigEndian<std::uint16_t>(std::span<std::byte, 2>{bytes.data() + 16U, 2U},
                                  encoded_page_size);
    bytes[18] = std::byte{1};
    bytes[19] = std::byte{1};
    bytes[20] = static_cast<std::byte>(options.reserved_bytes.value());
    bytes[21] = std::byte{64};
    bytes[22] = std::byte{32};
    bytes[23] = std::byte{32};
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 28U, 4U}, 1U);
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 44U, 4U},
                                  static_cast<std::uint32_t>(options.schema_format));
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 56U, 4U},
                                  static_cast<std::uint32_t>(options.text_encoding));
    StoreBigEndian<std::uint32_t>(std::span<std::byte, 4>{bytes.data() + 96U, 4U}, 3'054'000U);
    bytes[100] = static_cast<std::byte>(BtreePageType::kLeafTable);
    StoreBigEndian<std::uint16_t>(
        std::span<std::byte, 2>{bytes.data() + 105U, 2U},
        static_cast<std::uint16_t>(next_geometry->usable_size().value() == 65536U
                                       ? 0U
                                       : next_geometry->usable_size().value()));
    auto header = ParseDatabaseHeader(bytes);
    if (!header.has_value()) {
      return std::unexpected(std::move(header.error()));
    }

    pager_->current_header_ = *header;
    geometry_ = *next_geometry;
    record_options_.schema_format = static_cast<RecordSchemaFormat>(options.schema_format);
    return {};
  }

  [[nodiscard]] Result<std::pair<PageNumber, std::uint64_t>> CreateRoot(bool table) {
    auto page_geometry = geometry();
    if (!page_geometry.has_value()) {
      return std::unexpected(std::move(page_geometry.error()));
    }
    MutationPageOwner owner{*pager_};
    auto root = CreateBtreeRoot(owner, *page_geometry, table);
    if (!root.has_value()) {
      return std::unexpected(std::move(root.error()));
    }
    auto incarnation = TrackRoot(*root, true);
    if (!incarnation.has_value()) {
      owner.MarkRollbackRequiredAfter(incarnation.error().code(), owner.operation_checkpoint());
      return std::unexpected(std::move(incarnation.error()));
    }
    return std::pair{*root, *incarnation};
  }

  [[nodiscard]] Result<std::uint64_t> OpenRoot(PageNumber root_page, bool table) {
    auto page_geometry = geometry();
    if (!page_geometry.has_value()) {
      return std::unexpected(std::move(page_geometry.error()));
    }
    if (root_page.value() == 0U || root_page.value() > pager_->page_count() ||
        root_page == page_geometry->locking_page()) {
      return std::unexpected(Corruption("B-tree root page is invalid"));
    }
    auto free = RootOnFreelist(root_page, *page_geometry);
    if (!free.has_value()) {
      return std::unexpected(std::move(free.error()));
    }
    if (*free) {
      return std::unexpected(Corruption("B-tree root page is on the freelist"));
    }
    auto pin = pager_->ReadPage(root_page);
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    auto page = BtreePageView::Parse(pin->frame().bytes(), root_page, *page_geometry);
    if (!page.has_value()) {
      return std::unexpected(std::move(page.error()));
    }
    if (page->is_table() != table) {
      return std::unexpected(Corruption("B-tree root has the wrong kind"));
    }
    return TrackRoot(root_page, false);
  }

  [[nodiscard]] Status ValidateRoot(PageNumber root_page, RootValidation validation) const {
    auto active = CheckActive();
    if (!active.has_value()) {
      return active;
    }
    if (managed_ && validation.statement_epoch != statement_epoch_) {
      return std::unexpected(SchemaChanged("B-tree writer handle belongs to an ended statement"));
    }
    const auto found = root_incarnations_.find(root_page.value());
    if (found == root_incarnations_.end() || found->second != validation.incarnation) {
      return std::unexpected(SchemaChanged("B-tree root handle is stale"));
    }
    return {};
  }

  void InvalidateRoot(PageNumber root_page) noexcept {
    const auto found = root_incarnations_.find(root_page.value());
    if (found != root_incarnations_.end()) {
      ++found->second;
    }
  }

  [[nodiscard]] Pager& pager() const noexcept { return *pager_; }
  [[nodiscard]] BtreeWriteWorkspace& workspace() noexcept { return workspace_; }
  [[nodiscard]] RecordCodecOptions record_options() const noexcept { return record_options_; }
  [[nodiscard]] Result<WritableCursor> OpenCursor(MutationPageOwner& owner, PageNumber root_page,
                                                  bool table) const {
    auto page_geometry = geometry();
    if (!page_geometry.has_value()) {
      return std::unexpected(std::move(page_geometry.error()));
    }
    return WritableCursor::Open(owner, root_page, table, *page_geometry);
  }

 private:
  [[nodiscard]] Pager* LivePager() const noexcept {
    return pager_lifetime_ != nullptr && pager_lifetime_->pager == pager_ ? pager_ : nullptr;
  }

  [[nodiscard]] Result<std::uint64_t> TrackRoot(PageNumber root_page, bool recreate) {
    try {
      auto [entry, inserted] = root_incarnations_.try_emplace(root_page.value(), 0U);
      if (recreate && !inserted) {
        ++entry->second;
      }
      return entry->second;
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Result<bool> RootOnFreelist(PageNumber root_page,
                                            BtreePageGeometry geometry) const {
    auto page_one = pager_->ReadPage(PageNumber{1});
    if (!page_one.has_value()) {
      return std::unexpected(std::move(page_one.error()));
    }
    const ByteView header = page_one->frame().bytes();
    const auto declared_count = LoadBigEndian<std::uint32_t>(header.subspan<36U, 4U>());
    PageNumber trunk{LoadBigEndian<std::uint32_t>(header.subspan<32U, 4U>())};
    if ((declared_count == 0U) != (trunk.value() == 0U)) {
      return std::unexpected(Corruption("freelist header count and trunk disagree"));
    }
    std::uint32_t remaining = declared_count;
    while (trunk.value() != 0U) {
      if (remaining == 0U || trunk.value() > pager_->page_count() ||
          trunk == geometry.locking_page()) {
        return std::unexpected(Corruption("freelist trunk chain is invalid"));
      }
      if (trunk == root_page) {
        return true;
      }
      auto pin = pager_->ReadPage(trunk);
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      auto view = FreelistTrunkView::Parse(pin->frame().bytes(), geometry);
      if (!view.has_value()) {
        return std::unexpected(std::move(view.error()));
      }
      if (view->leaf_count() > remaining - 1U) {
        return std::unexpected(Corruption("freelist contains more pages than declared"));
      }
      --remaining;
      for (std::size_t index = 0U; index < view->leaf_count(); ++index) {
        auto leaf = view->leaf_page(index);
        if (!leaf.has_value()) {
          return std::unexpected(std::move(leaf.error()));
        }
        if (*leaf == root_page) {
          return true;
        }
        --remaining;
      }
      trunk = view->next_trunk().value_or(PageNumber{});
    }
    if (remaining != 0U) {
      return std::unexpected(Corruption("freelist contains fewer pages than declared"));
    }
    return false;
  }

  Pager* pager_;
  std::shared_ptr<pager_internal::PagerLifetime> pager_lifetime_;
  std::uint64_t generation_;
  BtreeWriteWorkspace workspace_;
  std::optional<BtreePageGeometry> geometry_;
  RecordCodecOptions record_options_;
  std::unordered_map<std::uint32_t, std::uint64_t> root_incarnations_;
  bool managed_ = false;
  bool statement_active_ = true;
  std::uint64_t statement_epoch_ = 0;
};

}  // namespace btree_internal

namespace {

[[nodiscard]] Result<std::shared_ptr<btree_internal::BtreeWriterCore>> OpenWriterCore(
    Pager& pager, bool managed) {
  if (!pager.in_write_transaction() || pager.state() == PagerState::kWriterFinished) {
    return std::unexpected(Misuse("B-tree write session requires an active write transaction"));
  }
  if (const auto failure = pager.write_failure_code(); failure.has_value()) {
    return std::unexpected(
        MakeError(*failure, "B-tree write session requires transaction rollback"));
  }
  std::optional<BtreePageGeometry> geometry;
  RecordCodecOptions record_options;
  if (const DatabaseHeader* header = pager.header(); header != nullptr) {
    auto parsed = BtreePageGeometry::Create(header->page_size(), header->usable_size());
    if (!parsed.has_value()) {
      return std::unexpected(std::move(parsed.error()));
    }
    geometry = *parsed;
    auto schema = NormalizeSchemaFormat(header->schema_format());
    if (!schema.has_value()) {
      return std::unexpected(std::move(schema.error()));
    }
    record_options.schema_format = static_cast<RecordSchemaFormat>(*schema);
    auto encoding = NormalizeTextEncoding(header->text_encoding());
    if (!encoding.has_value()) {
      return std::unexpected(std::move(encoding.error()));
    }
    if (*encoding != DatabaseTextEncoding::kUtf8 || header->largest_root_page().value() != 0U ||
        header->incremental_vacuum() != 0U) {
      return std::unexpected(
          Protocol("writable B-tree session does not support this database format"));
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
      return std::unexpected(Corruption("page one is not a table B-tree root"));
    }
  } else if (pager.page_count() != 0U) {
    return std::unexpected(Corruption("nonempty writable pager has no database header"));
  }
  auto workspace = btree_internal::BtreeWriteWorkspace::Create(pager.page_size());
  if (!workspace.has_value()) {
    return std::unexpected(std::move(workspace.error()));
  }
  std::shared_ptr<btree_internal::BtreeWriterCore> core;
  try {
    core = std::make_shared<btree_internal::BtreeWriterCore>(
        pager, pager.write_transaction_generation(), std::move(*workspace), geometry,
        record_options, managed);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  auto claimed = pager.ClaimWriteCoordinator();
  if (!claimed.has_value()) {
    return std::unexpected(std::move(claimed.error()));
  }
  return core;
}

}  // namespace

Result<BtreeWriteSession> BtreeWriteSession::Open(Pager& pager) {
  auto core = OpenWriterCore(pager, false);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }
  return BtreeWriteSession{std::move(*core)};
}

Result<BtreeWriteSession> BtreeWriteSession::OpenManaged(Pager& pager) {
  auto core = OpenWriterCore(pager, true);
  if (!core.has_value()) {
    return std::unexpected(std::move(core.error()));
  }
  return BtreeWriteSession{std::move(*core)};
}

Status BtreeWriteSession::BeginManagedStatement() {
  return core_ != nullptr ? core_->BeginManagedStatement()
                          : Status{std::unexpected(Misuse("B-tree write session is moved from"))};
}

void BtreeWriteSession::EndManagedStatement() noexcept {
  if (core_ != nullptr) {
    core_->EndManagedStatement();
  }
}

bool BtreeWriteSession::requires_rollback() const noexcept {
  return core_ != nullptr && core_->requires_rollback();
}

Status BtreeWriteSession::InitializeDatabase(BtreeDatabaseOptions options) {
  return core_ != nullptr ? core_->InitializeDatabase(options)
                          : Status{std::unexpected(Misuse("B-tree write session is moved from"))};
}

Result<TableBtreeWriter> BtreeWriteSession::CreateTableBtree() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("B-tree write session is moved from"));
  }
  auto created = core_->CreateRoot(true);
  if (!created.has_value()) {
    return std::unexpected(std::move(created.error()));
  }
  return TableBtreeWriter{core_, created->first, created->second, core_->statement_epoch()};
}

Result<IndexBtreeWriter> BtreeWriteSession::CreateIndexBtree(
    std::span<const IndexColumnOrder> columns) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("B-tree write session is moved from"));
  }
  auto copied = CopyColumns(columns);
  if (!copied.has_value()) {
    return std::unexpected(std::move(copied.error()));
  }
  auto created = core_->CreateRoot(false);
  if (!created.has_value()) {
    return std::unexpected(std::move(created.error()));
  }
  return IndexBtreeWriter{core_, created->first, created->second, core_->statement_epoch(),
                          std::move(*copied)};
}

Result<TableBtreeWriter> BtreeWriteSession::OpenTableBtree(PageNumber root_page) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("B-tree write session is moved from"));
  }
  auto incarnation = core_->OpenRoot(root_page, true);
  if (!incarnation.has_value()) {
    return std::unexpected(std::move(incarnation.error()));
  }
  return TableBtreeWriter{core_, root_page, *incarnation, core_->statement_epoch()};
}

Result<IndexBtreeWriter> BtreeWriteSession::OpenIndexBtree(
    PageNumber root_page, std::span<const IndexColumnOrder> columns) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("B-tree write session is moved from"));
  }
  auto copied = CopyColumns(columns);
  if (!copied.has_value()) {
    return std::unexpected(std::move(copied.error()));
  }
  auto incarnation = core_->OpenRoot(root_page, false);
  if (!incarnation.has_value()) {
    return std::unexpected(std::move(incarnation.error()));
  }
  return IndexBtreeWriter{core_, root_page, *incarnation, core_->statement_epoch(),
                          std::move(*copied)};
}

bool TableBtreeWriter::requires_rollback() const noexcept {
  return core_ != nullptr && core_->requires_rollback();
}

Status TableBtreeWriter::Insert(std::int64_t rowid, ByteView payload, BtreeInsertMode mode) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("table B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, true);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  return cursor->InsertTable(rowid, payload,
                             mode == BtreeInsertMode::kInsertOnly
                                 ? btree_internal::BtreeInsertMode::kInsertOnly
                                 : btree_internal::BtreeInsertMode::kReplace,
                             core_->workspace());
}

Result<bool> TableBtreeWriter::Delete(std::int64_t rowid) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("table B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, true);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  auto deleted = cursor->DeleteTable(rowid, core_->workspace());
  if (!deleted.has_value() && deleted.error().code() == ErrorCode::kNotFound) {
    return false;
  }
  if (!deleted.has_value()) {
    return std::unexpected(std::move(deleted.error()));
  }
  return true;
}

class TableBtreeMutationCursor::Impl final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Impl>> Create(
      std::shared_ptr<btree_internal::BtreeWriterCore> core, PageNumber root_page,
      btree_internal::BtreeWriterCore::RootValidation validation) {
    std::unique_ptr<Impl> impl;
    try {
      impl = std::make_unique<Impl>(std::move(core), root_page, validation);
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    } catch (const std::length_error&) {
      return std::unexpected(Error::OutOfMemory());
    }
    auto cursor = impl->core_->OpenCursor(impl->owner_, root_page, true);
    if (!cursor.has_value()) {
      return std::unexpected(std::move(cursor.error()));
    }
    impl->cursor_.emplace(std::move(*cursor));
    return impl;
  }

  Impl(std::shared_ptr<btree_internal::BtreeWriterCore> core, PageNumber root_page,
       btree_internal::BtreeWriterCore::RootValidation validation) noexcept
      : core_(std::move(core)),
        root_page_(root_page),
        validation_(validation),
        owner_(core_->pager()) {}

  [[nodiscard]] Status Validate() const { return core_->ValidateRoot(root_page_, validation_); }

  std::shared_ptr<btree_internal::BtreeWriterCore> core_;
  PageNumber root_page_;
  btree_internal::BtreeWriterCore::RootValidation validation_;
  btree_internal::MutationPageOwner owner_;
  std::optional<btree_internal::WritableCursor> cursor_;
  std::vector<std::byte> scratch_;
};

Result<TableBtreeMutationCursor> TableBtreeWriter::OpenMutationCursor() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("table B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(root_page_, {
                                                   .incarnation = incarnation_,
                                                   .statement_epoch = statement_epoch_,
                                               });
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto impl = TableBtreeMutationCursor::Impl::Create(core_, root_page_,
                                                     {
                                                         .incarnation = incarnation_,
                                                         .statement_epoch = statement_epoch_,
                                                     });
  if (!impl.has_value()) {
    return std::unexpected(std::move(impl.error()));
  }
  return TableBtreeMutationCursor{std::move(*impl)};
}

TableBtreeMutationCursor::TableBtreeMutationCursor(TableBtreeMutationCursor&&) noexcept = default;
TableBtreeMutationCursor::TableBtreeMutationCursor(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
TableBtreeMutationCursor& TableBtreeMutationCursor::operator=(TableBtreeMutationCursor&&) noexcept =
    default;
TableBtreeMutationCursor::~TableBtreeMutationCursor() = default;

bool TableBtreeMutationCursor::valid() const noexcept {
  return impl_ != nullptr && impl_->cursor_.has_value() &&
         impl_->cursor_->state() == btree_internal::WritableCursorState::kValid;
}

Result<bool> TableBtreeMutationCursor::First() {
  if (impl_ == nullptr || !impl_->cursor_.has_value()) {
    return std::unexpected(Misuse("table mutation cursor is moved from"));
  }
  auto valid = impl_->Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  return impl_->cursor_->FirstTable();
}

Result<bool> TableBtreeMutationCursor::Next() {
  if (impl_ == nullptr || !impl_->cursor_.has_value()) {
    return std::unexpected(Misuse("table mutation cursor is moved from"));
  }
  auto valid = impl_->Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  return impl_->cursor_->NextTable();
}

Result<TableBtreeMutationRow> TableBtreeMutationCursor::row() {
  if (impl_ == nullptr || !impl_->cursor_.has_value()) {
    return std::unexpected(Misuse("table mutation cursor is moved from"));
  }
  auto valid = impl_->Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto row = impl_->cursor_->CurrentTableRow(impl_->scratch_);
  if (!row.has_value()) {
    return std::unexpected(std::move(row.error()));
  }
  return TableBtreeMutationRow{
      .rowid = row->rowid,
      .payload = row->payload,
  };
}

Status TableBtreeMutationCursor::ReplaceCurrent(ByteView payload) {
  if (impl_ == nullptr || !impl_->cursor_.has_value()) {
    return std::unexpected(Misuse("table mutation cursor is moved from"));
  }
  auto valid = impl_->Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  return impl_->cursor_->ReplaceCurrentTable(payload, impl_->core_->workspace());
}

Result<bool> TableBtreeMutationCursor::DeleteAndNext() {
  if (impl_ == nullptr || !impl_->cursor_.has_value()) {
    return std::unexpected(Misuse("table mutation cursor is moved from"));
  }
  auto valid = impl_->Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  return impl_->cursor_->DeleteCurrentTableAndNext(impl_->core_->workspace());
}

Result<std::uint64_t> TableBtreeWriter::Clear() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("table B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto geometry = core_->geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  return btree_internal::ClearBtree(owner, *geometry, root_page_);
}

Status TableBtreeWriter::Drop() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("table B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  auto geometry = core_->geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto dropped = btree_internal::DropBtree(owner, *geometry, root_page_);
  if (!dropped.has_value()) {
    return dropped;
  }
  core_->InvalidateRoot(root_page_);
  return {};
}

bool IndexBtreeWriter::requires_rollback() const noexcept {
  return core_ != nullptr && core_->requires_rollback();
}

Result<std::optional<std::int64_t>> IndexBtreeWriter::FindPrefixRowId(
    std::span<const SqlValue> key) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  if (key.empty() || key.size() >= columns_.size()) {
    return std::unexpected(Misuse("index prefix does not match comparison metadata"));
  }
  auto cursor = IndexBtreeCursor::Open(core_->pager(), root_page_, columns_);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  auto found = cursor->Seek(key, BtreeSeekMode::kGreaterOrEqual);
  if (!found.has_value()) {
    return std::unexpected(std::move(found.error()));
  }
  if (!*found) {
    return std::optional<std::int64_t>{};
  }
  auto comparison = cursor->CompareCurrent(key);
  if (!comparison.has_value()) {
    return std::unexpected(std::move(comparison.error()));
  }
  if (*comparison != std::weak_ordering::equivalent) {
    return std::optional<std::int64_t>{};
  }
  auto payload = cursor->CopyPayload();
  if (!payload.has_value()) {
    return std::unexpected(std::move(payload.error()));
  }
  auto record = RecordView::Parse(payload->view(), core_->record_options());
  if (!record.has_value()) {
    return std::unexpected(std::move(record.error()));
  }
  if (record->field_count() != columns_.size()) {
    return std::unexpected(Corruption("index record field count does not match metadata"));
  }
  auto field = record->field(record->field_count() - 1U);
  if (!field.has_value()) {
    return std::unexpected(std::move(field.error()));
  }
  const std::optional<std::int64_t> rowid = field->integer_value();
  if (!rowid.has_value()) {
    return std::unexpected(Corruption("index record rowid suffix is not an integer"));
  }
  return rowid;
}

Status IndexBtreeWriter::Insert(std::span<const SqlValue> values) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  if (values.size() != columns_.size()) {
    return std::unexpected(Misuse("index values do not match comparison metadata"));
  }
  auto record = EncodeRecord(values, core_->record_options());
  if (!record.has_value()) {
    return std::unexpected(std::move(record.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  return cursor->InsertIndex(record->view(), values, columns_, core_->record_options(),
                             btree_internal::BtreeInsertMode::kInsertOnly, scratch,
                             core_->workspace());
}

Status IndexBtreeWriter::InsertEncoded(ByteView record) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  return cursor->InsertIndexRecord(record, columns_, core_->record_options(),
                                   btree_internal::BtreeInsertMode::kInsertOnly, scratch,
                                   core_->workspace());
}

Result<bool> IndexBtreeWriter::ContainsEncoded(ByteView key) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto key_view = RecordView::Parse(key, core_->record_options());
  if (!key_view.has_value()) {
    return std::unexpected(std::move(key_view.error()));
  }
  if (key_view->field_count() < columns_.size()) {
    return std::unexpected(
        Misuse("encoded index key has fewer fields than its comparison metadata"));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  auto seek = cursor->SeekIndexRecord(*key_view, columns_, core_->record_options(), scratch);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  return seek->exact;
}

Status IndexBtreeWriter::ReplaceEncoded(ByteView record) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  auto record_view = RecordView::Parse(record, core_->record_options());
  if (!record_view.has_value()) {
    return std::unexpected(std::move(record_view.error()));
  }
  if (record_view->field_count() < columns_.size()) {
    return std::unexpected(Misuse("encoded index record has fewer fields than its comparison key"));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  auto seek = cursor->SeekIndexRecord(*record_view, columns_, core_->record_options(), scratch);
  if (!seek.has_value()) {
    return std::unexpected(std::move(seek.error()));
  }
  if (!seek->exact) {
    return std::unexpected(NotFound("encoded index key does not exist"));
  }
  return cursor->InsertIndexRecord(record, columns_, core_->record_options(),
                                   btree_internal::BtreeInsertMode::kReplace, scratch,
                                   core_->workspace());
}

Result<bool> IndexBtreeWriter::Delete(std::span<const SqlValue> values) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  if (values.size() != columns_.size()) {
    return std::unexpected(Misuse("index values do not match comparison metadata"));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  auto deleted =
      cursor->DeleteIndex(values, columns_, core_->record_options(), scratch, core_->workspace());
  if (!deleted.has_value() && deleted.error().code() == ErrorCode::kNotFound) {
    return false;
  }
  if (!deleted.has_value()) {
    return std::unexpected(std::move(deleted.error()));
  }
  return true;
}

Result<bool> IndexBtreeWriter::DeleteEncoded(ByteView record) {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto cursor = core_->OpenCursor(owner, root_page_, false);
  if (!cursor.has_value()) {
    return std::unexpected(std::move(cursor.error()));
  }
  std::vector<std::byte> scratch;
  auto deleted = cursor->DeleteIndexRecord(record, columns_, core_->record_options(), scratch,
                                           core_->workspace());
  if (!deleted.has_value() && deleted.error().code() == ErrorCode::kNotFound) {
    return false;
  }
  if (!deleted.has_value()) {
    return std::unexpected(std::move(deleted.error()));
  }
  return true;
}

Result<std::uint64_t> IndexBtreeWriter::Clear() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid.error()));
  }
  auto geometry = core_->geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  return btree_internal::ClearBtree(owner, *geometry, root_page_);
}

Status IndexBtreeWriter::Drop() {
  if (core_ == nullptr) {
    return std::unexpected(Misuse("index B-tree writer is moved from"));
  }
  auto valid = core_->ValidateRoot(
      root_page_, {.incarnation = incarnation_, .statement_epoch = statement_epoch_});
  if (!valid.has_value()) {
    return valid;
  }
  auto geometry = core_->geometry();
  if (!geometry.has_value()) {
    return std::unexpected(std::move(geometry.error()));
  }
  btree_internal::MutationPageOwner owner{core_->pager()};
  auto dropped = btree_internal::DropBtree(owner, *geometry, root_page_);
  if (!dropped.has_value()) {
    return dropped;
  }
  core_->InvalidateRoot(root_page_);
  return {};
}

}  // namespace modern_sqlite
