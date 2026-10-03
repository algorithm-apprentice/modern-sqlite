#include "modern_sqlite/base/result.hpp"

#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace modern_sqlite {
namespace {

static_assert(std::same_as<Result<int>, std::expected<int, Error>>);
static_assert(std::same_as<Status, std::expected<void, Error>>);

constexpr int ExtendedSqliteCode(int primary_code, int extension) {
  return primary_code + (extension * 256);
}

constexpr std::array<std::pair<ErrorCode, int>, 26> kPrimaryCodes{{
    {ErrorCode::kGeneric, 1},
    {ErrorCode::kInternal, 2},
    {ErrorCode::kPermissionDenied, 3},
    {ErrorCode::kAborted, 4},
    {ErrorCode::kBusy, 5},
    {ErrorCode::kLocked, 6},
    {ErrorCode::kOutOfMemory, 7},
    {ErrorCode::kReadOnly, 8},
    {ErrorCode::kInterrupted, 9},
    {ErrorCode::kIo, 10},
    {ErrorCode::kCorruption, 11},
    {ErrorCode::kNotFound, 12},
    {ErrorCode::kFull, 13},
    {ErrorCode::kCannotOpen, 14},
    {ErrorCode::kProtocol, 15},
    {ErrorCode::kEmpty, 16},
    {ErrorCode::kSchemaChanged, 17},
    {ErrorCode::kTooLarge, 18},
    {ErrorCode::kConstraint, 19},
    {ErrorCode::kTypeMismatch, 20},
    {ErrorCode::kMisuse, 21},
    {ErrorCode::kNoLargeFileSupport, 22},
    {ErrorCode::kAuthorization, 23},
    {ErrorCode::kFormat, 24},
    {ErrorCode::kOutOfRange, 25},
    {ErrorCode::kNotDatabase, 26},
}};

Result<int> ReadValue(bool fail) {
  if (fail) {
    return std::unexpected(Error::Create(ErrorCode::kIo, "read failed"));
  }
  return 7;
}

Result<std::string> FormatValue(bool fail) {
  auto value = ReadValue(fail);
  if (!value.has_value()) {
    return std::unexpected(std::move(value.error()));
  }
  return std::to_string(*value);
}

TEST(Result, DefaultStatusIsSuccess) {
  const Status status;

  EXPECT_TRUE(status.has_value());
}

TEST(Result, StoresSuccessfulValue) {
  const Result<std::string> result = std::string{"value"};

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("value", *result);
}

TEST(Result, PropagatesTypedFailureWithoutDefaultValue) {
  const Result<std::string> result = FormatValue(true);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ErrorCode::kIo, result.error().code());
  EXPECT_EQ("read failed", result.error().message());
}

TEST(Result, PropagatesSuccessfulValue) {
  const Result<std::string> result = FormatValue(false);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("7", *result);
}

TEST(Error, PrimaryCodesMatchSQLite) {
  for (const auto& [code, sqlite_code] : kPrimaryCodes) {
    EXPECT_EQ(sqlite_code, ToSqlitePrimaryCode(code));
    EXPECT_NE("unknown", ErrorCodeName(code));
    const Error error = Error::Create(code, "");
    EXPECT_EQ(sqlite_code, error.sqlite_code());
    EXPECT_EQ(sqlite_code, error.primary_sqlite_code());
    EXPECT_FALSE(error.is_extended());

    const auto mapped = Error::FromSqliteCode(sqlite_code, "");
    ASSERT_TRUE(mapped.has_value()) << sqlite_code;
    EXPECT_EQ(code, mapped->code());
  }
}

TEST(Error, HasStableCodeNames) {
  EXPECT_EQ("generic", ErrorCodeName(ErrorCode::kGeneric));
  EXPECT_EQ("permission_denied", ErrorCodeName(ErrorCode::kPermissionDenied));
  EXPECT_EQ("out_of_memory", ErrorCodeName(ErrorCode::kOutOfMemory));
  EXPECT_EQ("io", ErrorCodeName(ErrorCode::kIo));
  EXPECT_EQ("schema_changed", ErrorCodeName(ErrorCode::kSchemaChanged));
  EXPECT_EQ("no_large_file_support", ErrorCodeName(ErrorCode::kNoLargeFileSupport));
  EXPECT_EQ("not_database", ErrorCodeName(ErrorCode::kNotDatabase));
}

TEST(Error, StoresMessageAndFormatsDiagnostics) {
  const Error error = Error::Create(ErrorCode::kCorruption, "malformed page");

  EXPECT_EQ(ErrorCode::kCorruption, error.code());
  EXPECT_EQ("malformed page", error.message());
  EXPECT_EQ("corruption: malformed page", error.ToString());
}

TEST(Error, CapturesFactoryCallSite) {
  const std::uint_least32_t expected_line = __LINE__ + 1;
  const Error error = Error::Create(ErrorCode::kInternal, "broken invariant");

  EXPECT_EQ(expected_line, error.location().line());
  EXPECT_NE(std::string_view{error.location().file_name()}.find("result_test.cpp"),
            std::string_view::npos);
}

TEST(Error, PreservesExtendedIoCode) {
  constexpr int kSqliteIoErrorRead = ExtendedSqliteCode(10, 1);

  const auto result = Error::FromSqliteCode(kSqliteIoErrorRead, "read failed");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(ErrorCode::kIo, result->code());
  EXPECT_EQ(10, result->primary_sqlite_code());
  EXPECT_EQ(kSqliteIoErrorRead, result->sqlite_code());
  EXPECT_TRUE(result->is_extended());
  EXPECT_EQ("io[266]: read failed", result->ToString());
}

TEST(Error, PreservesExtendedConstraintCode) {
  constexpr int kSqliteConstraintUnique = ExtendedSqliteCode(19, 8);

  const auto result = Error::FromSqliteCode(kSqliteConstraintUnique, "duplicate key");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(ErrorCode::kConstraint, result->code());
  EXPECT_EQ(kSqliteConstraintUnique, result->sqlite_code());
}

TEST(Error, RejectsNonErrorSQLiteCodes) {
  constexpr std::array<int, 7> kNonErrors{
      0, ExtendedSqliteCode(0, 1), 27, ExtendedSqliteCode(27, 1), 28, 100, 101,
  };

  for (const int sqlite_code : kNonErrors) {
    const auto result = Error::FromSqliteCode(sqlite_code, "not an error");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(SqliteCodeMappingError::kNotAnError, result.error());
  }
}

TEST(Error, RejectsUnknownSQLitePrimaryCode) {
  constexpr std::array<int, 3> kUnknownCodes{-1, -246, 255};

  for (const int sqlite_code : kUnknownCodes) {
    const auto result = Error::FromSqliteCode(sqlite_code, "unknown");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(SqliteCodeMappingError::kUnknownPrimaryCode, result.error());
  }
}

}  // namespace
}  // namespace modern_sqlite
