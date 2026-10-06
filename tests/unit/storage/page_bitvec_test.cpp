#include "modern_sqlite/storage/page_bitvec.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

TEST(PageBitvec, StoresSmallBitmapBitsAndRejectsInvalidPages) {
  PageBitvec bits = TakeValue(PageBitvec::Create(128U));

  EXPECT_FALSE(bits.Test(PageNumber{1}));
  EXPECT_FALSE(bits.Test(PageNumber{128}));
  EXPECT_TRUE(bits.Set(PageNumber{1}).has_value());
  EXPECT_TRUE(bits.Set(PageNumber{64}).has_value());
  EXPECT_TRUE(bits.Set(PageNumber{128}).has_value());
  EXPECT_TRUE(bits.Set(PageNumber{64}).has_value());
  EXPECT_TRUE(bits.Test(PageNumber{1}));
  EXPECT_TRUE(bits.Test(PageNumber{64}));
  EXPECT_TRUE(bits.Test(PageNumber{128}));

  const auto zero = bits.Set(PageNumber{});
  const auto beyond = bits.Set(PageNumber{129});
  ASSERT_FALSE(zero.has_value());
  ASSERT_FALSE(beyond.has_value());
  EXPECT_EQ(ErrorCode::kOutOfRange, zero.error().code());
  EXPECT_EQ(ErrorCode::kOutOfRange, beyond.error().code());
}

TEST(PageBitvec, SubdividesSparseHashNodesWithoutLosingBits) {
  constexpr std::uint32_t kPageCount = 1'000'000U;
  PageBitvec bits = TakeValue(PageBitvec::Create(kPageCount));

  for (std::uint32_t index = 0; index < 90U; ++index) {
    const PageNumber page{1U + index * 124U};
    ASSERT_TRUE(bits.Set(page).has_value()) << page.value();
  }
  for (std::uint32_t index = 0; index < 90U; ++index) {
    EXPECT_TRUE(bits.Test(PageNumber{1U + index * 124U})) << index;
  }
  EXPECT_FALSE(bits.Test(PageNumber{2}));
  EXPECT_FALSE(bits.Test(PageNumber{kPageCount + 1U}));
}

}  // namespace
}  // namespace modern_sqlite
