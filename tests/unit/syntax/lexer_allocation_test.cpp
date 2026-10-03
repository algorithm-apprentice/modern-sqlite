#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

#include "modern_sqlite/syntax/lexer.hpp"
#include "modern_sqlite/text/text.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

int main() {
  constexpr std::string_view kSql =
      "SELECT alpha, x'0011', 1_000, ?1 FROM sample "
      "WHERE value >= 1.25e+3 /* pinned lexer allocation test */;";
  const std::size_t before = allocation_count.load(std::memory_order_relaxed);
  std::uint64_t checksum = 0;

  for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
    modern_sqlite::Lexer lexer{modern_sqlite::Utf8View{kSql}};
    while (true) {
      const modern_sqlite::Token token = lexer.Next();
      checksum += static_cast<std::uint64_t>(token.span.length().value()) +
                  static_cast<std::uint64_t>(token.kind);
      if (token.kind == modern_sqlite::TokenKind::kEndOfInput) {
        break;
      }
    }
  }

  const std::size_t after = allocation_count.load(std::memory_order_relaxed);
  return after == before && checksum != 0 ? 0 : 1;
}
