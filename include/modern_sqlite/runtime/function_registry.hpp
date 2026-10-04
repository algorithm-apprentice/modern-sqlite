#ifndef MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_
#define MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

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

class FunctionRegistry final {
 public:
  constexpr explicit FunctionRegistry(std::span<const ScalarFunction> functions) noexcept
      : functions_(functions) {}

  [[nodiscard]] Result<const ScalarFunction*> Resolve(std::string_view name,
                                                      std::size_t argument_count) const;
  [[nodiscard]] constexpr std::span<const ScalarFunction> functions() const noexcept {
    return functions_;
  }

 private:
  std::span<const ScalarFunction> functions_;
};

[[nodiscard]] const FunctionRegistry& CoreFunctionRegistry() noexcept;

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_RUNTIME_FUNCTION_REGISTRY_HPP_
