#ifndef MODERN_SQLITE_STORAGE_BTREE_WRITER_INTERNAL_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_WRITER_INTERNAL_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/btree/cursor.hpp"
#include "modern_sqlite/storage/btree/page.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite::btree_internal {

inline constexpr std::size_t kMaximumMutationPages = 32;
inline constexpr std::size_t kStagedCellSlots = 4;

class MutationPageOwner final {
 public:
  explicit MutationPageOwner(Pager& pager) noexcept;

  MutationPageOwner(const MutationPageOwner&) = delete;
  MutationPageOwner& operator=(const MutationPageOwner&) = delete;
  MutationPageOwner(MutationPageOwner&&) = delete;
  MutationPageOwner& operator=(MutationPageOwner&&) = delete;
  ~MutationPageOwner() = default;

  [[nodiscard]] Status CheckActive() const;
  [[nodiscard]] Result<std::size_t> AcquireRead(PageNumber page_number);
  [[nodiscard]] Result<std::size_t> AcquireWrite(PageNumber page_number);
  [[nodiscard]] Result<std::size_t> AllocatePage();
  [[nodiscard]] Result<std::size_t> Borrow(PageNumber page_number) const;
  [[nodiscard]] Status Promote(std::size_t slot);
  void Release(std::size_t slot) noexcept;

  [[nodiscard]] Result<std::reference_wrapper<const PageFrame>> Frame(std::size_t slot) const;
  [[nodiscard]] Result<MutableByteView> MutableBytes(std::size_t slot);
  [[nodiscard]] std::optional<std::size_t> Find(PageNumber page_number) const noexcept;
  [[nodiscard]] bool IsWritable(std::size_t slot) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] Pager& pager() const noexcept { return *pager_; }
  [[nodiscard]] std::uint64_t mutation_sequence() const noexcept { return mutation_sequence_; }
  void NoteMutation() noexcept { ++mutation_sequence_; }
  void MarkRollbackRequiredAfter(ErrorCode code, std::uint64_t checkpoint) noexcept;

 private:
  struct OwnedPage {
    PageNumber page_number;
    std::variant<std::monostate, ReadPagePin, WritePagePin> pin;
  };

  [[nodiscard]] Result<std::size_t> EmptySlot() const;

  Pager* pager_;
  std::uint64_t generation_;
  std::array<OwnedPage, kMaximumMutationPages> pages_{};
  std::size_t size_ = 0;
  std::uint64_t mutation_sequence_ = 0;
};

struct AllocatedBtreePage {
  PageNumber page_number;
  std::size_t owner_slot;
  bool reused_freelist;
};

class BtreeWriteWorkspace;

[[nodiscard]] Result<AllocatedBtreePage> AllocateBtreePage(MutationPageOwner& owner,
                                                           BtreePageGeometry geometry);
[[nodiscard]] Status FreeBtreePage(MutationPageOwner& owner, BtreePageGeometry geometry,
                                   PageNumber page_number);

struct FormattedCell {
  ByteView bytes;
  std::size_t payload_size;
  std::optional<PageNumber> first_overflow_page;
};

[[nodiscard]] Result<FormattedCell> FillTableLeafCell(MutationPageOwner& owner,
                                                      BtreePageGeometry geometry,
                                                      BtreeWriteWorkspace& workspace,
                                                      std::int64_t rowid, ByteView payload);
[[nodiscard]] Result<FormattedCell> FillIndexCell(MutationPageOwner& owner,
                                                  BtreePageGeometry geometry,
                                                  BtreeWriteWorkspace& workspace, ByteView payload,
                                                  BtreePageType type,
                                                  std::optional<PageNumber> left_child);
[[nodiscard]] Status ClearCellOverflow(MutationPageOwner& owner, BtreePageGeometry geometry,
                                       const BtreeCellView& cell);

class BtreeWriteWorkspace final {
 public:
  [[nodiscard]] static Result<BtreeWriteWorkspace> Create(ByteCount page_size);

  BtreeWriteWorkspace(const BtreeWriteWorkspace&) = delete;
  BtreeWriteWorkspace& operator=(const BtreeWriteWorkspace&) = delete;
  BtreeWriteWorkspace(BtreeWriteWorkspace&&) noexcept = default;
  BtreeWriteWorkspace& operator=(BtreeWriteWorkspace&&) noexcept = default;
  ~BtreeWriteWorkspace() = default;

  [[nodiscard]] MutableByteView cell_scratch_with_prefix() noexcept;
  [[nodiscard]] MutableByteView cell_scratch() noexcept;
  [[nodiscard]] MutableByteView rebuild_scratch() noexcept;

 private:
  BtreeWriteWorkspace(ByteBuffer cell_scratch, ByteBuffer rebuild_scratch) noexcept;

  ByteBuffer cell_scratch_;
  ByteBuffer rebuild_scratch_;
};

struct StagedCell {
  std::size_t index;
  ByteView bytes;
};

class MutableBtreePage final {
 public:
  [[nodiscard]] static Result<MutableBtreePage> Open(MutationPageOwner& owner,
                                                     std::size_t owner_slot,
                                                     BtreePageGeometry geometry);
  [[nodiscard]] static Result<MutableBtreePage> Initialize(MutationPageOwner& owner,
                                                           std::size_t owner_slot,
                                                           BtreePageGeometry geometry,
                                                           BtreePageType type);

  MutableBtreePage(const MutableBtreePage&) = delete;
  MutableBtreePage& operator=(const MutableBtreePage&) = delete;
  MutableBtreePage(MutableBtreePage&& other) noexcept;
  MutableBtreePage& operator=(MutableBtreePage&&) = delete;
  ~MutableBtreePage() = default;

  [[nodiscard]] Status Zero(BtreePageType type);
  [[nodiscard]] Status Defragment(std::size_t maximum_fragments, BtreeWriteWorkspace& workspace);
  [[nodiscard]] Result<std::size_t> AllocateSpace(std::size_t size, BtreeWriteWorkspace& workspace);
  [[nodiscard]] Status FreeSpace(std::size_t offset, std::size_t size);
  [[nodiscard]] Status DropCell(std::size_t index);
  [[nodiscard]] Status OverwritePayload(std::size_t index, ByteView payload,
                                        BtreeWriteWorkspace& workspace);
  [[nodiscard]] Status InsertCell(std::size_t index, ByteView cell,
                                  std::optional<PageNumber> left_child, MutableByteView staged_copy,
                                  BtreeWriteWorkspace& workspace);

  void ClearStagedCells() noexcept;

  [[nodiscard]] PageNumber page_number() const noexcept { return page_number_; }
  [[nodiscard]] BtreePageType type() const noexcept { return type_; }
  [[nodiscard]] bool is_leaf() const noexcept;
  [[nodiscard]] bool is_table() const noexcept;
  [[nodiscard]] std::size_t cell_count() const noexcept { return cell_count_; }
  [[nodiscard]] std::size_t free_bytes() const noexcept { return free_bytes_; }
  [[nodiscard]] std::size_t staged_count() const noexcept { return staged_count_; }
  [[nodiscard]] std::optional<StagedCell> staged_cell(std::size_t slot) const noexcept;
  [[nodiscard]] std::size_t header_offset() const noexcept { return header_offset_; }
  [[nodiscard]] std::size_t cell_pointer_offset() const noexcept { return cell_pointer_offset_; }

 private:
  struct Metadata {
    std::size_t header_offset;
    std::size_t cell_pointer_offset;
    std::size_t cell_count;
    std::size_t free_bytes;
  };

  MutableBtreePage(MutationPageOwner& owner, std::size_t owner_slot, BtreePageGeometry geometry,
                   PageNumber page_number, BtreePageType type, Metadata metadata) noexcept;

  [[nodiscard]] Result<MutableByteView> Bytes();
  [[nodiscard]] Result<ByteView> Bytes() const;
  [[nodiscard]] Status ValidateCellImage(ByteView cell, std::optional<PageNumber> left_child) const;
  [[nodiscard]] Result<std::optional<std::size_t>> FindFreeblock(std::size_t size);

  MutationPageOwner* owner_;
  std::size_t owner_slot_;
  BtreePageGeometry geometry_;
  PageNumber page_number_;
  BtreePageType type_;
  std::size_t header_offset_;
  std::size_t cell_pointer_offset_;
  std::size_t cell_count_;
  std::size_t free_bytes_;
  std::array<std::optional<StagedCell>, kStagedCellSlots> staged_cells_{};
  std::size_t staged_count_ = 0;
};

enum class WritableCursorState : std::uint8_t {
  kInvalid,
  kValid,
  kFault,
};

struct TableSeekResult {
  std::size_t insertion_index;
  int comparison;
  bool exact;
  std::size_t tree_depth;
};

struct IndexSeekResult {
  std::size_t insertion_index;
  int comparison;
  bool exact;
  std::size_t tree_depth;
};

class WritableCursor final {
 public:
  [[nodiscard]] static Result<WritableCursor> Open(MutationPageOwner& owner, PageNumber root_page,
                                                   bool table);

  WritableCursor(const WritableCursor&) = delete;
  WritableCursor& operator=(const WritableCursor&) = delete;
  WritableCursor(WritableCursor&& other) noexcept;
  WritableCursor& operator=(WritableCursor&&) = delete;
  ~WritableCursor() = default;

  [[nodiscard]] Status CheckActive() const;
  [[nodiscard]] Result<TableSeekResult> SeekTable(std::int64_t rowid);
  [[nodiscard]] Result<IndexSeekResult> SeekIndex(std::span<const SqlValue> key,
                                                  std::span<const IndexColumnOrder> columns,
                                                  RecordCodecOptions options,
                                                  std::vector<std::byte>& scratch);
  [[nodiscard]] Result<BtreePageView> CurrentPage() const;
  [[nodiscard]] Status PromoteCurrent();
  [[nodiscard]] Status MoveToParent();
  [[nodiscard]] Status ResetToRoot();

  [[nodiscard]] WritableCursorState state() const noexcept { return state_; }
  [[nodiscard]] std::size_t depth() const noexcept { return frame_count_; }
  [[nodiscard]] std::size_t current_index() const noexcept;
  [[nodiscard]] std::size_t current_owner_slot() const noexcept;
  [[nodiscard]] PageNumber root_page() const noexcept { return root_page_; }
  [[nodiscard]] bool table() const noexcept { return table_; }

 private:
  struct Frame {
    std::size_t owner_slot = 0;
    std::size_t child_index = 0;
  };

  WritableCursor(MutationPageOwner& owner, PageNumber root_page, bool table,
                 BtreePageGeometry geometry, std::size_t root_slot) noexcept;

  [[nodiscard]] Result<PageNumber> ChildAt(const BtreePageView& page,
                                           std::size_t child_index) const;
  [[nodiscard]] Result<ByteView> ReadCellPayload(const BtreeCellView& cell,
                                                 std::vector<std::byte>& scratch);
  [[nodiscard]] Status Descend(std::size_t child_index, const BtreePageView& parent);
  void ReleaseDescendants() noexcept;
  void EnterFault() noexcept;

  MutationPageOwner* owner_;
  PageNumber root_page_;
  bool table_;
  BtreePageGeometry geometry_;
  std::array<Frame, kMaximumBtreeDepth> frames_{};
  std::size_t frame_count_ = 0;
  std::size_t current_index_ = 0;
  WritableCursorState state_ = WritableCursorState::kInvalid;
};

}  // namespace modern_sqlite::btree_internal

#endif  // MODERN_SQLITE_STORAGE_BTREE_WRITER_INTERNAL_HPP_
