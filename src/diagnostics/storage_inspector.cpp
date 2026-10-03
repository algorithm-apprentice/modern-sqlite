#include "modern_sqlite/diagnostics/storage_inspector.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/read_pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

struct PointerExpectation {
  PointerMapType type;
  std::optional<PageNumber> parent;
};

struct PageClaim {
  bool claimed = false;
  std::optional<PointerExpectation> pointer_expectation;
};

struct RootCandidate {
  PageNumber page;
  std::optional<bool> expected_table;
  std::int64_t schema_rowid = 0;
};

struct ByteRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

[[nodiscard]] Error MakeError(ErrorCode code, std::string message) {
  return Error::Create(code, std::move(message));
}

[[nodiscard]] Error Misuse(std::string message) {
  return MakeError(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error NotDatabase(std::string message) {
  return MakeError(ErrorCode::kNotDatabase, std::move(message));
}

[[nodiscard]] Error Protocol(std::string message) {
  return MakeError(ErrorCode::kProtocol, std::move(message));
}

[[nodiscard]] Result<RecordSchemaFormat> EffectiveSchemaFormat(std::uint32_t raw) {
  if (raw == 0 || raw == 1) {
    return RecordSchemaFormat::kOne;
  }
  if (raw == 2) {
    return RecordSchemaFormat::kTwo;
  }
  if (raw == 3) {
    return RecordSchemaFormat::kThree;
  }
  if (raw == 4) {
    return RecordSchemaFormat::kFour;
  }
  return std::unexpected(NotDatabase("database schema format is unsupported"));
}

[[nodiscard]] Result<std::uint8_t> EffectiveTextEncoding(std::uint32_t raw) {
  const auto effective = static_cast<std::uint8_t>(raw & 3U);
  if (effective == 0 || effective == 1) {
    return std::uint8_t{1};
  }
  return std::unexpected(Protocol("UTF-16 storage inspection is not implemented"));
}

[[nodiscard]] bool IsContinuation(std::uint8_t value) noexcept {
  return value >= 0x80U && value <= 0xbfU;
}

[[nodiscard]] bool IsValidUtf8(ByteView bytes) noexcept {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = std::to_integer<std::uint8_t>(bytes[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
      if (index + 1U >= bytes.size() ||
          !IsContinuation(std::to_integer<std::uint8_t>(bytes[index + 1U]))) {
        return false;
      }
      index += 2U;
      continue;
    }
    if (first >= 0xe0U && first <= 0xefU) {
      if (index + 2U >= bytes.size()) {
        return false;
      }
      const auto second = std::to_integer<std::uint8_t>(bytes[index + 1U]);
      const auto third = std::to_integer<std::uint8_t>(bytes[index + 2U]);
      const bool valid_second = first == 0xe0U   ? second >= 0xa0U && second <= 0xbfU
                                : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                                                 : IsContinuation(second);
      if (!valid_second || !IsContinuation(third)) {
        return false;
      }
      index += 3U;
      continue;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
      if (index + 3U >= bytes.size()) {
        return false;
      }
      const auto second = std::to_integer<std::uint8_t>(bytes[index + 1U]);
      const auto third = std::to_integer<std::uint8_t>(bytes[index + 2U]);
      const auto fourth = std::to_integer<std::uint8_t>(bytes[index + 3U]);
      const bool valid_second = first == 0xf0U   ? second >= 0x90U && second <= 0xbfU
                                : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                                                 : IsContinuation(second);
      if (!valid_second || !IsContinuation(third) || !IsContinuation(fourth)) {
        return false;
      }
      index += 4U;
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] StorageText CopyBytes(ByteView bytes) {
  StorageText copied;
  copied.bytes.assign(bytes.begin(), bytes.end());
  copied.valid_utf8 = IsValidUtf8(copied.bytes);
  return copied;
}

[[nodiscard]] StorageText CopyText(Utf8View text) { return CopyBytes(AsBytes(text.bytes())); }

[[nodiscard]] StorageText CoerceSchemaText(const RecordFieldView& field) {
  if (field.type() == SqlValueType::kText) {
    const std::optional<Utf8View> text = field.text_value();
    if (!text.has_value()) {
      std::terminate();
    }
    return CopyText(*text);
  }
  if (field.type() == SqlValueType::kBlob) {
    const std::optional<ByteView> blob = field.blob_value();
    if (!blob.has_value()) {
      std::terminate();
    }
    return CopyBytes(*blob);
  }
  const SqlValue text = CastValue(field.ToOwned(), CastTarget::kText);
  const std::optional<Utf8View> value = text.text_value();
  if (!value.has_value()) {
    std::terminate();
  }
  return CopyText(*value);
}

[[nodiscard]] std::optional<std::uint32_t> CoerceSchemaRoot(const RecordFieldView& field) {
  if (field.type() == SqlValueType::kNull) {
    return std::nullopt;
  }
  const SqlValue text = CastValue(field.ToOwned(), CastTarget::kText);
  const std::optional<Utf8View> value = text.text_value();
  if (!value.has_value()) {
    std::terminate();
  }
  const std::string_view bytes = value->bytes();
  if (bytes.empty()) {
    return std::nullopt;
  }
  std::uint64_t root = 0;
  for (const char character : bytes) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    constexpr std::uint64_t kMaximumRoot = std::numeric_limits<std::uint32_t>::max();
    if (root > (kMaximumRoot - digit) / 10U) {
      return std::nullopt;
    }
    root = root * 10U + digit;
  }
  return static_cast<std::uint32_t>(root);
}

[[nodiscard]] ByteCount CoercedSchemaTextSize(const RecordFieldView& field) {
  if (field.type() == SqlValueType::kNull) {
    return ByteCount{0};
  }
  if (field.type() == SqlValueType::kText) {
    const std::optional<Utf8View> text = field.text_value();
    if (!text.has_value()) {
      std::terminate();
    }
    return ByteCount{text->size_bytes()};
  }
  if (field.type() == SqlValueType::kBlob) {
    const std::optional<ByteView> blob = field.blob_value();
    if (!blob.has_value()) {
      std::terminate();
    }
    return ByteCount{blob->size()};
  }
  const SqlValue text = CastValue(field.ToOwned(), CastTarget::kText);
  const std::optional<Utf8View> value = text.text_value();
  if (!value.has_value()) {
    std::terminate();
  }
  return ByteCount{value->size_bytes()};
}

[[nodiscard]] bool TextEquals(const StorageText& text, std::string_view expected) noexcept {
  return text.bytes.size() == expected.size() && std::ranges::equal(text.bytes, AsBytes(expected));
}

[[nodiscard]] StorageSchemaObjectKind ClassifyObject(const StorageText& type) noexcept {
  if (TextEquals(type, "table")) {
    return StorageSchemaObjectKind::kTable;
  }
  if (TextEquals(type, "index")) {
    return StorageSchemaObjectKind::kIndex;
  }
  if (TextEquals(type, "view")) {
    return StorageSchemaObjectKind::kView;
  }
  if (TextEquals(type, "trigger")) {
    return StorageSchemaObjectKind::kTrigger;
  }
  return StorageSchemaObjectKind::kUnknown;
}

[[nodiscard]] std::string_view BtreePageTypeName(BtreePageType type) noexcept {
  switch (type) {
    case BtreePageType::kInteriorIndex:
      return "interior_index";
    case BtreePageType::kInteriorTable:
      return "interior_table";
    case BtreePageType::kLeafIndex:
      return "leaf_index";
    case BtreePageType::kLeafTable:
      return "leaf_table";
  }
  return "unknown";
}

[[nodiscard]] std::string_view SqlValueTypeName(SqlValueType type) noexcept {
  switch (type) {
    case SqlValueType::kNull:
      return "null";
    case SqlValueType::kInteger:
      return "integer";
    case SqlValueType::kReal:
      return "real";
    case SqlValueType::kText:
      return "text";
    case SqlValueType::kBlob:
      return "blob";
  }
  return "unknown";
}

[[nodiscard]] StorageHeaderReport MakeHeaderReport(const DatabaseHeader& header,
                                                   std::uint32_t snapshot_page_count,
                                                   RecordSchemaFormat schema_format,
                                                   std::uint8_t text_encoding) {
  return {
      .snapshot_page_count = snapshot_page_count,
      .header_page_count = header.header_page_count(),
      .page_size = header.page_size(),
      .reserved_bytes = header.reserved_bytes(),
      .usable_size = header.usable_size(),
      .write_version = header.write_version(),
      .read_version = header.read_version(),
      .file_change_counter = header.file_change_counter(),
      .first_freelist_trunk = header.first_freelist_trunk(),
      .freelist_page_count = header.freelist_page_count(),
      .schema_cookie = header.schema_cookie(),
      .raw_schema_format = header.schema_format(),
      .effective_schema_format = schema_format,
      .suggested_cache_size = header.suggested_cache_size(),
      .largest_root_page = header.largest_root_page(),
      .raw_text_encoding = header.text_encoding(),
      .effective_text_encoding = text_encoding,
      .user_version = header.user_version(),
      .incremental_vacuum = header.incremental_vacuum(),
      .application_id = header.application_id(),
      .version_valid_for = header.version_valid_for(),
      .sqlite_version = header.sqlite_version(),
      .auto_vacuum = header.largest_root_page().value() != 0,
  };
}

class Inspector final {
 public:
  Inspector(ReadPager& pager, const DatabaseHeader& header, BtreePageGeometry geometry,
            RecordSchemaFormat schema_format, StorageInspectionOptions options)
      : pager_(pager),
        header_(header),
        geometry_(geometry),
        auto_vacuum_(header.largest_root_page().value() != 0),
        record_options_{.schema_format = schema_format},
        options_(options),
        claims_(static_cast<std::size_t>(pager.page_count()) + 1U) {
    report_.header = MakeHeaderReport(header, pager.page_count(), schema_format, std::uint8_t{1});
    report_.pages.reserve(pager.page_count());
    for (std::uint32_t page = 1; page <= pager.page_count(); ++page) {
      StoragePageReport page_report;
      page_report.page_number = PageNumber{page};
      report_.pages.push_back(std::move(page_report));
    }
    report_.summary.snapshot_page_count = pager.page_count();
  }

  [[nodiscard]] Result<StorageInspectionReport> Run() {
    auto reserved = ReserveSpecialPages();
    if (!reserved.has_value()) {
      return std::unexpected(std::move(reserved.error()));
    }

    if (header_.incremental_vacuum() != 0 && !auto_vacuum_) {
      AddIssue(StorageIssueCode::kHeaderInvariant, PageNumber{1}, std::nullopt, std::nullopt,
               "incremental vacuum is enabled while auto-vacuum is disabled");
    }

    auto freelist = InspectFreelist();
    if (!freelist.has_value()) {
      return std::unexpected(std::move(freelist.error()));
    }

    auto schema_height = VisitTree(PageNumber{1}, PageNumber{1}, std::nullopt, true, 0, true, true,
                                   std::nullopt, std::nullopt);
    if (!schema_height.has_value()) {
      return std::unexpected(std::move(schema_height.error()));
    }
    if (!schema_height->has_value()) {
      report_.root_discovery_complete = false;
    }

    PrepareRoots();
    for (const RootCandidate& root : roots_) {
      if (stopped_) {
        break;
      }
      auto height = VisitTree(root.page, root.page, std::nullopt, root.expected_table, 0, true,
                              false, std::nullopt, std::nullopt);
      if (!height.has_value()) {
        return std::unexpected(std::move(height.error()));
      }
      if (!height->has_value()) {
        report_.root_discovery_complete = false;
      }
    }

    CheckLargestRoot();
    auto pointer_maps = CheckPointerMaps();
    if (!pointer_maps.has_value()) {
      return std::unexpected(std::move(pointer_maps.error()));
    }
    MarkUnusedPages();
    Finalize();
    return std::move(report_);
  }

 private:
  [[nodiscard]] StoragePageReport& PageReport(PageNumber page) {
    return report_.pages[static_cast<std::size_t>(page.value() - 1U)];
  }

  void AddIssue(StorageIssueCode code, std::optional<PageNumber> page,
                std::optional<std::size_t> cell, std::optional<PageNumber> related_page,
                std::string message) {
    if (stopped_) {
      return;
    }
    report_.issues.push_back(StorageIssue{
        .code = code,
        .page = page,
        .cell = cell,
        .related_page = related_page,
        .message = std::move(message),
    });
    if (report_.issues.size() >= options_.max_issues) {
      report_.issues_truncated = true;
      stopped_ = true;
    }
  }

  void AddSchemaIssue(std::optional<PageNumber> page, std::optional<std::size_t> cell,
                      std::optional<PageNumber> related_page, std::string message) {
    report_.root_discovery_complete = false;
    AddIssue(StorageIssueCode::kSchemaRecord, page, cell, related_page, std::move(message));
  }

  [[nodiscard]] bool IsSnapshotPage(PageNumber page) const noexcept {
    return page.value() != 0 && page.value() <= pager_.page_count();
  }

  [[nodiscard]] bool IsPointerMap(PageNumber page) const {
    const auto result = IsPointerMapPage(page, geometry_);
    return result.has_value() && *result;
  }

  void ClaimSpecial(PageNumber page, StoragePageRole role) {
    PageClaim& claim = claims_[page.value()];
    if (claim.claimed) {
      AddIssue(StorageIssueCode::kDuplicatePageReference, page, std::nullopt, page,
               "special page has more than one role");
      return;
    }
    claim.claimed = true;
    PageReport(page).role = role;
  }

  [[nodiscard]] bool ClaimPage(PageNumber page, StoragePageRole role,
                               std::optional<PageNumber> root, std::optional<PageNumber> parent,
                               std::optional<PointerExpectation> expectation,
                               std::optional<PageNumber> source_page,
                               std::optional<std::size_t> source_cell) {
    if (!IsSnapshotPage(page)) {
      AddIssue(StorageIssueCode::kInvalidPageReference, source_page, source_cell, page,
               "page reference is outside the snapshot");
      return false;
    }
    if (page == geometry_.locking_page()) {
      AddIssue(StorageIssueCode::kInvalidPageReference, source_page, source_cell, page,
               "ordinary content references the locking page");
      return false;
    }
    if (auto_vacuum_ && IsPointerMap(page)) {
      AddIssue(StorageIssueCode::kPointerMap, source_page, source_cell, page,
               "ordinary content references a pointer-map page");
      return false;
    }

    PageClaim& claim = claims_[page.value()];
    if (claim.claimed) {
      AddIssue(StorageIssueCode::kDuplicatePageReference, source_page, source_cell, page,
               "page has more than one owner");
      return false;
    }
    claim.claimed = true;
    claim.pointer_expectation = expectation;
    StoragePageReport& page_report = PageReport(page);
    page_report.role = role;
    page_report.root_page = root;
    page_report.parent_page = parent;
    return true;
  }

  [[nodiscard]] Result<std::optional<ReadPagePin>> ReadPageOrIssue(
      PageNumber page, StorageIssueCode issue_code, std::optional<PageNumber> issue_page,
      std::optional<std::size_t> issue_cell, std::string message) {
    auto pin = pager_.ReadPage(page);
    if (pin.has_value()) {
      return std::optional<ReadPagePin>{std::move(*pin)};
    }
    if (pin.error().code() == ErrorCode::kCorruption ||
        pin.error().code() == ErrorCode::kOutOfRange ||
        pin.error().code() == ErrorCode::kNotDatabase) {
      AddIssue(issue_code, issue_page, issue_cell, page, std::move(message));
      return std::optional<ReadPagePin>{};
    }
    return std::unexpected(std::move(pin.error()));
  }

  [[nodiscard]] Result<void> ReserveSpecialPages() {
    const PageNumber locking_page = geometry_.locking_page();
    if (locking_page.value() <= pager_.page_count()) {
      ClaimSpecial(locking_page, StoragePageRole::kLocking);
    }
    if (!auto_vacuum_) {
      return {};
    }
    for (std::uint32_t raw_page = 2; raw_page <= pager_.page_count(); ++raw_page) {
      const PageNumber page{raw_page};
      auto is_map = IsPointerMapPage(page, geometry_);
      if (!is_map.has_value()) {
        return std::unexpected(std::move(is_map.error()));
      }
      if (*is_map) {
        ClaimSpecial(page, StoragePageRole::kPointerMap);
      }
    }
    return {};
  }

  [[nodiscard]] std::optional<PointerExpectation> FreePageExpectation() const {
    if (!auto_vacuum_) {
      return std::nullopt;
    }
    return PointerExpectation{.type = PointerMapType::kFreePage, .parent = std::nullopt};
  }

  [[nodiscard]] Result<void> InspectFreelist() {
    PageNumber trunk = header_.first_freelist_trunk();
    std::size_t observed_pages = 0;
    while (trunk.value() != 0 && !stopped_) {
      const PageNumber current = trunk;
      if (!ClaimPage(current, StoragePageRole::kFreelistTrunk, std::nullopt, std::nullopt,
                     FreePageExpectation(), PageNumber{1}, std::nullopt)) {
        break;
      }
      ++observed_pages;
      auto pin = ReadPageOrIssue(current, StorageIssueCode::kFreelist, current, std::nullopt,
                                 "unable to read freelist trunk");
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      if (!pin->has_value()) {
        break;
      }
      auto view = FreelistTrunkView::Parse((*pin)->frame().bytes(), geometry_);
      if (!view.has_value()) {
        AddIssue(StorageIssueCode::kFreelist, current, std::nullopt, std::nullopt,
                 "freelist trunk is malformed");
        break;
      }
      for (std::size_t index = 0; index < view->leaf_count() && !stopped_; ++index) {
        auto leaf = view->leaf_page(index);
        if (!leaf.has_value()) {
          AddIssue(StorageIssueCode::kFreelist, current, index, std::nullopt,
                   "freelist leaf reference is malformed");
          continue;
        }
        if (ClaimPage(*leaf, StoragePageRole::kFreelistLeaf, std::nullopt, current,
                      FreePageExpectation(), current, index)) {
          ++observed_pages;
        }
      }
      trunk = view->next_trunk().value_or(PageNumber{0});
    }

    if (observed_pages != header_.freelist_page_count() && !stopped_) {
      AddIssue(StorageIssueCode::kFreelist, PageNumber{1}, std::nullopt, std::nullopt,
               "observed freelist size differs from the header count");
    }
    return {};
  }

  [[nodiscard]] std::optional<PointerExpectation> BtreeExpectation(
      PageNumber page, std::optional<PageNumber> parent) const {
    if (!auto_vacuum_ || page == PageNumber{1}) {
      return std::nullopt;
    }
    if (!parent.has_value()) {
      return PointerExpectation{.type = PointerMapType::kRootPage, .parent = std::nullopt};
    }
    return PointerExpectation{.type = PointerMapType::kBtreeChild, .parent = *parent};
  }

  [[nodiscard]] Result<std::optional<std::size_t>> VisitTree(
      PageNumber page_number, PageNumber root_page, std::optional<PageNumber> parent,
      std::optional<bool> expected_table, std::size_t depth, bool is_root, bool schema_tree,
      std::optional<std::int64_t> lower_bound, std::optional<std::int64_t> upper_bound) {
    if (stopped_) {
      return std::optional<std::size_t>{};
    }
    const std::optional<PageNumber> source_page =
        parent.has_value() ? parent : std::optional<PageNumber>{page_number};
    if (!ClaimPage(page_number, StoragePageRole::kBtree, root_page, parent,
                   BtreeExpectation(page_number, parent), source_page, std::nullopt)) {
      return std::optional<std::size_t>{};
    }
    if (depth >= kMaximumBtreeDepth) {
      AddIssue(StorageIssueCode::kDepthLimit, page_number, std::nullopt, std::nullopt,
               "B-tree depth exceeds the supported limit");
      return std::optional<std::size_t>{};
    }

    auto pin = ReadPageOrIssue(page_number, StorageIssueCode::kPageDecode, page_number,
                               std::nullopt, "unable to read B-tree page");
    if (!pin.has_value()) {
      return std::unexpected(std::move(pin.error()));
    }
    if (!pin->has_value()) {
      return std::optional<std::size_t>{};
    }
    auto page = BtreePageView::Parse((*pin)->frame().bytes(), page_number, geometry_);
    if (!page.has_value()) {
      AddIssue(StorageIssueCode::kPageDecode, page_number, std::nullopt, std::nullopt,
               "B-tree page is malformed");
      return std::optional<std::size_t>{};
    }

    StoragePageReport& page_report = PageReport(page_number);
    page_report.btree_type = page->type();
    page_report.depth_from_root = depth;
    page_report.cell_count = page->cell_count();
    page_report.cell_content_offset = page->cell_content_offset();
    page_report.fragmented_free_bytes = page->fragmented_free_bytes();
    page_report.cells.reserve(page->cell_count());

    const bool is_table = page->is_table();
    if (expected_table.has_value() && *expected_table != is_table) {
      AddIssue(StorageIssueCode::kTreeKindMismatch, page_number, std::nullopt, root_page,
               "B-tree page kind differs from its root");
      return std::optional<std::size_t>{};
    }
    if (!is_root && page->cell_count() == 0) {
      AddIssue(StorageIssueCode::kEmptyChildPage, page_number, std::nullopt, parent,
               "non-root B-tree page is empty");
      return std::optional<std::size_t>{};
    }
    if (!page->is_leaf() && page->cell_count() == 0 && !(is_root && page_number == PageNumber{1})) {
      AddIssue(StorageIssueCode::kEmptyChildPage, page_number, std::nullopt, parent,
               "empty interior B-tree page is not the page-one virtual root");
      return std::optional<std::size_t>{};
    }

    std::vector<std::optional<BtreeCellView>> cells(page->cell_count());
    std::vector<ByteRange> cell_ranges;
    cell_ranges.reserve(page->cell_count());
    std::vector<std::int64_t> table_keys;
    if (is_table) {
      table_keys.reserve(page->cell_count());
    }
    bool coverage_complete = true;
    std::optional<std::int64_t> previous_key;

    for (std::size_t index = 0; index < page->cell_count() && !stopped_; ++index) {
      auto offset = page->cell_offset(index);
      auto cell = page->cell(index);
      if (!offset.has_value() || !cell.has_value()) {
        AddIssue(StorageIssueCode::kPageDecode, page_number, index, std::nullopt,
                 "B-tree cell is malformed");
        coverage_complete = false;
        continue;
      }
      cells[index] = *cell;
      cell_ranges.push_back(ByteRange{
          .begin = offset->value(),
          .end = offset->value() + cell->encoded_size().value(),
      });

      StorageCellReport cell_report;
      cell_report.index = index;
      cell_report.left_child = cell->left_child();
      cell_report.rowid = cell->rowid();
      cell_report.payload_bytes = cell->payload_size();
      cell_report.local_payload_bytes = ByteCount{cell->local_payload().size()};

      if (is_table) {
        if (!cell->rowid().has_value()) {
          AddIssue(StorageIssueCode::kPageDecode, page_number, index, std::nullopt,
                   "table B-tree cell is missing its rowid");
        } else {
          const std::int64_t key = *cell->rowid();
          table_keys.push_back(key);
          if (previous_key.has_value() && key <= *previous_key) {
            AddIssue(StorageIssueCode::kTableKeyOrder, page_number, index, std::nullopt,
                     "table rowids are not strictly increasing");
          }
          if (lower_bound.has_value() && key <= *lower_bound) {
            AddIssue(StorageIssueCode::kTableKeyOrder, page_number, index, std::nullopt,
                     "table rowid is not above its lower bound");
          }
          if (upper_bound.has_value() && key > *upper_bound) {
            AddIssue(StorageIssueCode::kTableKeyOrder, page_number, index, std::nullopt,
                     "table rowid exceeds its upper bound");
          }
          previous_key = key;
        }
      }

      if (page->type() != BtreePageType::kInteriorTable) {
        auto payload = ReadPayload(*cell, page_number, root_page, index, cell_report);
        if (!payload.has_value()) {
          return std::unexpected(std::move(payload.error()));
        }
        if (payload->has_value()) {
          InspectRecord(**payload, page_number, index, cell_report, schema_tree, cell->rowid());
        } else if (schema_tree) {
          report_.root_discovery_complete = false;
        }
      }
      page_report.cells.push_back(std::move(cell_report));
    }
    if (schema_tree && page_report.cells.size() != page->cell_count()) {
      report_.root_discovery_complete = false;
    }

    AnalyzeCoverage(*page, page_number, cell_ranges, coverage_complete);
    if (page->is_leaf()) {
      return std::optional<std::size_t>{1U};
    }

    std::optional<std::size_t> expected_child_height;
    bool children_complete = true;
    for (std::size_t index = 0; index <= page->cell_count() && !stopped_; ++index) {
      std::optional<PageNumber> child;
      if (index == page->cell_count()) {
        child = page->rightmost_child();
      } else if (cells[index].has_value()) {
        child = cells[index]->left_child();
      }
      if (!child.has_value()) {
        AddIssue(StorageIssueCode::kPageDecode, page_number, index, std::nullopt,
                 "interior B-tree page is missing a child");
        children_complete = false;
        continue;
      }

      std::optional<std::int64_t> child_lower;
      std::optional<std::int64_t> child_upper;
      if (is_table && table_keys.size() == page->cell_count()) {
        child_lower =
            index == 0 ? lower_bound : std::optional<std::int64_t>{table_keys[index - 1U]};
        child_upper = index == page->cell_count() ? upper_bound
                                                  : std::optional<std::int64_t>{table_keys[index]};
      }
      auto child_height = VisitTree(*child, root_page, page_number, is_table, depth + 1U, false,
                                    schema_tree, child_lower, child_upper);
      if (!child_height.has_value()) {
        return std::unexpected(std::move(child_height.error()));
      }
      if (!child_height->has_value()) {
        children_complete = false;
        continue;
      }
      if (expected_child_height.has_value() && **child_height != *expected_child_height) {
        AddIssue(StorageIssueCode::kUnequalChildDepth, page_number, index, *child,
                 "B-tree child depth differs from its siblings");
      } else if (!expected_child_height.has_value()) {
        expected_child_height = **child_height;
      }
    }
    if (!children_complete || !expected_child_height.has_value()) {
      return std::optional<std::size_t>{};
    }
    return std::optional<std::size_t>{*expected_child_height + 1U};
  }

  void AnalyzeCoverage(const BtreePageView& page, PageNumber page_number,
                       std::vector<ByteRange> ranges, bool coverage_complete) {
    auto free_space = page.AnalyzeFreeSpace();
    if (!free_space.has_value()) {
      AddIssue(StorageIssueCode::kPageCoverage, page_number, std::nullopt, std::nullopt,
               "B-tree free-space metadata is malformed");
      return;
    }
    PageReport(page_number).free_bytes = free_space->total();
    auto freeblocks = free_space->freeblocks();
    while (auto block = freeblocks.Next()) {
      ranges.push_back(ByteRange{
          .begin = block->offset.value(),
          .end = block->offset.value() + block->size.value(),
      });
    }
    if (!coverage_complete || stopped_) {
      return;
    }

    std::ranges::sort(ranges, [](const ByteRange& left, const ByteRange& right) {
      return std::tie(left.begin, left.end) < std::tie(right.begin, right.end);
    });
    const std::size_t usable_size = geometry_.usable_size().value();
    std::size_t cursor = page.cell_content_offset().value();
    std::size_t fragmented = 0;
    for (const ByteRange& range : ranges) {
      if (range.begin < page.cell_content_offset().value() || range.end > usable_size ||
          range.end < range.begin) {
        AddIssue(StorageIssueCode::kPageCoverage, page_number, std::nullopt, std::nullopt,
                 "B-tree content range is outside the cell-content area");
        return;
      }
      if (range.begin < cursor) {
        AddIssue(StorageIssueCode::kPageCoverage, page_number, std::nullopt, std::nullopt,
                 "B-tree cells or freeblocks overlap");
        return;
      }
      fragmented += range.begin - cursor;
      cursor = range.end;
    }
    fragmented += usable_size - cursor;
    if (fragmented != page.fragmented_free_bytes().value()) {
      AddIssue(StorageIssueCode::kPageCoverage, page_number, std::nullopt, std::nullopt,
               "B-tree fragmented-byte count is inconsistent");
    }
  }

  [[nodiscard]] std::optional<PointerExpectation> OverflowExpectation(bool first,
                                                                      PageNumber owner) const {
    if (!auto_vacuum_) {
      return std::nullopt;
    }
    return PointerExpectation{
        .type = first ? PointerMapType::kFirstOverflow : PointerMapType::kLaterOverflow,
        .parent = owner,
    };
  }

  [[nodiscard]] Result<std::optional<ByteBuffer>> ReadPayload(const BtreeCellView& cell,
                                                              PageNumber page_number,
                                                              PageNumber root_page,
                                                              std::size_t cell_index,
                                                              StorageCellReport& cell_report) {
    const std::size_t payload_size = cell.payload_size().value();
    const std::size_t local_size = cell.local_payload().size();
    if (local_size > payload_size) {
      AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, std::nullopt,
               "local payload exceeds the declared payload size");
      return std::optional<ByteBuffer>{};
    }
    const std::size_t remaining_bytes = payload_size - local_size;
    const std::size_t capacity = geometry_.overflow_payload_capacity().value();
    const std::size_t required_pages =
        remaining_bytes == 0 ? 0 : 1U + (remaining_bytes - 1U) / capacity;
    if (required_pages > pager_.page_count()) {
      AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, std::nullopt,
               "declared payload requires more overflow pages than the snapshot");
      return std::optional<ByteBuffer>{};
    }

    ByteBuffer payload{cell.payload_size()};
    if (local_size != 0) {
      std::memcpy(payload.mutable_view().data(), cell.local_payload().data(), local_size);
    }
    if (required_pages == 0) {
      if (cell.first_overflow_page().has_value()) {
        AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index,
                 cell.first_overflow_page(), "fully local payload has an overflow reference");
      }
      return std::optional<ByteBuffer>{std::move(payload)};
    }
    if (!cell.first_overflow_page().has_value()) {
      AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, std::nullopt,
               "overflow-backed payload is missing its first overflow page");
      return std::optional<ByteBuffer>{};
    }

    PageNumber current = *cell.first_overflow_page();
    PageNumber previous = page_number;
    std::size_t output_offset = local_size;
    std::size_t remaining = remaining_bytes;
    for (std::size_t index = 0; index < required_pages && !stopped_; ++index) {
      const bool first = index == 0;
      const std::optional<PointerExpectation> optional_expectation =
          OverflowExpectation(first, previous);
      if (!ClaimPage(current, StoragePageRole::kOverflow, root_page, previous, optional_expectation,
                     page_number, cell_index)) {
        return std::optional<ByteBuffer>{};
      }
      cell_report.overflow_pages.push_back(current);
      auto pin = ReadPageOrIssue(current, StorageIssueCode::kOverflowChain, page_number, cell_index,
                                 "unable to read overflow page");
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      if (!pin->has_value()) {
        return std::optional<ByteBuffer>{};
      }
      auto overflow = OverflowPageView::Parse((*pin)->frame().bytes(), geometry_);
      if (!overflow.has_value()) {
        AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, current,
                 "overflow page is malformed");
        return std::optional<ByteBuffer>{};
      }

      const std::size_t copied = std::min(remaining, overflow->payload().size());
      std::memcpy(payload.mutable_view().data() + output_offset, overflow->payload().data(),
                  copied);
      output_offset += copied;
      remaining -= copied;

      if (index + 1U < required_pages) {
        if (!overflow->next_page().has_value()) {
          AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, current,
                   "overflow chain terminates before the declared payload");
          return std::optional<ByteBuffer>{};
        }
        previous = current;
        current = *overflow->next_page();
      } else if (overflow->next_page().has_value()) {
        AddIssue(StorageIssueCode::kOverflowChain, page_number, cell_index, overflow->next_page(),
                 "overflow chain continues past the declared payload");
      }
    }
    if (remaining != 0 || stopped_) {
      return std::optional<ByteBuffer>{};
    }
    return std::optional<ByteBuffer>{std::move(payload)};
  }

  void InspectRecord(const ByteBuffer& payload, PageNumber page_number, std::size_t cell_index,
                     StorageCellReport& cell_report, bool schema_tree,
                     std::optional<std::int64_t> rowid) {
    auto record = RecordView::Parse(payload.view(), record_options_);
    if (!record.has_value()) {
      AddIssue(StorageIssueCode::kRecordDecode, page_number, cell_index, std::nullopt,
               "cell payload is not a valid SQLite record");
      if (schema_tree) {
        report_.root_discovery_complete = false;
      }
      return;
    }

    StorageRecordReport record_report;
    record_report.field_types.reserve(record->field_count());
    RecordCursor cursor = record->cursor();
    while (auto field = cursor.Next()) {
      record_report.field_types.push_back(field->type());
    }
    cell_report.record = std::move(record_report);
    if (schema_tree) {
      ParseSchemaRecord(*record, rowid, page_number, cell_index);
    }
  }

  void ParseSchemaRecord(const RecordView& record, std::optional<std::int64_t> rowid,
                         PageNumber page_number, std::size_t cell_index) {
    if (!rowid.has_value() || record.field_count() != 5) {
      AddSchemaIssue(page_number, cell_index, std::nullopt,
                     "sqlite_schema record has the wrong shape");
      return;
    }
    std::array<RecordFieldView, 5> fields;
    for (std::size_t index = 0; index < fields.size(); ++index) {
      auto field = record.field(index);
      if (!field.has_value()) {
        AddSchemaIssue(page_number, cell_index, std::nullopt,
                       "sqlite_schema field cannot be decoded");
        return;
      }
      fields[index] = *field;
    }
    if (fields[0].type() == SqlValueType::kNull || fields[1].type() == SqlValueType::kNull ||
        fields[2].type() == SqlValueType::kNull || fields[3].type() == SqlValueType::kNull) {
      AddSchemaIssue(page_number, cell_index, std::nullopt,
                     "sqlite_schema required fields are null");
      return;
    }

    const std::optional<std::uint32_t> decoded_root = CoerceSchemaRoot(fields[3]);
    if (!decoded_root.has_value()) {
      AddSchemaIssue(page_number, cell_index, std::nullopt,
                     "sqlite_schema root page is not an unsigned decimal page number");
      return;
    }

    StorageObjectReport object{
        .schema_rowid = *rowid,
        .kind = StorageSchemaObjectKind::kUnknown,
        .declared_type = CoerceSchemaText(fields[0]),
        .name = CoerceSchemaText(fields[1]),
        .table_name = CoerceSchemaText(fields[2]),
        .root_page = *decoded_root,
        .sql_is_null = fields[4].type() == SqlValueType::kNull,
        .sql_bytes = CoercedSchemaTextSize(fields[4]),
    };
    object.kind = ClassifyObject(object.declared_type);
    const StorageSchemaObjectKind kind = object.kind;
    const std::uint32_t root = object.root_page;
    report_.objects.push_back(object);

    if (kind == StorageSchemaObjectKind::kUnknown) {
      AddSchemaIssue(page_number, cell_index, std::nullopt,
                     "sqlite_schema object type is unsupported");
      return;
    }
    if ((kind == StorageSchemaObjectKind::kView || kind == StorageSchemaObjectKind::kTrigger) &&
        root != 0) {
      AddSchemaIssue(page_number, cell_index, PageNumber{root},
                     "view or trigger has a nonzero root page");
      return;
    }
    if (kind == StorageSchemaObjectKind::kIndex && root == 0) {
      AddSchemaIssue(page_number, cell_index, std::nullopt, "index has a zero root page");
      return;
    }
    if (root == 0) {
      return;
    }
    if (root > pager_.page_count()) {
      AddSchemaIssue(page_number, cell_index, PageNumber{root},
                     "schema root is outside the snapshot");
      return;
    }
    if (kind == StorageSchemaObjectKind::kTable || kind == StorageSchemaObjectKind::kIndex) {
      root_candidates_.push_back(RootCandidate{
          .page = PageNumber{root},
          .expected_table =
              kind == StorageSchemaObjectKind::kIndex ? std::optional<bool>{false} : std::nullopt,
          .schema_rowid = *rowid,
      });
    }
  }

  void PrepareRoots() {
    std::ranges::sort(root_candidates_, [](const RootCandidate& left, const RootCandidate& right) {
      return std::tie(left.page, left.schema_rowid) < std::tie(right.page, right.schema_rowid);
    });
    PageNumber previous{0};
    for (const RootCandidate& candidate : root_candidates_) {
      if (candidate.page == PageNumber{1} || candidate.page == previous) {
        AddIssue(StorageIssueCode::kDuplicateRoot, PageNumber{1}, std::nullopt, candidate.page,
                 "multiple schema objects claim the same root page");
        report_.root_discovery_complete = false;
        continue;
      }
      roots_.push_back(candidate);
      previous = candidate.page;
    }
    report_.summary.root_count = 1U + roots_.size();
  }

  void CheckLargestRoot() {
    if (stopped_ || !report_.root_discovery_complete || !auto_vacuum_) {
      return;
    }
    std::uint32_t maximum = 1;
    for (const RootCandidate& root : roots_) {
      maximum = std::max(maximum, root.page.value());
    }
    if (maximum != header_.largest_root_page().value()) {
      AddIssue(StorageIssueCode::kHeaderInvariant, PageNumber{1}, std::nullopt,
               header_.largest_root_page(),
               "largest root page differs from the complete schema root set");
    }
  }

  [[nodiscard]] Result<void> CheckPointerMaps() {
    if (!auto_vacuum_) {
      return {};
    }
    for (std::uint32_t raw_page = 2; raw_page <= pager_.page_count() && !stopped_; ++raw_page) {
      const PageNumber page{raw_page};
      const PageClaim& claim = claims_[raw_page];
      if (!claim.pointer_expectation.has_value()) {
        continue;
      }
      auto map_page = PointerMapPageFor(page, geometry_);
      if (!map_page.has_value()) {
        AddIssue(StorageIssueCode::kPointerMap, page, std::nullopt, std::nullopt,
                 "page has no valid pointer-map location");
        continue;
      }
      auto pin = ReadPageOrIssue(*map_page, StorageIssueCode::kPointerMap, page, std::nullopt,
                                 "unable to read pointer-map page");
      if (!pin.has_value()) {
        return std::unexpected(std::move(pin.error()));
      }
      if (!pin->has_value()) {
        continue;
      }
      auto map = PointerMapView::Parse((*pin)->frame().bytes(), *map_page, geometry_);
      if (!map.has_value()) {
        AddIssue(StorageIssueCode::kPointerMap, page, std::nullopt, *map_page,
                 "pointer-map page is malformed");
        continue;
      }
      auto entry = map->entry(page);
      if (!entry.has_value()) {
        AddIssue(StorageIssueCode::kPointerMap, page, std::nullopt, *map_page,
                 "pointer-map entry is malformed");
        continue;
      }
      const PointerExpectation& expected = *claim.pointer_expectation;
      if (entry->type != expected.type || entry->parent != expected.parent) {
        AddIssue(StorageIssueCode::kPointerMap, page, std::nullopt, *map_page,
                 "pointer-map entry does not match the observed owner");
      }
    }
    return {};
  }

  void MarkUnusedPages() {
    if (stopped_ || !report_.root_discovery_complete) {
      return;
    }
    for (std::uint32_t page = 1; page <= pager_.page_count() && !stopped_; ++page) {
      if (!claims_[page].claimed) {
        AddIssue(StorageIssueCode::kUnusedPage, PageNumber{page}, std::nullopt, std::nullopt,
                 "snapshot page is not referenced");
      }
    }
  }

  static bool LessBytes(const std::vector<std::byte>& left, const std::vector<std::byte>& right) {
    return std::ranges::lexicographical_compare(left, right);
  }

  static bool LessObjects(const StorageObjectReport& left, const StorageObjectReport& right) {
    if (left.root_page != right.root_page) {
      return left.root_page < right.root_page;
    }
    if (left.declared_type.bytes != right.declared_type.bytes) {
      return LessBytes(left.declared_type.bytes, right.declared_type.bytes);
    }
    if (left.name.bytes != right.name.bytes) {
      return LessBytes(left.name.bytes, right.name.bytes);
    }
    if (left.table_name.bytes != right.table_name.bytes) {
      return LessBytes(left.table_name.bytes, right.table_name.bytes);
    }
    if (left.sql_is_null != right.sql_is_null) {
      return left.sql_is_null < right.sql_is_null;
    }
    if (left.sql_bytes != right.sql_bytes) {
      return left.sql_bytes < right.sql_bytes;
    }
    return left.schema_rowid < right.schema_rowid;
  }

  static bool LessIssues(const StorageIssue& left, const StorageIssue& right) {
    return std::tuple{
               left.page.has_value(),
               left.page.value_or(PageNumber{0}),
               left.cell.has_value(),
               left.cell.value_or(0),
               left.code,
               left.related_page.has_value(),
               left.related_page.value_or(PageNumber{0}),
               left.message,
           } < std::tuple{
                   right.page.has_value(),
                   right.page.value_or(PageNumber{0}),
                   right.cell.has_value(),
                   right.cell.value_or(0),
                   right.code,
                   right.related_page.has_value(),
                   right.related_page.value_or(PageNumber{0}),
                   right.message,
               };
  }

  void Finalize() {
    std::ranges::sort(report_.objects, LessObjects);
    std::ranges::sort(report_.issues, LessIssues);

    StorageInspectionSummary& summary = report_.summary;
    summary.object_count = report_.objects.size();
    summary.issue_count = report_.issues.size();
    for (const StoragePageReport& page : report_.pages) {
      switch (page.role) {
        case StoragePageRole::kUnreferenced:
          ++summary.unreferenced_pages;
          break;
        case StoragePageRole::kBtree:
          ++summary.btree_pages;
          break;
        case StoragePageRole::kOverflow:
          ++summary.overflow_pages;
          break;
        case StoragePageRole::kFreelistTrunk:
          ++summary.freelist_trunk_pages;
          break;
        case StoragePageRole::kFreelistLeaf:
          ++summary.freelist_leaf_pages;
          break;
        case StoragePageRole::kPointerMap:
          ++summary.pointer_map_pages;
          break;
        case StoragePageRole::kLocking:
          ++summary.locking_pages;
          break;
      }
      summary.cell_count += page.cells.size();
      summary.record_count += static_cast<std::size_t>(std::ranges::count_if(
          page.cells, [](const StorageCellReport& cell) { return cell.record.has_value(); }));
    }
  }

  ReadPager& pager_;
  const DatabaseHeader& header_;
  BtreePageGeometry geometry_;
  bool auto_vacuum_;
  RecordCodecOptions record_options_;
  StorageInspectionOptions options_;
  StorageInspectionReport report_;
  std::vector<PageClaim> claims_;
  std::vector<RootCandidate> root_candidates_;
  std::vector<RootCandidate> roots_;
  bool stopped_ = false;
};

class JsonWriter final {
 public:
  void Raw(std::string_view value) { output_.append(value); }

  void String(std::string_view value) {
    output_.push_back('"');
    constexpr std::string_view kHex = "0123456789abcdef";
    for (const char character : value) {
      const auto byte = static_cast<unsigned char>(character);
      switch (byte) {
        case '"':
          output_.append("\\\"");
          break;
        case '\\':
          output_.append("\\\\");
          break;
        case '\b':
          output_.append("\\b");
          break;
        case '\f':
          output_.append("\\f");
          break;
        case '\n':
          output_.append("\\n");
          break;
        case '\r':
          output_.append("\\r");
          break;
        case '\t':
          output_.append("\\t");
          break;
        default:
          if (byte < 0x20U) {
            output_.append("\\u00");
            output_.push_back(kHex[byte >> 4U]);
            output_.push_back(kHex[byte & 0x0fU]);
          } else {
            output_.push_back(static_cast<char>(byte));
          }
          break;
      }
    }
    output_.push_back('"');
  }

  template <typename Integer>
  void IntegerValue(Integer value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output_.append(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
  }

  void Boolean(bool value) { Raw(value ? "true" : "false"); }
  void Null() { Raw("null"); }

  [[nodiscard]] std::string Take() && { return std::move(output_); }

 private:
  std::string output_;
};

void RenderText(JsonWriter& writer, const StorageText& text) {
  writer.Raw("{\"encoding\":");
  if (text.valid_utf8) {
    writer.String("utf8");
    writer.Raw(",\"value\":");
    writer.String(AsStringView(text.bytes));
  } else {
    writer.String("hex");
    writer.Raw(R"(,"value":")");
    constexpr std::string_view kHex = "0123456789abcdef";
    for (const std::byte byte : text.bytes) {
      const auto value = std::to_integer<std::uint8_t>(byte);
      writer.Raw(std::string_view{&kHex[value >> 4U], 1});
      writer.Raw(std::string_view{&kHex[value & 0x0fU], 1});
    }
    writer.Raw("\"");
  }
  writer.Raw("}");
}

void RenderOptionalPage(JsonWriter& writer, std::optional<PageNumber> page) {
  if (page.has_value()) {
    writer.IntegerValue(page->value());
  } else {
    writer.Null();
  }
}

void RenderOptionalSize(JsonWriter& writer, std::optional<std::size_t> value) {
  if (value.has_value()) {
    writer.IntegerValue(*value);
  } else {
    writer.Null();
  }
}

void RenderOptionalCount(JsonWriter& writer, std::optional<ByteCount> value) {
  if (value.has_value()) {
    writer.IntegerValue(value->value());
  } else {
    writer.Null();
  }
}

void RenderHeader(JsonWriter& writer, const std::optional<StorageHeaderReport>& header) {
  if (!header.has_value()) {
    writer.Null();
    return;
  }
  writer.Raw("{\"snapshot_page_count\":");
  writer.IntegerValue(header->snapshot_page_count);
  writer.Raw(",\"header_page_count\":");
  writer.IntegerValue(header->header_page_count);
  writer.Raw(",\"page_size\":");
  writer.IntegerValue(header->page_size.value());
  writer.Raw(",\"reserved_bytes\":");
  writer.IntegerValue(header->reserved_bytes.value());
  writer.Raw(",\"usable_size\":");
  writer.IntegerValue(header->usable_size.value());
  writer.Raw(",\"write_version\":");
  writer.IntegerValue(header->write_version);
  writer.Raw(",\"read_version\":");
  writer.IntegerValue(header->read_version);
  writer.Raw(",\"file_change_counter\":");
  writer.IntegerValue(header->file_change_counter);
  writer.Raw(",\"first_freelist_trunk\":");
  writer.IntegerValue(header->first_freelist_trunk.value());
  writer.Raw(",\"freelist_page_count\":");
  writer.IntegerValue(header->freelist_page_count);
  writer.Raw(",\"schema_cookie\":");
  writer.IntegerValue(header->schema_cookie);
  writer.Raw(",\"raw_schema_format\":");
  writer.IntegerValue(header->raw_schema_format);
  writer.Raw(",\"effective_schema_format\":");
  writer.IntegerValue(static_cast<std::uint8_t>(header->effective_schema_format));
  writer.Raw(",\"suggested_cache_size\":");
  writer.IntegerValue(header->suggested_cache_size);
  writer.Raw(",\"largest_root_page\":");
  writer.IntegerValue(header->largest_root_page.value());
  writer.Raw(",\"raw_text_encoding\":");
  writer.IntegerValue(header->raw_text_encoding);
  writer.Raw(",\"effective_text_encoding\":");
  writer.IntegerValue(header->effective_text_encoding);
  writer.Raw(",\"user_version\":");
  writer.IntegerValue(header->user_version);
  writer.Raw(",\"incremental_vacuum\":");
  writer.IntegerValue(header->incremental_vacuum);
  writer.Raw(",\"application_id\":");
  writer.IntegerValue(header->application_id);
  writer.Raw(",\"version_valid_for\":");
  writer.IntegerValue(header->version_valid_for);
  writer.Raw(",\"sqlite_version\":");
  writer.IntegerValue(header->sqlite_version);
  writer.Raw(",\"auto_vacuum\":");
  writer.Boolean(header->auto_vacuum);
  writer.Raw("}");
}

void RenderObjects(JsonWriter& writer, const std::vector<StorageObjectReport>& objects) {
  writer.Raw("[");
  for (std::size_t index = 0; index < objects.size(); ++index) {
    if (index != 0) {
      writer.Raw(",");
    }
    const StorageObjectReport& object = objects[index];
    writer.Raw("{\"schema_rowid\":");
    writer.IntegerValue(object.schema_rowid);
    writer.Raw(",\"kind\":");
    writer.String(StorageSchemaObjectKindName(object.kind));
    writer.Raw(",\"declared_type\":");
    RenderText(writer, object.declared_type);
    writer.Raw(",\"name\":");
    RenderText(writer, object.name);
    writer.Raw(",\"table_name\":");
    RenderText(writer, object.table_name);
    writer.Raw(",\"root_page\":");
    writer.IntegerValue(object.root_page);
    writer.Raw(R"(,"sql":{"is_null":)");
    writer.Boolean(object.sql_is_null);
    writer.Raw(",\"bytes\":");
    writer.IntegerValue(object.sql_bytes.value());
    writer.Raw("}}");
  }
  writer.Raw("]");
}

void RenderRecord(JsonWriter& writer, const std::optional<StorageRecordReport>& record) {
  if (!record.has_value()) {
    writer.Null();
    return;
  }
  writer.Raw("{\"field_count\":");
  writer.IntegerValue(record->field_types.size());
  writer.Raw(",\"field_types\":[");
  for (std::size_t index = 0; index < record->field_types.size(); ++index) {
    if (index != 0) {
      writer.Raw(",");
    }
    writer.String(SqlValueTypeName(record->field_types[index]));
  }
  writer.Raw("]}");
}

void RenderCells(JsonWriter& writer, const std::vector<StorageCellReport>& cells) {
  writer.Raw("[");
  for (std::size_t index = 0; index < cells.size(); ++index) {
    if (index != 0) {
      writer.Raw(",");
    }
    const StorageCellReport& cell = cells[index];
    writer.Raw("{\"index\":");
    writer.IntegerValue(cell.index);
    writer.Raw(",\"left_child\":");
    RenderOptionalPage(writer, cell.left_child);
    writer.Raw(",\"rowid\":");
    if (cell.rowid.has_value()) {
      writer.IntegerValue(*cell.rowid);
    } else {
      writer.Null();
    }
    writer.Raw(",\"payload_bytes\":");
    writer.IntegerValue(cell.payload_bytes.value());
    writer.Raw(",\"local_payload_bytes\":");
    writer.IntegerValue(cell.local_payload_bytes.value());
    writer.Raw(",\"overflow_pages\":[");
    for (std::size_t overflow_index = 0; overflow_index < cell.overflow_pages.size();
         ++overflow_index) {
      if (overflow_index != 0) {
        writer.Raw(",");
      }
      writer.IntegerValue(cell.overflow_pages[overflow_index].value());
    }
    writer.Raw("],\"record\":");
    RenderRecord(writer, cell.record);
    writer.Raw("}");
  }
  writer.Raw("]");
}

void RenderPages(JsonWriter& writer, const std::vector<StoragePageReport>& pages) {
  writer.Raw("[");
  for (std::size_t index = 0; index < pages.size(); ++index) {
    if (index != 0) {
      writer.Raw(",");
    }
    const StoragePageReport& page = pages[index];
    writer.Raw("{\"page_number\":");
    writer.IntegerValue(page.page_number.value());
    writer.Raw(",\"role\":");
    writer.String(StoragePageRoleName(page.role));
    writer.Raw(",\"root_page\":");
    RenderOptionalPage(writer, page.root_page);
    writer.Raw(",\"parent_page\":");
    RenderOptionalPage(writer, page.parent_page);
    writer.Raw(",\"btree_type\":");
    if (page.btree_type.has_value()) {
      writer.String(BtreePageTypeName(*page.btree_type));
    } else {
      writer.Null();
    }
    writer.Raw(",\"depth_from_root\":");
    RenderOptionalSize(writer, page.depth_from_root);
    writer.Raw(",\"cell_count\":");
    RenderOptionalSize(writer, page.cell_count);
    writer.Raw(",\"cell_content_offset\":");
    RenderOptionalCount(writer, page.cell_content_offset);
    writer.Raw(",\"fragmented_free_bytes\":");
    RenderOptionalCount(writer, page.fragmented_free_bytes);
    writer.Raw(",\"free_bytes\":");
    RenderOptionalCount(writer, page.free_bytes);
    writer.Raw(",\"cells\":");
    RenderCells(writer, page.cells);
    writer.Raw("}");
  }
  writer.Raw("]");
}

void RenderSummary(JsonWriter& writer, const StorageInspectionSummary& summary) {
  writer.Raw("{\"snapshot_page_count\":");
  writer.IntegerValue(summary.snapshot_page_count);
  writer.Raw(",\"root_count\":");
  writer.IntegerValue(summary.root_count);
  writer.Raw(",\"object_count\":");
  writer.IntegerValue(summary.object_count);
  writer.Raw(",\"btree_pages\":");
  writer.IntegerValue(summary.btree_pages);
  writer.Raw(",\"overflow_pages\":");
  writer.IntegerValue(summary.overflow_pages);
  writer.Raw(",\"freelist_trunk_pages\":");
  writer.IntegerValue(summary.freelist_trunk_pages);
  writer.Raw(",\"freelist_leaf_pages\":");
  writer.IntegerValue(summary.freelist_leaf_pages);
  writer.Raw(",\"pointer_map_pages\":");
  writer.IntegerValue(summary.pointer_map_pages);
  writer.Raw(",\"locking_pages\":");
  writer.IntegerValue(summary.locking_pages);
  writer.Raw(",\"unreferenced_pages\":");
  writer.IntegerValue(summary.unreferenced_pages);
  writer.Raw(",\"cell_count\":");
  writer.IntegerValue(summary.cell_count);
  writer.Raw(",\"record_count\":");
  writer.IntegerValue(summary.record_count);
  writer.Raw(",\"issue_count\":");
  writer.IntegerValue(summary.issue_count);
  writer.Raw("}");
}

void RenderIssues(JsonWriter& writer, const std::vector<StorageIssue>& issues) {
  writer.Raw("[");
  for (std::size_t index = 0; index < issues.size(); ++index) {
    if (index != 0) {
      writer.Raw(",");
    }
    const StorageIssue& issue = issues[index];
    writer.Raw("{\"code\":");
    writer.String(StorageIssueCodeName(issue.code));
    writer.Raw(",\"page\":");
    RenderOptionalPage(writer, issue.page);
    writer.Raw(",\"cell\":");
    RenderOptionalSize(writer, issue.cell);
    writer.Raw(",\"related_page\":");
    RenderOptionalPage(writer, issue.related_page);
    writer.Raw(",\"message\":");
    writer.String(issue.message);
    writer.Raw("}");
  }
  writer.Raw("]");
}

}  // namespace

Result<StorageInspectionReport> InspectDatabase(Vfs& vfs, std::string_view path,
                                                StorageInspectionOptions options) {
  try {
    if (options.max_issues == 0) {
      return std::unexpected(Misuse("storage inspection issue limit must be nonzero"));
    }
    auto pager = ReadPager::Open(vfs, path);
    if (!pager.has_value()) {
      return std::unexpected(std::move(pager.error()));
    }
    auto begun = (*pager)->BeginRead();
    if (!begun.has_value()) {
      return std::unexpected(std::move(begun.error()));
    }

    std::optional<Result<StorageInspectionReport>> inspected;
    try {
      inspected.emplace([&]() -> Result<StorageInspectionReport> {
        if ((*pager)->page_count() == 0) {
          StorageInspectionReport empty;
          empty.summary.snapshot_page_count = 0;
          return empty;
        }
        const DatabaseHeader* header = (*pager)->header();
        if (header == nullptr) {
          return std::unexpected(
              MakeError(ErrorCode::kInternal, "nonempty snapshot has no database header"));
        }
        auto schema_format = EffectiveSchemaFormat(header->schema_format());
        if (!schema_format.has_value()) {
          return std::unexpected(std::move(schema_format.error()));
        }
        auto text_encoding = EffectiveTextEncoding(header->text_encoding());
        if (!text_encoding.has_value()) {
          return std::unexpected(std::move(text_encoding.error()));
        }
        auto geometry = BtreePageGeometry::Create(header->page_size(), header->usable_size());
        if (!geometry.has_value()) {
          return std::unexpected(std::move(geometry.error()));
        }
        Inspector inspector{**pager, *header, *geometry, *schema_format, options};
        return inspector.Run();
      }());
    } catch (const std::bad_alloc&) {
      inspected.emplace(std::unexpected(Error::OutOfMemory()));
    }

    auto ended = (*pager)->EndRead();
    if (!ended.has_value()) {
      return std::unexpected(std::move(ended.error()));
    }
    return std::move(*inspected);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::string> RenderStorageInspectionJson(const StorageInspectionReport& report) {
  try {
    JsonWriter writer;
    writer.Raw(R"({"format_version":1,"ok":)");
    writer.Boolean(report.ok());
    writer.Raw(",\"root_discovery_complete\":");
    writer.Boolean(report.root_discovery_complete);
    writer.Raw(",\"issues_truncated\":");
    writer.Boolean(report.issues_truncated);
    writer.Raw(",\"header\":");
    RenderHeader(writer, report.header);
    writer.Raw(",\"objects\":");
    RenderObjects(writer, report.objects);
    writer.Raw(",\"pages\":");
    RenderPages(writer, report.pages);
    writer.Raw(",\"summary\":");
    RenderSummary(writer, report.summary);
    writer.Raw(",\"issues\":");
    RenderIssues(writer, report.issues);
    writer.Raw("}");
    return std::move(writer).Take();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::string> RenderStorageInspectionErrorJson(ErrorCode code) {
  try {
    JsonWriter writer;
    writer.Raw(R"({"format_version":1,"ok":false,"fatal_error":{"code":)");
    writer.String(ErrorCodeName(code));
    writer.Raw("}}");
    return std::move(writer).Take();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace modern_sqlite
