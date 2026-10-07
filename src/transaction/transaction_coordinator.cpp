#include "modern_sqlite/transaction/transaction_coordinator.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/pager/pager.hpp"

namespace modern_sqlite {
namespace {

[[nodiscard]] Error MakeError(ErrorCode code, std::string_view message) noexcept {
  try {
    return Error::Create(code, std::string{message});
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error Misuse(std::string_view message) noexcept {
  return MakeError(ErrorCode::kMisuse, message);
}

[[nodiscard]] Error Internal(std::string_view message) noexcept {
  return MakeError(ErrorCode::kInternal, message);
}

[[nodiscard]] Error Busy(std::string_view message) noexcept {
  return MakeError(ErrorCode::kBusy, message);
}

[[nodiscard]] Error SchemaChanged(std::string_view message) noexcept {
  return MakeError(ErrorCode::kSchemaChanged, message);
}

[[nodiscard]] Error TooLarge(std::string_view message) noexcept {
  return MakeError(ErrorCode::kTooLarge, message);
}

[[nodiscard]] Error CombineErrors(const Error& primary, const Error& cleanup,
                                  std::string_view context) noexcept {
  try {
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
    return MakeError(primary.code(), primary.message());
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  } catch (const std::length_error&) {
    return Error::OutOfMemory();
  }
}

}  // namespace

namespace transaction_detail {

enum class TerminalPhase : std::uint8_t {
  kNone,
  kCommitAttempt,
  kCommitCleanup,
  kRollbackAttempt,
  kRollbackCleanup,
};

struct ActiveStatement {
  std::uint64_t token;
  StatementAccess access;
  StatementRollbackMode rollback;
  bool implicit;
  bool epoch_ended = false;
  std::optional<JournalSavepointId> savepoint;
};

class CoordinatorState final {
 public:
  explicit CoordinatorState(std::unique_ptr<Pager> pager) noexcept : pager_(std::move(pager)) {}

  [[nodiscard]] bool autocommit() const noexcept {
    return transaction_state_ == TransactionState::kAutocommit;
  }

  [[nodiscard]] TransactionState state() const noexcept { return transaction_state_; }

  [[nodiscard]] bool statement_active() const noexcept { return active_.has_value(); }

  [[nodiscard]] Result<std::uint64_t> ReserveStatementToken(TransactionStatementOptions options) {
    if (terminal_phase_ != TerminalPhase::kNone) {
      return std::unexpected(Misuse("transaction cleanup must finish before a new statement"));
    }
    if (active_.has_value()) {
      return std::unexpected(Busy("another transaction statement is active"));
    }
    switch (options.access) {
      case StatementAccess::kRead:
      case StatementAccess::kWrite:
        break;
      default:
        return std::unexpected(Misuse("statement access mode is invalid"));
    }
    switch (options.rollback) {
      case StatementRollbackMode::kTransaction:
      case StatementRollbackMode::kStatement:
        break;
      default:
        return std::unexpected(Misuse("statement rollback mode is invalid"));
    }
    if (options.access == StatementAccess::kRead &&
        options.rollback == StatementRollbackMode::kTransaction) {
      return std::unexpected(Misuse("read statements cannot request transaction rollback"));
    }
    if (next_statement_token_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TooLarge("transaction statement generation is exhausted"));
    }
    ++next_statement_token_;
    return next_statement_token_;
  }

  [[nodiscard]] Result<std::unique_ptr<TransactionWriter>> CreateWriterCapability(
      std::uint64_t token) {
    try {
      return std::unique_ptr<TransactionWriter>{new TransactionWriter{*this, token}};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] Status Begin(TransactionMode mode) {
    if (active_.has_value()) {
      return std::unexpected(Busy("cannot begin a transaction with an active statement"));
    }
    if (terminal_phase_ != TerminalPhase::kNone) {
      return std::unexpected(Misuse("transaction cleanup must finish before begin"));
    }
    if (transaction_state_ != TransactionState::kAutocommit ||
        pager_->state() != PagerState::kOpen) {
      return std::unexpected(Misuse("cannot start a transaction within a transaction"));
    }
    switch (mode) {
      case TransactionMode::kDeferred:
        transaction_state_ = TransactionState::kExplicit;
        return {};
      case TransactionMode::kImmediate:
        break;
      default:
        return std::unexpected(Misuse("transaction mode is invalid"));
    }

    auto begun_read = pager_->BeginRead();
    if (!begun_read.has_value()) {
      return begun_read;
    }
    auto begun_write = pager_->BeginWrite();
    if (!begun_write.has_value()) {
      Error primary = std::move(begun_write.error());
      auto ended = pager_->EndRead();
      if (!ended.has_value()) {
        return std::unexpected(CombineErrors(primary, ended.error(), "read cleanup"));
      }
      return std::unexpected(std::move(primary));
    }
    transaction_state_ = TransactionState::kExplicit;
    return {};
  }

  [[nodiscard]] Status Commit() {
    if (active_.has_value()) {
      return std::unexpected(Busy("cannot commit with an active statement"));
    }
    if (terminal_phase_ == TerminalPhase::kRollbackAttempt ||
        terminal_phase_ == TerminalPhase::kRollbackCleanup) {
      return std::unexpected(Misuse("rollback cleanup cannot be completed by commit"));
    }
    if (terminal_phase_ == TerminalPhase::kCommitAttempt ||
        terminal_phase_ == TerminalPhase::kCommitCleanup) {
      return ContinueCommit();
    }
    if (transaction_state_ == TransactionState::kAutocommit) {
      return std::unexpected(Misuse("cannot commit without an active transaction"));
    }

    writer_.reset();
    if (pager_->state() == PagerState::kOpen) {
      ResetLogicalState();
      return {};
    }
    terminal_phase_ = TerminalPhase::kCommitAttempt;
    return ContinueCommit();
  }

  [[nodiscard]] Status Rollback() {
    if (terminal_phase_ == TerminalPhase::kCommitCleanup) {
      return std::unexpected(Misuse("committed transaction cleanup cannot be rolled back"));
    }
    if (terminal_phase_ == TerminalPhase::kRollbackAttempt ||
        terminal_phase_ == TerminalPhase::kRollbackCleanup) {
      return ContinueRollback();
    }
    if (transaction_state_ == TransactionState::kAutocommit && !active_.has_value() &&
        pager_->state() == PagerState::kOpen && terminal_phase_ != TerminalPhase::kCommitAttempt) {
      return std::unexpected(Misuse("cannot rollback without an active transaction"));
    }

    EndActiveWriterEpoch();
    active_.reset();
    writer_.reset();
    if (pager_->state() == PagerState::kOpen) {
      ResetLogicalState();
      return {};
    }
    terminal_phase_ = TerminalPhase::kRollbackAttempt;
    return ContinueRollback();
  }

  [[nodiscard]] Status BeginStatement(TransactionStatementOptions options, std::uint64_t token) {
    if (active_.has_value()) {
      return std::unexpected(Busy("another transaction statement is active"));
    }
    if (terminal_phase_ != TerminalPhase::kNone) {
      return std::unexpected(Misuse("transaction cleanup must finish before a new statement"));
    }
    const bool implicit = transaction_state_ == TransactionState::kAutocommit;
    const bool began_read = pager_->state() == PagerState::kOpen;
    if (began_read) {
      auto read = pager_->BeginRead();
      if (!read.has_value()) {
        return read;
      }
    }

    if (options.access == StatementAccess::kRead) {
      active_ = ActiveStatement{
          .token = token,
          .access = options.access,
          .rollback = options.rollback,
          .implicit = implicit,
          .epoch_ended = false,
          .savepoint = std::nullopt,
      };
      return {};
    }

    const bool began_write = !pager_->in_write_transaction();
    if (began_write) {
      auto write = pager_->BeginWrite();
      if (!write.has_value()) {
        Error primary = std::move(write.error());
        if (began_read) {
          auto ended = pager_->EndRead();
          if (!ended.has_value()) {
            return std::unexpected(CombineErrors(primary, ended.error(), "read cleanup"));
          }
        }
        return std::unexpected(std::move(primary));
      }
    }

    std::optional<JournalSavepointId> statement_savepoint;
    if (!implicit && options.rollback == StatementRollbackMode::kStatement) {
      auto created = pager_->CreateSavepoint();
      if (!created.has_value()) {
        return AutomaticFullRollback(std::move(created.error()));
      }
      statement_savepoint = *created;
    }

    if (!writer_.has_value()) {
      auto opened = BtreeWriteSession::OpenManaged(*pager_);
      if (!opened.has_value()) {
        Error primary = std::move(opened.error());
        if (statement_savepoint.has_value()) {
          auto released = pager_->ReleaseSavepoint(*statement_savepoint);
          if (!released.has_value()) {
            primary = CombineErrors(primary, released.error(), "statement savepoint cleanup");
            return AutomaticFullRollback(std::move(primary));
          }
        } else if (implicit) {
          return AutomaticFullRollback(std::move(primary));
        }
        return std::unexpected(std::move(primary));
      }
      writer_ = std::move(*opened);
    }
    auto epoch = writer_->BeginManagedStatement();
    if (!epoch.has_value()) {
      return AutomaticFullRollback(std::move(epoch.error()));
    }

    active_ = ActiveStatement{
        .token = token,
        .access = options.access,
        .rollback = options.rollback,
        .implicit = implicit,
        .epoch_ended = false,
        .savepoint = statement_savepoint,
    };
    return {};
  }

  [[nodiscard]] bool StatementValid(std::uint64_t token) const noexcept {
    return FindStatement(token) != nullptr;
  }

  [[nodiscard]] Result<BtreeWriteSession*> WriterFor(std::uint64_t token) {
    const ActiveStatement* active = FindStatement(token);
    if (active == nullptr) {
      return std::unexpected(SchemaChanged("transaction statement is stale"));
    }
    if (active->access != StatementAccess::kWrite || !writer_.has_value() || active->epoch_ended) {
      return std::unexpected(Misuse("transaction statement has no active writer"));
    }
    return &*writer_;
  }

  [[nodiscard]] Status StatementSucceed(std::uint64_t token) {
    ActiveStatement* active = FindStatement(token);
    if (active == nullptr) {
      return std::unexpected(SchemaChanged("transaction statement is stale"));
    }
    if (terminal_phase_ == TerminalPhase::kCommitAttempt ||
        terminal_phase_ == TerminalPhase::kCommitCleanup) {
      return ContinueCommit();
    }
    if (terminal_phase_ == TerminalPhase::kRollbackAttempt ||
        terminal_phase_ == TerminalPhase::kRollbackCleanup) {
      return std::unexpected(Misuse("rollback cleanup cannot be completed by statement success"));
    }
    if (active->access == StatementAccess::kRead) {
      return FinishReadStatement();
    }

    EndActiveWriterEpoch();
    if (const auto failure = pager_->write_failure_code(); failure.has_value()) {
      Error error = MakeError(*failure, "write statement requires transaction rollback");
      if (!active->implicit && active->rollback == StatementRollbackMode::kStatement &&
          active->savepoint.has_value()) {
        return std::unexpected(std::move(error));
      }
      return AutomaticFullRollback(std::move(error));
    }
    if (!active->implicit) {
      if (active->savepoint.has_value()) {
        auto released = pager_->ReleaseSavepoint(*active->savepoint);
        if (!released.has_value()) {
          return AutomaticFullRollback(std::move(released.error()));
        }
      }
      active_.reset();
      return {};
    }

    writer_.reset();
    terminal_phase_ = TerminalPhase::kCommitAttempt;
    auto committed = ContinueCommit();
    if (!committed.has_value()) {
      Error primary = std::move(committed.error());
      if (terminal_phase_ == TerminalPhase::kCommitCleanup) {
        return std::unexpected(std::move(primary));
      }
      return AutomaticFullRollback(std::move(primary));
    }
    return {};
  }

  [[nodiscard]] Status StatementRollback(std::uint64_t token) {
    ActiveStatement* active = FindStatement(token);
    if (active == nullptr) {
      return std::unexpected(SchemaChanged("transaction statement is stale"));
    }
    if (terminal_phase_ == TerminalPhase::kCommitCleanup) {
      return std::unexpected(Misuse("committed statement cleanup cannot be rolled back"));
    }
    if (terminal_phase_ == TerminalPhase::kRollbackAttempt ||
        terminal_phase_ == TerminalPhase::kRollbackCleanup) {
      return ContinueRollback();
    }
    if (active->access == StatementAccess::kRead) {
      return FinishReadStatement();
    }

    EndActiveWriterEpoch();
    writer_.reset();
    if (active->implicit || active->rollback == StatementRollbackMode::kTransaction) {
      return PerformFullRollback();
    }

    if (!active->savepoint.has_value()) {
      return std::unexpected(Misuse("statement rollback savepoint is absent"));
    }
    auto rolled_back = pager_->RollbackToSavepoint(*active->savepoint);
    if (!rolled_back.has_value()) {
      return AutomaticFullRollback(std::move(rolled_back.error()));
    }
    auto released = pager_->ReleaseSavepoint(*active->savepoint);
    if (!released.has_value()) {
      return AutomaticFullRollback(std::move(released.error()));
    }
    active_.reset();
    return {};
  }

  [[nodiscard]] Status InitializeDatabase(std::uint64_t token, BtreeDatabaseOptions options) {
    auto writer = WriterFor(token);
    if (!writer.has_value()) {
      return std::unexpected(std::move(writer.error()));
    }
    return (*writer)->InitializeDatabase(options);
  }

  [[nodiscard]] Result<TableBtreeWriter> CreateTableBtree(std::uint64_t token) {
    auto writer = WriterFor(token);
    if (!writer.has_value()) {
      return std::unexpected(std::move(writer.error()));
    }
    return (*writer)->CreateTableBtree();
  }

  [[nodiscard]] Result<IndexBtreeWriter> CreateIndexBtree(
      std::uint64_t token, std::span<const IndexColumnOrder> columns) {
    auto writer = WriterFor(token);
    if (!writer.has_value()) {
      return std::unexpected(std::move(writer.error()));
    }
    return (*writer)->CreateIndexBtree(columns);
  }

  [[nodiscard]] Result<TableBtreeWriter> OpenTableBtree(std::uint64_t token, PageNumber root_page) {
    auto writer = WriterFor(token);
    if (!writer.has_value()) {
      return std::unexpected(std::move(writer.error()));
    }
    return (*writer)->OpenTableBtree(root_page);
  }

  [[nodiscard]] Result<IndexBtreeWriter> OpenIndexBtree(std::uint64_t token, PageNumber root_page,
                                                        std::span<const IndexColumnOrder> columns) {
    auto writer = WriterFor(token);
    if (!writer.has_value()) {
      return std::unexpected(std::move(writer.error()));
    }
    return (*writer)->OpenIndexBtree(root_page, columns);
  }

 private:
  [[nodiscard]] ActiveStatement* CurrentStatement() noexcept {
    if (!active_.has_value()) {
      return nullptr;
    }
    return std::addressof(*active_);
  }

  [[nodiscard]] const ActiveStatement* CurrentStatement() const noexcept {
    if (!active_.has_value()) {
      return nullptr;
    }
    return std::addressof(*active_);
  }

  [[nodiscard]] ActiveStatement* FindStatement(std::uint64_t token) noexcept {
    ActiveStatement* active = CurrentStatement();
    return active != nullptr && active->token == token ? active : nullptr;
  }

  [[nodiscard]] const ActiveStatement* FindStatement(std::uint64_t token) const noexcept {
    const ActiveStatement* active = CurrentStatement();
    return active != nullptr && active->token == token ? active : nullptr;
  }

  void EndActiveWriterEpoch() noexcept {
    ActiveStatement* active = CurrentStatement();
    if (active == nullptr) {
      return;
    }
    if (active->access == StatementAccess::kWrite && !active->epoch_ended && writer_.has_value()) {
      writer_->EndManagedStatement();
      active->epoch_ended = true;
    }
  }

  [[nodiscard]] Status FinishReadStatement() {
    const ActiveStatement* active = CurrentStatement();
    if (active == nullptr) {
      return std::unexpected(Internal("active read statement is absent"));
    }
    if (!active->implicit) {
      active_.reset();
      return {};
    }
    auto ended = pager_->EndRead();
    if (!ended.has_value()) {
      return ended;
    }
    active_.reset();
    return {};
  }

  [[nodiscard]] Status PerformFullRollback() {
    terminal_phase_ = TerminalPhase::kRollbackAttempt;
    return ContinueRollback();
  }

  [[nodiscard]] Status AutomaticFullRollback(Error primary) {
    EndActiveWriterEpoch();
    writer_.reset();
    auto rolled_back = PerformFullRollback();
    if (!rolled_back.has_value()) {
      return std::unexpected(CombineErrors(primary, rolled_back.error(), "full rollback"));
    }
    return std::unexpected(std::move(primary));
  }

  [[nodiscard]] Status ContinueCommit() {
    switch (pager_->state()) {
      case PagerState::kOpen:
        ResetLogicalState();
        return {};
      case PagerState::kReader:
        terminal_phase_ = TerminalPhase::kCommitCleanup;
        return FinishReadCleanup();
      case PagerState::kWriterLocked:
      case PagerState::kWriterCacheModified:
      case PagerState::kWriterDatabaseModified:
      case PagerState::kWriterFinished:
      case PagerState::kError:
        break;
    }
    auto committed = pager_->Commit();
    if (!committed.has_value()) {
      if (pager_->state() == PagerState::kWriterFinished) {
        terminal_phase_ = TerminalPhase::kCommitCleanup;
      } else {
        terminal_phase_ = TerminalPhase::kCommitAttempt;
      }
      return committed;
    }
    terminal_phase_ = TerminalPhase::kCommitCleanup;
    return FinishReadCleanup();
  }

  [[nodiscard]] Status ContinueRollback() {
    switch (pager_->state()) {
      case PagerState::kOpen:
        ResetLogicalState();
        return {};
      case PagerState::kReader:
        terminal_phase_ = TerminalPhase::kRollbackCleanup;
        return FinishReadCleanup();
      case PagerState::kWriterLocked:
      case PagerState::kWriterCacheModified:
      case PagerState::kWriterDatabaseModified:
      case PagerState::kWriterFinished:
      case PagerState::kError:
        break;
    }
    auto rolled_back = pager_->Rollback();
    if (!rolled_back.has_value()) {
      terminal_phase_ = TerminalPhase::kRollbackAttempt;
      return rolled_back;
    }
    terminal_phase_ = TerminalPhase::kRollbackCleanup;
    return FinishReadCleanup();
  }

  [[nodiscard]] Status FinishReadCleanup() {
    if (pager_->state() == PagerState::kReader) {
      auto ended = pager_->EndRead();
      if (!ended.has_value()) {
        return ended;
      }
    } else if (pager_->state() != PagerState::kOpen) {
      return std::unexpected(Misuse("pager terminal cleanup is not ready"));
    }
    ResetLogicalState();
    return {};
  }

  void ResetLogicalState() noexcept {
    writer_.reset();
    active_.reset();
    transaction_state_ = TransactionState::kAutocommit;
    terminal_phase_ = TerminalPhase::kNone;
  }

  std::unique_ptr<Pager> pager_;
  std::optional<BtreeWriteSession> writer_;
  std::optional<ActiveStatement> active_;
  std::uint64_t next_statement_token_ = 0;
  TransactionState transaction_state_ = TransactionState::kAutocommit;
  TerminalPhase terminal_phase_ = TerminalPhase::kNone;
};

}  // namespace transaction_detail

struct TransactionStatement::Impl final {
  Impl(std::shared_ptr<transaction_detail::CoordinatorState> shared_state, std::uint64_t value,
       StatementAccess statement_access, std::unique_ptr<TransactionWriter> capability) noexcept
      : state(std::move(shared_state)),
        token(value),
        access(statement_access),
        writer(std::move(capability)) {}

  std::shared_ptr<transaction_detail::CoordinatorState> state;
  std::uint64_t token;
  StatementAccess access;
  std::unique_ptr<TransactionWriter> writer;
};

TransactionStatement::TransactionStatement(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

TransactionStatement::TransactionStatement(TransactionStatement&&) noexcept = default;

TransactionStatement& TransactionStatement::operator=(TransactionStatement&&) noexcept = default;

TransactionStatement::~TransactionStatement() = default;

bool TransactionStatement::valid() const noexcept {
  return impl_ != nullptr && impl_->state != nullptr && impl_->state->StatementValid(impl_->token);
}

StatementAccess TransactionStatement::access() const noexcept {
  return valid() ? impl_->access : StatementAccess::kRead;
}

TransactionWriter* TransactionStatement::writer() noexcept {
  return valid() && impl_->access == StatementAccess::kWrite ? impl_->writer.get() : nullptr;
}

Status TransactionStatement::Succeed() {
  if (!valid()) {
    return std::unexpected(SchemaChanged("transaction statement is stale"));
  }
  try {
    return impl_->state->StatementSucceed(impl_->token);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status TransactionStatement::Rollback() {
  if (!valid()) {
    return std::unexpected(SchemaChanged("transaction statement is stale"));
  }
  try {
    return impl_->state->StatementRollback(impl_->token);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status TransactionWriter::InitializeDatabase(BtreeDatabaseOptions options) {
  return state_->InitializeDatabase(token_, options);
}

Result<TableBtreeWriter> TransactionWriter::CreateTableBtree() {
  return state_->CreateTableBtree(token_);
}

Result<IndexBtreeWriter> TransactionWriter::CreateIndexBtree(
    std::span<const IndexColumnOrder> columns) {
  return state_->CreateIndexBtree(token_, columns);
}

Result<TableBtreeWriter> TransactionWriter::OpenTableBtree(PageNumber root_page) {
  return state_->OpenTableBtree(token_, root_page);
}

Result<IndexBtreeWriter> TransactionWriter::OpenIndexBtree(
    PageNumber root_page, std::span<const IndexColumnOrder> columns) {
  return state_->OpenIndexBtree(token_, root_page, columns);
}

Result<TransactionCoordinator> TransactionCoordinator::Open(std::unique_ptr<Pager> pager) {
  if (pager == nullptr) {
    return std::unexpected(Misuse("transaction coordinator requires a pager"));
  }
  if (!pager->writable()) {
    return std::unexpected(Misuse("transaction coordinator requires a writable pager"));
  }
  if (pager->state() != PagerState::kOpen || pager->in_read_transaction()) {
    return std::unexpected(Misuse("transaction coordinator requires an idle pager"));
  }
  try {
    auto state = std::make_shared<transaction_detail::CoordinatorState>(std::move(pager));
    return TransactionCoordinator{std::move(state)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

bool TransactionCoordinator::valid() const noexcept { return state_ != nullptr; }

bool TransactionCoordinator::autocommit() const noexcept {
  return state_ == nullptr || state_->autocommit();
}

TransactionState TransactionCoordinator::state() const noexcept {
  return state_ == nullptr ? TransactionState::kAutocommit : state_->state();
}

bool TransactionCoordinator::statement_active() const noexcept {
  return state_ != nullptr && state_->statement_active();
}

Status TransactionCoordinator::Begin(TransactionMode mode) {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("transaction coordinator is moved from"));
  }
  try {
    return state_->Begin(mode);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status TransactionCoordinator::Commit() {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("transaction coordinator is moved from"));
  }
  try {
    return state_->Commit();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Status TransactionCoordinator::Rollback() {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("transaction coordinator is moved from"));
  }
  try {
    return state_->Rollback();
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<TransactionStatement> TransactionCoordinator::BeginStatement(
    TransactionStatementOptions options) {
  if (state_ == nullptr) {
    return std::unexpected(Misuse("transaction coordinator is moved from"));
  }
  try {
    auto token = state_->ReserveStatementToken(options);
    if (!token.has_value()) {
      return std::unexpected(std::move(token.error()));
    }
    std::unique_ptr<TransactionWriter> writer;
    if (options.access == StatementAccess::kWrite) {
      auto capability = state_->CreateWriterCapability(*token);
      if (!capability.has_value()) {
        return std::unexpected(std::move(capability.error()));
      }
      writer = std::move(*capability);
    }
    auto impl = std::make_unique<TransactionStatement::Impl>(state_, *token, options.access,
                                                             std::move(writer));
    auto begun = state_->BeginStatement(options, *token);
    if (!begun.has_value()) {
      return std::unexpected(std::move(begun.error()));
    }
    return TransactionStatement{std::move(impl)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  } catch (const std::length_error&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

}  // namespace modern_sqlite
