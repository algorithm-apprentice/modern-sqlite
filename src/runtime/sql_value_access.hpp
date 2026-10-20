#ifndef MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_
#define MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {

class SqlValueAccess {
 public:
  static constexpr std::size_t kOwnedStorageBytes = sizeof(SqlValue::ByteStorage);

  [[nodiscard]] static std::int64_t Integer(const SqlValue& value) noexcept;
  [[nodiscard]] static double Real(const SqlValue& value) noexcept;
  [[nodiscard]] static Utf8View Text(const SqlValue& value) noexcept;
  [[nodiscard]] static ByteView Blob(const SqlValue& value) noexcept;

  static void SetNull(SqlValue& value) noexcept;
  static void SetInteger(SqlValue& value, std::int64_t integer) noexcept;
  static void SetReal(SqlValue& value, double real) noexcept;
  static void Release(SqlValue& value) noexcept;
  [[nodiscard]] static Status CopyText(SqlValue& value, Utf8View source) noexcept;
  [[nodiscard]] static Status CopyBlob(SqlValue& value, ByteView source) noexcept;

  [[nodiscard]] static Result<ByteView> RetainBytes(ByteBuffer& storage, ByteView source,
                                                    ByteCount trailing_zeros = {}) noexcept try {
    if (trailing_zeros.value() > std::numeric_limits<std::size_t>::max() - source.size()) {
      return std::unexpected(
          Error::Create(ErrorCode::kTooLarge, "retained byte length exceeds the size limit"));
    }
    const std::size_t required = source.size() + trailing_zeros.value();
    ByteBuffer grown;
    ByteBuffer* destination = &storage;
    if (storage.size().value() < required) {
      grown = ByteBuffer{ByteCount{required}};
      destination = &grown;
    }
    if (!CopyBytes(destination->mutable_view(), source).has_value()) {
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "retained byte storage is too small"));
    }
    std::ranges::fill(destination->mutable_view().subspan(source.size(), trailing_zeros.value()),
                      std::byte{});
    if (destination == &grown) {
      storage = std::move(grown);
    }
    return storage.view().first(source.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }

 private:
  static constexpr ByteCount kTerminatorBytes{2};

  [[nodiscard]] static Status CopyByteValue(SqlValue& value, ByteView source,
                                            SqlValueType type) noexcept;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_SQL_VALUE_ACCESS_HPP_
