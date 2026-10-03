#include "modern_sqlite/base/result.hpp"

#include <optional>
#include <utility>

namespace modern_sqlite {
namespace {

constexpr std::uint32_t kPrimaryCodeMask = 0xff;
constexpr int kSqliteOk = 0;
constexpr int kSqliteNotice = 27;
constexpr int kSqliteWarning = 28;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;

[[nodiscard]] constexpr int PrimaryCode(int sqlite_code) noexcept {
  const auto unsigned_code = static_cast<std::uint32_t>(sqlite_code);
  return static_cast<int>(unsigned_code & kPrimaryCodeMask);
}

[[nodiscard]] constexpr bool IsNonErrorCode(int primary_code) noexcept {
  return primary_code == kSqliteOk || primary_code == kSqliteNotice ||
         primary_code == kSqliteWarning || primary_code == kSqliteRow ||
         primary_code == kSqliteDone;
}

[[nodiscard]] constexpr std::optional<ErrorCode> ErrorCodeFromPrimary(int primary_code) noexcept {
  switch (primary_code) {
    case 1:
      return ErrorCode::kGeneric;
    case 2:
      return ErrorCode::kInternal;
    case 3:
      return ErrorCode::kPermissionDenied;
    case 4:
      return ErrorCode::kAborted;
    case 5:
      return ErrorCode::kBusy;
    case 6:
      return ErrorCode::kLocked;
    case 7:
      return ErrorCode::kOutOfMemory;
    case 8:
      return ErrorCode::kReadOnly;
    case 9:
      return ErrorCode::kInterrupted;
    case 10:
      return ErrorCode::kIo;
    case 11:
      return ErrorCode::kCorruption;
    case 12:
      return ErrorCode::kNotFound;
    case 13:
      return ErrorCode::kFull;
    case 14:
      return ErrorCode::kCannotOpen;
    case 15:
      return ErrorCode::kProtocol;
    case 16:
      return ErrorCode::kEmpty;
    case 17:
      return ErrorCode::kSchemaChanged;
    case 18:
      return ErrorCode::kTooLarge;
    case 19:
      return ErrorCode::kConstraint;
    case 20:
      return ErrorCode::kTypeMismatch;
    case 21:
      return ErrorCode::kMisuse;
    case 22:
      return ErrorCode::kNoLargeFileSupport;
    case 23:
      return ErrorCode::kAuthorization;
    case 24:
      return ErrorCode::kFormat;
    case 25:
      return ErrorCode::kOutOfRange;
    case 26:
      return ErrorCode::kNotDatabase;
    default:
      return std::nullopt;
  }
}

}  // namespace

Error Error::Create(ErrorCode code, std::string message, std::source_location location) {
  return Error{
      code,
      ToSqlitePrimaryCode(code),
      std::move(message),
      location,
  };
}

std::expected<Error, SqliteCodeMappingError> Error::FromSqliteCode(int sqlite_code,
                                                                   std::string message,
                                                                   std::source_location location) {
  if (sqlite_code < 0) {
    return std::unexpected(SqliteCodeMappingError::kUnknownPrimaryCode);
  }

  const int primary_code = PrimaryCode(sqlite_code);
  const std::optional<ErrorCode> code = ErrorCodeFromPrimary(primary_code);
  if (!code.has_value()) {
    const SqliteCodeMappingError mapping_error = IsNonErrorCode(primary_code)
                                                     ? SqliteCodeMappingError::kNotAnError
                                                     : SqliteCodeMappingError::kUnknownPrimaryCode;
    return std::unexpected(mapping_error);
  }
  return Error{*code, sqlite_code, std::move(message), location};
}

std::string Error::ToString() const {
  std::string text{ErrorCodeName(code_)};
  if (is_extended()) {
    text.push_back('[');
    text.append(std::to_string(sqlite_code_));
    text.push_back(']');
  }
  if (!message_.empty()) {
    text.append(": ");
    text.append(message_);
  }
  return text;
}

}  // namespace modern_sqlite
