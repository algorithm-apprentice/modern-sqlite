#include <gtest/gtest.h>

#include <span>
#include <vector>
#include <version>

TEST(Toolchain, SupportsCpp23) {
#if defined(__cpp_lib_expected)
  EXPECT_GE(__cpp_lib_expected, 202202L);
#else
  FAIL() << "The standard library does not provide std::expected";
#endif
#if defined(_MSVC_LANG)
  EXPECT_EQ(__cplusplus, _MSVC_LANG);
  EXPECT_EQ(_MSVC_LANG, 202302L);
#endif
}

#if defined(_MSC_VER)
TEST(Toolchain, UsesQualifiedMsvcStandardLibraryAbi) {
  EXPECT_EQ(_ITERATOR_DEBUG_LEVEL, 0);
  EXPECT_EQ(_MSVC_STL_HARDENING, 1);
  EXPECT_EQ(_MSVC_STL_HARDENING_SPAN, 1);
  EXPECT_EQ(_MSVC_STL_HARDENING_VECTOR, 1);
}

TEST(Toolchain, EnforcesHardenedMsvcBounds) {
  EXPECT_DEATH(([] {
                 const int value = 7;
                 const std::span<const int> values{&value, 1};
                 static_cast<void>(values[1]);
               }()),
               "out of range");
  EXPECT_DEATH(([] {
                 const std::vector<int> values{7};
                 static_cast<void>(values[1]);
               }()),
               "out of range");
}
#endif
