#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/storage/btree/database.hpp"
#include "modern_sqlite/storage/btree/writer.hpp"
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"
#include "tests/unit/storage/btree/database_test_support.hpp"

namespace {

bool fail_allocations = false;
std::size_t minimum_failing_size = 1;
std::size_t allocation_index = 0;
std::optional<std::size_t> failing_allocation{};
modern_sqlite::Vm* callback_vm = nullptr;

constexpr std::size_t kNestedCallbackTextBytes = 256;

[[nodiscard]] bool ShouldFailAllocation(std::size_t size) {
  const std::size_t index = allocation_index++;
  return (fail_allocations && size >= minimum_failing_size) || failing_allocation == index;
}

[[nodiscard]] void* Allocate(std::size_t size) {
  if (ShouldFailAllocation(size)) {
    throw std::bad_alloc{};
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (ShouldFailAllocation(size)) {
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

[[nodiscard]] bool SweepVmCreation(const modern_sqlite::BytecodeProgram& program) {
  allocation_index = 0;
  const auto baseline = modern_sqlite::Vm::Create(program, modern_sqlite::VmEnvironment::Core());
  const std::size_t allocations = allocation_index;
  if (!baseline.has_value() || allocations == 0U || allocations > 128U) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    allocation_index = 0;
    failing_allocation = failure;
    const auto rejected = modern_sqlite::Vm::Create(program, modern_sqlite::VmEnvironment::Core());
    failing_allocation.reset();
    if (rejected.has_value() || rejected.error().code() != modern_sqlite::ErrorCode::kOutOfMemory) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> NestedStepDiagnosticOom(
    const modern_sqlite::ScalarFunctionContext&,
    std::span<const modern_sqlite::SqlValue> arguments) {
  using namespace modern_sqlite;
  if (callback_vm == nullptr || arguments.size() != 1U) {
    return std::unexpected(Error::Create(ErrorCode::kInternal, "nested Step probe is incomplete"));
  }
  fail_allocations = true;
  const auto rejected = callback_vm->Step();
  fail_allocations = false;
  const auto preserved = arguments[0].text_value();
  const bool correct = !rejected.has_value() &&
                       rejected.error().code() == ErrorCode::kOutOfMemory &&
                       callback_vm->state() == VmState::Running && preserved.has_value() &&
                       preserved->size_bytes() == kNestedCallbackTextBytes;
  return SqlValue::Integer(correct ? 1 : 0);
}

[[nodiscard]] bool CheckNestedStepDiagnosticOom(modern_sqlite::BtreeDatabase& database,
                                                modern_sqlite::SchemaVersionRequirement schema) {
  using namespace modern_sqlite;
  const std::array functions{
      ScalarFunction{"nested_step_oom", FunctionArity::Exact(1),
                     FunctionDeterminism::kDeterministic, FunctionCollationUse::kNone,
                     NestedStepDiagnosticOom},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  ProgramInput input;
  input.schema_version = schema;
  input.register_count = 2;
  input.constants.push_back(SqlValue::Text(std::string(kNestedCallbackTextBytes, 'x')));
  input.symbols = {"nested_step_oom", "BINARY"};
  input.result_columns = {
      ResultColumnMetadata{.name = "value", .declared_type = {}, .affinity = TypeAffinity::kText},
      ResultColumnMetadata{
          .name = "preserved", .declared_type = {}, .affinity = TypeAffinity::kInteger},
  };
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      CallScalarInstruction{.function = SymbolId(0),
                            .collation = SymbolId(1),
                            .first_argument = RegisterId(0),
                            .argument_count = 1,
                            .output = RegisterId(1)},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      HaltInstruction{},
  };
  const auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  auto vm = Vm::Create(*program, VmEnvironment{registry, collations});
  if (!vm.has_value() ||
      !vm->AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
    return false;
  }
  callback_vm = &*vm;
  const auto row = vm->Step();
  callback_vm = nullptr;
  if (!row.has_value() || *row != VmStep::kRow || vm->row().size() != 2U ||
      vm->row()[1].integer_value() != 1 ||
      vm->row()[0].text_value().value_or(Utf8View{}).size_bytes() != kNestedCallbackTextBytes) {
    std::fprintf(stderr, "nested Step diagnostic OOM changed its outer execution\n");
    return false;
  }
  const auto done = vm->Step();
  return done.has_value() && *done == VmStep::kDone;
}

[[nodiscard]] bool CheckLargeColumnAllocationFailures(bool metadata_only = false) {
  using namespace modern_sqlite;
  constexpr std::size_t kBytes = 5'001U;
  test::WritePagerFixedVfs vfs{false};
  auto database = test::OpenWriteDatabase<BtreeDatabase>(vfs, 64U);
  if (database == nullptr || !database->pager().BeginRead().has_value() ||
      !database->pager().BeginWrite().has_value()) {
    return false;
  }
  auto session = BtreeWriteSession::Open(*database);
  if (!session.has_value() || !session->InitializeDatabase().has_value()) {
    return false;
  }
  auto table = session->CreateTableBtree();
  if (!table.has_value()) {
    return false;
  }
  const std::array values{SqlValue::Blob(ByteBuffer{ByteCount{kBytes}}),
                          SqlValue::Blob(ByteBuffer{ByteCount{kBytes}})};
  const auto record = EncodeRecord(values);
  if (!record.has_value() || !table->Insert(1, record->view()).has_value()) {
    return false;
  }
  ProgramInput input;
  const auto* header = database->pager().header();
  input.schema_version = SchemaVersionRequirement{
      .schema_cookie = header == nullptr ? 0U : header->schema_cookie(), .generation = 0};
  input.register_count = metadata_only ? 3U : 2U;
  if (metadata_only) {
    input.symbols = {"typeof", "length"};
  }
  input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(table->root_page().value()),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 2,
      .fields =
          {
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 0},
              CursorFieldSource{.kind = CursorFieldSourceKind::kRecordField, .record_field = 1},
          },
      .index_columns = {},
  });
  input.result_columns = {
      ResultColumnMetadata{.name = "first", .declared_type = {}, .affinity = TypeAffinity::kBlob},
      ResultColumnMetadata{.name = "second", .declared_type = {}, .affinity = TypeAffinity::kBlob},
  };
  input.instructions = {
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      RewindInstruction{.cursor = CursorId(0), .empty_target = InstructionAddress(7)},
      ReadFieldInstruction{
          .cursor = CursorId(0), .field = CursorFieldId(0), .output = RegisterId(0)},
      ReadFieldInstruction{
          .cursor = CursorId(0), .field = CursorFieldId(1), .output = RegisterId(1)},
      CloseCursorInstruction{.cursor = CursorId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
  };
  if (metadata_only) {
    input.instructions[2] = CallColumnScalarInstruction{
        .function = SymbolId{0},
        .cursor = CursorId{0},
        .field = CursorFieldId{0},
        .argument = RegisterId{2},
        .output = RegisterId{0},
    };
    input.instructions[3] = CallColumnScalarInstruction{
        .function = SymbolId{1},
        .cursor = CursorId{0},
        .field = CursorFieldId{1},
        .argument = RegisterId{2},
        .output = RegisterId{1},
    };
  }
  const auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  const auto create = [&]() -> Result<Vm> {
    auto vm = Vm::Create(*program, VmEnvironment::Core());
    if (!vm.has_value()) {
      return std::unexpected(std::move(vm.error()));
    }
    auto attached = vm->AttachExecutionContext(VmExecutionContext{*database, 0});
    if (!attached.has_value()) {
      return std::unexpected(std::move(attached.error()));
    }
    return vm;
  };
  auto baseline = create();
  if (!baseline.has_value()) {
    return false;
  }
  const auto pins = database->pager().cache_pin_count();
  allocation_index = 0;
  const auto row = baseline->Step();
  const auto allocations = allocation_index;
  if (!row.has_value() || *row != VmStep::kRow || allocations == 0U || allocations > 128U ||
      database->pager().cache_pin_count() != pins) {
    return false;
  }
  const auto done = baseline->Step();
  if (!done.has_value() || *done != VmStep::kDone) {
    return false;
  }
  for (std::size_t failure = 0; failure < allocations; ++failure) {
    auto vm = create();
    if (!vm.has_value()) {
      return false;
    }
    allocation_index = 0;
    failing_allocation = failure;
    const auto rejected = vm->Step();
    failing_allocation.reset();
    if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
        vm->state() != VmState::kError || !vm->row().empty() ||
        database->pager().cache_pin_count() != pins || !vm->Reset().has_value() ||
        !vm->AttachExecutionContext(VmExecutionContext{*database, 0}).has_value()) {
      return false;
    }
    const auto retried = vm->Step();
    if (!retried.has_value() || *retried != VmStep::kRow || vm->row().size() != 2U) {
      return false;
    }
    if (metadata_only) {
      if (vm->row()[0].text_value().value_or(Utf8View{}).bytes() != "blob" ||
          vm->row()[1].integer_value() != static_cast<std::int64_t>(kBytes)) {
        return false;
      }
    } else if (vm->row()[0].blob_value().value_or(ByteView{}).size() != kBytes ||
               vm->row()[1].blob_value().value_or(ByteView{}).size() != kBytes) {
      return false;
    }
    const auto completed = vm->Step();
    if (!completed.has_value() || *completed != VmStep::kDone) {
      return false;
    }
  }
  return database->pager().Rollback().has_value();
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
  if (!CheckLargeColumnAllocationFailures() || !CheckLargeColumnAllocationFailures(true)) {
    std::fprintf(stderr, "large column allocation/cleanup control failed\n");
    return 1;
  }

  PosixVfs vfs;
  auto opened = BtreeDatabase::Open(vfs, FixturePath().string());
  if (!opened.has_value() || !(*opened)->pager().BeginRead().has_value()) {
    return 1;
  }

  ProgramInput input;
  input.schema_version = SchemaVersionRequirement{
      .schema_cookie = (*opened)->pager().header()->schema_cookie(),
      .generation = 17,
  };
  if (!CheckNestedStepDiagnosticOom(**opened, input.schema_version)) {
    return 1;
  }
  input.register_count = 1;
  input.parameter_count = 1;
  input.symbols = {"lower", "BINARY"};
  input.constants.push_back(SqlValue::Text(std::string(256, 'c')));
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kText,
  });
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      CallScalarInstruction{.function = SymbolId(0),
                            .collation = SymbolId(1),
                            .first_argument = RegisterId(0),
                            .argument_count = 1,
                            .output = RegisterId(0)},
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

  ProgramInput subroutine_input;
  subroutine_input.schema_version = input.schema_version;
  subroutine_input.register_count = 1;
  subroutine_input.symbols = {"lower", "BINARY"};
  subroutine_input.constants.push_back(SqlValue::Text(std::string(256, 's')));
  subroutine_input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kText,
  });
  subroutine_input.subroutines.push_back(
      SubroutineDescriptor{.entry = InstructionAddress(3), .end = InstructionAddress(6)});
  subroutine_input.instructions = {
      CallSubroutineInstruction{.subroutine = SubroutineId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      CallScalarInstruction{.function = SymbolId(0),
                            .collation = SymbolId(1),
                            .first_argument = RegisterId(0),
                            .argument_count = 1,
                            .output = RegisterId(0)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId(0)},
  };
  const auto subroutine_program = BytecodeProgram::Create(subroutine_input);
  if (!subroutine_program.has_value()) {
    return 1;
  }
  if (!SweepVmCreation(*subroutine_program)) {
    return 1;
  }
  auto subroutine_vm = Vm::Create(*subroutine_program, VmEnvironment::Core());
  if (!subroutine_vm.has_value() ||
      !subroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto subroutine_failure = subroutine_vm->Step();
  fail_allocations = false;
  if (subroutine_failure.has_value() ||
      subroutine_failure.error().code() != ErrorCode::kOutOfMemory ||
      subroutine_vm->state() != VmState::kError || !subroutine_vm->Reset().has_value() ||
      !subroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto recovered_subroutine = subroutine_vm->Step();
  if (!recovered_subroutine.has_value() || *recovered_subroutine != VmStep::kRow ||
      subroutine_vm->row().size() != 1U ||
      subroutine_vm->row()[0].text_value().value_or(Utf8View{}).size_bytes() != 256U) {
    return 1;
  }
  const auto finished_subroutine = subroutine_vm->Step();
  if (!finished_subroutine.has_value() || *finished_subroutine != VmStep::kDone) {
    return 1;
  }

  ProgramInput read_subroutine_input;
  read_subroutine_input.schema_version = input.schema_version;
  read_subroutine_input.register_count = 1;
  read_subroutine_input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 1,
      .fields = {},
      .index_columns = {},
  });
  read_subroutine_input.result_columns.push_back(ResultColumnMetadata{
      .name = "rowid",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kInteger,
  });
  read_subroutine_input.subroutines.push_back(SubroutineDescriptor{
      .entry = InstructionAddress(3),
      .end = InstructionAddress(8),
      .kind = SubroutineKind::kRead,
  });
  read_subroutine_input.instructions = {
      CallSubroutineInstruction{.subroutine = SubroutineId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      NullRowInstruction{.cursor = CursorId(0)},
      ReadRowIdInstruction{.cursor = CursorId(0), .output = RegisterId(0)},
      CloseCursorInstruction{.cursor = CursorId(0)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId(0)},
  };
  const auto read_subroutine_program = BytecodeProgram::Create(read_subroutine_input);
  if (!read_subroutine_program.has_value() || !SweepVmCreation(*read_subroutine_program)) {
    return 1;
  }
  auto read_subroutine_vm = Vm::Create(*read_subroutine_program, VmEnvironment::Core());
  if (!read_subroutine_vm.has_value() ||
      !read_subroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto read_failure = read_subroutine_vm->Step();
  fail_allocations = false;
  if (read_failure.has_value() || read_failure.error().code() != ErrorCode::kOutOfMemory ||
      read_subroutine_vm->state() != VmState::kError || !read_subroutine_vm->Reset().has_value() ||
      !read_subroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto read_recovery = read_subroutine_vm->Step();
  if (!read_recovery.has_value() || *read_recovery != VmStep::kRow ||
      read_subroutine_vm->row().size() != 1U ||
      read_subroutine_vm->row()[0].type() != SqlValueType::kNull) {
    return 1;
  }
  const auto read_finished = read_subroutine_vm->Step();
  if (!read_finished.has_value() || *read_finished != VmStep::kDone) {
    return 1;
  }

  ProgramInput null_input;
  null_input.schema_version = input.schema_version;
  null_input.register_count = 1;
  null_input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 1,
      .fields = {CursorFieldSource{.kind = CursorFieldSourceKind::kRowId, .record_field = 0}},
      .index_columns = {},
  });
  null_input.result_columns.push_back(ResultColumnMetadata{
      .name = "rowid",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kInteger,
  });
  null_input.instructions = {
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      RewindInstruction{.cursor = CursorId(0), .empty_target = InstructionAddress(10)},
      ReadRowIdInstruction{.cursor = CursorId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      NullRowInstruction{.cursor = CursorId(0)},
      ReadRowIdInstruction{.cursor = CursorId(0), .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      NextInstruction{.cursor = CursorId(0), .next_target = InstructionAddress(7)},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
  };
  auto null_program = BytecodeProgram::Create(null_input);
  if (!null_program.has_value()) {
    return 1;
  }
  auto null_vm = Vm::Create(*null_program, VmEnvironment::Core());
  if (!null_vm.has_value() ||
      !null_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto physical = null_vm->Step();
  if (!physical.has_value() || *physical != VmStep::kRow ||
      null_vm->row()[0].type() != SqlValueType::kInteger) {
    return 1;
  }
  fail_allocations = true;
  const auto virtual_row = null_vm->Step();
  const bool virtual_null = virtual_row.has_value() && *virtual_row == VmStep::kRow &&
                            null_vm->row()[0].type() == SqlValueType::kNull;
  if (!virtual_null) {
    fail_allocations = false;
    return 1;
  }
  const auto virtual_done = null_vm->Step();
  fail_allocations = false;
  if (!virtual_done.has_value() || *virtual_done != VmStep::kDone) {
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

  ProgramInput aggregate_input;
  aggregate_input.schema_version = input.schema_version;
  aggregate_input.register_count = 1;
  aggregate_input.symbols = {"sum", "BINARY"};
  aggregate_input.constants.push_back(SqlValue::Integer(1));
  aggregate_input.aggregates.push_back(AggregateDescriptor{
      .function = SymbolId(0),
      .collation = SymbolId(1),
      .argument_count = 1,
  });
  aggregate_input.instructions = {
      OpenAggregateInstruction{.aggregate = AggregateId(0)},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      StepAggregateInstruction{
          .aggregate = AggregateId(0),
          .first_argument = RegisterId(0),
      },
      FinalizeAggregateInstruction{.aggregate = AggregateId(0), .output = RegisterId(0)},
      CloseAggregateInstruction{.aggregate = AggregateId(0)},
      HaltInstruction{},
  };
  auto aggregate_program = BytecodeProgram::Create(aggregate_input);
  if (!aggregate_program.has_value()) {
    return 1;
  }
  auto aggregate_vm = Vm::Create(*aggregate_program, VmEnvironment::Core());
  if (!aggregate_vm.has_value() ||
      !aggregate_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto aggregate_failure = aggregate_vm->Step();
  fail_allocations = false;
  if (aggregate_failure.has_value() ||
      aggregate_failure.error().code() != ErrorCode::kOutOfMemory ||
      aggregate_vm->state() != VmState::kError || !aggregate_vm->Reset().has_value() ||
      !aggregate_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto aggregate_recovered = aggregate_vm->Step();
  if (!aggregate_recovered.has_value() || *aggregate_recovered != VmStep::kDone) {
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

  ProgramInput rowid_list_input;
  rowid_list_input.schema_version = SchemaVersionRequirement{
      .schema_cookie = (*opened)->pager().header()->schema_cookie(),
      .generation = 0,
  };
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
  if (!rowid_list_vm.has_value() ||
      !rowid_list_vm->AttachExecutionContext(VmExecutionContext{**opened, 0}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto rowid_list_failure = rowid_list_vm->Step();
  fail_allocations = false;
  if (rowid_list_failure.has_value() ||
      rowid_list_failure.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  if (!rowid_list_vm->DetachExecutionContext().has_value()) {
    return 1;
  }

  ProgramInput sorter_input;
  sorter_input.schema_version = input.schema_version;
  sorter_input.register_count = 2;
  sorter_input.constants.push_back(SqlValue::Integer(1));
  sorter_input.constants.push_back(SqlValue::Text(std::string(256, 's')));
  sorter_input.symbols.emplace_back("BINARY");
  sorter_input.sorters.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  });
  sorter_input.instructions = {
      OpenSorterInstruction{.sorter = SorterId(0)},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      InsertSorterInstruction{
          .sorter = SorterId(0),
          .first_value = RegisterId(0),
          .value_count = 2,
      },
      HaltInstruction{},
  };
  auto sorter_program = BytecodeProgram::Create(sorter_input);
  if (!sorter_program.has_value()) {
    return 1;
  }
  auto sorter_factory_result =
      TemporaryStorageFactory::Create(vfs, (*opened)->pager(),
                                      TemporaryStorageOptions{
                                          .mode = TemporaryStoreMode::kMemory,
                                          .sorter_memory_threshold = ByteCount{1U << 20U},
                                      });
  if (!sorter_factory_result.has_value()) {
    return 1;
  }
  const TemporaryStorageFactory sorter_factory = std::move(*sorter_factory_result);
  auto sorter_vm = Vm::Create(*sorter_program, VmEnvironment::Core());
  if (!sorter_vm.has_value() ||
      !sorter_vm->AttachExecutionContext(VmExecutionContext{**opened, 17, sorter_factory})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto sorter_failure = sorter_vm->Step();
  fail_allocations = false;
  if (sorter_failure.has_value() || sorter_failure.error().code() != ErrorCode::kOutOfMemory ||
      sorter_vm->state() != VmState::kError) {
    return 1;
  }

  ProgramInput relation_input;
  relation_input.schema_version = input.schema_version;
  relation_input.register_count = 2;
  relation_input.constants.push_back(SqlValue::Integer(1));
  relation_input.constants.push_back(SqlValue::Integer(10));
  relation_input.symbols.emplace_back("BINARY");
  relation_input.relations.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
                  .null_placement = BytecodeNullPlacement::kFirst,
              },
          },
  });
  relation_input.instructions = {
      OpenRelationInstruction{.relation = RelationId(0)},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      InsertRelationInstruction{
          .relation = RelationId(0),
          .first_value = RegisterId(0),
          .value_count = 2,
          .mode = RelationInsertMode::kKeepExisting,
          .duplicate_target = InstructionAddress(4),
      },
      HaltInstruction{},
  };
  auto relation_program = BytecodeProgram::Create(relation_input);
  if (!relation_program.has_value()) {
    return 1;
  }
  auto relation_vm = Vm::Create(*relation_program, VmEnvironment::Core());
  if (!relation_vm.has_value() ||
      !relation_vm->AttachExecutionContext(VmExecutionContext{**opened, 17, sorter_factory})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto relation_failure = relation_vm->Step();
  fail_allocations = false;
  if (relation_failure.has_value() || relation_failure.error().code() != ErrorCode::kOutOfMemory ||
      relation_vm->state() != VmState::kError) {
    return 1;
  }

  ProgramInput staged_record_input;
  staged_record_input.schema_version = input.schema_version;
  staged_record_input.register_count = 2;
  constexpr std::size_t kStagedRecordBytes = 512;
  staged_record_input.constants.push_back(SqlValue::Text(std::string(kStagedRecordBytes, 'r')));
  staged_record_input.result_columns.push_back(
      ResultColumnMetadata{.name = "record", .declared_type = {}, .affinity = TypeAffinity::kNone});
  staged_record_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId{0}, .output = RegisterId{0}},
      ResultRowInstruction{.first = RegisterId{0}, .count = 1},
      EncodeRecordInstruction{
          .first_value = RegisterId{0}, .value_count = 1, .output = RegisterId{1}},
      ResultRowInstruction{.first = RegisterId{1}, .count = 1},
      HaltInstruction{},
  };
  const auto staged_record_program = BytecodeProgram::Create(staged_record_input);
  if (!staged_record_program.has_value() || !SweepVmCreation(*staged_record_program)) {
    return 1;
  }
  auto staged_record_vm = Vm::Create(*staged_record_program, VmEnvironment::Core());
  if (!staged_record_vm.has_value() ||
      !staged_record_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto initial_record_row = staged_record_vm->Step();
  if (!initial_record_row.has_value() || *initial_record_row != VmStep::kRow) {
    return 1;
  }
  fail_allocations = true;
  const auto record_failure = staged_record_vm->Step();
  fail_allocations = false;
  if (record_failure.has_value() || record_failure.error().code() != ErrorCode::kOutOfMemory ||
      staged_record_vm->state() != VmState::kError || !staged_record_vm->row().empty()) {
    return 1;
  }
  if (!staged_record_vm->Reset().has_value() ||
      !staged_record_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto reset_record_row = staged_record_vm->Step();
  const auto encoded_record_row = staged_record_vm->Step();
  if (!reset_record_row.has_value() || *reset_record_row != VmStep::kRow ||
      !encoded_record_row.has_value() || *encoded_record_row != VmStep::kRow ||
      staged_record_vm->row()[0].type() != SqlValueType::kBlob) {
    return 1;
  }
  const auto encoded_done = staged_record_vm->Step();
  if (!encoded_done.has_value() || *encoded_done != VmStep::kDone) {
    return 1;
  }

  ProgramInput coroutine_input;
  coroutine_input.schema_version = input.schema_version;
  coroutine_input.register_count = 1;
  coroutine_input.constants.push_back(SqlValue::Integer(42));
  coroutine_input.coroutines.push_back(CoroutineDescriptor{
      .entry = InstructionAddress(4),
      .end = InstructionAddress(7),
      .first_output = RegisterId(0),
      .output_count = 1,
  });
  coroutine_input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress(3)},
      JumpInstruction{.target = InstructionAddress(1)},
      HaltInstruction{},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto coroutine_program = BytecodeProgram::Create(coroutine_input);
  if (!coroutine_program.has_value() || !SweepVmCreation(*coroutine_program)) {
    return 1;
  }
  auto coroutine_vm = Vm::Create(*coroutine_program, VmEnvironment::Core());
  if (!coroutine_vm.has_value() ||
      !coroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto coroutine_done = coroutine_vm->Step();
  fail_allocations = false;
  if (!coroutine_done.has_value() || *coroutine_done != VmStep::kDone) {
    return 1;
  }
  ProgramInput read_coroutine_input;
  read_coroutine_input.schema_version = input.schema_version;
  read_coroutine_input.register_count = 1;
  read_coroutine_input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(2),
      .storage = CursorStorageKind::kRowIdTable,
      .record_field_count = 1,
      .fields = {},
      .index_columns = {},
  });
  read_coroutine_input.result_columns.push_back(ResultColumnMetadata{
      .name = "value", .declared_type = {}, .affinity = TypeAffinity::kInteger});
  read_coroutine_input.coroutines.push_back(CoroutineDescriptor{
      .entry = InstructionAddress(5),
      .end = InstructionAddress(11),
      .first_output = RegisterId(0),
      .output_count = 1,
      .kind = CoroutineKind::Read,
  });
  read_coroutine_input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress(4)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress(4)},
      HaltInstruction{},
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      NullRowInstruction{.cursor = CursorId(0)},
      ReadRowIdInstruction{.cursor = CursorId(0), .output = RegisterId(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      CloseCursorInstruction{.cursor = CursorId(0)},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto read_coroutine_program = BytecodeProgram::Create(read_coroutine_input);
  if (!read_coroutine_program.has_value() || !SweepVmCreation(*read_coroutine_program)) {
    return 1;
  }
  auto read_coroutine_vm = Vm::Create(*read_coroutine_program, VmEnvironment::Core());
  if (!read_coroutine_vm.has_value() ||
      !read_coroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto read_coroutine_row = read_coroutine_vm->Step();
  if (!read_coroutine_row.has_value() || *read_coroutine_row != VmStep::kRow) {
    return 1;
  }
  fail_allocations = true;
  const auto read_coroutine_done = read_coroutine_vm->Step();
  fail_allocations = false;
  if (!read_coroutine_done.has_value() || *read_coroutine_done != VmStep::kDone) {
    return 1;
  }

  ProgramInput comparison_input;
  comparison_input.schema_version = input.schema_version;
  comparison_input.register_count = 5;
  for (const std::int64_t value : {1, 2, 1, 3}) {
    comparison_input.constants.push_back(SqlValue::Integer(value));
  }
  comparison_input.symbols.emplace_back("BINARY");
  comparison_input.result_columns.push_back(ResultColumnMetadata{
      .name = "comparison", .declared_type = {}, .affinity = TypeAffinity::kInteger});
  comparison_input.record_comparisons.push_back(RecordComparisonDescriptor{
      .field_count = 2,
      .key_columns =
          {
              RecordComparisonColumnMetadata{
                  .field_index = 0,
                  .ordering =
                      {
                          .collation = SymbolId(0),
                          .order = BytecodeSortOrder::kAscending,
                          .null_placement = BytecodeNullPlacement::kFirst,
                      },
              },
              RecordComparisonColumnMetadata{
                  .field_index = 1,
                  .ordering =
                      {
                          .collation = SymbolId(0),
                          .order = BytecodeSortOrder::kAscending,
                          .null_placement = BytecodeNullPlacement::kFirst,
                      },
              },
          },
  });
  comparison_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      LoadConstantInstruction{.constant = ConstantId(2), .output = RegisterId(2)},
      LoadConstantInstruction{.constant = ConstantId(3), .output = RegisterId(3)},
      CompareRecordsInstruction{
          .comparison = RecordComparisonId(0),
          .left_first = RegisterId(0),
          .right_first = RegisterId(2),
          .output = RegisterId(4),
      },
      ResultRowInstruction{.first = RegisterId(4), .count = 1},
      HaltInstruction{},
  };
  auto comparison_program = BytecodeProgram::Create(comparison_input);
  if (!comparison_program.has_value() || !SweepVmCreation(*comparison_program)) {
    return 1;
  }
  auto comparison_vm = Vm::Create(*comparison_program, VmEnvironment::Core());
  if (!comparison_vm.has_value() ||
      !comparison_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto comparison_row = comparison_vm->Step();
  fail_allocations = false;
  if (!comparison_row.has_value() || *comparison_row != VmStep::kRow ||
      comparison_vm->row().size() != 1U || comparison_vm->row()[0].integer_value() != -1) {
    return 1;
  }
  const auto comparison_done = comparison_vm->Step();
  if (!comparison_done.has_value() || *comparison_done != VmStep::kDone) {
    return 1;
  }

  const auto overflow_index_descriptor = [] {
    return ReadCursorDescriptor{
        .root_page = RootPageNumber(102),
        .storage = CursorStorageKind::kIndex,
        .record_field_count = 2,
        .fields = {},
        .index_columns =
            {
                IndexColumnMetadata{
                    .collation = SymbolId(0),
                    .order = BytecodeSortOrder::kAscending,
                },
                IndexColumnMetadata{
                    .collation = SymbolId(0),
                    .order = BytecodeSortOrder::kAscending,
                },
            },
        .index_key_field_count = 2,
    };
  };
  const auto overflow_result_column = [] {
    return ResultColumnMetadata{
        .name = "key",
        .declared_type = std::nullopt,
        .affinity = TypeAffinity::kText,
    };
  };

  ProgramInput index_seek_input;
  index_seek_input.schema_version = input.schema_version;
  index_seek_input.register_count = 1;
  index_seek_input.constants.push_back(SqlValue::Text(std::string(3000, 'm')));
  index_seek_input.symbols.emplace_back("BINARY");
  index_seek_input.cursors.push_back(overflow_index_descriptor());
  index_seek_input.result_columns.push_back(overflow_result_column());
  index_seek_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      SeekIndexInstruction{
          .cursor = CursorId(0),
          .first_key = RegisterId(0),
          .key_count = 1,
          .missing_target = InstructionAddress(6),
          .mode = IndexSeekMode::kGreaterOrEqual,
      },
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
  };
  auto index_seek_program = BytecodeProgram::Create(index_seek_input);
  if (!index_seek_program.has_value()) {
    return 1;
  }
  auto index_seek_vm = Vm::Create(*index_seek_program, VmEnvironment::Core());
  if (!index_seek_vm.has_value() ||
      !index_seek_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto index_seek_setup = index_seek_vm->Step();
  if (!index_seek_setup.has_value() || *index_seek_setup != VmStep::kRow) {
    return 1;
  }
  fail_allocations = true;
  const auto index_seek_failure = index_seek_vm->Step();
  fail_allocations = false;
  if (index_seek_failure.has_value() ||
      index_seek_failure.error().code() != ErrorCode::kOutOfMemory ||
      index_seek_vm->state() != VmState::kError) {
    return 1;
  }

  ProgramInput index_range_input;
  index_range_input.schema_version = input.schema_version;
  index_range_input.register_count = 1;
  index_range_input.constants.push_back(SqlValue::Text(std::string(3000, 'm')));
  index_range_input.symbols.emplace_back("BINARY");
  index_range_input.cursors.push_back(overflow_index_descriptor());
  index_range_input.result_columns.push_back(overflow_result_column());
  index_range_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      OpenReadCursorInstruction{.cursor = CursorId(0)},
      RewindInstruction{
          .cursor = CursorId(0),
          .empty_target = InstructionAddress(7),
      },
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      CheckIndexRangeInstruction{
          .cursor = CursorId(0),
          .first_key = RegisterId(0),
          .key_count = 1,
          .end_target = InstructionAddress(7),
          .mode = IndexRangeEndMode::kInclusive,
      },
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
      CloseCursorInstruction{.cursor = CursorId(0)},
      HaltInstruction{},
  };
  auto index_range_program = BytecodeProgram::Create(index_range_input);
  if (!index_range_program.has_value()) {
    return 1;
  }
  auto index_range_vm = Vm::Create(*index_range_program, VmEnvironment::Core());
  if (!index_range_vm.has_value() ||
      !index_range_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
    return 1;
  }
  const auto index_range_setup = index_range_vm->Step();
  if (!index_range_setup.has_value() || *index_range_setup != VmStep::kRow) {
    return 1;
  }
  fail_allocations = true;
  const auto index_range_failure = index_range_vm->Step();
  fail_allocations = false;
  if (index_range_failure.has_value() ||
      index_range_failure.error().code() != ErrorCode::kOutOfMemory ||
      index_range_vm->state() != VmState::kError) {
    return 1;
  }

  if (!(*opened)->pager().EndRead().has_value()) {
    return 1;
  }

  test::WritePagerFixedVfs write_vfs{false};
  std::unique_ptr<BtreeDatabase> write_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(write_vfs, 64U);
  Pager* write_pager = write_pager_database == nullptr ? nullptr : &write_pager_database->pager();
  if (write_pager == nullptr) {
    return 1;
  }
  auto coordinator = TransactionCoordinator::Open(std::move(write_pager_database));
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
  insert_input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
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
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
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
  std::unique_ptr<BtreeDatabase> delete_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(delete_vfs, 64U);
  Pager* delete_pager =
      delete_pager_database == nullptr ? nullptr : &delete_pager_database->pager();
  if (delete_pager == nullptr) {
    return 1;
  }
  auto delete_coordinator = TransactionCoordinator::Open(std::move(delete_pager_database));
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
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
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

  std::vector<SqlValue> update_fields;
  update_fields.emplace_back();
  update_fields.push_back(SqlValue::Text("updated"));
  auto update_record = EncodeRecord(update_fields);
  if (!update_record.has_value()) {
    return 1;
  }
  ProgramInput update_input;
  update_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  update_input.statement_kind = ProgramStatementKind::kUpdate;
  update_input.transaction_access = ProgramTransactionAccess::kWrite;
  update_input.rollback_mode = ProgramRollbackMode::kStatement;
  update_input.mutation_result.publishes_changes = true;
  update_input.register_count = 3;
  update_input.constants.push_back(SqlValue::Integer(1));
  update_input.constants.push_back(SqlValue::Integer(2));
  update_input.constants.push_back(SqlValue::Blob(std::move(*update_record)));
  update_input.write_cursors.push_back(WriteCursorDescriptor{
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
      .index_columns = {},
      .key_term_count = 0,
      .unique = false,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kRowIdTable,
  });
  update_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      LoadConstantInstruction{.constant = ConstantId(2), .output = RegisterId(2)},
      OpenWriteCursorInstruction{.cursor = WriteCursorId(0)},
      CheckUpdateRowIdInstruction{
          .cursor = WriteCursorId(0),
          .old_rowid = RegisterId(0),
          .new_rowid = RegisterId(1),
      },
      UpdateTableInstruction{
          .cursor = WriteCursorId(0),
          .old_rowid = RegisterId(0),
          .new_rowid = RegisterId(1),
          .record = RegisterId(2),
      },
      HaltInstruction{},
  };
  auto update_program = BytecodeProgram::Create(update_input);
  std::unique_ptr<BtreeDatabase> update_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(delete_vfs, 64U);
  Pager* update_pager =
      update_pager_database == nullptr ? nullptr : &update_pager_database->pager();
  if (update_pager == nullptr) {
    return 1;
  }
  auto update_coordinator = TransactionCoordinator::Open(std::move(update_pager_database));
  if (!update_coordinator.has_value()) {
    return 1;
  }
  auto update_statement = update_coordinator->BeginStatement(TransactionStatementOptions{
      .access = StatementAccess::kWrite,
      .rollback = StatementRollbackMode::kStatement,
  });
  if (!update_program.has_value() || !update_statement.has_value() ||
      update_statement->writer() == nullptr) {
    return 1;
  }
  auto update_vm = Vm::Create(*update_program, VmEnvironment::Core());
  if (!update_vm.has_value() ||
      !update_vm->AttachExecutionContext(VmExecutionContext{*update_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto update_failure = update_vm->Step();
  fail_allocations = false;
  if (update_failure.has_value() || update_failure.error().code() != ErrorCode::kOutOfMemory ||
      update_vm->change_count() != 0U) {
    return 1;
  }
  if (!update_vm->DetachExecutionContext().has_value() ||
      !update_statement->Rollback().has_value()) {
    return 1;
  }

  test::WritePagerFixedVfs create_vfs{false};
  std::unique_ptr<BtreeDatabase> create_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(create_vfs, 64U);
  Pager* create_pager =
      create_pager_database == nullptr ? nullptr : &create_pager_database->pager();
  if (create_pager == nullptr) {
    return 1;
  }
  auto create_coordinator = TransactionCoordinator::Open(std::move(create_pager_database));
  if (!create_coordinator.has_value()) {
    return 1;
  }
  ProgramInput create_input;
  create_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  create_input.statement_kind = ProgramStatementKind::kCreateTable;
  create_input.transaction_access = ProgramTransactionAccess::kWrite;
  create_input.rollback_mode = ProgramRollbackMode::kStatement;
  create_input.register_count = 2;
  create_input.instructions = {
      EnsureDatabaseInitializedInstruction{},
      CreateTableRootInstruction{.output = RegisterId(0)},
      IncrementSchemaCookieInstruction{.output = RegisterId(1)},
      HaltInstruction{},
  };
  auto create_program = BytecodeProgram::Create(create_input);
  auto create_statement = create_coordinator->BeginStatement(TransactionStatementOptions{
      .access = StatementAccess::kWrite,
      .rollback = StatementRollbackMode::kStatement,
  });
  if (!create_program.has_value() || !create_statement.has_value() ||
      create_statement->writer() == nullptr) {
    return 1;
  }
  auto create_vm = Vm::Create(*create_program, VmEnvironment::Core());
  if (!create_vm.has_value() ||
      !create_vm->AttachExecutionContext(VmExecutionContext{*create_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto create_step_failure = create_vm->Step();
  fail_allocations = false;
  if (create_step_failure.has_value() ||
      create_step_failure.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  if (!create_vm->DetachExecutionContext().has_value() ||
      !create_statement->Rollback().has_value() || !create_vfs.database_bytes().empty()) {
    return 1;
  }

  test::WritePagerFixedVfs create_index_vfs{false};
  std::unique_ptr<BtreeDatabase> create_index_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(create_index_vfs, 64U);
  Pager* create_index_pager =
      create_index_pager_database == nullptr ? nullptr : &create_index_pager_database->pager();
  if (create_index_pager == nullptr) {
    return 1;
  }
  Pager& refill_pager = *create_index_pager;
  auto create_index_coordinator =
      TransactionCoordinator::Open(std::move(create_index_pager_database));
  if (!create_index_coordinator.has_value()) {
    return 1;
  }
  ProgramInput create_index_input;
  create_index_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  create_index_input.statement_kind = ProgramStatementKind::kCreateIndex;
  create_index_input.transaction_access = ProgramTransactionAccess::kWrite;
  create_index_input.rollback_mode = ProgramRollbackMode::kStatement;
  create_index_input.register_count = 1;
  create_index_input.symbols.emplace_back("BINARY");
  create_index_input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(0),
      .columns = {},
      .rowid_alias = std::nullopt,
      .index_columns =
          {
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
          },
      .key_term_count = 1,
      .unique = false,
      .unique_not_null = false,
      .pending_root = true,
      .storage = WriteCursorStorageKind::kIndex,
  });
  create_index_input.instructions = {
      CreateIndexRootInstruction{
          .cursor = WriteCursorId(0),
          .output = RegisterId(0),
      },
      CloseWriteCursorInstruction{.cursor = WriteCursorId(0)},
      HaltInstruction{},
  };
  auto create_index_program = BytecodeProgram::Create(create_index_input);
  auto create_index_statement =
      create_index_coordinator->BeginStatement(TransactionStatementOptions{
          .access = StatementAccess::kWrite,
          .rollback = StatementRollbackMode::kStatement,
      });
  if (!create_index_program.has_value() || !create_index_statement.has_value() ||
      create_index_statement->writer() == nullptr) {
    return 1;
  }
  auto create_index_vm = Vm::Create(*create_index_program, VmEnvironment::Core());
  if (!create_index_vm.has_value() ||
      !create_index_vm
           ->AttachExecutionContext(VmExecutionContext{*create_index_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto create_index_failure = create_index_vm->Step();
  fail_allocations = false;
  if (create_index_failure.has_value() ||
      create_index_failure.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  if (!create_index_vm->DetachExecutionContext().has_value() ||
      !create_index_statement->Rollback().has_value() ||
      !create_index_vfs.database_bytes().empty()) {
    return 1;
  }

  auto refill_initialization = create_index_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!refill_initialization.has_value() ||
      !refill_initialization->writer()->InitializeDatabase().has_value() ||
      !refill_initialization->Succeed().has_value()) {
    return 1;
  }
  const ByteBuffer refill_original = ByteBuffer::CopyOf(create_index_vfs.database_bytes());
  auto refill_factory = TemporaryStorageFactory::Create(
      create_index_vfs, refill_pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory});
  if (!refill_factory.has_value()) {
    return 1;
  }
  create_index_input.register_count = 2;
  create_index_input.write_effects = ProgramWriteEffects{.multi_write = true, .may_abort = true};
  create_index_input.sorters.push_back(OrderingRecordDescriptor{
      .field_count = 2,
      .key_field_count = 1,
      .key_columns =
          {
              OrderingColumnMetadata{.collation = SymbolId(0)},
          },
      .index_refill_cursor = WriteCursorId(0),
  });
  create_index_input.instructions = {
      CreateIndexRootInstruction{.cursor = WriteCursorId(0), .output = RegisterId(0)},
      OpenSorterInstruction{.sorter = SorterId(0)},
  };
  const std::array keys{std::string(80, 'a'), std::string(2000, 'b'), std::string{"c"},
                        std::string(2500, 'd')};
  for (std::uint32_t row = 0; row < keys.size(); ++row) {
    create_index_input.constants.push_back(SqlValue::Text(keys[row]));
    create_index_input.constants.push_back(SqlValue::Integer(static_cast<std::int64_t>(row) + 1));
    create_index_input.instructions.emplace_back(
        LoadConstantInstruction{.constant = ConstantId(row * 2U), .output = RegisterId(0)});
    create_index_input.instructions.emplace_back(
        LoadConstantInstruction{.constant = ConstantId(row * 2U + 1U), .output = RegisterId(1)});
    create_index_input.instructions.emplace_back(InsertSorterInstruction{
        .sorter = SorterId(0), .first_value = RegisterId(0), .value_count = 2});
  }
  const auto refill_rewind = static_cast<std::uint32_t>(create_index_input.instructions.size());
  create_index_input.instructions.emplace_back(RewindSorterInstruction{
      .sorter = SorterId(0), .empty_target = InstructionAddress(refill_rewind + 3U)});
  create_index_input.instructions.emplace_back(InsertSorterIndexInstruction{.sorter = SorterId(0)});
  create_index_input.instructions.emplace_back(NextSorterInstruction{
      .sorter = SorterId(0), .next_target = InstructionAddress(refill_rewind + 1U)});
  create_index_input.instructions.emplace_back(
      CloseWriteCursorInstruction{.cursor = WriteCursorId(0)});
  create_index_input.instructions.emplace_back(CloseSorterInstruction{.sorter = SorterId(0)});
  create_index_input.instructions.emplace_back(HaltInstruction{});
  const auto refill_program = BytecodeProgram::Create(create_index_input);
  if (!refill_program.has_value() || !SweepVmCreation(*refill_program)) {
    return 1;
  }
  const auto attempt_refill =
      [&](std::optional<std::size_t> failure) -> std::optional<std::size_t> {
    auto statement_result = create_index_coordinator->BeginStatement(TransactionStatementOptions{
        .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement});
    auto runtime = Vm::Create(*refill_program, VmEnvironment::Core());
    if (!statement_result.has_value() || !runtime.has_value() ||
        !runtime
             ->AttachExecutionContext(
                 VmExecutionContext{*statement_result->writer(), 0, *refill_factory})
             .has_value()) {
      return std::nullopt;
    }
    allocation_index = 0;
    failing_allocation = failure;
    const auto stepped = runtime->Step();
    const std::size_t count = allocation_index;
    failing_allocation.reset();
    if (failure.has_value()
            ? stepped.has_value() || stepped.error().code() != ErrorCode::kOutOfMemory ||
                  runtime->state() != VmState::kError
            : !stepped.has_value() || *stepped != VmStep::kDone) {
      return std::nullopt;
    }
    if (!runtime->DetachExecutionContext().has_value() ||
        !statement_result->Rollback().has_value() ||
        !std::ranges::equal(refill_original.view(), create_index_vfs.database_bytes()) ||
        create_index_vfs.pathless_file_present()) {
      return std::nullopt;
    }
    return count;
  };
  constexpr std::size_t kMaximumRefillStepAllocations = 1024;
  const std::optional<std::size_t> refill_allocations = attempt_refill(std::nullopt);
  if (!refill_allocations.has_value() || *refill_allocations == 0U ||
      *refill_allocations > kMaximumRefillStepAllocations) {
    return 1;
  }
  for (std::size_t failure = 0; failure < *refill_allocations; ++failure) {
    if (!attempt_refill(failure).has_value()) {
      return 1;
    }
  }

  test::WritePagerFixedVfs index_vfs{false};
  std::unique_ptr<BtreeDatabase> index_pager_database =
      test::OpenWriteDatabase<BtreeDatabase>(index_vfs, 64U);
  Pager* index_pager = index_pager_database == nullptr ? nullptr : &index_pager_database->pager();
  if (index_pager == nullptr) {
    return 1;
  }
  auto index_coordinator = TransactionCoordinator::Open(std::move(index_pager_database));
  if (!index_coordinator.has_value()) {
    return 1;
  }
  auto index_initialization = index_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!index_initialization.has_value() || index_initialization->writer() == nullptr ||
      !index_initialization->writer()->InitializeDatabase().has_value()) {
    return 1;
  }
  const std::array<IndexColumnOrder, 2> index_columns{
      IndexColumnOrder{BinaryCollation()},
      IndexColumnOrder{BinaryCollation()},
  };
  auto index_writer = modern_sqlite::test::CreateIndexBtree(
      *(index_initialization->writer()), index_columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (index_columns).size(),
                                   .all_field_count = (index_columns).size()});
  if (!index_writer.has_value()) {
    return 1;
  }
  const PageNumber index_root = index_writer->root_page();
  if (!index_initialization->Succeed().has_value()) {
    return 1;
  }

  ProgramInput index_input;
  index_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  index_input.statement_kind = ProgramStatementKind::kInsert;
  index_input.transaction_access = ProgramTransactionAccess::kWrite;
  index_input.rollback_mode = ProgramRollbackMode::kStatement;
  index_input.mutation_result = MutationResultMetadata{
      .publishes_changes = true,
      .publishes_last_insert_rowid = true,
  };
  index_input.register_count = 2;
  index_input.constants.push_back(SqlValue::Text("key"));
  index_input.constants.push_back(SqlValue::Integer(1));
  index_input.symbols.emplace_back("BINARY");
  index_input.write_cursors.push_back(WriteCursorDescriptor{
      .root_page = RootPageNumber(index_root.value()),
      .columns = {},
      .rowid_alias = std::nullopt,
      .index_columns =
          {
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
          },
      .key_term_count = 1,
      .unique = true,
      .unique_not_null = false,
      .storage = WriteCursorStorageKind::kIndex,
  });
  index_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      OpenWriteCursorInstruction{.cursor = WriteCursorId(0)},
      CheckUniqueIndexInstruction{
          .cursor = WriteCursorId(0),
          .first_key = RegisterId(0),
          .key_count = 1,
          .ignored_rowid = std::nullopt,
      },
      InsertIndexInstruction{
          .cursor = WriteCursorId(0),
          .first_value = RegisterId(0),
          .value_count = 2,
      },
      HaltInstruction{},
  };
  auto index_program = BytecodeProgram::Create(index_input);
  auto index_statement = index_coordinator->BeginStatement(TransactionStatementOptions{
      .access = StatementAccess::kWrite,
      .rollback = StatementRollbackMode::kStatement,
  });
  if (!index_program.has_value() || !index_statement.has_value() ||
      index_statement->writer() == nullptr) {
    return 1;
  }
  auto index_vm = Vm::Create(*index_program, VmEnvironment::Core());
  if (!index_vm.has_value() ||
      !index_vm->AttachExecutionContext(VmExecutionContext{*index_statement->writer(), 0})
           .has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto index_failure = index_vm->Step();
  fail_allocations = false;
  if (index_failure.has_value() || index_failure.error().code() != ErrorCode::kOutOfMemory ||
      index_vm->state() != VmState::kError) {
    return 1;
  }
  if (!index_vm->DetachExecutionContext().has_value() || !index_statement->Rollback().has_value()) {
    return 1;
  }

  auto analyze_seed = index_coordinator->BeginStatement(
      TransactionStatementOptions{.access = StatementAccess::kWrite});
  if (!analyze_seed.has_value() || analyze_seed->writer() == nullptr) {
    return 1;
  }
  auto analyze_index = modern_sqlite::test::OpenIndexBtree(
      *(analyze_seed->writer()), index_root, index_columns,
      modern_sqlite::IndexKeyShape{.key_field_count = (index_columns).size(),
                                   .all_field_count = (index_columns).size()});
  if (!analyze_index.has_value()) {
    return 1;
  }
  for (std::size_t row = 0; row < keys.size(); ++row) {
    const std::array values{SqlValue::Text(keys[row]),
                            SqlValue::Integer(static_cast<std::int64_t>(row) + 1)};
    if (!analyze_index->Insert(values).has_value()) {
      return 1;
    }
  }
  if (!analyze_seed->Succeed().has_value()) {
    return 1;
  }
  const ByteBuffer analyze_original = ByteBuffer::CopyOf(index_vfs.database_bytes());
  ProgramInput analyze_input;
  analyze_input.schema_version = SchemaVersionRequirement{.schema_cookie = 0, .generation = 0};
  analyze_input.statement_kind = ProgramStatementKind::kAnalyze;
  analyze_input.transaction_access = ProgramTransactionAccess::kWrite;
  analyze_input.rollback_mode = ProgramRollbackMode::kStatement;
  analyze_input.register_count = 1;
  analyze_input.symbols.emplace_back("BINARY");
  analyze_input.cursors.push_back(ReadCursorDescriptor{
      .root_page = RootPageNumber(index_root.value()),
      .storage = CursorStorageKind::kIndex,
      .record_field_count = 2,
      .fields = {},
      .index_columns =
          {
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
              IndexColumnMetadata{
                  .collation = SymbolId(0),
                  .order = BytecodeSortOrder::kAscending,
              },
          },
      .index_key_field_count = 2,
  });
  analyze_input.instructions = {
      ComputeIndexStat1Instruction{
          .cursor = CursorId(0),
          .key_term_count = 1,
          .emit_empty = false,
          .output = RegisterId(0),
      },
      HaltInstruction{},
  };
  auto analyze_program = BytecodeProgram::Create(analyze_input);
  if (!analyze_program.has_value() || !SweepVmCreation(*analyze_program)) {
    return 1;
  }
  const auto attempt_analyze =
      [&](std::optional<std::size_t> failure) -> std::optional<std::size_t> {
    auto statement_result = index_coordinator->BeginStatement(TransactionStatementOptions{
        .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement});
    auto runtime = Vm::Create(*analyze_program, VmEnvironment::Core());
    if (!statement_result.has_value() || !runtime.has_value() ||
        !runtime->AttachExecutionContext(VmExecutionContext{*statement_result->writer(), 0})
             .has_value()) {
      return std::nullopt;
    }
    allocation_index = 0;
    failing_allocation = failure;
    const auto stepped = runtime->Step();
    const std::size_t count = allocation_index;
    failing_allocation.reset();
    if (failure.has_value()
            ? stepped.has_value() || stepped.error().code() != ErrorCode::kOutOfMemory ||
                  runtime->state() != VmState::kError
            : !stepped.has_value() || *stepped != VmStep::kDone) {
      return std::nullopt;
    }
    if (!runtime->DetachExecutionContext().has_value() ||
        !statement_result->Rollback().has_value() ||
        !std::ranges::equal(analyze_original.view(), index_vfs.database_bytes())) {
      return std::nullopt;
    }
    return count;
  };
  if (!attempt_analyze(std::nullopt).has_value()) {
    return 1;
  }
  constexpr std::size_t kMaximumAnalyzeStepAllocations = 1024;
  const std::optional<std::size_t> analyze_allocations = attempt_analyze(std::nullopt);
  if (!analyze_allocations.has_value() || *analyze_allocations == 0U ||
      *analyze_allocations > kMaximumAnalyzeStepAllocations) {
    return 1;
  }
  for (std::size_t failure = 0; failure < *analyze_allocations; ++failure) {
    if (!attempt_analyze(failure).has_value()) {
      return 1;
    }
  }
  {
    VmLimits limits;
    limits.maximum_value_bytes = 1024;
    auto statement_result = index_coordinator->BeginStatement(TransactionStatementOptions{
        .access = StatementAccess::kWrite, .rollback = StatementRollbackMode::kStatement});
    auto runtime = Vm::Create(*analyze_program, VmEnvironment::Core(), limits);
    if (!statement_result.has_value() || !runtime.has_value() ||
        !runtime->AttachExecutionContext(VmExecutionContext{*statement_result->writer(), 0})
             .has_value()) {
      return 1;
    }
    const auto limited = runtime->Step();
    if (limited.has_value() || limited.error().code() != ErrorCode::kTooLarge ||
        runtime->state() != VmState::kError || !runtime->DetachExecutionContext().has_value() ||
        !statement_result->Rollback().has_value() ||
        !std::ranges::equal(analyze_original.view(), index_vfs.database_bytes())) {
      return 1;
    }
  }
  return 0;
} catch (...) {
  fail_allocations = false;
  return 1;
}
