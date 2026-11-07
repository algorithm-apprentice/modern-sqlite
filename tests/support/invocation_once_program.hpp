#ifndef MODERN_SQLITE_TEST_SUPPORT_INVOCATION_ONCE_PROGRAM_HPP_
#define MODERN_SQLITE_TEST_SUPPORT_INVOCATION_ONCE_PROGRAM_HPP_

#include "modern_sqlite/bytecode/program.hpp"

namespace modern_sqlite::test_support {

[[nodiscard]] inline ProgramInput InvocationOnceCallableProgramInput() {
  ProgramInput input;
  input.register_count = 4;
  input.once_site_count = 1;
  input.constants.push_back(SqlValue{});
  input.constants.push_back(SqlValue::Integer(7));
  input.result_columns.push_back(ResultColumnMetadata{
      .name = "value", .declared_type = "INTEGER", .affinity = TypeAffinity::kInteger});
  input.subroutines.push_back(
      SubroutineDescriptor{.entry = InstructionAddress{18}, .end = InstructionAddress{21}});
  input.coroutines.push_back(CoroutineDescriptor{.entry = InstructionAddress{10},
                                                 .end = InstructionAddress{14},
                                                 .first_output = RegisterId{1},
                                                 .output_count = 1});
  input.coroutines.push_back(CoroutineDescriptor{.entry = InstructionAddress{14},
                                                 .end = InstructionAddress{18},
                                                 .first_output = RegisterId{2},
                                                 .output_count = 1});
  input.instructions = {
      LoadConstantInstruction{.constant = ConstantId{0}, .output = RegisterId{0}},
      InitCoroutineInstruction{.coroutine = CoroutineId{0}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{0},
                                 .exhausted_target = InstructionAddress{5},
                                 .first_destination = RegisterId{3}},
      ResultRowInstruction{.first = RegisterId{3}, .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{0}},
      InitCoroutineInstruction{.coroutine = CoroutineId{1}},
      ResumeCoroutineInstruction{.coroutine = CoroutineId{1},
                                 .exhausted_target = InstructionAddress{9},
                                 .first_destination = RegisterId{3}},
      ResultRowInstruction{.first = RegisterId{3}, .count = 1},
      CancelCoroutineInstruction{.coroutine = CoroutineId{1}},
      HaltInstruction{},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      CopyInstruction{.input = RegisterId{0}, .output = RegisterId{1}},
      YieldCoroutineInstruction{.coroutine = CoroutineId{0}},
      EndCoroutineInstruction{.coroutine = CoroutineId{0}},
      CallSubroutineInstruction{.subroutine = SubroutineId{0}},
      CopyInstruction{.input = RegisterId{0}, .output = RegisterId{2}},
      YieldCoroutineInstruction{.coroutine = CoroutineId{1}},
      EndCoroutineInstruction{.coroutine = CoroutineId{1}},
      OnceInstruction{.site = OnceId{0}, .already_seen_target = InstructionAddress{20}},
      LoadConstantInstruction{.constant = ConstantId{1}, .output = RegisterId{0}},
      ReturnSubroutineInstruction{.subroutine = SubroutineId{0}},
  };
  return input;
}

}  // namespace modern_sqlite::test_support

#endif  // MODERN_SQLITE_TEST_SUPPORT_INVOCATION_ONCE_PROGRAM_HPP_
