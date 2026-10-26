#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/collation.hpp"
#include "modern_sqlite/runtime/function_registry.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "tests/support/aligned_allocation.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
bool inject_failure = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0U ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] bool IsOutOfMemory(const modern_sqlite::Error& error) noexcept {
  return error.code() == modern_sqlite::ErrorCode::kOutOfMemory;
}

[[nodiscard]] const modern_sqlite::AggregateFunction* Resolve(std::string_view name,
                                                              std::size_t arguments) {
  const auto function = modern_sqlite::CoreFunctionRegistry().ResolveAggregate(name, arguments);
  return function.has_value() ? *function : nullptr;
}

struct StepOutcome {
  std::size_t allocations = 0;
  bool step_succeeded = false;
  bool final_succeeded = false;
  bool returned_oom = false;
};

[[nodiscard]] StepOutcome RunCount(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  const AggregateFunction* const function = Resolve("count", 0);
  if (function == nullptr) {
    return {};
  }
  AggregateState state = function->CreateState();
  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<AggregateStepEffect> stepped;
  try {
    stepped = function->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                             std::span<const SqlValue>{});
  } catch (...) {
    inject_failure = false;
    return {};
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (!stepped.has_value()) {
    return StepOutcome{
        .allocations = allocations,
        .returned_oom = IsOutOfMemory(stepped.error()),
    };
  }
  const Result<SqlValue> finalized = function->Finalize(state);
  return StepOutcome{
      .allocations = allocations,
      .step_succeeded = true,
      .final_succeeded = finalized.has_value(),
  };
}

[[nodiscard]] StepOutcome RunMin(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  const AggregateFunction* const function = Resolve("min", 1);
  if (function == nullptr) {
    return {};
  }
  AggregateState state = function->CreateState();
  const std::array arguments{SqlValue::Text(std::string(256U, 'x'))};
  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<AggregateStepEffect> stepped;
  try {
    stepped = function->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                             arguments);
  } catch (...) {
    inject_failure = false;
    return {};
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (!stepped.has_value()) {
    return StepOutcome{
        .allocations = allocations,
        .returned_oom = IsOutOfMemory(stepped.error()),
    };
  }
  const Result<SqlValue> finalized = function->Finalize(state);
  return StepOutcome{
      .allocations = allocations,
      .step_succeeded = true,
      .final_succeeded = finalized.has_value(),
  };
}

[[nodiscard]] StepOutcome RunNumeric(std::optional<std::size_t> failure, const std::string& text,
                                     modern_sqlite::SqlValueType expected_type) {
  using namespace modern_sqlite;
  const AggregateFunction* const function = Resolve("sum", 1);
  if (function == nullptr) {
    return {};
  }
  AggregateState state = function->CreateState();
  const std::array arguments{SqlValue::Text(text)};
  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<AggregateStepEffect> stepped;
  try {
    stepped = function->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                             arguments);
  } catch (...) {
    inject_failure = false;
    return {};
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (!stepped.has_value()) {
    const bool returned_oom = IsOutOfMemory(stepped.error());
    state.Reset();
    const auto repeated = function->Step(
        state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}}, arguments);
    if (!repeated.has_value()) {
      throw std::runtime_error{"numeric aggregate failed after allocation-error reset"};
    }
    const auto recovered = function->Finalize(state);
    if (!recovered.has_value() || recovered->type() != expected_type) {
      throw std::runtime_error{"numeric aggregate did not recover its result type"};
    }
    return StepOutcome{
        .allocations = allocations,
        .returned_oom = returned_oom,
    };
  }
  const Result<SqlValue> finalized = function->Finalize(state);
  return StepOutcome{
      .allocations = allocations,
      .step_succeeded = true,
      .final_succeeded = finalized.has_value() && finalized->type() == expected_type,
  };
}

[[nodiscard]] StepOutcome RunConcat(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  const AggregateFunction* const function = Resolve("group_concat", 1);
  if (function == nullptr) {
    return {};
  }
  AggregateState state = function->CreateState();
  const std::array arguments{SqlValue::Text(std::string(256U, 'x'))};
  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<AggregateStepEffect> stepped;
  try {
    stepped = function->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                             arguments);
  } catch (...) {
    inject_failure = false;
    return {};
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (!stepped.has_value()) {
    return StepOutcome{
        .allocations = allocations,
        .returned_oom = IsOutOfMemory(stepped.error()),
    };
  }
  const Result<SqlValue> finalized = function->Finalize(state);
  return StepOutcome{
      .allocations = allocations,
      .step_succeeded = true,
      .final_succeeded = finalized.has_value(),
      .returned_oom = !finalized.has_value() && IsOutOfMemory(finalized.error()),
  };
}

[[nodiscard]] StepOutcome RunFirstSeparatorConversion(std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  const AggregateFunction* const function = Resolve("group_concat", 2);
  if (function == nullptr) {
    return {};
  }
  AggregateState state = function->CreateState();
  const std::array arguments{
      SqlValue::Text("a"),
      SqlValue::Real(1.2345678901234567e+200),
  };
  allocation_index.store(0, std::memory_order_relaxed);
  if (failure.has_value()) {
    failing_allocation = *failure;
    inject_failure = true;
  }
  Result<AggregateStepEffect> stepped;
  try {
    stepped = function->Step(state, AggregateFunctionContext{BinaryCollation(), ByteCount{1024}},
                             arguments);
  } catch (...) {
    inject_failure = false;
    return {};
  }
  inject_failure = false;
  const std::size_t allocations = allocation_index.load(std::memory_order_relaxed);
  if (!stepped.has_value()) {
    return StepOutcome{
        .allocations = allocations,
        .returned_oom = IsOutOfMemory(stepped.error()),
    };
  }
  const Result<SqlValue> finalized = function->Finalize(state);
  return StepOutcome{
      .allocations = allocations,
      .step_succeeded = true,
      .final_succeeded = finalized.has_value(),
  };
}

[[nodiscard]] bool StepsWarmedNumericArgumentsWithoutAllocating() {
  using namespace modern_sqlite;
  const std::string numeric = "7" + std::string(256U, ' ');
  const std::array inputs{
      SqlValue::Text(numeric),
      SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(numeric))),
      SqlValue::Text("7x" + std::string(256U, ' ')),
  };
  const AggregateFunctionContext context{BinaryCollation(), ByteCount{1024}};
  for (const std::string_view name : {"sum", "total", "avg"}) {
    const AggregateFunction* const function = Resolve(name, 1);
    if (function == nullptr) {
      return false;
    }
    for (const SqlValue& input : inputs) {
      AggregateState state = function->CreateState();
      const std::array initial{SqlValue::Integer(1)};
      if (!function->Step(state, context, initial).has_value()) {
        return false;
      }
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = 0;
      inject_failure = true;
      Result<AggregateStepEffect> stepped;
      try {
        stepped = function->Step(state, context, std::span<const SqlValue>{&input, 1});
      } catch (...) {
        inject_failure = false;
        return false;
      }
      inject_failure = false;
      if (!stepped.has_value() || allocation_index.load(std::memory_order_relaxed) != 0U) {
        return false;
      }
      const auto result = function->Finalize(state);
      if (!result.has_value()) {
        return false;
      }
      if (name == "sum" && &input == &inputs.front()) {
        if (result->type() != SqlValueType::kInteger || result->integer_value() != 8) {
          return false;
        }
      } else if (result->type() != SqlValueType::kReal ||
                 result->real_value() != (name == "avg" ? 4.0 : 8.0)) {
        return false;
      }
    }
  }
  for (const std::string_view name : {"abs", "sign"}) {
    const auto function = CoreFunctionRegistry().Resolve(name, 1);
    if (!function.has_value()) {
      return false;
    }
    for (const SqlValue& input : inputs) {
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = 0;
      inject_failure = true;
      Result<SqlValue> result;
      try {
        result = (*function)->Invoke(ScalarFunctionContext{BinaryCollation()},
                                     std::span<const SqlValue>{&input, 1});
      } catch (...) {
        inject_failure = false;
        return false;
      }
      inject_failure = false;
      if (!result.has_value() || allocation_index.load(std::memory_order_relaxed) != 0U) {
        return false;
      }
      if (name == "abs") {
        if (result->type() != SqlValueType::kReal || result->real_value() != 7.0) {
          return false;
        }
      } else if (&input == &inputs.front()) {
        if (result->type() != SqlValueType::kInteger || result->integer_value() != 1) {
          return false;
        }
      } else if (result->type() != SqlValueType::kNull) {
        return false;
      }
    }
  }
  return true;
}

template <typename Runner>
[[nodiscard]] bool ExhaustImmediateFailures(Runner&& runner, std::size_t expected_allocations) {
  const StepOutcome baseline = runner(std::nullopt);
  if (!baseline.step_succeeded || !baseline.final_succeeded ||
      baseline.allocations != expected_allocations) {
    return false;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const StepOutcome outcome = runner(failure);
    if (outcome.step_succeeded || !outcome.returned_oom) {
      return false;
    }
  }
  return true;
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
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}

int main() try {
  if (!ExhaustImmediateFailures(RunCount, 1U)) {
    return 1;
  }
  if (!ExhaustImmediateFailures(RunMin, 2U)) {
    return 2;
  }
  if (!StepsWarmedNumericArgumentsWithoutAllocating()) {
    return 8;
  }
  struct NumericCase {
    std::string text;
    modern_sqlite::SqlValueType type;
  };
  const std::array numeric_cases{
      NumericCase{.text = std::string(256U, '7'), .type = modern_sqlite::SqlValueType::kReal},
      NumericCase{.text = std::string(256U, '0') + std::string{"1\0e2", 4},
                  .type = modern_sqlite::SqlValueType::kInteger},
      NumericCase{.text = std::string(256U, '0') + std::string{"9007199254740992\0x", 18},
                  .type = modern_sqlite::SqlValueType::kReal},
  };
  constexpr std::size_t kOriginalNumericAllocationCeiling = 2;
  constexpr std::size_t kNumericStateAllocations = 1;
  static_assert(kNumericStateAllocations <= kOriginalNumericAllocationCeiling);
  for (const NumericCase& numeric : numeric_cases) {
    if (!ExhaustImmediateFailures(
            [&numeric](std::optional<std::size_t> failure) {
              return RunNumeric(failure, numeric.text, numeric.type);
            },
            kNumericStateAllocations)) {
      return 3;
    }
  }
  if (!ExhaustImmediateFailures(RunFirstSeparatorConversion, 3U)) {
    return 4;
  }

  const StepOutcome baseline = RunConcat(std::nullopt);
  if (!baseline.step_succeeded || !baseline.final_succeeded || baseline.allocations != 2U) {
    return 5;
  }
  for (std::size_t failure = 0; failure < baseline.allocations; ++failure) {
    const StepOutcome outcome = RunConcat(failure);
    if (!outcome.returned_oom || (outcome.step_succeeded && outcome.final_succeeded)) {
      return 6;
    }
  }
  return 0;
} catch (...) {
  inject_failure = false;
  return 7;
}
