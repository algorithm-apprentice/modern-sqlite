#include <cstddef>
#include <cstdint>
#include <span>

#include "read_fuzz.hpp"

#ifndef MODERN_SQLITE_READ_FUZZ_DATABASE
#error "MODERN_SQLITE_READ_FUZZ_DATABASE must name the read compatibility fixture"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  modern_sqlite::fuzz::RunReadSqlInput(std::span<const std::uint8_t>{data, size},
                                       MODERN_SQLITE_READ_FUZZ_DATABASE);
  return 0;
}
