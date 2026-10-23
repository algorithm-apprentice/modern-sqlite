#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <span>
#include <string>

#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/storage/btree/database.hpp"
#include "modern_sqlite/vm/vm.hpp"

namespace {

std::atomic<std::size_t> allocation_count = 0;
std::atomic<std::size_t> live_allocations = 0;

constexpr std::size_t kOwnedTextBytes = 512;

enum class RegisterCleanupEnd {
  Halt,
  Failure,
  ForeignException,
  Reset,
  Destruction,
};

struct CallbackException {};

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* allocation = std::malloc(size == 0 ? 1 : size); allocation != nullptr) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return allocation;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  void* allocation = nullptr;
  if (posix_memalign(&allocation, alignment, size == 0 ? alignment : size) == 0) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return allocation;
  }
  throw std::bad_alloc{};
}

void Deallocate(void* allocation) noexcept {
  if (allocation != nullptr) {
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(allocation);
  }
}

[[nodiscard]] std::filesystem::path FixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "btree_read" / "sqlite-3.54.0-btree-read.db";
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> OwnedText(
    const modern_sqlite::ScalarFunctionContext&, std::span<const modern_sqlite::SqlValue>) {
  return modern_sqlite::SqlValue::Text(std::string(kOwnedTextBytes, 'x'));
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> FailingFunction(
    const modern_sqlite::ScalarFunctionContext&, std::span<const modern_sqlite::SqlValue>) {
  return std::unexpected(modern_sqlite::Error::Create(modern_sqlite::ErrorCode::kAborted,
                                                      "expected callback failure"));
}

[[nodiscard]] modern_sqlite::Result<modern_sqlite::SqlValue> ThrowingFunction(
    const modern_sqlite::ScalarFunctionContext&, std::span<const modern_sqlite::SqlValue>) {
  throw CallbackException{};
}

[[nodiscard]] bool CheckRegisterCleanup(modern_sqlite::BtreeDatabase& database,
                                        modern_sqlite::SchemaVersionRequirement schema,
                                        RegisterCleanupEnd end) {
  using namespace modern_sqlite;
  const std::array functions{
      ScalarFunction{"owned", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, OwnedText},
      ScalarFunction{"failure", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, FailingFunction},
      ScalarFunction{"throwing", FunctionArity::Exact(0), FunctionDeterminism::kDeterministic,
                     FunctionCollationUse::kNone, ThrowingFunction},
  };
  const FunctionRegistry registry{functions};
  const std::array<const Collation*, 1> collations{&BinaryCollation()};
  ProgramInput input;
  input.schema_version = schema;
  input.register_count = 1;
  input.parameter_count = 1;
  input.symbols = {"owned", "failure", "throwing", "BINARY"};
  input.result_columns.push_back(
      ResultColumnMetadata{.name = "value", .declared_type = {}, .affinity = TypeAffinity::kNone});
  input.instructions = {
      CallScalarInstruction{.function = SymbolId(0),
                            .collation = SymbolId(3),
                            .first_argument = RegisterId(0),
                            .argument_count = 0,
                            .output = RegisterId(0)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
  };
  if (end == RegisterCleanupEnd::Failure || end == RegisterCleanupEnd::ForeignException) {
    input.instructions.emplace_back(
        CallScalarInstruction{.function = SymbolId(end == RegisterCleanupEnd::Failure ? 1U : 2U),
                              .collation = SymbolId(3),
                              .first_argument = RegisterId(0),
                              .argument_count = 0,
                              .output = RegisterId(0)});
  }
  input.instructions.emplace_back(HaltInstruction{});
  auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  const SqlValue binding = SqlValue::Text(std::string(kOwnedTextBytes, 'b'));
  const std::size_t before_vm = live_allocations.load(std::memory_order_relaxed);
  {
    auto vm = Vm::Create(*program, VmEnvironment{registry, collations});
    if (!vm.has_value() || !vm->Bind(ParameterId(0), binding).has_value() ||
        !vm->AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
      return false;
    }
    const std::size_t before_row = live_allocations.load(std::memory_order_relaxed);
    const auto row = vm->Step();
    if (!row.has_value() || *row != VmStep::kRow || vm->row().size() != 1U ||
        !vm->row()[0].text_value().has_value() ||
        vm->row()[0].text_value()->size_bytes() != kOwnedTextBytes ||
        live_allocations.load(std::memory_order_relaxed) <= before_row) {
      return false;
    }
    if (end == RegisterCleanupEnd::Halt) {
      const auto done = vm->Step();
      if (!done.has_value() || *done != VmStep::kDone) {
        return false;
      }
    } else if (end == RegisterCleanupEnd::Reset) {
      if (!vm->Reset().has_value()) {
        return false;
      }
    } else if (end == RegisterCleanupEnd::Failure) {
      const auto rejected = vm->Step();
      if (rejected.has_value() || rejected.error().code() != ErrorCode::kAborted ||
          vm->state() != VmState::kError) {
        return false;
      }
    } else if (end == RegisterCleanupEnd::ForeignException) {
      bool caught = false;
      try {
        const auto result = vm->Step();
        if (result.has_value()) {
          return false;
        }
      } catch (const CallbackException&) {
        caught = true;
      }
      if (!caught || vm->state() != VmState::kError) {
        return false;
      }
    }
    if (vm->bindings().size() != 1U ||
        vm->bindings()[0].text_value()->bytes() != binding.text_value()->bytes()) {
      return false;
    }
    if (end != RegisterCleanupEnd::Destruction &&
        (live_allocations.load(std::memory_order_relaxed) != before_row || !vm->row().empty())) {
      std::fprintf(stderr, "register byte cleanup failed: live=%zu expected=%zu\n",
                   live_allocations.load(std::memory_order_relaxed), before_row);
      return false;
    }
    if (end != RegisterCleanupEnd::Reset && end != RegisterCleanupEnd::Destruction &&
        !vm->DetachExecutionContext().has_value()) {
      return false;
    }
  }
  return live_allocations.load(std::memory_order_relaxed) == before_vm;
}

[[nodiscard]] bool CheckValueCoroutineControl(modern_sqlite::BtreeDatabase& database,
                                              modern_sqlite::SchemaVersionRequirement schema) {
  using namespace modern_sqlite;
  ProgramInput input;
  input.schema_version = schema;
  input.register_count = 1;
  input.constants.push_back(SqlValue::Integer(42));
  input.coroutines.push_back(CoroutineDescriptor{
      .entry = InstructionAddress(5),
      .end = InstructionAddress(8),
      .first_output = RegisterId(0),
      .output_count = 1,
  });
  input.instructions = {
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress(3)},
      JumpInstruction{.target = InstructionAddress(1)},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress(4)},
      HaltInstruction{},
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
  };
  const auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  auto vm = Vm::Create(*program, VmEnvironment::Core());
  if (!vm.has_value()) {
    return false;
  }
  const auto before = allocation_count.load(std::memory_order_relaxed);
  constexpr std::size_t kCoroutineRuns = 4U;
  for (std::size_t iteration = 0; iteration < kCoroutineRuns; ++iteration) {
    if (!vm->AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
      return false;
    }
    const auto done = vm->Step();
    if (!done.has_value() || *done != VmStep::kDone || !vm->Reset().has_value()) {
      return false;
    }
  }
  return allocation_count.load(std::memory_order_relaxed) == before;
}

[[nodiscard]] bool CheckRecordStagingAndOwnerMoves(modern_sqlite::BtreeDatabase& database,
                                                   modern_sqlite::SchemaVersionRequirement schema) {
  using namespace modern_sqlite;
  ProgramInput input;
  input.schema_version = schema;
  input.register_count = 6;
  input.constants.push_back(SqlValue::Text(std::string(kOwnedTextBytes, 'p')));
  input.constants.push_back(SqlValue::Text(""));
  input.result_columns = {
      ResultColumnMetadata{.name = "first", .declared_type = {}, .affinity = TypeAffinity::kNone},
      ResultColumnMetadata{.name = "second", .declared_type = {}, .affinity = TypeAffinity::kNone},
  };
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(4)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(5)},
      BinaryInstruction{.operation = BinaryOperation::kConcatenate,
                        .left = RegisterId(4),
                        .right = RegisterId(5),
                        .output = RegisterId(0)},
      EncodeRecordInstruction{
          .first_value = RegisterId(0), .value_count = 1, .output = RegisterId(1)},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      EncodeRecordInstruction{
          .first_value = RegisterId(0), .value_count = 1, .output = RegisterId(1)},
      MoveInstruction{.first_input = RegisterId(0), .first_output = RegisterId(2), .count = 2},
      ResultRowInstruction{.first = RegisterId(2), .count = 2},
      ResultRowInstruction{.first = RegisterId(0), .count = 2},
      HaltInstruction{},
  };
  auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  auto vm = Vm::Create(*program, VmEnvironment::Core());
  if (!vm.has_value() ||
      !vm->AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
    return false;
  }
  const auto first = vm->Step();
  if (!first.has_value() || *first != VmStep::kRow || vm->row().size() != 2U) {
    return false;
  }
  const auto before = allocation_count.load(std::memory_order_relaxed);
  const auto moved = vm->Step();
  if (!moved.has_value() || *moved != VmStep::kRow || vm->row()[0].type() != SqlValueType::kText ||
      vm->row()[1].type() != SqlValueType::kBlob ||
      vm->row()[0].text_value().value_or(Utf8View{}).size_bytes() != kOwnedTextBytes ||
      vm->row()[1].blob_value().value_or(ByteView{}).size() <= kOwnedTextBytes) {
    return false;
  }
  const auto cleared = vm->Step();
  if (!cleared.has_value() || *cleared != VmStep::kRow ||
      vm->row()[0].type() != SqlValueType::kNull || vm->row()[1].type() != SqlValueType::kNull) {
    return false;
  }
  const auto done = vm->Step();
  return done.has_value() && *done == VmStep::kDone &&
         allocation_count.load(std::memory_order_relaxed) == before;
}

[[nodiscard]] bool CheckRegisterComparisons(modern_sqlite::BtreeDatabase& database,
                                            modern_sqlite::SchemaVersionRequirement schema) {
  using namespace modern_sqlite;
  for (const std::uint32_t output : {0U, 2U, 4U}) {
    ProgramInput input;
    input.schema_version = schema;
    input.register_count = 5;
    input.symbols = {"BINARY", "NOCASE"};
    input.constants.push_back(SqlValue::Integer(1));
    input.constants.push_back(SqlValue::Text(std::string(kOwnedTextBytes, 'B')));
    input.constants.push_back(SqlValue::Real(1.0));
    input.constants.push_back(SqlValue::Text(std::string(kOwnedTextBytes, 'a')));
    input.record_comparisons.push_back(OrderingRecordDescriptor{
        .field_count = 2,
        .key_field_count = 2,
        .key_columns =
            {
                OrderingColumnMetadata{.collation = SymbolId(0)},
                OrderingColumnMetadata{
                    .collation = SymbolId(1),
                    .order = BytecodeSortOrder::kDescending,
                },
            },
    });
    for (std::uint32_t index = 0; index < 4U; ++index) {
      input.instructions.emplace_back(
          LoadConstantInstruction{.constant = ConstantId(index), .output = RegisterId(index)});
    }
    input.instructions.emplace_back(CompareRecordsInstruction{
        .comparison = RecordComparisonId(0),
        .left_first = RegisterId(0),
        .right_first = RegisterId(2),
        .output = RegisterId(output),
    });
    input.instructions.emplace_back(ResultRowInstruction{.first = RegisterId(output), .count = 1});
    input.instructions.emplace_back(HaltInstruction{});
    input.result_columns.push_back(ResultColumnMetadata{
        .name = "comparison", .declared_type = {}, .affinity = TypeAffinity::kInteger});
    const auto program = BytecodeProgram::Create(input);
    if (!program.has_value()) {
      return false;
    }
    auto vm = Vm::Create(*program, VmEnvironment::Core());
    if (!vm.has_value()) {
      return false;
    }
    const std::size_t before = allocation_count.load(std::memory_order_relaxed);
    constexpr std::size_t kComparisonRuns = 4U;
    for (std::size_t iteration = 0; iteration < kComparisonRuns; ++iteration) {
      if (!vm->AttachExecutionContext(VmExecutionContext{database, schema.generation})
               .has_value()) {
        return false;
      }
      const auto row = vm->Step();
      if (!row.has_value() || *row != VmStep::kRow || vm->row().size() != 1U ||
          vm->row()[0].integer_value() != -1) {
        return false;
      }
      const auto done = vm->Step();
      if (!done.has_value() || *done != VmStep::kDone || !vm->Reset().has_value()) {
        return false;
      }
    }
    const std::size_t after = allocation_count.load(std::memory_order_relaxed);
    if (after != before) {
      std::fprintf(stderr, "resident register comparison allocated %zu times\n", after - before);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CheckStaticLoads(modern_sqlite::BtreeDatabase& database,
                                    modern_sqlite::SchemaVersionRequirement schema) {
  using namespace modern_sqlite;
  ProgramInput input;
  input.schema_version = schema;
  input.register_count = 4;
  input.parameter_count = 1;
  input.constants.push_back(SqlValue::Text(std::string(kOwnedTextBytes, 'c')));
  for (std::size_t column = 0; column < input.register_count; ++column) {
    input.result_columns.push_back(ResultColumnMetadata{
        .name = "value", .declared_type = {}, .affinity = TypeAffinity::kNone});
  }
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadParameterInstruction{.parameter = ParameterId(0), .output = RegisterId(1)},
      CopyInstruction{.input = RegisterId(0), .output = RegisterId(2)},
      CopyInstruction{.input = RegisterId(1), .output = RegisterId(3)},
      ResultRowInstruction{.first = RegisterId(0), .count = 4},
      HaltInstruction{},
  };
  const auto program = BytecodeProgram::Create(input);
  if (!program.has_value()) {
    return false;
  }
  auto vm = Vm::Create(*program, VmEnvironment::Core());
  const SqlValue binding =
      SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(std::string(kOwnedTextBytes, 'b'))));
  if (!vm.has_value() || !vm->Bind(ParameterId(0), binding).has_value() ||
      !vm->AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
    return false;
  }
  const std::size_t before = allocation_count.load(std::memory_order_relaxed);
  const auto row = vm->Step();
  const std::size_t after = allocation_count.load(std::memory_order_relaxed);
  if (!row.has_value() || *row != VmStep::kRow || after != before || vm->row().size() != 4U) {
    std::fprintf(stderr, "static register loads/copies allocated %zu times\n", after - before);
    return false;
  }
  if (vm->row()[0].text_value()->data() != program->constant(ConstantId(0)).text_value()->data() ||
      vm->row()[2].text_value()->data() != vm->row()[0].text_value()->data() ||
      vm->row()[1].blob_value()->data() != vm->bindings()[0].blob_value()->data() ||
      vm->row()[3].blob_value()->data() != vm->row()[1].blob_value()->data()) {
    return false;
  }
  SqlValue clone = vm->row()[3].Clone();
  Vm moved = std::move(*vm);
  if (clone.blob_value()->data() == moved.row()[3].blob_value()->data() ||
      !moved.Reset().has_value() ||
      !moved.Bind(ParameterId(0), SqlValue::Blob(ByteBuffer::CopyOf(AsBytes("replacement"))))
           .has_value() ||
      !moved.AttachExecutionContext(VmExecutionContext{database, schema.generation}).has_value()) {
    return false;
  }
  const auto rebound = moved.Step();
  if (!rebound.has_value() || *rebound != VmStep::kRow ||
      AsStringView(*moved.row()[3].blob_value()) != "replacement" ||
      clone.blob_value()->size() != kOwnedTextBytes || !moved.Reset().has_value() ||
      !moved.ClearBindings().has_value()) {
    return false;
  }
  return moved.bindings()[0].type() == SqlValueType::kNull;
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
void operator delete(void* allocation) noexcept { Deallocate(allocation); }
void operator delete[](void* allocation) noexcept { Deallocate(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { Deallocate(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { Deallocate(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept { Deallocate(allocation); }
void operator delete[](void* allocation, std::align_val_t) noexcept { Deallocate(allocation); }
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  Deallocate(allocation);
}
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept {
  Deallocate(allocation);
}

int main() try {
  using namespace modern_sqlite;

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

  ProgramInput subroutine_input;
  subroutine_input.schema_version = input.schema_version;
  subroutine_input.register_count = 2;
  subroutine_input.constants.push_back(SqlValue::Integer(1'000));
  subroutine_input.constants.push_back(SqlValue::Integer(1));
  subroutine_input.result_columns.push_back(ResultColumnMetadata{
      .name = "value",
      .declared_type = std::nullopt,
      .affinity = TypeAffinity::kInteger,
  });
  subroutine_input.subroutines.push_back(
      SubroutineDescriptor{.entry = InstructionAddress(6), .end = InstructionAddress(8)});
  subroutine_input.instructions = {
      LoadConstantInstruction{.constant = ConstantId(0), .output = RegisterId(0)},
      LoadConstantInstruction{.constant = ConstantId(1), .output = RegisterId(1)},
      CallSubroutineInstruction{.subroutine = SubroutineId(0)},
      JumpIfInstruction{.condition = JumpCondition::kIfTrue,
                        .input = RegisterId(0),
                        .target = InstructionAddress(2)},
      ResultRowInstruction{.first = RegisterId(0), .count = 1},
      HaltInstruction{},
      BinaryInstruction{.operation = BinaryOperation::kSubtract,
                        .left = RegisterId(0),
                        .right = RegisterId(1),
                        .output = RegisterId(0)},
      ReturnSubroutineInstruction{.subroutine = SubroutineId(0)},
  };
  const auto subroutine_program = BytecodeProgram::Create(subroutine_input);
  if (!subroutine_program.has_value()) {
    return 1;
  }
  auto subroutine_vm = Vm::Create(*subroutine_program, VmEnvironment::Core());
  if (!subroutine_vm.has_value()) {
    return 1;
  }
  const std::size_t before_subroutines = allocation_count.load(std::memory_order_relaxed);
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    if (!subroutine_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
      return 1;
    }
    const auto row = subroutine_vm->Step();
    if (!row.has_value() || *row != VmStep::kRow || subroutine_vm->row().size() != 1U ||
        subroutine_vm->row()[0].integer_value() != 0) {
      return 1;
    }
    const auto done = subroutine_vm->Step();
    if (!done.has_value() || *done != VmStep::kDone || !subroutine_vm->Reset().has_value()) {
      return 1;
    }
  }
  if (allocation_count.load(std::memory_order_relaxed) != before_subroutines) {
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
  if (!aggregate_vm.has_value()) {
    return 1;
  }
  const std::size_t before_aggregate = allocation_count.load(std::memory_order_relaxed);
  constexpr std::size_t kAggregateRuns = 8U;
  for (std::size_t iteration = 0; iteration < kAggregateRuns; ++iteration) {
    if (!aggregate_vm->AttachExecutionContext(VmExecutionContext{**opened, 17}).has_value()) {
      return 1;
    }
    const auto done = aggregate_vm->Step();
    if (!done.has_value() || *done != VmStep::kDone || !aggregate_vm->Reset().has_value()) {
      return 1;
    }
  }
  const std::size_t after_aggregate = allocation_count.load(std::memory_order_relaxed);

  if (!CheckValueCoroutineControl(**opened, input.schema_version) ||
      !CheckRecordStagingAndOwnerMoves(**opened, input.schema_version) ||
      !CheckRegisterComparisons(**opened, input.schema_version) ||
      !CheckStaticLoads(**opened, input.schema_version)) {
    return 1;
  }

  for (const RegisterCleanupEnd end :
       {RegisterCleanupEnd::Halt, RegisterCleanupEnd::Failure, RegisterCleanupEnd::ForeignException,
        RegisterCleanupEnd::Reset, RegisterCleanupEnd::Destruction}) {
    if (!CheckRegisterCleanup(**opened, input.schema_version, end)) {
      std::fprintf(stderr, "register cleanup control failed: %d\n", static_cast<int>(end));
      return 1;
    }
  }

  if (!(*opened)->pager().EndRead().has_value()) {
    return 1;
  }
  return after_rowid_list == before_rowid_list + 1U &&
                 after_aggregate == before_aggregate + kAggregateRuns
             ? 0
             : 1;
} catch (...) {
  return 1;
}
