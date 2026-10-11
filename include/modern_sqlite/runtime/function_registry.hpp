#ifndef MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_
#define MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {

enum class FunctionArityKind : std::uint8_t {
  kExact,
  kAtLeast,
};

class FunctionArity final {
 public:
  [[nodiscard]] static constexpr FunctionArity Exact(std::size_t argument_count) noexcept {
    return FunctionArity{FunctionArityKind::kExact, argument_count};
  }

  [[nodiscard]] static constexpr FunctionArity AtLeast(
      std::size_t minimum_argument_count) noexcept {
    return FunctionArity{FunctionArityKind::kAtLeast, minimum_argument_count};
  }

  [[nodiscard]] constexpr FunctionArityKind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr bool is_exact() const noexcept {
    return kind_ == FunctionArityKind::kExact;
  }
  [[nodiscard]] constexpr std::size_t minimum_argument_count() const noexcept {
    return minimum_argument_count_;
  }
  [[nodiscard]] constexpr bool Accepts(std::size_t argument_count) const noexcept {
    return is_exact() ? argument_count == minimum_argument_count_
                      : argument_count >= minimum_argument_count_;
  }

 private:
  constexpr FunctionArity(FunctionArityKind kind, std::size_t minimum_argument_count) noexcept
      : kind_(kind), minimum_argument_count_(minimum_argument_count) {}

  FunctionArityKind kind_;
  std::size_t minimum_argument_count_;
};

enum class FunctionDeterminism : std::uint8_t {
  kDeterministic,
  kNonDeterministic,
};

enum class FunctionCollationUse : std::uint8_t {
  kNone,
  kRequired,
};

class ScalarFunctionContext final {
 public:
  explicit ScalarFunctionContext(const Collation& collation) noexcept : collation_(collation) {}

  [[nodiscard]] const Collation& collation() const noexcept { return collation_; }

 private:
  const Collation& collation_;
};

using ScalarFunctionCallback = Result<SqlValue> (*)(const ScalarFunctionContext&,
                                                    std::span<const SqlValue>);

class ScalarFunction final {
 public:
  constexpr ScalarFunction(std::string_view name, FunctionArity arity,
                           FunctionDeterminism determinism, FunctionCollationUse collation_use,
                           ScalarFunctionCallback callback) noexcept
      : name_(name),
        arity_(arity),
        determinism_(determinism),
        collation_use_(collation_use),
        callback_(callback) {}

  [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
  [[nodiscard]] constexpr FunctionArity arity() const noexcept { return arity_; }
  [[nodiscard]] constexpr bool is_deterministic() const noexcept {
    return determinism_ == FunctionDeterminism::kDeterministic;
  }
  [[nodiscard]] constexpr bool uses_collation() const noexcept {
    return collation_use_ == FunctionCollationUse::kRequired;
  }

  [[nodiscard]] Result<SqlValue> Invoke(const ScalarFunctionContext& context,
                                        std::span<const SqlValue> arguments) const;

 private:
  std::string_view name_;
  FunctionArity arity_;
  FunctionDeterminism determinism_;
  FunctionCollationUse collation_use_;
  ScalarFunctionCallback callback_;
};

enum class AggregateExtremumKind : std::uint8_t {
  kNone,
  kMinimum,
  kMaximum,
};

enum class AggregateStepEffect : std::uint8_t {
  kNoRepresentativePreference,
  kKeepRepresentative,
  kReplaceRepresentative,
};

class AggregateFunctionContext final {
 public:
  AggregateFunctionContext(const Collation& collation, ByteCount maximum_value_bytes) noexcept
      : collation_(collation), maximum_value_bytes_(maximum_value_bytes) {}

  [[nodiscard]] const Collation& collation() const noexcept { return collation_; }
  [[nodiscard]] ByteCount maximum_value_bytes() const noexcept { return maximum_value_bytes_; }

 private:
  const Collation& collation_;
  ByteCount maximum_value_bytes_;
};

class AggregateFunction;
class FunctionRegistry;

class AggregateState final {
 public:
  AggregateState(const AggregateState&) = delete;
  AggregateState& operator=(const AggregateState&) = delete;
  AggregateState(AggregateState&& other) noexcept;
  AggregateState& operator=(AggregateState&& other) noexcept;
  ~AggregateState();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool finalized() const noexcept;
  void Reset() noexcept;

 private:
  friend class AggregateFunction;

  struct Impl;

  explicit AggregateState(const AggregateFunction& function) noexcept;

  const AggregateFunction* function_;
  std::unique_ptr<Impl> impl_;
  bool finalized_ = false;
};

class AggregateFunction final {
 public:
  AggregateFunction(const AggregateFunction&) = delete;
  AggregateFunction& operator=(const AggregateFunction&) = delete;
  AggregateFunction(AggregateFunction&&) = delete;
  AggregateFunction& operator=(AggregateFunction&&) = delete;

  [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }
  [[nodiscard]] constexpr FunctionArity arity() const noexcept { return arity_; }
  [[nodiscard]] constexpr bool is_deterministic() const noexcept {
    return determinism_ == FunctionDeterminism::kDeterministic;
  }
  [[nodiscard]] constexpr bool uses_collation() const noexcept {
    return collation_use_ == FunctionCollationUse::kRequired;
  }
  [[nodiscard]] constexpr AggregateExtremumKind extremum_kind() const noexcept {
    return extremum_kind_;
  }

  [[nodiscard]] AggregateState CreateState() const noexcept;
  [[nodiscard]] Result<AggregateStepEffect> Step(AggregateState& state,
                                                 const AggregateFunctionContext& context,
                                                 std::span<const SqlValue> arguments) const;
  [[nodiscard]] Result<SqlValue> Finalize(AggregateState& state) const;

 private:
  friend const FunctionRegistry& CoreFunctionRegistry() noexcept;

  enum class Kind : std::uint8_t {
    kCount,
    kSum,
    kTotal,
    kAverage,
    kMinimum,
    kMaximum,
    kGroupConcat,
  };

  constexpr AggregateFunction(std::string_view name, FunctionArity arity,
                              FunctionDeterminism determinism, FunctionCollationUse collation_use,
                              AggregateExtremumKind extremum_kind, Kind kind) noexcept
      : name_(name),
        arity_(arity),
        determinism_(determinism),
        collation_use_(collation_use),
        extremum_kind_(extremum_kind),
        kind_(kind) {}

  std::string_view name_;
  FunctionArity arity_;
  FunctionDeterminism determinism_;
  FunctionCollationUse collation_use_;
  AggregateExtremumKind extremum_kind_;
  Kind kind_;
};

class FunctionRegistry final {
 public:
  constexpr explicit FunctionRegistry(
      std::span<const ScalarFunction> functions,
      std::span<const AggregateFunction> aggregate_functions = {}) noexcept
      : functions_(functions), aggregate_functions_(aggregate_functions) {}

  [[nodiscard]] Result<const ScalarFunction*> Resolve(std::string_view name,
                                                      std::size_t argument_count) const;
  [[nodiscard]] Result<const AggregateFunction*> ResolveAggregate(std::string_view name,
                                                                  std::size_t argument_count) const;
  [[nodiscard]] constexpr std::span<const ScalarFunction> functions() const noexcept {
    return functions_;
  }
  [[nodiscard]] constexpr std::span<const AggregateFunction> aggregate_functions() const noexcept {
    return aggregate_functions_;
  }

 private:
  std::span<const ScalarFunction> functions_;
  std::span<const AggregateFunction> aggregate_functions_;
};

[[nodiscard]] const FunctionRegistry& CoreFunctionRegistry() noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_
