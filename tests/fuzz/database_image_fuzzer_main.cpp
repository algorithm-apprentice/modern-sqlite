#include <cstddef>
#include <cstdint>
#include <span>

#include "read_fuzz.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  modern_sqlite::fuzz::RunDatabaseImageInput(std::span<const std::uint8_t>{data, size});
  return 0;
}
