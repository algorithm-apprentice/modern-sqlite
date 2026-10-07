#ifndef MODERN_SQLITE_VM_VM_HPP_
#define MODERN_SQLITE_VM_VM_HPP_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"

namespace modern_sqlite {

class Collation;
class FunctionRegistry;
class Pager;
class TransactionWriter;

enum class VmState : std::uint8_t {
  kReady,
  kRow,
  kDone,
  kError,
  kInvalid,
};

enum class VmStep : std::uint8_t {
  kRow,
  kDone,
};

struct VmLimits {
  std::size_t maximum_value_bytes = 1'000'000'000;
  std::uint64_t maximum_instructions_per_step = std::numeric_limits<std::uint64_t>::max();
};

class VmEnvironment final {
 public:
  VmEnvironment(const FunctionRegistry& functions,
                std::span<const Collation* const> collations) noexcept
      : functions_(&functions), collations_(collations) {}

  [[nodiscard]] static VmEnvironment Core() noexcept;

 private:
  friend class Vm;

  const FunctionRegistry* functions_;
  std::span<const Collation* const> collations_;
};

class VmExecutionContext final {
 public:
  VmExecutionContext(Pager& pager, std::uint64_t catalog_generation) noexcept
      : pager_(&pager), catalog_generation_(catalog_generation) {}

  VmExecutionContext(TransactionWriter& writer, std::uint64_t catalog_generation) noexcept;

 private:
  friend class Vm;

  Pager* pager_;
  std::uint64_t catalog_generation_;
  TransactionWriter* writer_ = nullptr;
};

class Vm final {
 public:
  [[nodiscard]] static Result<Vm> Create(const BytecodeProgram& program, VmEnvironment environment,
                                         VmLimits limits = {});
  static Result<Vm> Create(BytecodeProgram&& program, VmEnvironment environment,
                           VmLimits limits = {}) = delete;
  static Result<Vm> Create(const BytecodeProgram&& program, VmEnvironment environment,
                           VmLimits limits = {}) = delete;

  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;
  Vm(Vm&&) noexcept;
  Vm& operator=(Vm&&) noexcept;
  ~Vm();

  [[nodiscard]] Status Bind(ParameterId parameter, const SqlValue& value);
  [[nodiscard]] Status ClearBindings();
  [[nodiscard]] Status AttachExecutionContext(VmExecutionContext context);
  [[nodiscard]] Status DetachExecutionContext();
  [[nodiscard]] Result<VmStep> Step();
  [[nodiscard]] Status Reset();

  [[nodiscard]] VmState state() const noexcept;
  [[nodiscard]] bool has_execution_context() const noexcept;
  [[nodiscard]] std::span<const SqlValue> row() const noexcept;
  [[nodiscard]] std::span<const SqlValue> bindings() const noexcept;
  [[nodiscard]] std::uint64_t executed_instruction_count() const noexcept;

 private:
  struct Impl;

  explicit Vm(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_VM_VM_HPP_
