#ifndef MODERN_SQLITE_DIAGNOSTICS_STORAGE_INSPECTOR_HPP_
#define MODERN_SQLITE_DIAGNOSTICS_STORAGE_INSPECTOR_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

enum class StorageIssueCode : std::uint8_t {
  kHeaderInvariant,
  kSchemaRecord,
  kDuplicateRoot,
  kInvalidPageReference,
  kDuplicatePageReference,
  kPageDecode,
  kTreeKindMismatch,
  kEmptyChildPage,
  kDepthLimit,
  kUnequalChildDepth,
  kTableKeyOrder,
  kPageCoverage,
  kRecordDecode,
  kOverflowChain,
  kFreelist,
  kPointerMap,
  kUnusedPage,
};

enum class StoragePageRole : std::uint8_t {
  kUnreferenced,
  kBtree,
  kOverflow,
  kFreelistTrunk,
  kFreelistLeaf,
  kPointerMap,
  kLocking,
};

enum class StorageSchemaObjectKind : std::uint8_t {
  kUnknown,
  kTable,
  kIndex,
  kView,
  kTrigger,
};

[[nodiscard]] constexpr std::string_view StorageIssueCodeName(StorageIssueCode code) noexcept {
  switch (code) {
    case StorageIssueCode::kHeaderInvariant:
      return "header_invariant";
    case StorageIssueCode::kSchemaRecord:
      return "schema_record";
    case StorageIssueCode::kDuplicateRoot:
      return "duplicate_root";
    case StorageIssueCode::kInvalidPageReference:
      return "invalid_page_reference";
    case StorageIssueCode::kDuplicatePageReference:
      return "duplicate_page_reference";
    case StorageIssueCode::kPageDecode:
      return "page_decode";
    case StorageIssueCode::kTreeKindMismatch:
      return "tree_kind_mismatch";
    case StorageIssueCode::kEmptyChildPage:
      return "empty_child_page";
    case StorageIssueCode::kDepthLimit:
      return "depth_limit";
    case StorageIssueCode::kUnequalChildDepth:
      return "unequal_child_depth";
    case StorageIssueCode::kTableKeyOrder:
      return "table_key_order";
    case StorageIssueCode::kPageCoverage:
      return "page_coverage";
    case StorageIssueCode::kRecordDecode:
      return "record_decode";
    case StorageIssueCode::kOverflowChain:
      return "overflow_chain";
    case StorageIssueCode::kFreelist:
      return "freelist";
    case StorageIssueCode::kPointerMap:
      return "pointer_map";
    case StorageIssueCode::kUnusedPage:
      return "unused_page";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view StoragePageRoleName(StoragePageRole role) noexcept {
  switch (role) {
    case StoragePageRole::kUnreferenced:
      return "unreferenced";
    case StoragePageRole::kBtree:
      return "btree";
    case StoragePageRole::kOverflow:
      return "overflow";
    case StoragePageRole::kFreelistTrunk:
      return "freelist_trunk";
    case StoragePageRole::kFreelistLeaf:
      return "freelist_leaf";
    case StoragePageRole::kPointerMap:
      return "pointer_map";
    case StoragePageRole::kLocking:
      return "locking";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view StorageSchemaObjectKindName(
    StorageSchemaObjectKind kind) noexcept {
  switch (kind) {
    case StorageSchemaObjectKind::kUnknown:
      return "unknown";
    case StorageSchemaObjectKind::kTable:
      return "table";
    case StorageSchemaObjectKind::kIndex:
      return "index";
    case StorageSchemaObjectKind::kView:
      return "view";
    case StorageSchemaObjectKind::kTrigger:
      return "trigger";
  }
  return "unknown";
}

struct StorageText {
  std::vector<std::byte> bytes;
  bool valid_utf8 = true;
};

struct StorageHeaderReport {
  std::uint32_t snapshot_page_count = 0;
  std::uint32_t header_page_count = 0;
  ByteCount page_size;
  ByteCount reserved_bytes;
  ByteCount usable_size;
  std::uint8_t write_version = 0;
  std::uint8_t read_version = 0;
  std::uint32_t file_change_counter = 0;
  PageNumber first_freelist_trunk;
  std::uint32_t freelist_page_count = 0;
  std::uint32_t schema_cookie = 0;
  std::uint32_t raw_schema_format = 0;
  RecordSchemaFormat effective_schema_format = RecordSchemaFormat::kOne;
  std::int32_t suggested_cache_size = 0;
  PageNumber largest_root_page;
  std::uint32_t raw_text_encoding = 0;
  std::uint8_t effective_text_encoding = 1;
  std::uint32_t user_version = 0;
  std::uint32_t incremental_vacuum = 0;
  std::uint32_t application_id = 0;
  std::uint32_t version_valid_for = 0;
  std::uint32_t sqlite_version = 0;
  bool auto_vacuum = false;
};

struct StorageObjectReport {
  std::int64_t schema_rowid = 0;
  StorageSchemaObjectKind kind = StorageSchemaObjectKind::kUnknown;
  StorageText declared_type;
  StorageText name;
  StorageText table_name;
  std::uint32_t root_page = 0;
  bool sql_is_null = true;
  ByteCount sql_bytes;
};

struct StorageRecordReport {
  std::vector<SqlValueType> field_types;
};

struct StorageCellReport {
  std::size_t index = 0;
  std::optional<PageNumber> left_child;
  std::optional<std::int64_t> rowid;
  ByteCount payload_bytes;
  ByteCount local_payload_bytes;
  std::vector<PageNumber> overflow_pages;
  std::optional<StorageRecordReport> record;
};

struct StoragePageReport {
  PageNumber page_number;
  StoragePageRole role = StoragePageRole::kUnreferenced;
  std::optional<PageNumber> root_page;
  std::optional<PageNumber> parent_page;
  std::optional<BtreePageType> btree_type;
  std::optional<std::size_t> depth_from_root;
  std::optional<std::size_t> cell_count;
  std::optional<ByteCount> cell_content_offset;
  std::optional<ByteCount> fragmented_free_bytes;
  std::optional<ByteCount> free_bytes;
  std::vector<StorageCellReport> cells;
};

struct StorageIssue {
  StorageIssueCode code = StorageIssueCode::kHeaderInvariant;
  std::optional<PageNumber> page;
  std::optional<std::size_t> cell;
  std::optional<PageNumber> related_page;
  std::string message;
};

struct StorageInspectionSummary {
  std::uint32_t snapshot_page_count = 0;
  std::size_t root_count = 0;
  std::size_t object_count = 0;
  std::size_t btree_pages = 0;
  std::size_t overflow_pages = 0;
  std::size_t freelist_trunk_pages = 0;
  std::size_t freelist_leaf_pages = 0;
  std::size_t pointer_map_pages = 0;
  std::size_t locking_pages = 0;
  std::size_t unreferenced_pages = 0;
  std::size_t cell_count = 0;
  std::size_t record_count = 0;
  std::size_t issue_count = 0;
};

struct StorageInspectionOptions {
  std::size_t max_issues = 100;
};

struct StorageInspectionReport {
  std::optional<StorageHeaderReport> header;
  std::vector<StorageObjectReport> objects;
  std::vector<StoragePageReport> pages;
  std::vector<StorageIssue> issues;
  StorageInspectionSummary summary;
  bool root_discovery_complete = true;
  bool issues_truncated = false;

  [[nodiscard]] bool ok() const noexcept { return issues.empty() && !issues_truncated; }
};

[[nodiscard]] Result<StorageInspectionReport> InspectDatabase(
    Vfs& vfs, std::string_view path, StorageInspectionOptions options = {});

[[nodiscard]] Result<std::string> RenderStorageInspectionJson(
    const StorageInspectionReport& report);
[[nodiscard]] Result<std::string> RenderStorageInspectionErrorJson(ErrorCode code);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_DIAGNOSTICS_STORAGE_INSPECTOR_HPP_
