#ifndef MODERN_SQLITE_BASE_RESULT_HPP_
#define MODERN_SQLITE_BASE_RESULT_HPP_

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace modern_sqlite {

enum class ErrorCode : std::int32_t {
  kGeneric = 1,
  kInternal = 2,
  kPermissionDenied = 3,
  kAborted = 4,
  kBusy = 5,
  kLocked = 6,
  kOutOfMemory = 7,
  kReadOnly = 8,
  kInterrupted = 9,
  kIo = 10,
  kCorruption = 11,
  kNotFound = 12,
  kFull = 13,
  kCannotOpen = 14,
  kProtocol = 15,
  kEmpty = 16,
  kSchemaChanged = 17,
  kTooLarge = 18,
  kConstraint = 19,
  kTypeMismatch = 20,
  kMisuse = 21,
  kNoLargeFileSupport = 22,
  kAuthorization = 23,
  kFormat = 24,
  kOutOfRange = 25,
  kNotDatabase = 26,
};

[[nodiscard]] constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kGeneric:
      return "generic";
    case ErrorCode::kInternal:
      return "internal";
    case ErrorCode::kPermissionDenied:
      return "permission_denied";
    case ErrorCode::kAborted:
      return "aborted";
    case ErrorCode::kBusy:
      return "busy";
    case ErrorCode::kLocked:
      return "locked";
    case ErrorCode::kOutOfMemory:
      return "out_of_memory";
    case ErrorCode::kReadOnly:
      return "read_only";
    case ErrorCode::kInterrupted:
      return "interrupted";
    case ErrorCode::kIo:
      return "io";
    case ErrorCode::kCorruption:
      return "corruption";
    case ErrorCode::kNotFound:
      return "not_found";
    case ErrorCode::kFull:
      return "full";
    case ErrorCode::kCannotOpen:
      return "cannot_open";
    case ErrorCode::kProtocol:
      return "protocol";
    case ErrorCode::kEmpty:
      return "empty";
    case ErrorCode::kSchemaChanged:
      return "schema_changed";
    case ErrorCode::kTooLarge:
      return "too_large";
    case ErrorCode::kConstraint:
      return "constraint";
    case ErrorCode::kTypeMismatch:
      return "type_mismatch";
    case ErrorCode::kMisuse:
      return "misuse";
    case ErrorCode::kNoLargeFileSupport:
      return "no_large_file_support";
    case ErrorCode::kAuthorization:
      return "authorization";
    case ErrorCode::kFormat:
      return "format";
    case ErrorCode::kOutOfRange:
      return "out_of_range";
    case ErrorCode::kNotDatabase:
      return "not_database";
  }
  return "unknown";
}

[[nodiscard]] constexpr int ToSqlitePrimaryCode(ErrorCode code) noexcept {
  return static_cast<int>(code);
}

enum class SqliteCodeMappingError {
  kNotAnError,
  kUnknownPrimaryCode,
};

class Error final {
 public:
  [[nodiscard]] static Error Create(
      ErrorCode code, std::string message,
      std::source_location location = std::source_location::current());
  [[nodiscard]] static Error OutOfMemory(
      std::source_location location = std::source_location::current()) noexcept;

  [[nodiscard]] static std::expected<Error, SqliteCodeMappingError> FromSqliteCode(
      int sqlite_code, std::string message,
      std::source_location location = std::source_location::current());

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] int primary_sqlite_code() const noexcept { return ToSqlitePrimaryCode(code_); }
  [[nodiscard]] int sqlite_code() const noexcept { return sqlite_code_; }
  [[nodiscard]] bool is_extended() const noexcept { return sqlite_code_ != primary_sqlite_code(); }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] const std::source_location& location() const noexcept { return location_; }
  [[nodiscard]] std::string ToString() const;

 private:
  Error(ErrorCode code, int sqlite_code, std::string message,
        std::source_location location) noexcept
      : code_(code), sqlite_code_(sqlite_code), message_(std::move(message)), location_(location) {}

  ErrorCode code_;
  int sqlite_code_;
  std::string message_;
  std::source_location location_;
};

template <typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BASE_RESULT_HPP_
