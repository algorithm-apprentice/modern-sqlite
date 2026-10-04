#include <cstdlib>
#include <new>
#include <string>
#include <variant>

#include "modern_sqlite/bytecode/program.hpp"

namespace {

bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }

int main() try {
  using namespace modern_sqlite;

  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{.schema_cookie = 1, .generation = 2};
  input.register_count = 1;
  input.constants.push_back(SqlValue::Text("payload"));
  input.symbols.emplace_back("BINARY");
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = "TEXT",
      .affinity = TypeAffinity::kText,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
  };

  auto created = BytecodeProgram::Create(input);
  if (!created.has_value()) {
    return 1;
  }
  const BytecodeProgram& program = *created;

  std::size_t checksum = 0;
  fail_allocations = true;
  for (const auto& instruction : program.instructions()) {
    checksum += static_cast<std::size_t>(InstructionKindOf(instruction));
    checksum += InstructionKindName(InstructionKindOf(instruction)).size();
    std::visit([&checksum](const auto&) { ++checksum; }, instruction);
  }
  for (const auto& constant : program.constants()) {
    checksum += static_cast<std::size_t>(constant.type());
  }
  for (const auto& symbol : program.symbols()) {
    checksum += symbol.size();
  }
  for (const auto& column : program.result_columns()) {
    checksum += column.name.size();
    checksum +=
        column.declared_type.transform([](const std::string& value) { return value.size(); })
            .value_or(0);
  }
  checksum += program.constant(ConstantId(0)).text_value().value_or(Utf8View{}).size_bytes();
  fail_allocations = false;

  return checksum == 0 ? 1 : 0;
} catch (...) {
  return 1;
}
