#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/support/aligned_allocation.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

bool fail_allocations = false;
constexpr std::size_t kSavepointNameBytes = 128U;
std::optional<std::size_t> savepoint_failure;
std::size_t savepoint_allocation = 0U;
bool savepoint_name_copied = false;
bool savepoint_failure_observed = false;
std::size_t last_allocation_size = 0U;
std::size_t savepoint_name_allocation_size = 0U;
bool count_query_allocations = false;
std::size_t query_allocation = 0U;
std::optional<std::size_t> query_failure;
bool query_failure_observed = false;
std::optional<std::size_t> binding_copy_size;
std::size_t binding_copy_attempts = 0U;
bool fail_binding_copy = false;

void BeforeAllocation(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (count_query_allocations) {
    const std::size_t index = query_allocation++;
    if (query_failure == index) {
      query_failure_observed = true;
      throw std::bad_alloc{};
    }
  }
  if (binding_copy_size == size) {
    ++binding_copy_attempts;
    if (fail_binding_copy) {
      throw std::bad_alloc{};
    }
  }
  last_allocation_size = size;
  if (savepoint_failure.has_value()) {
    if (!savepoint_name_copied && size == savepoint_name_allocation_size) {
      savepoint_name_copied = true;
    } else if (savepoint_name_copied && savepoint_allocation++ == *savepoint_failure) {
      savepoint_failure_observed = true;
      throw std::bad_alloc{};
    }
  }
}

[[nodiscard]] void* Allocate(std::size_t size) {
  BeforeAllocation(size);
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  BeforeAllocation(size);
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] modern_sqlite::WriteStatement Prepare(modern_sqlite::WriteSession& session,
                                                    std::string_view sql) {
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    throw std::runtime_error{"failed to prepare write-session OOM fixture"};
  }
  return std::move(*prepared->statement);
}

void ExecuteDone(modern_sqlite::WriteSession& session, std::string_view sql) {
  auto statement = Prepare(session, sql);
  const auto result = statement.Step();
  if (!result.has_value() || *result != modern_sqlite::WriteStep::kDone) {
    throw std::runtime_error("write-session OOM setup did not complete");
  }
}

[[nodiscard]] bool CheckFailedSavepointRollbackContinuation() {
  using namespace modern_sqlite;
  constexpr std::size_t kSavepointAllocationWindow = 8U;
  for (std::size_t failure = 0U; failure < kSavepointAllocationWindow; ++failure) {
    auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
    auto& observed_vfs = *vfs;
    auto opened = WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath);
    if (!opened.has_value()) {
      return false;
    }
    auto& session = *opened;
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "BEGIN IMMEDIATE");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    const std::string name(kSavepointNameBytes, 's');
    savepoint_name_allocation_size = last_allocation_size;
    auto savepoint = Prepare(session, "SAVEPOINT " + name);
    if (!observed_vfs.journal_present()) {
      return false;
    }
    observed_vfs.FailNextDatabaseUnlock(DatabaseLock::kShared, ErrorCode::kIo);
    savepoint_allocation = 0U;
    savepoint_name_copied = false;
    savepoint_failure_observed = false;
    savepoint_failure = failure;

    const auto first = savepoint.Step();

    savepoint_failure.reset();
    const bool pending_rollback = savepoint_failure_observed && !first.has_value() &&
                                  !session.autocommit() && !observed_vfs.journal_present() &&
                                  observed_vfs.database_lock() != DatabaseLock::kNone &&
                                  observed_vfs.database_lock() != DatabaseLock::kShared;
    if (!pending_rollback) {
      continue;
    }
    if (!savepoint_name_copied || first.error().code() != ErrorCode::kOutOfMemory) {
      return false;
    }
    const auto deletes = observed_vfs.total_journal_deletes();

    const auto completed = savepoint.Step();

    if (completed.has_value() || completed.error().code() != ErrorCode::kOutOfMemory ||
        !session.autocommit() || observed_vfs.database_lock() != DatabaseLock::kNone ||
        observed_vfs.total_journal_deletes() != deletes) {
      std::fprintf(stderr, "failed SAVEPOINT did not retain its error through rollback cleanup\n");
      return false;
    }
    auto query = Prepare(session, "SELECT id FROM Items");
    const auto empty = query.Step();
    if (!empty.has_value() || *empty != WriteStep::kDone) {
      std::fprintf(stderr, "failed SAVEPOINT rollback did not restore the explicit write\n");
      return false;
    }
    const auto reset = savepoint.Reset();
    if (reset.has_value() || reset.error().code() != ErrorCode::kOutOfMemory) {
      std::fprintf(stderr, "failed SAVEPOINT Reset did not report its original error\n");
      return false;
    }
    const auto fresh = savepoint.Step();
    if (!fresh.has_value() || *fresh != WriteStep::kDone || session.autocommit()) {
      std::fprintf(stderr, "failed SAVEPOINT could not execute freshly after cleanup\n");
      return false;
    }
    ExecuteDone(session, "ROLLBACK");
    return true;
  }
  std::fprintf(stderr, "no named SAVEPOINT allocation reached the combined cleanup failure\n");
  return false;
}

[[nodiscard]] std::optional<std::size_t> CheckMutationQueryFailure(
    modern_sqlite::TemporaryStoreMode mode, std::optional<std::size_t> failure) {
  using namespace modern_sqlite;
  auto owned = std::make_unique<test::WritePagerFixedVfs>(false);
  const auto& observed_vfs = *owned;
  auto opened = WriteSession::Open(
      std::move(owned), test::kWritePagerDatabasePath,
      WriteSessionOptions{.temporary_storage = TemporaryStorageOptions{.mode = mode}});
  if (!opened.has_value()) {
    return std::nullopt;
  }
  auto& session = *opened;
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1,'first')");
  ExecuteDone(session, "INSERT INTO Items VALUES(2,'second')");
  auto statement = Prepare(session,
                           "UPDATE Items SET Name=(SELECT DISTINCT Name||?1 FROM Items "
                           "ORDER BY 1 DESC LIMIT 1)");
  if (!statement.Bind(1, SqlValue::Text("-query")).has_value()) {
    return std::nullopt;
  }
  query_allocation = 0U;
  query_failure_observed = false;
  query_failure = failure;
  count_query_allocations = true;
  const auto result = statement.Step();
  count_query_allocations = false;
  query_failure.reset();
  const std::size_t allocations = query_allocation;
  if (!failure.has_value()) {
    if (!result.has_value() || *result != WriteStep::kDone || session.changes() != 2U ||
        !session.autocommit() || observed_vfs.pathless_file_present()) {
      return std::nullopt;
    }
    return allocations;
  }
  if (!query_failure_observed || result.has_value() ||
      result.error().code() != ErrorCode::kOutOfMemory || !session.autocommit() ||
      observed_vfs.pathless_file_present() || observed_vfs.database_lock() != DatabaseLock::kNone) {
    return std::nullopt;
  }
  const auto reset = statement.Reset();
  if (reset.has_value() || reset.error().code() != ErrorCode::kOutOfMemory) {
    return std::nullopt;
  }
  auto query = Prepare(session, "SELECT id,Name FROM Items ORDER BY id");
  for (std::int64_t id = 1; id <= 2; ++id) {
    const auto row = query.Step();
    if (!row.has_value() || *row != WriteStep::kRow || query.row()[0].integer_value() != id) {
      return std::nullopt;
    }
    const auto text = query.row()[1].text_value();
    if (!text.has_value() || text->bytes() != (id == 1 ? "first" : "second")) {
      return std::nullopt;
    }
  }
  const auto done = query.Step();
  const auto retried = statement.Step();
  if (!done.has_value() || *done != WriteStep::kDone || !retried.has_value() ||
      *retried != WriteStep::kDone || session.changes() != 2U ||
      observed_vfs.pathless_file_present()) {
    return std::nullopt;
  }
  return allocations;
}

[[nodiscard]] bool CheckMutationQueryAllocationFailures() {
  using namespace modern_sqlite;
  for (const auto mode : {TemporaryStoreMode::kMemory, TemporaryStoreMode::kFile}) {
    const auto baseline = CheckMutationQueryFailure(mode, std::nullopt);
    if (!baseline.has_value() || *baseline == 0U || *baseline > 512U) {
      std::fprintf(stderr, "mutation query allocation baseline is invalid\n");
      return false;
    }
    for (std::size_t failure = 0; failure < *baseline; ++failure) {
      if (!CheckMutationQueryFailure(mode, failure).has_value()) {
        std::fprintf(stderr, "mutation query mode=%u allocation=%zu failed rollback or recovery\n",
                     static_cast<unsigned>(mode), failure);
        return false;
      }
    }
    std::printf("mutation query mode=%u verified all %zu allocation prefixes\n",
                static_cast<unsigned>(mode), *baseline);
  }
  return true;
}

// Allocation-fault diagnostics must remain independent of C++ formatting allocations.
// NOLINTBEGIN(modernize-use-std-print)
[[nodiscard]] bool CheckSourceFreeFunctionFailureKeepsPriorWrites() {
  using namespace modern_sqlite;
  const std::string input(128U, 'a');
  const std::string expected(128U, 'A');
  for (const bool reads_table : {false, true}) {
    auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
    auto opened = WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath);
    if (!opened.has_value()) {
      return false;
    }
    auto& session = *opened;
    ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
    ExecuteDone(session, "INSERT INTO Items VALUES(1)");
    ExecuteDone(session, "BEGIN");
    ExecuteDone(session, "INSERT INTO Items VALUES(2)");
    auto scalar = Prepare(
        session, reads_table ? "SELECT upper(?1) FROM Items WHERE id=1" : "SELECT upper(?1)");
    if (!scalar.Bind(1, SqlValue::Text(input)).has_value()) {
      return false;
    }

    query_allocation = 0U;
    query_failure_observed = false;
    query_failure = 0U;
    count_query_allocations = true;
    const auto failed = scalar.Step();
    count_query_allocations = false;
    query_failure.reset();

    if (!query_failure_observed || failed.has_value() ||
        failed.error().code() != ErrorCode::kOutOfMemory || session.autocommit() != reads_table ||
        session.changes() != 1U || session.last_insert_rowid() != 2) {
      std::fprintf(stderr, "function OOM changed transaction state: reads_table=%u autocommit=%u\n",
                   static_cast<unsigned>(reads_table), static_cast<unsigned>(session.autocommit()));
      return false;
    }
    const auto reset = scalar.Reset();
    if (reset.has_value() || reset.error().code() != ErrorCode::kOutOfMemory ||
        session.autocommit() != reads_table) {
      std::fprintf(stderr, "function OOM reset changed transaction state: reads_table=%u\n",
                   static_cast<unsigned>(reads_table));
      return false;
    }
    const auto retried = scalar.Step();
    if (!retried.has_value() || *retried != WriteStep::kRow || scalar.row().size() != 1U) {
      if (!retried.has_value()) {
        std::fprintf(stderr, "function OOM retry failed: reads_table=%u error=%s\n",
                     static_cast<unsigned>(reads_table), retried.error().ToString().c_str());
      }
      return false;
    }
    const auto text = scalar.row()[0].text_value();
    if (!text.has_value() || text->bytes() != expected) {
      return false;
    }
    const auto completed = scalar.Step();
    if (!completed.has_value() || *completed != WriteStep::kDone) {
      return false;
    }

    auto rows = Prepare(session, "SELECT id FROM Items ORDER BY id");
    const std::int64_t expected_rows = reads_table ? 1 : 2;
    for (std::int64_t id = 1; id <= expected_rows; ++id) {
      const auto row = rows.Step();
      if (!row.has_value() || *row != WriteStep::kRow || rows.row()[0].integer_value() != id) {
        return false;
      }
    }
    const auto done = rows.Step();
    if (!done.has_value() || *done != WriteStep::kDone) {
      return false;
    }
    if (!reads_table) {
      ExecuteDone(session, "ROLLBACK");
      auto original = Prepare(session, "SELECT id FROM Items");
      const auto row = original.Step();
      if (!row.has_value() || *row != WriteStep::kRow || original.row()[0].integer_value() != 1) {
        return false;
      }
      const auto end = original.Step();
      if (!end.has_value() || *end != WriteStep::kDone) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::size_t> CheckTransactionCommandFailure(
    std::string_view sql, bool completes, std::optional<std::size_t> failure,
    bool fail_cleanup = false) {
  using namespace modern_sqlite;
  auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
  auto& observed = *vfs;
  auto opened = WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath);
  if (!opened.has_value()) {
    return std::nullopt;
  }
  auto& session = *opened;
  ExecuteDone(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY)");
  ExecuteDone(session, "INSERT INTO Items VALUES(1)");
  ExecuteDone(session, "BEGIN");
  ExecuteDone(session, "INSERT INTO Items VALUES(2)");
  auto command = Prepare(session, sql);
  if (fail_cleanup) {
    observed.FailNextDatabaseUnlock(DatabaseLock::kNone, ErrorCode::kIo);
  }
  query_allocation = 0U;
  query_failure_observed = false;
  query_failure = failure;
  count_query_allocations = true;

  const auto result = command.Step();

  count_query_allocations = false;
  query_failure.reset();
  const std::size_t allocations = query_allocation;
  if (!failure.has_value()) {
    if (session.autocommit() || result.has_value() != completes ||
        (completes && *result != WriteStep::kDone) ||
        (!completes && result.error().code() != ErrorCode::kGeneric)) {
      return std::nullopt;
    }
  } else {
    if (!query_failure_observed || result.has_value() ||
        result.error().code() != ErrorCode::kOutOfMemory || session.autocommit() == fail_cleanup) {
      return std::nullopt;
    }
    const std::string original_error = result.error().ToString();
    if (fail_cleanup) {
      if (result.error().message().find("rollback failed with io:") == std::string_view::npos) {
        return std::nullopt;
      }
      const auto deletes = observed.total_journal_deletes();
      const auto writes = observed.total_database_writes();
      const auto continued = command.Step();
      if (continued.has_value() || continued.error().code() != ErrorCode::kOutOfMemory ||
          continued.error().ToString() != original_error || !session.autocommit() ||
          deletes != observed.total_journal_deletes() ||
          writes != observed.total_database_writes()) {
        return std::nullopt;
      }
    }
    const auto reset = command.Reset();
    if (reset.has_value() || reset.error().code() != ErrorCode::kOutOfMemory ||
        reset.error().ToString() != original_error) {
      return std::nullopt;
    }
  }
  auto rows = Prepare(session, "SELECT id FROM Items ORDER BY id");
  const std::int64_t expected_rows = failure.has_value() ? 1 : 2;
  for (std::int64_t id = 1; id <= expected_rows; ++id) {
    const auto row = rows.Step();
    if (!row.has_value() || *row != WriteStep::kRow || rows.row()[0].integer_value() != id) {
      return std::nullopt;
    }
  }
  const auto done = rows.Step();
  if (!done.has_value() || *done != WriteStep::kDone) {
    return std::nullopt;
  }
  if (!failure.has_value()) {
    ExecuteDone(session, "ROLLBACK");
  }
  return allocations;
}

[[nodiscard]] bool CheckTransactionCommandResourceFailures() {
  const std::string name(128U, 's');
  for (const auto& [sql, completes] :
       {std::pair{"SAVEPOINT " + name, true}, std::pair{std::string{"BEGIN"}, false},
        std::pair{"RELEASE " + name, false}}) {
    const auto baseline = CheckTransactionCommandFailure(sql, completes, std::nullopt);
    if (!baseline.has_value()) {
      std::fprintf(stderr, "transaction command baseline lost its ordinary result: %s\n",
                   sql.c_str());
      return false;
    }
    if (*baseline > 64U || (completes && *baseline == 0U)) {
      std::fprintf(stderr,
                   "transaction command baseline is invalid: completed=%u allocations=%zu\n",
                   static_cast<unsigned>(completes), *baseline);
      return false;
    }
    if (*baseline == 0U) {
      std::printf(
          "transaction command ordinary-error baseline has zero allocations; "
          "prior writes and exact error verified\n");
    }
    for (std::size_t failure = 0U; failure < *baseline; ++failure) {
      if (!CheckTransactionCommandFailure(sql, completes, failure).has_value()) {
        std::fprintf(stderr, "transaction command allocation=%zu failed: %s\n", failure,
                     sql.c_str());
        return false;
      }
      if (!CheckTransactionCommandFailure(sql, completes, failure, true).has_value()) {
        std::fprintf(stderr, "transaction command cleanup allocation=%zu lost error: %s\n", failure,
                     sql.c_str());
        return false;
      }
    }
    std::printf("transaction command completed=%u verified all %zu allocation prefixes\n",
                static_cast<unsigned>(completes), *baseline);
  }
  return true;
}

template <typename Session>
[[nodiscard]] bool CheckReprepareBindingOwnership(const char* facade,
                                                  modern_sqlite::SqlValueType type,
                                                  bool inject_copy_failure,
                                                  bool missing_table = false) {
  using namespace modern_sqlite;
  constexpr std::size_t kBindingBytes = 65'537U;
  auto source_vfs = std::make_unique<test::WritePagerFixedVfs>(false);
  const auto& source_image = *source_vfs;
  auto writer = WriteSession::Open(std::move(source_vfs), test::kWritePagerDatabasePath);
  if (!writer.has_value()) {
    return false;
  }
  ExecuteDone(*writer, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  ExecuteDone(*writer, "INSERT INTO Items VALUES(1,'first')");
  auto target_vfs = std::make_unique<test::WritePagerFixedVfs>(false);
  auto& target_image = *target_vfs;
  target_image.LoadDatabase(source_image.database_bytes());
  auto statement = [&] {
    auto session = Session::Open(std::move(target_vfs), test::kWritePagerDatabasePath);
    if (!session.has_value()) {
      throw std::runtime_error{"unable to open reprepare binding fixture"};
    }
    auto prepared = session->Prepare(Utf8View{"SELECT ?1 AS binding, id FROM Items WHERE id=1"});
    if (!prepared.has_value() || !prepared->statement.has_value()) {
      throw std::runtime_error{"unable to prepare binding ownership control"};
    }
    return std::move(*prepared->statement);
  }();
  std::string bytes(kBindingBytes, 'b');
  bytes[kBindingBytes / 2U] = '\0';
  const SqlValue binding = type == SqlValueType::kText
                               ? SqlValue::Text(bytes)
                               : SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(bytes)));
  query_allocation = 0U;
  count_query_allocations = true;
  last_allocation_size = 0U;
  const auto bound = statement.Bind(1, binding);
  count_query_allocations = false;
  const std::size_t measured_copy_size = last_allocation_size;
  if (!bound.has_value() || query_allocation != 1U || measured_copy_size < kBindingBytes) {
    std::fprintf(stderr, "reprepare facade=%s did not measure one owned binding copy\n", facade);
    return false;
  }
  const auto parameter_name = statement.parameter_name(1);
  if (parameter_name != "?1" || statement.result_columns().size() != 2U) {
    return false;
  }
  if (missing_table) {
    auto replacement_vfs = std::make_unique<test::WritePagerFixedVfs>(false);
    const auto& replacement_image = *replacement_vfs;
    auto replacement =
        WriteSession::Open(std::move(replacement_vfs), test::kWritePagerDatabasePath);
    if (!replacement.has_value()) {
      return false;
    }
    ExecuteDone(*replacement, "CREATE TABLE Other(id INTEGER PRIMARY KEY)");
    ExecuteDone(*replacement, "CREATE TABLE Marker(id INTEGER PRIMARY KEY)");
    target_image.LoadDatabase(replacement_image.database_bytes());
  } else {
    ExecuteDone(*writer, "CREATE INDEX Items_name ON Items(Name)");
    target_image.LoadDatabase(source_image.database_bytes());
  }
  const auto matches_binding = [&](const SqlValue& observed) {
    if (const auto expected = binding.text_value(); expected.has_value()) {
      const auto actual = observed.text_value();
      return actual.has_value() && actual->bytes() == expected->bytes() &&
             actual->data() != expected->data();
    }
    const auto expected = binding.blob_value();
    const auto actual = observed.blob_value();
    return expected.has_value() && actual.has_value() && std::ranges::equal(*actual, *expected) &&
           actual->data() != expected->data();
  };
  binding_copy_size = measured_copy_size;
  binding_copy_attempts = 0U;
  fail_binding_copy = inject_copy_failure;

  const auto first = statement.Step();

  fail_binding_copy = false;
  binding_copy_size.reset();
  using Step = typename decltype(first)::value_type;
  const bool first_row = first.has_value() && *first == Step::kRow &&
                         statement.row().size() == 2U && matches_binding(statement.row()[0]) &&
                         statement.row()[1].integer_value() == 1;
  const bool copy_free_fault = !inject_copy_failure || binding_copy_attempts == 0U;
  const bool first_correct =
      missing_table ? (!first.has_value() && first.error().code() == ErrorCode::kGeneric &&
                       binding_copy_attempts == 0U)
                    : first_row;
  const int code = first.has_value() ? 0 : static_cast<int>(first.error().code());
  std::printf(
      "reprepare bindings facade=%s type=%u fault=%u measured_copy=%zu attempts=%zu "
      "code=%d row=%u replacement_failure=%u\n",
      facade, static_cast<unsigned>(type), static_cast<unsigned>(inject_copy_failure),
      measured_copy_size, binding_copy_attempts, code, static_cast<unsigned>(first_row),
      static_cast<unsigned>(missing_table));
  if (statement.parameter_name(1) != parameter_name || statement.result_columns().size() != 2U ||
      statement.result_columns()[0].name != "binding") {
    return false;
  }
  const auto reset = statement.Reset();
  if (first.has_value() ? !reset.has_value()
                        : (reset.has_value() || reset.error().code() != first.error().code())) {
    return false;
  }
  if (missing_table) {
    ExecuteDone(*writer, "CREATE INDEX Items_name ON Items(Name)");
    ExecuteDone(*writer, "CREATE TABLE Marker(id INTEGER PRIMARY KEY)");
    target_image.LoadDatabase(source_image.database_bytes());
  }
  binding_copy_size = measured_copy_size;
  binding_copy_attempts = 0U;
  fail_binding_copy = true;
  const auto retried = statement.Step();
  fail_binding_copy = false;
  binding_copy_size.reset();
  if (!retried.has_value() || *retried != Step::kRow || statement.row().size() != 2U ||
      !matches_binding(statement.row()[0]) || statement.row()[1].integer_value() != 1 ||
      binding_copy_attempts != 0U) {
    std::fprintf(stderr, "reprepare facade=%s lost binding ownership across reset/retry\n", facade);
    return false;
  }
  const auto done = statement.Step();
  if (!done.has_value() || *done != Step::kDone || !statement.Reset().has_value()) {
    return false;
  }
  const SqlValue rebound = type == SqlValueType::kText
                               ? SqlValue::Text("rebound")
                               : SqlValue::Blob(ByteBuffer::CopyOf(AsBytes("rebound")));
  if (!statement.Bind(1, rebound).has_value()) {
    return false;
  }
  const auto fresh = statement.Step();
  if (!fresh.has_value() || *fresh != Step::kRow || statement.row().size() != 2U) {
    return false;
  }
  if (const auto expected = rebound.text_value(); expected.has_value()) {
    const auto actual = statement.row()[0].text_value();
    if (!actual.has_value() || actual->bytes() != expected->bytes()) {
      return false;
    }
  } else {
    const auto expected_blob = rebound.blob_value();
    const auto actual_blob = statement.row()[0].blob_value();
    if (!expected_blob.has_value() || !actual_blob.has_value() ||
        !std::ranges::equal(*actual_blob, *expected_blob)) {
      return false;
    }
  }
  return statement.Finalize().has_value() && first_correct && copy_free_fault;
}

[[nodiscard]] bool CheckReprepareBindingOwners() {
  using namespace modern_sqlite;
  bool correct = true;
  for (const SqlValueType type : {SqlValueType::kText, SqlValueType::kBlob}) {
    for (const bool failure : {false, true}) {
      const bool read = CheckReprepareBindingOwnership<ReadSession>("read", type, failure);
      const bool write = CheckReprepareBindingOwnership<WriteSession>("write", type, failure);
      correct = read && write && correct;
    }
    const bool read = CheckReprepareBindingOwnership<ReadSession>("read", type, false, true);
    const bool write = CheckReprepareBindingOwnership<WriteSession>("write", type, false, true);
    correct = read && write && correct;
  }
  return correct;
}
// NOLINTEND(modernize-use-std-print)

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
  using namespace modern_sqlite;
  auto vfs = std::make_unique<test::WritePagerFixedVfs>(false);
  test::WritePagerFixedVfs& test_vfs = *vfs;
  auto opened = WriteSession::Open(std::move(vfs), test::kWritePagerDatabasePath);
  if (!opened.has_value()) {
    return 1;
  }
  WriteSession& session = *opened;

  WriteStatement create = Prepare(session, "CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT)");
  const auto created = create.Step();
  if (!created.has_value() || *created != WriteStep::kDone) {
    return 1;
  }

  WriteStatement insert = Prepare(session, "INSERT INTO Items VALUES(?1,?2)");
  if (!insert.Bind(1, SqlValue::Integer(1)).has_value() ||
      !insert.Bind(2, SqlValue::Text("value")).has_value()) {
    return 1;
  }
  fail_allocations = true;
  const auto failed = insert.Step();
  fail_allocations = false;
  if (failed.has_value() || failed.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }
  const Status reset = insert.Reset();
  if (reset.has_value() || reset.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }

  fail_allocations = true;
  const auto prepare_failure = session.Prepare(Utf8View{"SELECT id FROM Items"});
  fail_allocations = false;
  if (prepare_failure.has_value() || prepare_failure.error().code() != ErrorCode::kOutOfMemory) {
    return 1;
  }

  WriteStatement select = Prepare(session, "SELECT id FROM Items");
  const auto done = select.Step();
  if (!done.has_value() || *done != WriteStep::kDone) {
    return 1;
  }
  WriteStatement first = Prepare(session, "INSERT INTO Items VALUES(1,'first')");
  const auto inserted = first.Step();
  if (!inserted.has_value() || *inserted != WriteStep::kDone || session.changes() != 1U) {
    return 1;
  }
  WriteStatement rolled_back = Prepare(session, "INSERT INTO Items VALUES(2,'rolled back')");
  test_vfs.FailNextDatabaseLock(DatabaseLock::kExclusive, ErrorCode::kBusy);
  const auto commit_failure = rolled_back.Step();
  if (commit_failure.has_value() || commit_failure.error().code() != ErrorCode::kBusy) {
    return 1;
  }
  fail_allocations = true;
  const Status retired_reset = rolled_back.Reset();
  fail_allocations = false;
  if (retired_reset.has_value() || retired_reset.error().code() != ErrorCode::kBusy ||
      session.changes() != 0U || !session.autocommit()) {
    return 1;
  }
  const auto retried = rolled_back.Step();
  if (!retried.has_value() || *retried != WriteStep::kDone || session.changes() != 1U) {
    return 1;
  }
  return CheckFailedSavepointRollbackContinuation() && CheckMutationQueryAllocationFailures() &&
                 CheckSourceFreeFunctionFailureKeepsPriorWrites() &&
                 CheckTransactionCommandResourceFailures() && CheckReprepareBindingOwners()
             ? 0
             : 1;
} catch (...) {
  fail_allocations = false;
  count_query_allocations = false;
  query_failure.reset();
  savepoint_failure.reset();
  binding_copy_size.reset();
  fail_binding_copy = false;
  return 1;
}
