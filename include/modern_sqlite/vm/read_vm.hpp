#ifndef MODERN_SQLITE_VM_READ_VM_HPP_
#define MODERN_SQLITE_VM_READ_VM_HPP_

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
class ReadPager;

enum class ReadVmState : std::uint8_t {
  kReady,
  kRow,
  kDone,
  kError,
  kInvalid,
};

enum class ReadVmStep : std::uint8_t {
  kRow,
  kDone,
};

struct ReadVmLimits {
  std::size_t maximum_value_bytes = 1'000'000'000;
  std::uint64_t maximum_instructions_per_step = std::numeric_limits<std::uint64_t>::max();
};

class ReadVmEnvironment final {
 public:
  ReadVmEnvironment(ReadPager& pager, std::uint64_t catalog_generation,
                    const FunctionRegistry& functions,
                    std::span<const Collation* const> collations) noexcept
      : pager_(&pager),
        catalog_generation_(catalog_generation),
        functions_(&functions),
        collations_(collations) {}

  [[nodiscard]] static ReadVmEnvironment Core(ReadPager& pager,
                                              std::uint64_t catalog_generation) noexcept;

 private:
  friend class ReadVm;

  ReadPager* pager_;
  std::uint64_t catalog_generation_;
  const FunctionRegistry* functions_;
  std::span<const Collation* const> collations_;
};

class ReadVm final {
 public:
  [[nodiscard]] static Result<ReadVm> Create(const BytecodeProgram& program,
                                             ReadVmEnvironment environment,
                                             ReadVmLimits limits = {});
  static Result<ReadVm> Create(BytecodeProgram&& program, ReadVmEnvironment environment,
                               ReadVmLimits limits = {}) = delete;
  static Result<ReadVm> Create(const BytecodeProgram&& program, ReadVmEnvironment environment,
                               ReadVmLimits limits = {}) = delete;

  ReadVm(const ReadVm&) = delete;
  ReadVm& operator=(const ReadVm&) = delete;
  ReadVm(ReadVm&&) noexcept;
  ReadVm& operator=(ReadVm&&) noexcept;
  ~ReadVm();

  [[nodiscard]] Status Bind(ParameterId parameter, const SqlValue& value);
  [[nodiscard]] Status ClearBindings();
  [[nodiscard]] Result<ReadVmStep> Step();
  [[nodiscard]] Status Reset();

  [[nodiscard]] ReadVmState state() const noexcept;
  [[nodiscard]] std::span<const SqlValue> row() const noexcept;
  [[nodiscard]] std::span<const SqlValue> bindings() const noexcept;
  [[nodiscard]] std::uint64_t executed_instruction_count() const noexcept;

 private:
  struct Impl;

  explicit ReadVm(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_VM_READ_VM_HPP_
