#include "modern_sqlite/storage/database_format.hpp"

#include <gtest/gtest.h>

#include "modern_sqlite/base/result.hpp"

namespace modern_sqlite {
namespace {

TEST(DatabaseFormat, NormalizesLegacySchemaFormatZero) {
  EXPECT_EQ(DatabaseSchemaFormat::kOne, NormalizeSchemaFormat(0).value());
  EXPECT_EQ(DatabaseSchemaFormat::kOne, NormalizeSchemaFormat(1).value());
  EXPECT_EQ(DatabaseSchemaFormat::kFour, NormalizeSchemaFormat(4).value());

  const Result<DatabaseSchemaFormat> invalid = NormalizeSchemaFormat(5);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(ErrorCode::kNotDatabase, invalid.error().code());
}

TEST(DatabaseFormat, UsesTheLowEncodingBitsAndDefaultsZeroToUtf8) {
  EXPECT_EQ(DatabaseTextEncoding::kUtf8, NormalizeTextEncoding(0).value());
  EXPECT_EQ(DatabaseTextEncoding::kUtf8, NormalizeTextEncoding(1).value());
  EXPECT_EQ(DatabaseTextEncoding::kUtf16LittleEndian, NormalizeTextEncoding(2).value());
  EXPECT_EQ(DatabaseTextEncoding::kUtf16BigEndian, NormalizeTextEncoding(3).value());
  EXPECT_EQ(DatabaseTextEncoding::kUtf8, NormalizeTextEncoding(4).value());
  EXPECT_EQ(DatabaseTextEncoding::kUtf8, NormalizeTextEncoding(5).value());
}

}  // namespace
}  // namespace modern_sqlite
