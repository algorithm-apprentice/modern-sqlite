#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

bool fail_allocations = false;
std::size_t minimum_failing_size = 1;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations && size >= minimum_failing_size) {
    throw std::bad_alloc{};
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations && size >= minimum_failing_size) {
    throw std::bad_alloc{};
  }
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
  input.register_count = 1;
  input.parameter_count = 1;
  input.constants.push_back(SqlValue::Text(std::string(256, 'c')));
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kText,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
  };
  auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return 1;
  }

  fail_allocations = true;
  const auto create_failure = Vm::Create(*program, VmEnvironment::Core());
  fail_allocations = false;
  if (create_failure.has_value() || create_failure.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }

  auto created = Vm::Create(*program, VmEnvironment::Core());
  if (!created.has_value()) {
    return 1;
  }
  Vm& vm = *created;
  if (!vm.AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const SqlValue binding = SqlValue::Text(std::string(256, 'b'));

  fail_allocations = true;
  const Status bind_failure = vm.Bind(ParameterId(0), binding);
  fail_allocations = false;
  if (bind_failure.has_value() || bind_failure.error().code() != ErrorCode::kOutOfMemory ||
      vm.state() != VmState::kReady) {
    return 1;
  }

  fail_allocations = true;
  const auto step_failure = vm.Step();
  fail_allocations = false;
  if (step_failure.has_value() || step_failure.error().code() != ErrorCode::kOutOfMemory ||
      vm.state() != VmState::kError) {
    return 1;
  }

  auto movable = Vm::Create(*program, VmEnvironment::Core());
  if (!movable.has_value()) {
    return 1;
  }
  const Vm destination = std::move(*movable);
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  fail_allocations = true;
  const auto moved_step = movable->Step();
  fail_allocations = false;
  if (moved_step.has_value() || moved_step.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  fail_allocations = true;
  const Status moved_bind = movable->Bind(ParameterId(0), binding);
  fail_allocations = false;
  if (moved_bind.has_value() || moved_bind.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  if (destination.state() != VmState::kReady) {
    return 1;
  }

  ProgramInput branch_input;
  branch_input.schema_version = input.schema_version;
  branch_input.register_count = 1;
  branch_input.constants.push_back(SqlValue::Text("1" + std::string(127, 'x')));
  branch_input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kText,
  });
  branch_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      JumpIfInstruction{
          .condition = JumpCondition::kIfTrue,
          .input = RegisterId(0),
          .target = InstructionAddress(4),
      },
      HaltInstruction{},
      HaltInstruction{},
  };
  auto branch_program = BytecodeProgram::Create(branch_input);
  if (!branch_program.has_value()) {
    return 1;
  }
  auto branch_vm = Vm::Create(*branch_program, VmEnvironment::Core());
  if (!branch_vm.has_value() ||
      !branch_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value() ||
      !branch_vm->Step().has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto branch_done = branch_vm->Step();
  fail_allocations = false;
  if (!branch_done.has_value() || *branch_done != VmStep::kDone) {
    return 1;
  }

  ProgramInput bitwise_input;
  bitwise_input.schema_version = input.schema_version;
  bitwise_input.register_count = 2;
  bitwise_input.constants.push_back(SqlValue::Text("3" + std::string(127, 'x')));
  bitwise_input.constants.push_back(SqlValue::Integer(1));
  bitwise_input.result_columns = {
      ResultColumnMetadata{
          .name = "text",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kText,
      },
      ResultColumnMetadata{
          .name = "mask",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kInteger,
      },
  };
  bitwise_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      BinaryInstruction{
          .operation = BinaryOperation::kBitwiseAnd,
          .left = RegisterId(0),
          .right = RegisterId(1),
          .output = RegisterId(1),
      },
      HaltInstruction{},
  };
  auto bitwise_program = BytecodeProgram::Create(bitwise_input);
  if (!bitwise_program.has_value()) {
    return 1;
  }
  auto bitwise_vm = Vm::Create(*bitwise_program, VmEnvironment::Core());
  if (!bitwise_vm.has_value() ||
      !bitwise_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value() ||
      !bitwise_vm->Step().has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto bitwise_done = bitwise_vm->Step();
  fail_allocations = false;
  if (!bitwise_done.has_value() || *bitwise_done != VmStep::kDone) {
    return 1;
  }

  ProgramInput concatenate_input;
  concatenate_input.schema_version = input.schema_version;
  concatenate_input.register_count = 3;
  concatenate_input.constants.push_back(SqlValue::Text(std::string(100, 'a')));
  concatenate_input.constants.push_back(SqlValue::Text(std::string(100, 'b')));
  concatenate_input.result_columns = {
      ResultColumnMetadata{
          .name = "left",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kText,
      },
      ResultColumnMetadata{
          .name = "right",
          .declared_type = std::nullopt,
          .affinity = TypeAffinity::kText,
      },
  };
  concatenate_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      BinaryInstruction{
          .operation = BinaryOperation::kConcatenate,
          .left = RegisterId(0),
          .right = RegisterId(1),
          .output = RegisterId(2),
      },
      HaltInstruction{},
  };
  auto concatenate_program = BytecodeProgram::Create(concatenate_input);
  if (!concatenate_program.has_value()) {
    return 1;
  }
  auto concatenate_vm =
      Vm::Create(*concatenate_program, VmEnvironment::Core(), VmLimits{.maximum_value_bytes = 150});
  if (!concatenate_vm.has_value() ||
      !concatenate_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value() ||
      !concatenate_vm->Step().has_value()) {
    return 1;
  }
  minimum_failing_size = 64;
  fail_allocations = true;
  const auto concatenate_failure = concatenate_vm->Step();
  fail_allocations = false;
  minimum_failing_size = 1;
  if (concatenate_failure.has_value() ||
      concatenate_failure.error().code() != ErrorCode::kTooLarge) {
    return 1;
  }

  if (!(*opened)->EndRead().has_value()) {
    return 1;
  }

  test::WritePagerFixedVfs write_vfs{false};
  std::unique_ptr<Pager> write_pager = test::OpenWritePager(write_vfs, 64U);
  if (write_pager == nullptr) {
    return 1;
  }
  auto coordinator = TransactionCoordinator::Open(std::move(write_pager));
  if (!coordinator.has_value()) {
    return 1;
  }
  auto statement =
      coordinator->BeginStatement(TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!statement.has_value() || statement->writer() == nullptr ||
      !statement->writer()->InitializeDatabase().has_value()) {
    return 1;
  }
  ProgramInput insert_input;
  insert_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  insert_input.statement_kind = ProgramStatementKind::kInsert;
  insert_input.transaction_access = ProgramTransactionAccess::kWrite;
  insert_input.register_count = 4;
  insert_input.constants.emplace_back();
  insert_input.constants.push_back(SqlValue::Text("value"));
  insert_input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(1),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = false,
                  .rowid_alias = true,
                  .default_value = std::nullopt,
              },
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = true,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = 0,
  });
  insert_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(2)},
      OpenWriteCursorInstruction{.cursor = WriteCursorId(0)},
      ResolveInsertRowIdInstruction{
          .cursor = WriteCursorId(0),
          .input = RegisterId(0),
          .output = RegisterId(1),
      },
      BuildTableRecordInstruction{
          .cursor = WriteCursorId(0),
          .first_value = RegisterId(1),
          .value_count = 2,
          .output = RegisterId(3),
      },
      InsertTableInstruction{
          .cursor = WriteCursorId(0),
          .rowid = RegisterId(1),
          .record = RegisterId(3),
      },
      HaltInstruction{},
  };
  auto insert_program = BytecodeProgram::Create(insert_input);
  if (!insert_program.has_value()) {
    return 1;
  }
  auto insert_vm = Vm::Create(*insert_program, VmEnvironment::Core());
  if (!insert_vm.has_value() ||
      !insert_vm->AttachExecutionContext(VmExecutionContext{*statement->writer(), 0}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto insert_failure = insert_vm->Step();
  fail_allocations = false;
  if (insert_failure.has_value() || insert_failure.error().code() != ErrorCode::kOutOfMemory ||
      insert_vm->change_count() != 0U) {
    return 1;
  }
  if (!insert_vm->DetachExecutionContext().has_value() || !statement->Rollback().has_value()) {
    return 1;
  }

  test::WritePagerFixedVfs delete_vfs{false};
  std::unique_ptr<Pager> delete_pager = test::OpenWritePager(delete_vfs, 64U);
  if (delete_pager == nullptr) {
    return 1;
  }
  auto delete_coordinator = TransactionCoordinator::Open(std::move(delete_pager));
  if (!delete_coordinator.has_value()) {
    return 1;
  }
  auto initialize_statement = delete_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!initialize_statement.has_value() || initialize_statement->writer() == nullptr ||
      !initialize_statement->writer()->InitializeDatabase().has_value() ||
      !initialize_statement->Succeed().has_value()) {
    return 1;
  }
  auto seed_statement = delete_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!seed_statement.has_value() || seed_statement->writer() == nullptr) {
    return 1;
  }
  auto seed_vm = Vm::Create(*insert_program, VmEnvironment::Core());
  if (!seed_vm.has_value() ||
      !seed_vm->AttachExecutionContext(VmExecutionContext{*seed_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  const auto seeded = seed_vm->Step();
  if (!seeded.has_value() || *seeded != VmStep::kDone ||
      !seed_vm->DetachExecutionContext().has_value() || !seed_statement->Succeed().has_value()) {
    return 1;
  }

  ProgramInput delete_input;
  delete_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  delete_input.statement_kind = ProgramStatementKind::kDelete;
  delete_input.transaction_access = ProgramTransactionAccess::kWrite;
  delete_input.mutation_result.publishes_changes = true;
  delete_input.register_count = 1;
  delete_input.constants.push_back(SqlValue::Integer(1));
  delete_input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(1),
      .columns =
          {
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kInteger,
                  .not_null = false,
                  .rowid_alias = true,
                  .default_value = std::nullopt,
              },
              WriteColumnDescriptor{
                  .affinity = TypeAffinity::kText,
                  .not_null = true,
                  .rowid_alias = false,
                  .default_value = std::nullopt,
              },
          },
      .rowid_alias = 0,
  });
  delete_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      OpenWriteCursorInstruction{.cursor = WriteCursorId(0)},
      DeleteTableInstruction{.cursor = WriteCursorId(0), .rowid = RegisterId(0)},
      HaltInstruction{},
  };
  auto delete_program = BytecodeProgram::Create(delete_input);
  auto delete_statement = delete_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!delete_program.has_value() || !delete_statement.has_value() ||
      delete_statement->writer() == nullptr) {
    return 1;
  }
  auto delete_vm = Vm::Create(*delete_program, VmEnvironment::Core());
  if (!delete_vm.has_value() ||
      !delete_vm->AttachExecutionContext(VmExecutionContext{*delete_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto delete_failure = delete_vm->Step();
  fail_allocations = false;
  if (delete_failure.has_value() || delete_failure.error().code() != ErrorCode::kOutOfMemory ||
      delete_vm->change_count() != 0U) {
    return 1;
  }
  if (!delete_vm->DetachExecutionContext().has_value() ||
      !delete_statement->Rollback().has_value()) {
    return 1;
  }
  return 0;
} catch (...) {
  fail_allocations = false;
  return 1;
}
