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
};

struct InsertBaseline {
  ByteBuffer committed_image;
  std::size_t mutation_count;
};

struct RecoveryResult {
  bool committed;
  std::size_t persistent_mutations;
  CrashSnapshot snapshot;
};

struct RecoveryImages {
  ByteView initial;
  ByteView committed;
};

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

void VerifyRows(const std::vector<ExpectedRow>& rows, bool committed) {
  const std::vector<ExpectedRow> expected =
      committed ? std::vector<ExpectedRow>{{.rowid = 1, .name = "one"}, {.rowid = 2, .name = "two"}}
                : std::vector<ExpectedRow>{{.rowid = 1, .name = "one"}};
  if (rows.size() != expected.size()) {
    throw std::runtime_error{"crash harness recovered the wrong row count"};
  }
  for (std::size_t index = 0; index < rows.size(); ++index) {
    if (rows[index].rowid != expected[index].rowid || rows[index].name != expected[index].name) {
      throw std::runtime_error{"crash harness recovered the wrong row"};
    }
  }
}

[[nodiscard]] ByteBuffer CreateInitialImage() {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  const WritePagerFixedVfs* const vfs = owned_vfs.get();
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  if (!ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY,Name TEXT NOT NULL)") ||
      !ExecuteDone(session, "INSERT INTO Items VALUES(1,'one')") || vfs->journal_present()) {
    throw std::runtime_error{"crash harness could not create its initial image"};
  }
  return ByteBuffer::CopyOf(vfs->database_bytes());
}

[[nodiscard]] InsertBaseline CreateInsertBaseline(ByteView initial_image, bool writes_are_durable) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadDatabase(initial_image);
  vfs->SetDatabaseWritesDurable(writes_are_durable);
  vfs->ArmCrashCut(std::nullopt);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  if (!ExecuteDone(session, "INSERT INTO Items VALUES(2,'two')") || vfs->journal_present()) {
    throw std::runtime_error{"crash harness could not create its committed baseline"};
  }
  const std::size_t mutation_count = vfs->mutation_count();
  if (mutation_count == 0U || mutation_count > kMaximumCrashCuts) {
    throw std::runtime_error{"crash harness baseline has an invalid mutation count"};
  }
  return InsertBaseline{
      .committed_image = ByteBuffer::CopyOf(vfs->database_bytes()),
      .mutation_count = mutation_count,
  };
}

[[nodiscard]] CrashSnapshot RunInsertCut(ByteView initial_image, bool writes_are_durable,
                                         std::size_t cut) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadDatabase(initial_image);
  vfs->SetDatabaseWritesDurable(writes_are_durable);
  vfs->ArmCrashCut(cut);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  static_cast<void>(ExecuteDone(session, "INSERT INTO Items VALUES(2,'two')"));
  CrashSnapshot snapshot = vfs->CrashAndSnapshot();
  if (!snapshot.cut_triggered || snapshot.mutation_count != cut) {
    throw std::runtime_error{"crash harness did not trigger the requested cut"};
  }
  return snapshot;
}

[[nodiscard]] bool ImageEquals(ByteView left, ByteView right) {
  return left.size() == right.size() && std::ranges::equal(left, right);
}

[[nodiscard]] RecoveryResult Recover(const CrashSnapshot& snapshot, RecoveryImages images) {
  auto owned_vfs = std::make_unique<WritePagerFixedVfs>(false);
  WritePagerFixedVfs* const vfs = owned_vfs.get();
  vfs->LoadCrashSnapshot(snapshot);
  WriteSession session =
      TakeValue(WriteSession::Open(std::move(owned_vfs), kWritePagerDatabasePath));
  const std::vector<ExpectedRow> rows = QueryRows(session);
  const bool rows_are_committed = rows.size() == 2U;
  const bool old_state = ImageEquals(vfs->database_bytes(), images.initial);
  const bool committed_state = ImageEquals(vfs->database_bytes(), images.committed);
  if (old_state == committed_state || rows_are_committed != committed_state) {
    throw std::runtime_error{"crash harness recovered a partial or ambiguous image"};
  }
  VerifyRows(rows, committed_state);
  const std::size_t persistent_mutations = vfs->mutation_count();
  return RecoveryResult{
      .committed = committed_state,
      .persistent_mutations = persistent_mutations,
      .snapshot = vfs->CrashAndSnapshot(),
  };
}

void VerifyInsertCuts(ByteView initial_image, bool writes_are_durable) {
  const InsertBaseline baseline = CreateInsertBaseline(initial_image, writes_are_durable);
  const RecoveryImages images{
      .initial = initial_image,
      .committed = baseline.committed_image.view(),
  };
  for (std::size_t cut = 1U; cut <= baseline.mutation_count; ++cut) {
    const CrashSnapshot crashed = RunInsertCut(initial_image, writes_are_durable, cut);
    const RecoveryResult first = Recover(crashed, images);
    const RecoveryResult second = Recover(first.snapshot, images);
    if (first.committed != second.committed || second.persistent_mutations != 0U) {
      throw std::runtime_error{"crash harness recovery is not idempotent"};
    }
  }
}

}  // namespace

void RunWriteSessionCrashHarness() {
  const ByteBuffer initial_image = CreateInitialImage();
  VerifyInsertCuts(initial_image.view(), false);
  VerifyInsertCuts(initial_image.view(), true);
}

}  // namespace modern_sqlite::test
