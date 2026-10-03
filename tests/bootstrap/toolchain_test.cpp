#include <gtest/gtest.h>

#include <version>

TEST(Toolchain, SupportsCpp23) {
#if defined(__cpp_lib_expected)
  EXPECT_GE(__cpp_lib_expected, 202202L);
#else
  FAIL() << "The standard library does not provide std::expected";
#endif
}
