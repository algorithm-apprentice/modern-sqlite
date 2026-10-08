#include "tests/compatibility/write_session_crash_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite::test {
namespace {

constexpr std::size_t kMaximumCrashCuts = 2048;
using CrashSnapshot = WritePagerCrashSnapshot<kWritePagerFileCapacity>;

struct ExpectedRow {
  std::int64_t rowid;
  std::string name;

  bool operator==(const ExpectedRow&) const = default;
};

struct LogicalState {
  std::vector<ExpectedRow> rows;
  bool temp_visible = false;

  bool operator==(const LogicalState&) const = default;
};

using ScenarioExecutor = bool (*)(WriteSession&);

struct CrashScenario {
  std::string_view id;
  ScenarioExecutor execute;
  LogicalState terminal;
};

struct ScenarioBaseline {
  ByteBuffer terminal_image;
  std::size_t mutation_count;
  bool terminal_is_distinct;
};

struct RecoveryResult {
  bool terminal;
  std::size_t persistent_mutations;
  CrashSnapshot snapshot;
};

struct RecoveryImages {
  ByteView initial;
  ByteView terminal;
  bool terminal_is_distinct;
};

[[nodiscard]] std::string ShellQuote(std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted.append("'\\''");
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error{std::string{message}};
  }
  return std::move(value).value();
}

[[nodiscard]] WriteStatement PrepareOne(WriteSession& session, std::string_view sql) {
  WritePrepareOutput prepared = TakeValue(session.Prepare(Utf8View{sql}));
  if (!prepared.statement.has_value() || prepared.next_offset.value() != sql.size()) {
    throw std::runtime_error{"crash harness failed to prepare exactly one statement"};
  }
  return std::move(*prepared.statement);
}

[[nodiscard]] bool ExecuteDone(WriteSession& session, std::string_view sql) {
  auto prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value() ||
      prepared->next_offset.value() != sql.size()) {
    return false;
  }
  WriteStatement statement = std::move(*prepared->statement);
  const Result<WriteStep> stepped = statement.Step();
  const Status finalized = statement.Finalize();
  return stepped.has_value() && *stepped == WriteStep::kDone && finalized.has_value();
}

[[nodiscard]] bool ExecuteExpectedError(WriteSession& session, std::string_view sql,
                                        ErrorCode expected) {
  auto prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value() ||
      prepared->next_offset.value() != sql.size()) {
    return false;
  }
  WriteStatement statement = std::move(*prepared->statement);
  const Result<WriteStep> stepped = statement.Step();
  if (stepped.has_value() || stepped.error().code() != expected) {
    return false;
  }
  const Status finalized = statement.Finalize();
  return !finalized.has_value() && finalized.error().code() == expected;
}

[[nodiscard]] std::vector<ExpectedRow> QueryRows(WriteSession& session) {
  WriteStatement statement = PrepareOne(session, "SELECT id,Name FROM Items");
  std::vector<ExpectedRow> rows;
  while (true) {
    const WriteStep stepped = TakeValue(statement.Step());
    if (stepped == WriteStep::kDone) {
      if (!statement.Finalize().has_value()) {
        throw std::runtime_error{"crash harness could not finalize its validation query"};
      }
      return rows;
    }
    if (statement.row().size() != 2U || statement.row()[0].type() != SqlValueType::kInteger ||
        statement.row()[1].type() != SqlValueType::kText) {
      throw std::runtime_error{"crash harness read an invalid row shape"};
    }
    rows.push_back(ExpectedRow{
        .rowid = TakeOptional(statement.row()[0].integer_value(), "rowid is missing"),
        .name =
            std::string{TakeOptional(statement.row()[1].text_value(), "name is missing").bytes()},
    });
  }
}

[[nodiscard]] bool TempVisible(WriteSession& session) {
  WriteStatement statement =
      PrepareOne(session, "SELECT name FROM sqlite_schema WHERE name='Temp'");
  const WriteStep first = TakeValue(statement.Step());
  if (first == WriteStep::kDone) {
    if (!statement.Finalize().has_value()) {
      throw std::runtime_error{"crash harness could not finalize its catalog query"};
    }
    return false;
  }
  if (statement.row().size() != 1U || statement.row()[0].type() != SqlValueType::kText ||
      TakeOptional(statement.row()[0].text_value(), "schema name is missing").bytes() != "Temp") {
    throw std::runtime_error{"crash harness read an invalid catalog row"};
  }
  if (TakeValue(statement.Step()) != WriteStep::kDone || !statement.Finalize().has_value()) {
    throw std::runtime_error{"crash harness catalog query returned duplicate rows"};
  }
  return true;
}

[[nodiscard]] LogicalState QueryState(WriteSession& session) {
  return LogicalState{
      .rows = QueryRows(session),
      .temp_visible = TempVisible(session),
  };
}

[[nodiscard]] LogicalState InitialState() {
  return LogicalState{
      .rows =
          {
              {.rowid = 1, .name = "one"},
              {.rowid = 2, .name = "two"},
              {.rowid = 3, .name = "three"},
          },
      .temp_visible = false,
  };
}

[[nodiscard]] ByteBuffer CreateInitialImage() {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  const WritePagerFixedVfs* const vfs = owned_vfs.get();
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  if (!ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY,Name TEXT NOT NULL)") ||
      !ExecuteDone(session, "INSERT INTO Items VALUES(1,'one')") ||
      !ExecuteDone(session, "INSERT INTO Items VALUES(2,'two')") ||
      !ExecuteDone(session, "INSERT INTO Items VALUES(3,'three')") || vfs->journal_present() ||
      QueryState(session) != InitialState()) {
    throw std::runtime_error{"crash harness could not create its initial image"};
  }
  return ByteBuffer::CopyOf(vfs->database_bytes());
}

[[nodiscard]] bool ImplicitInsert(WriteSession& session) {
  return ExecuteDone(session, "INSERT INTO Items VALUES(4,'four')");
}

[[nodiscard]] bool ImplicitCreate(WriteSession& session) {
  return ExecuteDone(session, "CREATE TABLE Temp(id INTEGER PRIMARY KEY)");
}

[[nodiscard]] bool ImplicitCreateIndex(WriteSession& session) {
  return ExecuteDone(session, "CREATE UNIQUE INDEX items_name ON Items(Name DESC)");
}

[[nodiscard]] bool ImplicitAnalyze(WriteSession& session) {
  return ExecuteDone(session, "ANALYZE Items");
}

[[nodiscard]] bool ExactRowIdMove(WriteSession& session) {
  return ExecuteDone(session, "UPDATE Items SET id=10,Name='moved' WHERE id=1");
}

[[nodiscard]] bool ScanRowIdMove(WriteSession& session) {
  return ExecuteDone(session, "UPDATE Items SET id=id+10,Name=Name||'x' WHERE id>=2");
}

[[nodiscard]] bool ScanDelete(WriteSession& session) {
  return ExecuteDone(session, "DELETE FROM Items WHERE id>=2");
}

[[nodiscard]] bool ExplicitCommit(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") &&
         ExecuteDone(session, "INSERT INTO Items VALUES(4,'four')") &&
         ExecuteDone(session, "UPDATE Items SET Name='updated' WHERE id=1") &&
         ExecuteDone(session, "DELETE FROM Items WHERE id=2") && ExecuteDone(session, "COMMIT");
}

[[nodiscard]] bool ConstraintThenCommit(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") &&
         ExecuteDone(session, "INSERT INTO Items VALUES(4,'kept')") &&
         ExecuteExpectedError(session, "INSERT INTO Items VALUES(4,'duplicate')",
                              ErrorCode::kConstraint) &&
         ExecuteDone(session, "COMMIT");
}

[[nodiscard]] bool NamedRollbackThenCommit(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") &&
         ExecuteDone(session, "UPDATE Items SET Name='outer' WHERE id=1") &&
         ExecuteDone(session, "SAVEPOINT s") &&
         ExecuteDone(session, "INSERT INTO Items VALUES(4,'temporary')") &&
         ExecuteDone(session, "ROLLBACK TO s") && ExecuteDone(session, "RELEASE s") &&
         ExecuteDone(session, "COMMIT");
}

[[nodiscard]] bool TransactionSavepointRelease(WriteSession& session) {
  return ExecuteDone(session, "SAVEPOINT outer") &&
         ExecuteDone(session, "INSERT INTO Items VALUES(4,'savepoint')") &&
         ExecuteDone(session, "RELEASE outer");
}

[[nodiscard]] bool CreateThenFullRollback(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") &&
         ExecuteDone(session, "CREATE TABLE Temp(id INTEGER PRIMARY KEY)") &&
         ExecuteDone(session, "ROLLBACK");
}

[[nodiscard]] bool CreateThenRollbackTo(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") && ExecuteDone(session, "SAVEPOINT schema_scope") &&
         ExecuteDone(session, "CREATE TABLE Temp(id INTEGER PRIMARY KEY)") &&
         ExecuteDone(session, "ROLLBACK TO schema_scope") &&
         ExecuteDone(session, "RELEASE schema_scope") && ExecuteDone(session, "COMMIT");
}

[[nodiscard]] bool FullDmlRollback(WriteSession& session) {
  return ExecuteDone(session, "BEGIN") &&
         ExecuteDone(session, "UPDATE Items SET Name='rolled' WHERE id>=1") &&
         ExecuteDone(session, "DELETE FROM Items WHERE id=3") && ExecuteDone(session, "ROLLBACK");
}

[[nodiscard]] CrashScenario ImplicitInsertScenario() {
  return CrashScenario{
      .id = "implicit-insert",
      .execute = ImplicitInsert,
      .terminal =
          LogicalState{
              .rows =
                  {
                      {.rowid = 1, .name = "one"},
                      {.rowid = 2, .name = "two"},
                      {.rowid = 3, .name = "three"},
                      {.rowid = 4, .name = "four"},
                  },
              .temp_visible = false,
          },
  };
}

[[nodiscard]] std::vector<CrashScenario> TransactionScenarios() {
  return {
      CrashScenario{
          .id = "implicit-create",
          .execute = ImplicitCreate,
          .terminal =
              LogicalState{
                  .rows = InitialState().rows,
                  .temp_visible = true,
              },
      },
      CrashScenario{
          .id = "implicit-create-index",
          .execute = ImplicitCreateIndex,
          .terminal = InitialState(),
      },
      CrashScenario{
          .id = "implicit-analyze",
          .execute = ImplicitAnalyze,
          .terminal = InitialState(),
      },
      CrashScenario{
          .id = "exact-rowid-move",
          .execute = ExactRowIdMove,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 2, .name = "two"},
                          {.rowid = 3, .name = "three"},
                          {.rowid = 10, .name = "moved"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "scan-rowid-move",
          .execute = ScanRowIdMove,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 1, .name = "one"},
                          {.rowid = 12, .name = "twox"},
                          {.rowid = 13, .name = "threex"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "scan-delete",
          .execute = ScanDelete,
          .terminal =
              LogicalState{
                  .rows = {{.rowid = 1, .name = "one"}},
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "explicit-commit",
          .execute = ExplicitCommit,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 1, .name = "updated"},
                          {.rowid = 3, .name = "three"},
                          {.rowid = 4, .name = "four"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "constraint-then-commit",
          .execute = ConstraintThenCommit,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 1, .name = "one"},
                          {.rowid = 2, .name = "two"},
                          {.rowid = 3, .name = "three"},
                          {.rowid = 4, .name = "kept"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "named-rollback-then-commit",
          .execute = NamedRollbackThenCommit,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 1, .name = "outer"},
                          {.rowid = 2, .name = "two"},
                          {.rowid = 3, .name = "three"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "transaction-savepoint-release",
          .execute = TransactionSavepointRelease,
          .terminal =
              LogicalState{
                  .rows =
                      {
                          {.rowid = 1, .name = "one"},
                          {.rowid = 2, .name = "two"},
                          {.rowid = 3, .name = "three"},
                          {.rowid = 4, .name = "savepoint"},
                      },
                  .temp_visible = false,
              },
      },
      CrashScenario{
          .id = "create-full-rollback",
          .execute = CreateThenFullRollback,
          .terminal = InitialState(),
      },
      CrashScenario{
          .id = "create-rollback-to",
          .execute = CreateThenRollbackTo,
          .terminal = InitialState(),
      },
      CrashScenario{
          .id = "full-dml-rollback",
          .execute = FullDmlRollback,
          .terminal = InitialState(),
      },
  };
}

[[nodiscard]] bool ImageEquals(ByteView left, ByteView right) {
  return left.size() == right.size() && std::ranges::equal(left, right);
}

[[nodiscard]] ScenarioBaseline CreateBaseline(const CrashScenario& scenario, ByteView initial_image,
                                              bool writes_are_durable) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadDatabase(initial_image);
  vfs->SetDatabaseWritesDurable(writes_are_durable);
  vfs->ArmCrashCut(std::nullopt);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  if (!scenario.execute(session) || !session.autocommit() || vfs->journal_present() ||
      QueryState(session) != scenario.terminal) {
    throw std::runtime_error{std::string{scenario.id} + " could not create its terminal baseline"};
  }
  const std::size_t mutation_count = vfs->mutation_count();
  if (mutation_count == 0U || mutation_count > kMaximumCrashCuts) {
    throw std::runtime_error{std::string{scenario.id} + " has an invalid mutation count"};
  }
  ByteBuffer terminal_image = ByteBuffer::CopyOf(vfs->database_bytes());
  const bool terminal_is_distinct = !ImageEquals(initial_image, terminal_image.view());
  return ScenarioBaseline{
      .terminal_image = std::move(terminal_image),
      .mutation_count = mutation_count,
      .terminal_is_distinct = terminal_is_distinct,
  };
}

[[nodiscard]] CrashSnapshot RunCut(const CrashScenario& scenario, ByteView initial_image,
                                   bool writes_are_durable, std::size_t cut) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadDatabase(initial_image);
  vfs->SetDatabaseWritesDurable(writes_are_durable);
  vfs->ArmCrashCut(cut);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  static_cast<void>(scenario.execute(session));
  CrashSnapshot snapshot = vfs->CrashAndSnapshot();
  if (!snapshot.cut_triggered || snapshot.mutation_count != cut) {
    throw std::runtime_error{"the requested crash cut did not trigger"};
  }
  return snapshot;
}

[[nodiscard]] RecoveryResult Recover(const CrashSnapshot& snapshot, RecoveryImages images,
                                     const LogicalState& terminal_state) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadCrashSnapshot(snapshot);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  const LogicalState actual = QueryState(session);
  const bool old_image = ImageEquals(vfs->database_bytes(), images.initial);
  const bool terminal_image = ImageEquals(vfs->database_bytes(), images.terminal);
  bool recovered_terminal = false;
  if (images.terminal_is_distinct) {
    if (old_image == terminal_image) {
      throw std::runtime_error{"recovery produced a partial or ambiguous image"};
    }
    recovered_terminal = terminal_image;
  } else if (!old_image) {
    throw std::runtime_error{"rollback recovery did not restore the initial image"};
  }
  const LogicalState& expected = recovered_terminal ? terminal_state : InitialState();
  if (actual != expected || !session.autocommit()) {
    throw std::runtime_error{"recovery produced the wrong logical state"};
  }
  const std::size_t persistent_mutations = vfs->mutation_count();
  return RecoveryResult{
      .terminal = recovered_terminal,
      .persistent_mutations = persistent_mutations,
      .snapshot = vfs->CrashAndSnapshot(),
  };
}

void VerifyScenario(const CrashScenario& scenario, ByteView initial_image, bool writes_are_durable,
                    WriteSessionCrashVerification verification) {
  try {
    const ScenarioBaseline baseline = CreateBaseline(scenario, initial_image, writes_are_durable);
    const RecoveryImages images{
        .initial = initial_image,
        .terminal = baseline.terminal_image.view(),
        .terminal_is_distinct = baseline.terminal_is_distinct,
    };
    const std::size_t first_cut = verification.cut_filter.value_or(1U);
    const std::size_t final_cut = verification.cut_filter.value_or(baseline.mutation_count);
    if (first_cut == 0U || final_cut > baseline.mutation_count) {
      throw std::runtime_error{"requested crash cut is outside the scenario mutation range"};
    }
    for (std::size_t cut = first_cut; cut <= final_cut; ++cut) {
      try {
        const CrashSnapshot crashed = RunCut(scenario, initial_image, writes_are_durable, cut);
        const RecoveryResult first = Recover(crashed, images, scenario.terminal);
        if (verification.verify != nullptr) {
          if (!first.snapshot.main.present ||
              first.snapshot.main.size > first.snapshot.main.bytes.size()) {
            throw std::runtime_error{"recovery produced an invalid main-database snapshot"};
          }
          verification.verify(verification.context, scenario.id, cut, writes_are_durable,
                              first.terminal,
                              ByteView{first.snapshot.main.bytes}.first(first.snapshot.main.size));
        }
        const RecoveryResult second = Recover(first.snapshot, images, scenario.terminal);
        if (first.terminal != second.terminal || second.persistent_mutations != 0U) {
          throw std::runtime_error{"recovery is not idempotent"};
        }
      } catch (const std::exception& error) {
        std::string message = std::string{scenario.id} + " cut=" + std::to_string(cut) +
                              " durability=" + (writes_are_durable ? "durable: " : "volatile: ") +
                              error.what();
        if (!verification.executable.empty()) {
          message.append("\nreproduce: ");
          message.append(ShellQuote(verification.executable));
          message.append(" --case ");
          message.append(std::string{scenario.id});
          message.append(" --cut ");
          message.append(std::to_string(cut));
          message.append(" --durability ");
          message.append(writes_are_durable ? "durable" : "volatile");
        }
        throw std::runtime_error{message};
      }
    }
  } catch (const std::exception& error) {
    const std::string prefix = std::string{scenario.id} + ": ";
    if (std::string_view{error.what()}.starts_with(prefix)) {
      throw;
    }
    throw std::runtime_error{prefix + error.what()};
  }
}

void VerifyScenarioBothDurabilities(const CrashScenario& scenario, ByteView initial_image,
                                    WriteSessionCrashVerification verification) {
  if (!verification.scenario_filter.empty() && verification.scenario_filter != scenario.id) {
    return;
  }
  if (!verification.durability_filter.has_value() || !verification.durability_filter.value()) {
    VerifyScenario(scenario, initial_image, false, verification);
  }
  if (!verification.durability_filter.has_value() || verification.durability_filter.value()) {
    VerifyScenario(scenario, initial_image, true, verification);
  }
}

}  // namespace

void RunWriteSessionCrashHarness() { RunWriteSessionCrashHarness({}); }

void RunWriteSessionCrashHarness(WriteSessionCrashVerification verification) {
  const ByteBuffer initial_image = CreateInitialImage();
  VerifyScenarioBothDurabilities(ImplicitInsertScenario(), initial_image.view(), verification);
}

void RunWriteSessionTransactionCrashHarness() { RunWriteSessionTransactionCrashHarness({}); }

void RunWriteSessionTransactionCrashHarness(WriteSessionCrashVerification verification) {
  const ByteBuffer initial_image = CreateInitialImage();
  for (const CrashScenario& scenario : TransactionScenarios()) {
    VerifyScenarioBothDurabilities(scenario, initial_image.view(), verification);
  }
}

}  // namespace modern_sqlite::test
