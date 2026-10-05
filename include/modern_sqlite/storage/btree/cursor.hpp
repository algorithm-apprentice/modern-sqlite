#ifndef MODERN_SQLITE_STORAGE_BTREE_CURSOR_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_CURSOR_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

inline constexpr std::size_t kMaximumBtreeDepth = 20;

enum class BtreeSeekMode : std::uint8_t {
  kEqual,
  kGreaterOrEqual,
  kGreater,
  kLessOrEqual,
  kLess,
};

class BtreePayloadView final {
 public:
  [[nodiscard]] constexpr ByteCount size() const noexcept { return size_; }
  [[nodiscard]] constexpr ByteView local_bytes() const noexcept { return local_bytes_; }
  [[nodiscard]] constexpr bool is_fully_local() const noexcept {
    return size_.value() == local_bytes_.size();
  }

 private:
  friend class TableBtreeCursor;
  friend class IndexBtreeCursor;

  constexpr BtreePayloadView(ByteCount size, ByteView local_bytes) noexcept
      : size_(size), local_bytes_(local_bytes) {}

  ByteCount size_;
  ByteView local_bytes_;
};

class TableBtreeCursor final {
 private:
  struct Impl;

  explicit TableBtreeCursor(std::unique_ptr<Impl> impl) noexcept;

 public:
  [[nodiscard]] static Result<TableBtreeCursor> Open(Pager& pager, PageNumber root_page);

  TableBtreeCursor(const TableBtreeCursor&) = delete;
  TableBtreeCursor& operator=(const TableBtreeCursor&) = delete;
  TableBtreeCursor(TableBtreeCursor&&) noexcept;
  TableBtreeCursor& operator=(TableBtreeCursor&&) noexcept;
  ~TableBtreeCursor();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<bool> First();
  [[nodiscard]] Result<bool> Last();
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Result<bool> Previous();
  [[nodiscard]] Result<bool> Seek(std::int64_t rowid, BtreeSeekMode mode);

  [[nodiscard]] Result<std::int64_t> rowid() const;
  [[nodiscard]] Result<BtreePayloadView> payload() const;
  [[nodiscard]] Status ReadPayload(ByteOffset offset, MutableByteView destination);
  [[nodiscard]] Result<ByteBuffer> CopyPayload();

 private:
  std::unique_ptr<Impl> impl_;
};

class IndexBtreeCursor final {
 private:
  struct Impl;

  explicit IndexBtreeCursor(std::unique_ptr<Impl> impl) noexcept;

 public:
  [[nodiscard]] static Result<IndexBtreeCursor> Open(Pager& pager, PageNumber root_page,
                                                     std::span<const IndexColumnOrder> columns);

  IndexBtreeCursor(const IndexBtreeCursor&) = delete;
  IndexBtreeCursor& operator=(const IndexBtreeCursor&) = delete;
  IndexBtreeCursor(IndexBtreeCursor&&) noexcept;
  IndexBtreeCursor& operator=(IndexBtreeCursor&&) noexcept;
  ~IndexBtreeCursor();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<bool> First();
  [[nodiscard]] Result<bool> Last();
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Result<bool> Previous();
  [[nodiscard]] Result<bool> Seek(std::span<const SqlValue> key, BtreeSeekMode mode);

  [[nodiscard]] Result<BtreePayloadView> payload() const;
  [[nodiscard]] Status ReadPayload(ByteOffset offset, MutableByteView destination);
  [[nodiscard]] Result<ByteBuffer> CopyPayload();

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_CURSOR_HPP_
