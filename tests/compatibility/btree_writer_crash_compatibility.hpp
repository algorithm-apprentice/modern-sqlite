#ifndef MODERN_SQLITE_TESTS_COMPATIBILITY_BTREE_WRITER_CRASH_COMPATIBILITY_HPP_
#define MODERN_SQLITE_TESTS_COMPATIBILITY_BTREE_WRITER_CRASH_COMPATIBILITY_HPP_

#include <cstddef>
#include <filesystem>

namespace modern_sqlite::test {

inline constexpr std::size_t kBtreeCompatibilityWorkerCount = 8;

void RunBtreeWriterCrashCompatibility(const std::filesystem::path& directory,
                                      std::size_t worker = 0, std::size_t worker_count = 1);

}  // namespace modern_sqlite::test

#endif  // MODERN_SQLITE_TESTS_COMPATIBILITY_BTREE_WRITER_CRASH_COMPATIBILITY_HPP_
