#include <cstddef>
#include <cstdint>
#include <span>

#include "write_fuzz.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  modern_sqlite::fuzz::RunWriteDatabaseImageInput(std::span<const std::uint8_t>{data, size});
  return 0;
}
