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
  [[nodiscard]] Result<std::size_t> Borrow(PageNumber page_number) const;
  [[nodiscard]] Status Promote(std::size_t slot);
  void Release(std::size_t slot) noexcept;

  [[nodiscard]] Result<std::reference_wrapper<const PageFrame>> Frame(std::size_t slot) const;
  [[nodiscard]] Result<MutableByteView> MutableBytes(std::size_t slot);
  [[nodiscard]] std::optional<std::size_t> Find(PageNumber page_number) const noexcept;
  [[nodiscard]] bool IsWritable(std::size_t slot) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] Pager& pager() const noexcept { return *pager_; }

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
