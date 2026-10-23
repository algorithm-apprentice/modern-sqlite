#ifndef MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_
#define MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {

class SqlValueAccess {
 public:
  static constexpr std::size_t kOwnedStorageBytes = sizeof(SqlValue::ByteStorage);
  static constexpr ByteCount kSharedColumnTerminatorBytes{3};

  [[nodiscard]] static std::int64_t Integer(const SqlValue& value) noexcept;
  [[nodiscard]] static double Real(const SqlValue& value) noexcept;
  [[nodiscard]] static Utf8View Text(const SqlValue& value) noexcept;
  [[nodiscard]] static ByteView Blob(const SqlValue& value) noexcept;
  // Borrows the input or conversion scratch until either is changed or destroyed.
  [[nodiscard]] static const SqlValue& ArithmeticNumeric(const SqlValue& value,
                                                         std::optional<SqlValue>& scratch) {
    const SqlValueType type = value.type();
    if (type == SqlValueType::kNull || type == SqlValueType::kInteger ||
        type == SqlValueType::kReal) {
      return value;
    }
    scratch.emplace(CoerceNumericForArithmetic(value));
    return *scratch;
  }
  static const SqlValue& ArithmeticNumeric(SqlValue&&, std::optional<SqlValue>&) = delete;
  static const SqlValue& ArithmeticNumeric(const SqlValue&&, std::optional<SqlValue>&) = delete;

  static void SetNull(SqlValue& value) noexcept;
  static void SetInteger(SqlValue& value, std::int64_t integer) noexcept;
  static void SetReal(SqlValue& value, double real) noexcept;
  static void Release(SqlValue& value) noexcept;
  [[nodiscard]] static Status CopyText(SqlValue& value, Utf8View source) noexcept;
  [[nodiscard]] static Status CopyBlob(SqlValue& value, ByteView source) noexcept;
  [[nodiscard]] static Status Copy(SqlValue& destination, const SqlValue& source) noexcept;
  [[nodiscard]] static bool IsStaticBorrow(const SqlValue& value) noexcept;
  [[nodiscard]] static Result<MutableByteView> PrepareBytes(SqlValue& value,
                                                            ByteCount size) noexcept;
  static void CommitBytes(SqlValue& value, ByteCount size, SqlValueType type) noexcept;
  static void ShareColumnBytes(SqlValue& value, std::shared_ptr<const ByteBuffer> storage,
                               ByteCount size, SqlValueType type) noexcept;

  // The source's byte owner must remain immutable and stable until the borrow ends.
  static void BorrowStable(SqlValue& destination, const SqlValue& source) noexcept;
  static void BorrowStable(SqlValue& destination, SqlValue&& source) = delete;
  static void BorrowStable(SqlValue& destination, const SqlValue&& source) = delete;
  // An independent owner must keep the source bytes immutable until the borrow ends.
  static void BorrowStableBlob(SqlValue& destination, ByteView source) noexcept;

  [[nodiscard]] static Result<ByteView> RetainBytes(ByteBuffer& storage, ByteView source,
                                                    ByteCount trailing_zeros = {}) noexcept {
    auto retained = RetainStorage(storage, ByteCount{source.size()}, trailing_zeros, source);
    if (!retained.has_value()) {
      return std::unexpected(std::move(retained.error()));
    }
    return ByteView{*retained};
  }

 private:
  static constexpr ByteCount kTerminatorBytes{2};

  [[nodiscard]] static Result<MutableByteView> RetainStorage(
      ByteBuffer& storage, ByteCount size, ByteCount trailing_zeros,
      std::optional<ByteView> source) noexcept try {
    if (source.has_value() && source->size() != size.value()) {
      return std::unexpected(
          Error::Create(ErrorCode::kMisuse, "retained byte source has the wrong size"));
    }
    if (trailing_zeros.value() > std::numeric_limits<std::size_t>::max() - size.value()) {
      return std::unexpected(
          Error::Create(ErrorCode::kTooLarge, "retained byte length exceeds the size limit"));
    }
    const std::size_t required = size.value() + trailing_zeros.value();
    ByteBuffer grown;
    ByteBuffer* destination = &storage;
    if (storage.size().value() < required) {
      grown = ByteBuffer{ByteCount{required}};
      destination = &grown;
    }
    if (source.has_value() && !CopyBytes(destination->mutable_view(), *source).has_value()) {
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "retained byte storage is too small"));
    }
    std::ranges::fill(destination->mutable_view().subspan(size.value(), trailing_zeros.value()),
                      std::byte{});
    if (destination == &grown) {
      storage = std::move(grown);
    }
    return storage.mutable_view().first(size.value());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }

  [[nodiscard]] static Status CopyByteValue(SqlValue& value, ByteView source,
                                            SqlValueType type) noexcept;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_
