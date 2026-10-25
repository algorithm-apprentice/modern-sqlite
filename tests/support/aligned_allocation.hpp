#ifndef MODERN_SQLITE_TESTS_SUPPORT_ALIGNED_ALLOCATION_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_ALIGNED_ALLOCATION_HPP_

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace modern_sqlite::test_support {

[[nodiscard]] inline int AllocateAligned(void** memory, std::size_t alignment,
                                         std::size_t size) noexcept {
#if defined(_WIN32)
  if (alignment < sizeof(void*) || !std::has_single_bit(alignment)) {
    return EINVAL;
  }
  *memory = ::_aligned_malloc(size, alignment);
  return *memory != nullptr ? 0 : ENOMEM;
#else
  return ::posix_memalign(memory, alignment, size);
#endif
}

inline void FreeAligned(void* memory) noexcept {
#if defined(_WIN32)
  ::_aligned_free(memory);
#else
  std::free(memory);
#endif
}

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TESTS_SUPPORT_ALIGNED_ALLOCATION_HPP_
