#ifndef MODERN_SQLITE_TESTS_COMPATIBILITY_WRITE_SESSION_CRASH_HARNESS_HPP_
#define MODERN_SQLITE_TESTS_COMPATIBILITY_WRITE_SESSION_CRASH_HARNESS_HPP_

#include <cstddef>
#include <optional>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"

namespace modern_sqlite::test {

struct WriteSessionCrashVerification {
  void* context = nullptr;
  void (*verify)(void* context, std::string_view scenario, std::size_t cut, bool writes_are_durable,
                 bool terminal, ByteView image) = nullptr;
  std::string_view scenario_filter{};
  std::optional<std::size_t> cut_filter{};
  std::optional<bool> durability_filter{};
  std::string_view executable{};
};

void RunWriteSessionCrashHarness();
void RunWriteSessionCrashHarness(WriteSessionCrashVerification verification);
void RunWriteSessionTransactionCrashHarness();
void RunWriteSessionTransactionCrashHarness(WriteSessionCrashVerification verification);

}  // namespace modern_sqlite::test

#endif  // MODERN_SQLITE_TESTS_COMPATIBILITY_WRITE_SESSION_CRASH_HARNESS_HPP_
