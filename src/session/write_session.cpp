#include "modern_sqlite/session/write_session.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/lowering/plan_lowering.hpp"
#include "modern_sqlite/optimizer/physical_plan.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/planner/logical_plan.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "modern_sqlite/transaction/transaction_coordinator.hpp"
#include "modern_sqlite/vm/vm.hpp"
#include "session_internal.hpp"

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
  std::string message{"plan lowering failed with "};
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
  return combined.has_value() ? std::move(*combined)
                              : SessionError(primary.code(), std::string{primary.message()});
}

using TransactionCommand =
    std::variant<BoundBeginTransaction, BoundCommitTransaction, BoundRollbackTransaction,
                 BoundSavepoint, BoundReleaseSavepoint, BoundRollbackToSavepoint>;

struct StatementExecution {
  StatementExecution(std::unique_ptr<BytecodeProgram> owned_program, Vm owned_vm) noexcept
      : program(std::move(owned_program)), vm(std::move(owned_vm)) {}

  std::unique_ptr<BytecodeProgram> program;
  Vm vm;
};

struct CompiledStatement {
  std::unique_ptr<StatementExecution> execution;
  std::optional<TransactionCommand> transaction;
  std::vector<std::optional<std::string>> parameter_names;
  bool creates_schema = false;
};

template <typename Statement>
void CopyParameterNames(const Statement& statement,
                        std::vector<std::optional<std::string>>& output) {
  output.reserve(statement.parameters().size());
  for (const BoundParameter& parameter : statement.parameters()) {
    output.push_back(parameter.name);
  }
}

[[nodiscard]] std::vector<std::optional<std::string>> ParameterNames(
    const BoundStatement& statement) {
  std::vector<std::optional<std::string>> result;
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BoundSelect> || std::is_same_v<T, BoundInsert> ||
                      std::is_same_v<T, BoundUpdate> || std::is_same_v<T, BoundDelete>) {
          CopyParameterNames(value, result);
        }
      },
      statement);
  return result;
}

[[nodiscard]] StatementAccess ToStatementAccess(ProgramTransactionAccess access) noexcept {
  return access == ProgramTransactionAccess::kWrite ? StatementAccess::kWrite
                                                    : StatementAccess::kRead;
}

[[nodiscard]] StatementRollbackMode ToRollbackMode(ProgramRollbackMode mode) noexcept {
  return mode == ProgramRollbackMode::kStatement ? StatementRollbackMode::kStatement
                                                 : StatementRollbackMode::kTransaction;
}

[[nodiscard]] Error SqlTransactionError(Error error) {
  if (error.code() != ErrorCode::kMisuse) {
    return error;
  }
  return SessionError(ErrorCode::kGeneric, std::string{error.message()});
}

}  // namespace

struct WriteSession::State final {
  State(std::unique_ptr<Vfs> owned_vfs, Pager& owned_pager,
        TransactionCoordinator owned_coordinator) noexcept
      : vfs(std::move(owned_vfs)), pager(&owned_pager), coordinator(std::move(owned_coordinator)) {}

  ~State() {
    if (coordinator.valid() && (!coordinator.autocommit() || coordinator.statement_active())) {
      try {
        [[maybe_unused]] const Status rolled_back = coordinator.Rollback();
      } catch (const std::bad_alloc&) {
        return;
      } catch (const std::length_error&) {
        return;
      }
    }
  }

  [[nodiscard]] Status RefreshCatalog() {
    if (coordinator.statement_active()) {
      return catalog != nullptr
                 ? Status{}
                 : Status{std::unexpected(SessionError(
                       ErrorCode::kBusy, "catalog load is blocked by an active statement"))};
    }
    auto statement =
        coordinator.BeginStatement(TransactionStatementOptions{.access = StatementAccess::kRead});
    if (!statement.has_value()) {
      return std::unexpected(std::move(statement.error()));
    }

    bool reload = catalog == nullptr;
    if (!reload) {
      auto required = CatalogRequiresReload(*pager, *catalog);
      if (!required.has_value()) {
        auto rolled_back = statement->Rollback();
        return !rolled_back.has_value()
                   ? Status{std::unexpected(CombineErrors(required.error(), rolled_back.error(),
                                                          "catalog probe rollback"))}
                   : Status{std::unexpected(std::move(required.error()))};
      }
      reload = *required;
    }
    if (reload) {
      const auto next = session_detail::NextCatalogGeneration(catalog_generation);
      if (!next.has_value()) {
        auto rolled_back = statement->Rollback();
        Error error = TooLarge("catalog generation is exhausted");
        return !rolled_back.has_value() ? Status{std::unexpected(CombineErrors(
                                              error, rolled_back.error(), "catalog load rollback"))}
                                        : Status{std::unexpected(std::move(error))};
      }
      auto loaded = LoadCatalog(*pager, CatalogLoadOptions{.generation = *next});
      if (!loaded.has_value()) {
        auto rolled_back = statement->Rollback();
        return !rolled_back.has_value()
                   ? Status{std::unexpected(CombineErrors(loaded.error(), rolled_back.error(),
                                                          "catalog load rollback"))}
                   : Status{std::unexpected(std::move(loaded.error()))};
      }
      catalog = std::move(*loaded);
      catalog_generation = *next;
    }
    return statement->Succeed();
  }

  [[nodiscard]] Result<CompiledStatement> Compile(SyntaxTree tree) {
    if (catalog == nullptr) {
      return std::unexpected(Internal("statement compilation requires a catalog"));
    }
    auto bound = BindStatement(std::move(tree), catalog);
    if (!bound.has_value()) {
      return std::unexpected(BindFailure(bound.error()));
    }
    std::vector<std::optional<std::string>> parameter_names = ParameterNames(*bound);
    auto logical = BuildLogicalStatementPlan(std::move(*bound));
    if (!logical.has_value()) {
      return std::unexpected(LogicalFailure(logical.error()));
    }
    auto physical = OptimizeLogicalStatementPlan(std::move(*logical));
    if (!physical.has_value()) {
      return std::unexpected(OptimizerFailure(physical.error()));
    }

    CompiledStatement output{
        .execution = nullptr,
        .transaction = std::nullopt,
        .parameter_names = std::move(parameter_names),
        .creates_schema = false,
    };
    if (const auto* read = std::get_if<PhysicalPlan>(&*physical); read != nullptr) {
      auto lowered = LowerPlan(*read);
      if (!lowered.has_value()) {
        return std::unexpected(LoweringFailure(lowered.error()));
      }
      auto program = std::make_unique<BytecodeProgram>(std::move(*lowered));
      auto vm = Vm::Create(*program, VmEnvironment::Core());
      if (!vm.has_value()) {
        return std::unexpected(std::move(vm.error()));
      }
      output.execution = std::make_unique<StatementExecution>(std::move(program), std::move(*vm));
      return output;
    }
    if (const auto* mutation = std::get_if<PhysicalMutationPlan>(&*physical); mutation != nullptr) {
      if (const auto* create = std::get_if<PhysicalCreateTableMutation>(&mutation->payload());
          create != nullptr) {
        output.creates_schema = !create->no_op;
      }
      auto lowered = LowerPlan(*mutation);
      if (!lowered.has_value()) {
        return std::unexpected(LoweringFailure(lowered.error()));
      }
      auto program = std::make_unique<BytecodeProgram>(std::move(*lowered));
      auto vm = Vm::Create(*program, VmEnvironment::Core());
      if (!vm.has_value()) {
        return std::unexpected(std::move(vm.error()));
      }
      output.execution = std::make_unique<StatementExecution>(std::move(program), std::move(*vm));
      return output;
    }
    std::visit(
        [&](auto&& command) {
          using T = std::decay_t<decltype(command)>;
          if constexpr (!std::is_same_v<T, PhysicalPlan> &&
                        !std::is_same_v<T, PhysicalMutationPlan>) {
            output.transaction.emplace(std::forward<decltype(command)>(command));
          }
        },
        std::move(*physical));
    if (!output.transaction.has_value()) {
      return std::unexpected(Internal("physical statement plan has no executable payload"));
    }
    return output;
  }

  std::unique_ptr<Vfs> vfs;
  Pager* pager;
  TransactionCoordinator coordinator;
  CatalogSnapshotPtr catalog;
  std::uint64_t catalog_generation = 0;
  std::uint64_t changes = 0;
  std::int64_t last_insert_rowid = 0;
};

struct WriteStatement::Impl final {
  Impl(std::shared_ptr<WriteSession::State> shared_state, std::string retained_sql,
       CompiledStatement compiled) noexcept
      : state(std::move(shared_state)),
        sql(std::move(retained_sql)),
        parameter_names(std::move(compiled.parameter_names)),
        execution(std::move(compiled.execution)),
        transaction(std::move(compiled.transaction)),
        creates_schema(compiled.creates_schema) {}

  ~Impl() { FinalizeNoThrow(); }

  [[nodiscard]] bool valid() const noexcept {
    return lifecycle != StatementState::kFinalized &&
           (execution != nullptr || transaction.has_value());
  }

  [[nodiscard]] Result<void> Recompile() {
    ParseResult parsed = ParseOne(Utf8View{sql});
    if (!parsed.has_value()) {
      return std::unexpected(ParseFailure(parsed.error()));
    }
    if (!parsed->tree.has_value() || parsed->next_offset.value() != sql.size()) {
      return std::unexpected(Internal("retained writable statement did not reparse exactly"));
    }
    auto compiled = state->Compile(std::move(*parsed->tree));
    if (!compiled.has_value()) {
      return std::unexpected(std::move(compiled.error()));
    }
    if (compiled->parameter_names != parameter_names ||
        (execution == nullptr) != (compiled->execution == nullptr) ||
        transaction.has_value() != compiled->transaction.has_value()) {
      return std::unexpected(Internal("automatic reprepare changed statement metadata"));
    }
    if (execution != nullptr) {
      const std::span<const SqlValue> bindings = execution->vm.bindings();
      for (std::size_t index = 0; index < bindings.size(); ++index) {
        auto rebound = compiled->execution->vm.Bind(ParameterId(static_cast<std::uint32_t>(index)),
                                                    bindings[index]);
        if (!rebound.has_value()) {
          return std::unexpected(std::move(rebound.error()));
        }
      }
    }
    execution = std::move(compiled->execution);
    transaction = std::move(compiled->transaction);
    creates_schema = compiled->creates_schema;
    return {};
  }

  [[nodiscard]] Status EnsureCurrentProgram() {
    if (execution == nullptr) {
      return {};
    }
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

  [[nodiscard]] Result<WriteStep> Fail(Error error) {
    last_error = error;
    lifecycle = StatementState::kError;
    return std::unexpected(std::move(error));
  }

  [[nodiscard]] Status RollbackActiveStatement() {
    if (!active_statement.has_value()) {
      return {};
    }
    Status rolled_back = active_statement->Rollback();
    if (!rolled_back.has_value()) {
      return rolled_back;
    }
    active_statement.reset();
    success_pending = false;
    pending_candidate.reset();
    pending_changes.reset();
    return {};
  }

  [[nodiscard]] Result<WriteStep> FailAndRollback(Error error) {
    if (execution != nullptr && execution->vm.has_execution_context()) {
      auto detached = execution->vm.DetachExecutionContext();
      if (!detached.has_value()) {
        error = CombineErrors(error, detached.error(), "VM execution-context cleanup");
      }
    }
    auto rolled_back = RollbackActiveStatement();
    if (!rolled_back.has_value()) {
      error = CombineErrors(error, rolled_back.error(), "statement rollback");
    }
    if (execution != nullptr && execution->program->mutation_result().publishes_changes) {
      state->changes = 0;
    }
    return Fail(std::move(error));
  }

  [[nodiscard]] Status CompletePendingSuccess() {
    if (!success_pending || !active_statement.has_value()) {
      return {};
    }
    auto succeeded = active_statement->Succeed();
    if (!succeeded.has_value()) {
      return succeeded;
    }
    active_statement.reset();
    success_pending = false;
    if (pending_candidate != nullptr) {
      state->catalog = std::move(pending_candidate);
      state->catalog_generation = state->catalog->version().generation;
    }
    if (pending_changes.has_value()) {
      state->changes = *pending_changes;
    }
    pending_changes.reset();
    return {};
  }

  [[nodiscard]] Status ExecuteTransactionCommand() {
    if (!transaction.has_value()) {
      return std::unexpected(Internal("transaction command is missing"));
    }
    Status status = std::visit(
        [&](const auto& command) -> Status {
          using T = std::decay_t<decltype(command)>;
          if constexpr (std::is_same_v<T, BoundBeginTransaction>) {
            return state->coordinator.Begin(command.mode == BeginTransactionMode::kImmediate
                                                ? TransactionMode::kImmediate
                                                : TransactionMode::kDeferred);
          } else if constexpr (std::is_same_v<T, BoundCommitTransaction>) {
            return state->coordinator.Commit();
          } else if constexpr (std::is_same_v<T, BoundRollbackTransaction>) {
            return state->coordinator.Rollback();
          } else if constexpr (std::is_same_v<T, BoundSavepoint>) {
            return state->coordinator.Savepoint(Utf8View{command.name});
          } else if constexpr (std::is_same_v<T, BoundReleaseSavepoint>) {
            return state->coordinator.Release(Utf8View{command.name});
          } else {
            static_assert(std::is_same_v<T, BoundRollbackToSavepoint>);
            return state->coordinator.RollbackTo(Utf8View{command.name});
          }
        },
        *transaction);
    if (!status.has_value()) {
      return std::unexpected(SqlTransactionError(std::move(status.error())));
    }
    return {};
  }

  [[nodiscard]] Result<WriteStep> Step() {
    if (!valid()) {
      return std::unexpected(Misuse("cannot step a finalized writable statement"));
    }
    bool retrying_transaction_cleanup = false;
    if (lifecycle != StatementState::kRow &&
        (lifecycle == StatementState::kDone || lifecycle == StatementState::kError)) {
      if (lifecycle == StatementState::kError && transaction.has_value()) {
        last_error.reset();
        lifecycle = StatementState::kReady;
        retrying_transaction_cleanup = true;
      } else {
        auto reset = ResetForReexecution(false);
        if (!reset.has_value()) {
          return Fail(std::move(reset.error()));
        }
      }
    }
    if (lifecycle == StatementState::kReady) {
      if (!retrying_transaction_cleanup) {
        auto refreshed = state->RefreshCatalog();
        if (!refreshed.has_value()) {
          return Fail(std::move(refreshed.error()));
        }
        auto current = EnsureCurrentProgram();
        if (!current.has_value()) {
          return Fail(std::move(current.error()));
        }
      }
      if (transaction.has_value()) {
        auto executed = ExecuteTransactionCommand();
        if (!executed.has_value()) {
          return Fail(std::move(executed.error()));
        }
        lifecycle = StatementState::kDone;
        return WriteStep::kDone;
      }

      if (creates_schema) {
        const auto next = session_detail::NextCatalogGeneration(state->catalog_generation);
        if (!next.has_value()) {
          return Fail(TooLarge("catalog generation is exhausted"));
        }
        candidate_generation = *next;
      }
      const BytecodeProgram& program = *execution->program;
      const StatementAccess access = ToStatementAccess(program.transaction_access());
      auto statement = state->coordinator.BeginStatement(TransactionStatementOptions{
          .access = access,
          .rollback = access == StatementAccess::kRead ? StatementRollbackMode::kStatement
                                                       : ToRollbackMode(program.rollback_mode()),
      });
      if (!statement.has_value()) {
        return Fail(std::move(statement.error()));
      }
      active_statement.emplace(std::move(*statement));
      Status attached = program.transaction_access() == ProgramTransactionAccess::kWrite
                            ? execution->vm.AttachExecutionContext(VmExecutionContext{
                                  *active_statement->writer(), state->catalog_generation})
                            : execution->vm.AttachExecutionContext(
                                  VmExecutionContext{*state->pager, state->catalog_generation});
      if (!attached.has_value()) {
        return FailAndRollback(std::move(attached.error()));
      }
    }

    auto stepped = execution->vm.Step();
    if (!stepped.has_value()) {
      if (const auto event = execution->vm.last_insert_rowid_event(); event.has_value()) {
        state->last_insert_rowid = *event;
      }
      return FailAndRollback(std::move(stepped.error()));
    }
    if (*stepped == VmStep::kRow) {
      lifecycle = StatementState::kRow;
      return WriteStep::kRow;
    }

    if (const auto event = execution->vm.last_insert_rowid_event(); event.has_value()) {
      state->last_insert_rowid = *event;
    }
    auto detached = execution->vm.DetachExecutionContext();
    if (!detached.has_value()) {
      return FailAndRollback(std::move(detached.error()));
    }
    if (creates_schema) {
      if (!candidate_generation.has_value()) {
        return FailAndRollback(Internal("CREATE TABLE candidate generation is missing"));
      }
      auto loaded =
          LoadCatalog(*state->pager, CatalogLoadOptions{.generation = *candidate_generation});
      if (!loaded.has_value()) {
        return FailAndRollback(std::move(loaded.error()));
      }
      pending_candidate = std::move(*loaded);
    }
    if (execution->program->mutation_result().publishes_changes) {
      pending_changes = execution->vm.change_count();
    }
    success_pending = true;
    auto completed = CompletePendingSuccess();
    if (!completed.has_value()) {
      return Fail(std::move(completed.error()));
    }
    lifecycle = StatementState::kDone;
    return WriteStep::kDone;
  }

  [[nodiscard]] Status ResetForReexecution(bool report_previous_error) {
    std::optional<Error> previous;
    if (lifecycle == StatementState::kError && last_error.has_value()) {
      previous = std::move(last_error);
      last_error.reset();
    }
    if (execution != nullptr) {
      auto reset = execution->vm.Reset();
      if (!reset.has_value()) {
        return previous.has_value()
                   ? Status{std::unexpected(CombineErrors(*previous, reset.error(), "VM reset"))}
                   : reset;
      }
    }
    if (success_pending) {
      auto completed = CompletePendingSuccess();
      if (!completed.has_value()) {
        return previous.has_value()
                   ? Status{std::unexpected(
                         CombineErrors(*previous, completed.error(), "statement completion retry"))}
                   : completed;
      }
    } else {
      auto rolled_back = RollbackActiveStatement();
      if (!rolled_back.has_value()) {
        return previous.has_value()
                   ? Status{std::unexpected(
                         CombineErrors(*previous, rolled_back.error(), "statement reset rollback"))}
                   : rolled_back;
      }
    }
    lifecycle = StatementState::kReady;
    candidate_generation.reset();
    if (report_previous_error && previous.has_value()) {
      return std::unexpected(std::move(*previous));
    }
    return {};
  }

  [[nodiscard]] Status Reset() {
    if (!valid()) {
      return std::unexpected(Misuse("cannot reset a finalized writable statement"));
    }
    return ResetForReexecution(true);
  }

  [[nodiscard]] Status Finalize() {
    if (lifecycle == StatementState::kFinalized) {
      return {};
    }
    std::optional<Error> error;
    if (lifecycle == StatementState::kError && last_error.has_value()) {
      error = std::move(last_error);
      last_error.reset();
    }
    if (execution != nullptr) {
      auto reset = execution->vm.Reset();
      if (!reset.has_value()) {
        error = error.has_value()
                    ? std::optional<Error>{CombineErrors(*error, reset.error(), "VM reset")}
                    : std::optional<Error>{std::move(reset.error())};
      }
    }
    if (success_pending) {
      auto completed = CompletePendingSuccess();
      if (!completed.has_value()) {
        error = error.has_value() ? std::optional<Error>{CombineErrors(
                                        *error, completed.error(), "statement completion retry")}
                                  : std::optional<Error>{std::move(completed.error())};
      }
    } else {
      auto rolled_back = RollbackActiveStatement();
      if (!rolled_back.has_value()) {
        error = error.has_value() ? std::optional<Error>{CombineErrors(*error, rolled_back.error(),
                                                                       "finalize rollback")}
                                  : std::optional<Error>{std::move(rolled_back.error())};
      }
    }
    execution.reset();
    transaction.reset();
    parameter_names.clear();
    sql.clear();
    lifecycle = StatementState::kFinalized;
    state.reset();
    if (error.has_value()) {
      return std::unexpected(std::move(*error));
    }
    return {};
  }

  void FailForOutOfMemory() noexcept {
    try {
      if (execution != nullptr) {
        [[maybe_unused]] const Status reset = execution->vm.Reset();
      }
      if (active_statement.has_value()) {
        [[maybe_unused]] const Status rolled_back = active_statement->Rollback();
        active_statement.reset();
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
        [[maybe_unused]] const Status reset = execution->vm.Reset();
      }
      if (active_statement.has_value()) {
        [[maybe_unused]] const Status rolled_back = active_statement->Rollback();
      }
    } catch (const std::bad_alloc&) {
      return;
    } catch (const std::length_error&) {
      return;
    }
    active_statement.reset();
    execution.reset();
    transaction.reset();
    parameter_names.clear();
    sql.clear();
    last_error.reset();
    lifecycle = StatementState::kFinalized;
    state.reset();
  }

  std::shared_ptr<WriteSession::State> state;
  std::string sql;
  std::vector<std::optional<std::string>> parameter_names;
  std::unique_ptr<StatementExecution> execution;
  std::optional<TransactionCommand> transaction;
  std::optional<TransactionStatement> active_statement;
  CatalogSnapshotPtr pending_candidate;
  std::optional<std::uint64_t> pending_changes;
  std::optional<std::uint64_t> candidate_generation;
  std::optional<Error> last_error;
  StatementState lifecycle = StatementState::kReady;
  bool creates_schema = false;
  bool success_pending = false;
};

WriteStatement::WriteStatement(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WriteStatement::WriteStatement(WriteStatement&&) noexcept = default;
WriteStatement& WriteStatement::operator=(WriteStatement&&) noexcept = default;
WriteStatement::~WriteStatement() = default;

bool WriteStatement::valid() const noexcept { return impl_ != nullptr && impl_->valid(); }

std::size_t WriteStatement::parameter_count() const noexcept {
  return valid() ? impl_->parameter_names.size() : 0U;
}

std::optional<std::string_view> WriteStatement::parameter_name(
    std::size_t parameter_index_value) const noexcept {
  if (!valid() || parameter_index_value == 0U ||
      parameter_index_value > impl_->parameter_names.size()) {
    return std::nullopt;
  }
  const auto& name = impl_->parameter_names[parameter_index_value - 1U];
  return name.has_value() ? std::optional<std::string_view>{*name} : std::nullopt;
}

std::size_t WriteStatement::parameter_index(std::string_view name) const noexcept {
  if (!valid()) {
    return 0U;
  }
  for (std::size_t index = 0; index < impl_->parameter_names.size(); ++index) {
    if (impl_->parameter_names[index] == name) {
      return index + 1U;
    }
  }
  return 0U;
}

std::span<const ResultColumnMetadata> WriteStatement::result_columns() const noexcept {
  return valid() && impl_->execution != nullptr ? impl_->execution->program->result_columns()
                                                : std::span<const ResultColumnMetadata>{};
}

std::span<const SqlValue> WriteStatement::row() const noexcept {
  return valid() && impl_->execution != nullptr ? impl_->execution->vm.row()
                                                : std::span<const SqlValue>{};
}

Status WriteStatement::Bind(std::size_t parameter_index_value, const SqlValue& value) {
  try {
    if (!valid()) {
      return std::unexpected(Misuse("cannot bind a finalized writable statement"));
    }
    if (impl_->execution == nullptr || impl_->lifecycle != StatementState::kReady) {
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

Result<WriteStep> WriteStatement::Step() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("cannot step a moved-from writable statement"));
    }
    return impl_->Step();
  } catch (const std::bad_alloc&) {
    impl_->FailForOutOfMemory();
    return std::unexpected(Error::OutOfMemory());
  }
}

Status WriteStatement::Reset() {
  try {
    if (impl_ == nullptr) {
      return std::unexpected(Misuse("cannot reset a moved-from writable statement"));
    }
    return impl_->Reset();
  } catch (const std::bad_alloc&) {
    impl_->FailForOutOfMemory();
    return std::unexpected(Error::OutOfMemory());
  }
}

Status WriteStatement::Finalize() {
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

Result<WriteSession> WriteSession::Open(std::string_view path) {
  try {
    return Open(std::make_unique<PosixVfs>(), path);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<WriteSession> WriteSession::Open(std::unique_ptr<Vfs> vfs, std::string_view path) {
  try {
    if (vfs == nullptr) {
      return std::unexpected(Misuse("write session requires an owned VFS"));
    }
    auto pager = Pager::OpenWritable(*vfs, path);
    if (!pager.has_value()) {
      return std::unexpected(std::move(pager.error()));
    }
    Pager* const pager_identity = pager->get();
    auto coordinator = TransactionCoordinator::Open(std::move(*pager));
    if (!coordinator.has_value()) {
      return std::unexpected(std::move(coordinator.error()));
    }
    auto state = std::make_shared<State>(std::move(vfs), *pager_identity, std::move(*coordinator));
    return WriteSession(std::move(state));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

WriteSession::WriteSession(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
WriteSession::WriteSession(WriteSession&&) noexcept = default;
WriteSession& WriteSession::operator=(WriteSession&&) noexcept = default;
WriteSession::~WriteSession() = default;

bool WriteSession::valid() const noexcept { return state_ != nullptr; }

Result<WritePrepareOutput> WriteSession::Prepare(Utf8View source) {
  try {
    if (!valid()) {
      return std::unexpected(Misuse("cannot prepare on a moved-from write session"));
    }
    ParseResult parsed = ParseOne(source);
    if (!parsed.has_value()) {
      return std::unexpected(ParseFailure(parsed.error()));
    }
    if (!parsed->tree.has_value()) {
      return WritePrepareOutput{
          .statement = std::nullopt,
          .next_offset = parsed->next_offset,
      };
    }
    const std::size_t consumed = parsed->next_offset.value();
    if (consumed > source.size_bytes()) {
      return std::unexpected(Internal("parser returned an out-of-range tail offset"));
    }
    std::string retained_sql{source.bytes().substr(0, consumed)};

    auto refreshed = state_->RefreshCatalog();
    if (!refreshed.has_value()) {
      return std::unexpected(std::move(refreshed.error()));
    }
    auto compiled = state_->Compile(std::move(*parsed->tree));
    if (!compiled.has_value()) {
      return std::unexpected(std::move(compiled.error()));
    }
    auto impl = std::make_unique<WriteStatement::Impl>(state_, std::move(retained_sql),
                                                       std::move(*compiled));
    WritePrepareOutput output{
        .statement = std::nullopt,
        .next_offset = parsed->next_offset,
    };
    output.statement.emplace(WriteStatement(std::move(impl)));
    return output;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

std::uint64_t WriteSession::changes() const noexcept { return valid() ? state_->changes : 0U; }

std::int64_t WriteSession::last_insert_rowid() const noexcept {
  return valid() ? state_->last_insert_rowid : 0;
}

bool WriteSession::autocommit() const noexcept {
  return valid() && state_->coordinator.autocommit();
}

}  // namespace modern_sqlite
