#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace {

bool fail_allocations = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  if (fail_allocations) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
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
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

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
  return retried.has_value() && *retried == WriteStep::kDone && session.changes() == 1U ? 0 : 1;
} catch (...) {
  fail_allocations = false;
  return 1;
}
