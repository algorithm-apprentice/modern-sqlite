#ifndef MODERN_SQLITE_TESTS_FUZZ_READ_FUZZ_HPP_
#define MODERN_SQLITE_TESTS_FUZZ_READ_FUZZ_HPP_

#include <cstdint>
#include <span>
#include <string_view>

namespace modern_sqlite::fuzz {

void RunReadSqlInput(std::span<const std::uint8_t> input, std::string_view database_path);
void RunDatabaseImageInput(std::span<const std::uint8_t> input);

}  // namespace modern_sqlite::fuzz

#endif  // MODERN_SQLITE_TESTS_FUZZ_READ_FUZZ_HPP_
