#include "modern_sqlite/session/read_session.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/binder/bound_select.hpp"
#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/lowering/plan_lowering.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "read_session_internal.hpp"

namespace modern_sqlite {
namespace {

enum class StatementState : std::uint8_t {
  kReady,
  kRow,
  kDone,
  kError,
  kFinalized,
};

[[nodiscard]] Error SessionError(ErrorCode code, std::string message) {
  return Error::Create(code, std::move(message));
}

[[nodiscard]] Error Misuse(std::string message) {
  return SessionError(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Internal(std::string message) {
  return SessionError(ErrorCode::kInternal, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return SessionError(ErrorCode::kTooLarge, std::move(message));
}

void CleanupReadStateAfterAllocationFailure(Pager& pager) noexcept {
  try {
    [[maybe_unused]] const auto cleanup = pager.CleanupReadState();
  } catch (const std::bad_alloc&) {
    return;
  } catch (const std::length_error&) {
    return;
  }
}

[[nodiscard]] Error ParseFailure(const ParseError& error) {
  ErrorCode code = ErrorCode::kGeneric;
  switch (error.code) {
    case ParseErrorCode::kResourceLimitExceeded:
    case ParseErrorCode::kExpressionDepthExceeded:
    case ParseErrorCode::kParserDepthExceeded:
      code = ErrorCode::kTooLarge;
      break;
    case ParseErrorCode::kInternalInvariant:
      code = ErrorCode::kInternal;
      break;
    case ParseErrorCode::kIllegalToken:
    case ParseErrorCode::kUnexpectedToken:
    case ParseErrorCode::kUnsupportedSyntax:
      break;
  }

  std::string message{"parse failed with "};
  message.append(ParseErrorCodeName(error.code));
  message.append(" at byte ");
  message.append(std::to_string(error.span.begin().value()));
  message.append(": ");
  message.append(ParseErrorMessage(error));
  return SessionError(code, std::move(message));
}

[[nodiscard]] Error BindFailure(const BindError& error) {
  std::string message{"bind failed with "};
  message.append(BindErrorCodeName(error.code));
  message.append(" at byte ");
  message.append(std::to_string(error.span.begin().value()));
  if (!error.detail.empty()) {
    message.append(": ");
    message.append(error.detail);
  }
  return SessionError(error.base_error_code(), std::move(message));
}

[[nodiscard]] Error LogicalFailure(const LogicalPlanError& error) {
  std::string message{"logical planning failed with "};
  message.append(LogicalPlanErrorCodeName(error.code));
  if (!error.detail.empty()) {
    message.append(": ");
    message.append(error.detail);
  }
  return SessionError(error.base_error_code(), std::move(message));
}

[[nodiscard]] Error OptimizerFailure(const OptimizerError& error) {
  std::string message{"physical optimization failed with "};
  message.append(OptimizerErrorCodeName(error.code));
  if (!error.detail.empty()) {
    message.append(": ");
    message.append(error.detail);
  }
  return SessionError(error.base_error_code(), std::move(message));
}

[[nodiscard]] Error LoweringFailure(const PlanLoweringError& error) {
  std::string message{"read lowering failed with "};
  message.append(PlanLoweringErrorCodeName(error.code));
  if (!error.detail.empty()) {
    message.append(": ");
    message.append(error.detail);
  }
  return SessionError(error.base_error_code(), std::move(message));
}

[[nodiscard]] Error CombineErrors(const Error& primary, const Error& cleanup,
                                  std::string_view context) {
  std::string message{primary.message()};
  message.append("; ");
  message.append(context);
  message.append(" failed with ");
  message.append(ErrorCodeName(cleanup.code()));
  message.append(": ");
  message.append(cleanup.message());
  auto combined = Error::FromSqliteCode(primary.sqlite_code(), std::move(message));
  if (combined.has_value()) {
    return std::move(*combined);
  }
  return SessionError(primary.code(), std::string{primary.message()});
}

struct StatementExecution {
  StatementExecution(std::unique_ptr<BytecodeProgram> owned_program, Vm owned_vm) noexcept
      : program(std::move(owned_program)), vm(std::move(owned_vm)) {}

  std::unique_ptr<BytecodeProgram> program;
  Vm vm;
};

struct CompiledStatement {
  std::unique_ptr<StatementExecution> execution;
  std::vector<std::optional<std::string>> parameter_names;
};

}  // namespace

struct ReadSession::State final {
  State(std::unique_ptr<Vfs> owned_vfs, std::unique_ptr<Pager> owned_pager) noexcept
      : vfs(std::move(owned_vfs)), pager(std::move(owned_pager)) {}

  [[nodiscard]] Status CleanupBarrier() {
    if (active_readers != 0U) {
      return {};
    }
    return pager->CleanupReadState();
  }

  [[nodiscard]] Status RefreshCatalog() {
    if (!pager->in_read_transaction()) {
      return std::unexpected(Misuse("catalog refresh requires an active read transaction"));
    }

    bool reload = catalog == nullptr;
    if (!reload) {
      auto required = CatalogRequiresReload(*pager, *catalog);
      if (!required.has_value()) {
        return std::unexpected(std::move(required.error()));
      }
      reload = *required;
    }
    if (!reload) {
      return {};
    }

    const auto next = session_detail::NextCatalogGeneration(catalog_generation);
    if (!next.has_value()) {
      return std::unexpected(TooLarge("catalog generation is exhausted"));
    }
    CatalogLoadOptions options;
    options.generation = *next;
    auto loaded = LoadCatalog(*pager, options);
    if (!loaded.has_value()) {
      return std::unexpected(std::move(loaded.error()));
    }
    catalog = std::move(*loaded);
    catalog_generation = *next;
    return {};
  }

  [[nodiscard]] Status AcquireReadReference() {
    if (active_readers == std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(TooLarge("active read statement count is exhausted"));
    }
    if (active_readers == 0U) {
      auto cleanup = CleanupBarrier();
      if (!cleanup.has_value()) {
        return cleanup;
      }
      auto begun = pager->BeginRead();
      if (!begun.has_value()) {
        return begun;
      }
      auto refreshed = RefreshCatalog();
      if (!refreshed.has_value()) {
        auto ended = pager->EndRead();
        if (!ended.has_value()) {
          return std::unexpected(
              CombineErrors(refreshed.error(), ended.error(), "read transaction cleanup"));
        }
        return refreshed;
      }
    }
    ++active_readers;
    return {};
  }

  [[nodiscard]] Status ReleaseReadReference() {
    if (active_readers == 0U) {
      return std::unexpected(Internal("read transaction reference count underflow"));
    }
    --active_readers;
    if (active_readers != 0U) {
      return {};
    }
    return pager->EndRead();
  }

  [[nodiscard]] Result<CompiledStatement> Compile(SyntaxTree tree) {
    if (catalog == nullptr) {
      return std::unexpected(Internal("statement compilation requires a catalog"));
    }

    auto bound = BindSelectStatement(std::move(tree), catalog);
    if (!bound.has_value()) {
      return std::unexpected(BindFailure(bound.error()));
    }

    std::vector<std::optional<std::string>> parameter_names;
    parameter_names.reserve(bound->parameters().size());
    for (const BoundParameter& parameter : bound->parameters()) {
      parameter_names.push_back(parameter.name);
    }

    auto logical = BuildLogicalPlan(std::move(*bound));
    if (!logical.has_value()) {
      return std::unexpected(LogicalFailure(logical.error()));
    }
    auto physical = OptimizeLogicalPlan(std::move(*logical));
    if (!physical.has_value()) {
      return std::unexpected(OptimizerFailure(physical.error()));
    }
    auto lowered = LowerPlan(*physical);
    if (!lowered.has_value()) {
      return std::unexpected(LoweringFailure(lowered.error()));
    }

    auto program = std::make_unique<BytecodeProgram>(std::move(*lowered));
    auto vm = Vm::Create(*program, VmEnvironment::Core());
    if (!vm.has_value()) {
      return std::unexpected(std::move(vm.error()));
    }
    auto execution = std::make_unique<StatementExecution>(std::move(program), std::move(*vm));
    return CompiledStatement{
        .execution = std::move(execution),
        .parameter_names = std::move(parameter_names),
    };
  }

  std::unique_ptr<Vfs> vfs;
  std::unique_ptr<Pager> pager;
  CatalogSnapshotPtr catalog;
  std::uint64_t catalog_generation = 0;
  std::size_t active_readers = 0;
};

struct ReadStatement::Impl final {
  Impl(std::shared_ptr<ReadSession::State> shared_state, std::string retained_sql,
       CompiledStatement compiled) noexcept
      : state(std::move(shared_state)),
        sql(std::move(retained_sql)),
        parameter_names(std::move(compiled.parameter_names)),
        execution(std::move(compiled.execution)) {}

  ~Impl() { FinalizeNoThrow(); }

  [[nodiscard]] bool valid() const noexcept {
    return lifecycle != StatementState::kFinalized && execution != nullptr;
  }

  [[nodiscard]] Result<void> Recompile() {
    ParseResult parsed = ParseOne(Utf8View{sql});
    if (!parsed.has_value()) {
      return std::unexpected(ParseFailure(parsed.error()));
    }
    if (!parsed->tree.has_value() || parsed->next_offset.value() != sql.size()) {
      return std::unexpected(Internal("retained statement SQL did not reparse exactly"));
    }

    auto compiled = state->Compile(std::move(*parsed->tree));
    if (!compiled.has_value()) {
      return std::unexpected(std::move(compiled.error()));
    }
    if (compiled->parameter_names != parameter_names ||
        compiled->execution->vm.bindings().size() != execution->vm.bindings().size()) {
      return std::unexpected(Internal("automatic reprepare changed parameter metadata"));
    }

    const std::span<const SqlValue> bindings = execution->vm.bindings();
    for (std::size_t index = 0; index < bindings.size(); ++index) {
      auto bound = compiled->execution->vm.Bind(ParameterId(static_cast<std::uint32_t>(index)),
                                                bindings[index]);
      if (!bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
      }
    }
    execution = std::move(compiled->execution);
    return {};
  }

  [[nodiscard]] Status EnsureCurrentProgram() {
    if (state->catalog == nullptr) {
      return std::unexpected(Internal("statement execution requires a catalog"));
    }
    const CatalogVersion current = state->catalog->version();
    const SchemaVersionRequirement compiled = execution->program->schema_version();
    if (compiled.schema_cookie == current.schema_cookie &&
        compiled.generation == current.generation) {
      return {};
    }
    return Recompile();
  }

  [[nodiscard]] Result<ReadStep> Fail(Error error) {
    last_error = error;
    lifecycle = StatementState::kError;
    return std::unexpected(std::move(error));
  }

  [[nodiscard]] Result<ReadStep> FailAndRelease(Error error) {
    if (execution != nullptr && execution->vm.has_execution_context()) {
      auto detached = execution->vm.DetachExecutionContext();
      if (!detached.has_value()) {
        error = CombineErrors(error, detached.error(), "VM execution-context cleanup");
      }
    }
    if (holds_read_reference) {
      auto released = state->ReleaseReadReference();
      holds_read_reference = false;
      if (!released.has_value()) {
        error = CombineErrors(error, released.error(), "read transaction cleanup");
      }
    }
    return Fail(std::move(error));
  }

  [[nodiscard]] Result<ReadStep> Step() {
    if (!valid()) {
      return std::unexpected(Misuse("cannot step a finalized statement"));
    }

    auto cleanup = state->CleanupBarrier();
    if (!cleanup.has_value()) {
      return std::unexpected(std::move(cleanup.error()));
    }
    if (lifecycle != StatementState::kRow) {
      if (lifecycle == StatementState::kDone || lifecycle == StatementState::kError) {
        auto reset = execution->vm.Reset();
        if (!reset.has_value()) {
          return Fail(std::move(reset.error()));
        }
        last_error.reset();
        lifecycle = StatementState::kReady;
      }
    }

    if (lifecycle == StatementState::kReady) {
      if (execution->program->requires_database_snapshot()) {
        auto acquired = state->AcquireReadReference();
        if (!acquired.has_value()) {
          return Fail(std::move(acquired.error()));
        }
        holds_read_reference = true;
      }
      auto current = EnsureCurrentProgram();
      if (!current.has_value()) {
        return FailAndRelease(std::move(current.error()));
      }
      auto attached = execution->vm.AttachExecutionContext(
          VmExecutionContext{*state->pager, state->catalog_generation});
      if (!attached.has_value()) {
        return FailAndRelease(std::move(attached.error()));
      }
    }

    auto stepped = execution->vm.Step();
    if (!stepped.has_value()) {
      return FailAndRelease(std::move(stepped.error()));
    }
    if (*stepped == VmStep::kRow) {
      lifecycle = StatementState::kRow;
      return ReadStep::kRow;
    }

    auto detached = execution->vm.DetachExecutionContext();
    if (!detached.has_value()) {
      return FailAndRelease(std::move(detached.error()));
    }
    if (holds_read_reference) {
      auto released = state->ReleaseReadReference();
      holds_read_reference = false;
      if (!released.has_value()) {
        return Fail(std::move(released.error()));
      }
    }
    lifecycle = StatementState::kDone;
    return ReadStep::kDone;
  }

  [[nodiscard]] Status Reset() {
    if (!valid()) {
      return std::unexpected(Misuse("cannot reset a finalized statement"));
    }

    auto cleanup = state->CleanupBarrier();
    if (!cleanup.has_value()) {
      if (last_error.has_value()) {
        return std::unexpected(
            CombineErrors(*last_error, cleanup.error(), "pending read-state cleanup"));
      }
      return cleanup;
    }

    std::optional<Error> previous;
    if (lifecycle == StatementState::kError && last_error.has_value()) {
      previous = std::move(last_error);
      last_error.reset();
    }
    auto reset = execution->vm.Reset();
    if (!reset.has_value()) {
      return std::unexpected(std::move(reset.error()));
    }
    if (holds_read_reference) {
      auto released = state->ReleaseReadReference();
      holds_read_reference = false;
      if (!released.has_value()) {
        Error error = previous.has_value()
                          ? CombineErrors(*previous, released.error(), "read transaction cleanup")
                          : std::move(released.error());
        last_error = error;
        lifecycle = StatementState::kError;
        return std::unexpected(std::move(error));
      }
    }
    lifecycle = StatementState::kReady;
    if (previous.has_value()) {
      return std::unexpected(std::move(*previous));
    }
    return {};
  }

  [[nodiscard]] Status Finalize() {
    if (lifecycle == StatementState::kFinalized) {
      return {};
    }

    std::optional<Error> primary;
    if (lifecycle == StatementState::kError && last_error.has_value()) {
      primary = std::move(last_error);
    }
    if (execution != nullptr) {
      auto reset = execution->vm.Reset();
      if (!reset.has_value() && !primary.has_value()) {
        primary = std::move(reset.error());
      }
    }
    if (holds_read_reference) {
      auto released = state->ReleaseReadReference();
      holds_read_reference = false;
      if (!released.has_value()) {
        primary = primary.has_value() ? std::optional<Error>{CombineErrors(
                                            *primary, released.error(), "read transaction cleanup")}
                                      : std::optional<Error>{std::move(released.error())};
      }
    } else if (state != nullptr) {
      auto cleanup = state->CleanupBarrier();
      if (!cleanup.has_value()) {
        primary = primary.has_value()
                      ? std::optional<Error>{CombineErrors(*primary, cleanup.error(),
                                                           "pending read-state cleanup")}
                      : std::optional<Error>{std::move(cleanup.error())};
      }
    }

    execution.reset();
    parameter_names.clear();
    sql.clear();
    last_error.reset();
    lifecycle = StatementState::kFinalized;
    state.reset();
    if (primary.has_value()) {
      return std::unexpected(std::move(*primary));
    }
    return {};
  }

  void FailForOutOfMemory() noexcept {
    if (!valid()) {
      return;
    }
    try {
      if (execution != nullptr) {
        [[maybe_unused]] const auto reset = execution->vm.Reset();
      }
      if (holds_read_reference && state != nullptr) {
        holds_read_reference = false;
        [[maybe_unused]] const auto released = state->ReleaseReadReference();
      }
      last_error = Error::OutOfMemory();
      lifecycle = StatementState::kError;
    } catch (const std::bad_alloc&) {
      last_error.reset();
      lifecycle = StatementState::kError;
    } catch (const std::length_error&) {
      last_error.reset();
      lifecycle = StatementState::kError;
    }
  }

  void FinalizeNoThrow() noexcept {
    if (lifecycle == StatementState::kFinalized) {
      return;
    }
    try {
      if (execution != nullptr) {
        [[maybe_unused]] const auto reset = execution->vm.Reset();
      }
      if (holds_read_reference && state != nullptr) {
        holds_read_reference = false;
        [[maybe_unused]] const auto released = state->ReleaseReadReference();
      } else if (state != nullptr) {
        [[maybe_unused]] const auto cleanup = state->CleanupBarrier();
      }
    } catch (const std::bad_alloc&) {
      holds_read_reference = false;
    } catch (const std::length_error&) {
      holds_read_reference = false;
    }
    holds_read_reference = false;
    execution.reset();
    parameter_names.clear();
    sql.clear();
    last_error.reset();
    lifecycle = StatementState::kFinalized;
    state.reset();
  }

  std::shared_ptr<ReadSession::State> state;
  std::string sql;
  std::vector<std::optional<std::string>> parameter_names;
  std::unique_ptr<StatementExecution> execution;
  std::optional<Error> last_error;
  StatementState lifecycle = StatementState::kReady;
  bool holds_read_reference = false;
};

ReadStatement::ReadStatement(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ReadStatement::ReadStatement(ReadStatement&&) noexcept = default;

ReadStatement& ReadStatement::operator=(ReadStatement&&) noexcept = default;

ReadStatement::~ReadStatement() = default;

bool ReadStatement::valid() const noexcept { return impl_ != nullptr && impl_->valid(); }

std::size_t ReadStatement::parameter_count() const noexcept {
  return valid() ? impl_->parameter_names.size() : 0U;
}

std::optional<std::string_view> ReadStatement::parameter_name(
    std::size_t parameter_index_value) const noexcept {
  if (!valid() || parameter_index_value == 0U ||
      parameter_index_value > impl_->parameter_names.size()) {
    return std::nullopt;
  }
  const auto& name = impl_->parameter_names[parameter_index_value - 1U];
  if (!name.has_value()) {
    return std::nullopt;
  }
  return *name;
}

std::size_t ReadStatement::parameter_index(std::string_view name) const noexcept {
  if (!valid()) {
    return 0U;
  }
  for (std::size_t index = 0; index < impl_->parameter_names.size(); ++index) {
    const auto& parameter_name = impl_->parameter_names[index];
    if (parameter_name == name) {
      return index + 1U;
    }
  }
  return 0U;
}

std::span<const ResultColumnMetadata> ReadStatement::result_columns() const noexcept {
  if (!valid()) {
    return {};
  }
  return impl_->execution->program->result_columns();
}

std::span<const SqlValue> ReadStatement::row() const noexcept {
  return valid() ? impl_->execution->vm.row() : std::span<const SqlValue>{};
}

Status ReadStatement::Bind(std::size_t parameter_index_value, const SqlValue& value) {
  try {
    if (!valid()) {
      return std::unexpected(Misuse("cannot bind a finalized statement"));
    }
    if (impl_->lifecycle != StatementState::kReady) {
      return std::unexpected(Misuse("parameters can only be bound while a statement is ready"));
    }
    if (parameter_index_value == 0U || parameter_index_value > impl_->parameter_names.size()) {
      return std::unexpected(
          SessionError(ErrorCode::kOutOfRange, "parameter index is out of range"));
    }
    return impl_->execution->vm.Bind(
        ParameterId(static_cast<std::uint32_t>(parameter_index_value - 1U)), value);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<ReadStep> ReadStatement::Step() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("cannot step a moved-from statement"));
    }
    return impl_->Step();
  } catch (const std::bad_alloc&) {
    if (impl_ != nullptr) {
      impl_->FailForOutOfMemory();
    }
    return std::unexpected(Error::OutOfMemory());
  }
}

Status ReadStatement::Reset() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("cannot reset a moved-from statement"));
    }
    return impl_->Reset();
  } catch (const std::bad_alloc&) {
    if (impl_ != nullptr) {
      impl_->FailForOutOfMemory();
    }
    return std::unexpected(Error::OutOfMemory());
  }
}

Status ReadStatement::Finalize() {
  try {
    if (impl_ == nullptr) {
      return {};
    }
    return impl_->Finalize();
  } catch (const std::bad_alloc&) {
    impl_->FinalizeNoThrow();
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<ReadSession> ReadSession::Open(std::string_view path) {
  try {
    return Open(std::make_unique<PosixVfs>(), path);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<ReadSession> ReadSession::Open(std::unique_ptr<Vfs> vfs, std::string_view path) {
  try {
    if (vfs == nullptr) {
      return std::unexpected(Misuse("read session requires an owned VFS"));
    }
    auto pager = Pager::Open(*vfs, path);
    if (!pager.has_value()) {
      return std::unexpected(std::move(pager.error()));
    }
    auto state = std::make_shared<State>(std::move(vfs), std::move(*pager));
    return ReadSession(std::move(state));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

ReadSession::ReadSession(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

ReadSession::ReadSession(ReadSession&&) noexcept = default;

ReadSession& ReadSession::operator=(ReadSession&&) noexcept = default;

ReadSession::~ReadSession() = default;

bool ReadSession::valid() const noexcept { return state_ != nullptr; }

Result<ReadPrepareOutput> ReadSession::Prepare(Utf8View source) {
  bool temporary_transaction = false;
  try {
    if (!valid()) {
      return std::unexpected(Misuse("cannot prepare on a moved-from read session"));
    }

    ParseResult parsed = ParseOne(source);
    if (!parsed.has_value()) {
      return std::unexpected(ParseFailure(parsed.error()));
    }
    if (!parsed->tree.has_value()) {
      return ReadPrepareOutput{
          .statement = std::nullopt,
          .next_offset = parsed->next_offset,
      };
    }
    if (!std::holds_alternative<SelectStatement>(parsed->tree->statement())) {
      return std::unexpected(
          SessionError(ErrorCode::kGeneric, "read session only supports SELECT statements"));
    }

    const std::size_t consumed = parsed->next_offset.value();
    if (consumed > source.size_bytes()) {
      return std::unexpected(Internal("parser returned an out-of-range tail offset"));
    }
    std::string retained_sql{source.bytes().substr(0, consumed)};

    if (state_->active_readers == 0U) {
      auto cleanup = state_->CleanupBarrier();
      if (!cleanup.has_value()) {
        return std::unexpected(std::move(cleanup.error()));
      }
      auto begun = state_->pager->BeginRead();
      if (!begun.has_value()) {
        return std::unexpected(std::move(begun.error()));
      }
      temporary_transaction = true;
    }

    auto refreshed = state_->RefreshCatalog();
    if (!refreshed.has_value()) {
      if (temporary_transaction) {
        auto ended = state_->pager->EndRead();
        temporary_transaction = false;
        if (!ended.has_value()) {
          return std::unexpected(
              CombineErrors(refreshed.error(), ended.error(), "read transaction cleanup"));
        }
      }
      return std::unexpected(std::move(refreshed.error()));
    }

    auto compiled = state_->Compile(std::move(*parsed->tree));
    if (!compiled.has_value()) {
      if (temporary_transaction) {
        auto ended = state_->pager->EndRead();
        temporary_transaction = false;
        if (!ended.has_value()) {
          return std::unexpected(
              CombineErrors(compiled.error(), ended.error(), "read transaction cleanup"));
        }
      }
      return std::unexpected(std::move(compiled.error()));
    }

    if (temporary_transaction) {
      auto ended = state_->pager->EndRead();
      temporary_transaction = false;
      if (!ended.has_value()) {
        return std::unexpected(std::move(ended.error()));
      }
    }

    auto impl = std::make_unique<ReadStatement::Impl>(state_, std::move(retained_sql),
                                                      std::move(*compiled));
    ReadPrepareOutput output{
        .statement = std::nullopt,
        .next_offset = parsed->next_offset,
    };
    output.statement.emplace(ReadStatement(std::move(impl)));
    return output;
  } catch (const std::bad_alloc&) {
    if (temporary_transaction && state_ != nullptr) {
      CleanupReadStateAfterAllocationFailure(*state_->pager);
    }
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace modern_sqlite
