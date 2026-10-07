#ifndef MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_HPP_
#define MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_HPP_

#include <cstdint>
#include <span>

namespace modern_sqlite::fuzz {

void RunWriteSqlInput(std::span<const std::uint8_t> input);

}  // namespace modern_sqlite::fuzz

#endif  // MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_HPP_
