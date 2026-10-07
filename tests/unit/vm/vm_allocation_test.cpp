#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <new>

#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/vm/vm.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* allocation = nullptr;
  if (posix_memalign(&allocation, alignment, size == 0 ? alignment : size) == 0) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "btree_read" / "sqlite-3.54.0-btree-read.db";
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
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}

int main() try {
  using namespace modern_sqlite;

  PosixVfs vfs;
  auto opened = Pager::Open(vfs, FixturePath().string());
  if (!opened.has_value() || !(*opened)->BeginRead().has_value()) {
    return 1;
  }

  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{
      .schema_cookie = (*opened)->header()->schema_cookie(),
      .generation = 17,
  };
  input.register_count = 2;
  input.constants.push_back(SqlValue::Integer(1'000));
  input.constants.push_back(SqlValue::Integer(1));
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kInteger,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      BinaryInstruction{
          .operation = BinaryOperation::kSubtract,
          .left = RegisterId(0),
          .right = RegisterId(1),
          .output = RegisterId(0),
      },
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = RegisterId(0),
          .target = InstructionAddress(2),
      },
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
  };
  auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return 1;
  }
  auto created = Vm::Create(*program, VmEnvironment::Core());
  if (!created.has_value()) {
    return 1;
  }
  Vm& vm = *created;

  const std::size_t before = allocation_count.load(std::memory_order_relaxed);
  std::uint64_t checksum = 0;
  for (std::size_t iteration = 0; iteration < 10; ++iteration) {
    if (!vm.AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
      return 1;
    }
    const auto row = vm.Step();
    if (!row.has_value() || *row != VmStep::kRow || vm.row().size() != 1U) {
      return 1;
    }
    checksum += static_cast<std::uint64_t>(vm.row().front().integer_value().value_or(-1));
    const auto done = vm.Step();
    if (!done.has_value() || *done != VmStep::kDone || !vm.Reset().has_value()) {
      return 1;
    }
  }
  const std::size_t after = allocation_count.load(std::memory_order_relaxed);
  if (after != before || checksum != 0) {
    return 1;
  }

  ProgramInput rowid_list_input;
  rowid_list_input.schema_version = input.schema_version;
  rowid_list_input.register_count = 1;
  rowid_list_input.constants.push_back(SqlValue::Integer(1));
  rowid_list_input.instructions = {
      ClearRowIdListInstruction{},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      AppendRowIdListInstruction{.input = RegisterId(0)},
      HaltInstruction{},
  };
  auto rowid_list_program = BytecodeProgram::Create(rowid_list_input);
  if (!rowid_list_program.has_value()) {
    return 1;
  }
  auto rowid_list_vm = Vm::Create(*rowid_list_program, VmEnvironment::Core());
  if (!rowid_list_vm.has_value()) {
    return 1;
  }
  const std::size_t before_rowid_list = allocation_count.load(std::memory_order_relaxed);
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    if (!rowid_list_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
      return 1;
    }
    const auto done = rowid_list_vm->Step();
    if (!done.has_value() || *done != VmStep::kDone || !rowid_list_vm->Reset().has_value()) {
      return 1;
    }
  }
  const std::size_t after_rowid_list = allocation_count.load(std::memory_order_relaxed);

  if (!(*opened)->EndRead().has_value()) {
    return 1;
  }
  return after_rowid_list == before_rowid_list + 1U ? 0 : 1;
} catch (...) {
  return 1;
}
