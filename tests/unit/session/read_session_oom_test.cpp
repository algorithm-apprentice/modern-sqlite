#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
bool inject_failure = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
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
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::filesystem::path FixturePath(std::string_view name) {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "session" / std::string{name};
}

[[nodiscard]] std::vector<char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error{"unable to read session OOM fixture"};
  }
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class TemporaryDatabase final {
 public:
  explicit TemporaryDatabase(const std::vector<char>& bytes) {
    static std::atomic<std::size_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-session-oom-" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db");
    journal_path_ = path_;
    journal_path_ += "-journal";
    wal_path_ = path_;
    wal_path_ += "-wal";
    Rewrite(bytes);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  ~TemporaryDatabase() noexcept {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(journal_path_, error);
    std::filesystem::remove(wal_path_, error);
  }

  void Rewrite(const std::vector<char>& bytes) {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error{"unable to create session OOM database"};
    }
    if (!bytes.empty()) {
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
      throw std::runtime_error{"unable to write session OOM database"};
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
  std::filesystem::path journal_path_;
  std::filesystem::path wal_path_;
};

struct LockTrackingState {
  bool shared_lock_held = false;
};

class LockTrackingFile final : public modern_sqlite::File {
 public:
  LockTrackingFile(std::unique_ptr<modern_sqlite::File> delegate,
                   std::shared_ptr<LockTrackingState> state)
      : delegate_(std::move(delegate)), state_(std::move(state)) {}

 private:
  modern_sqlite::Result<modern_sqlite::ByteCount> DoReadAt(
      modern_sqlite::MutableByteView destination, modern_sqlite::FileOffset offset) override {
    auto read = delegate_->ReadAt(destination, offset);
    if (!read.has_value()) {
      return std::unexpected(std::move(read.error()));
    }
    return read->bytes_read();
  }

  modern_sqlite::Status DoWriteAt(modern_sqlite::ByteView source,
                                  modern_sqlite::FileOffset offset) override {
    return delegate_->WriteAt(source, offset);
  }

  modern_sqlite::Status DoTruncate(modern_sqlite::FileSize size) override {
    return delegate_->Truncate(size);
  }

  modern_sqlite::Status DoSync(modern_sqlite::SyncOptions options) override {
    return delegate_->Sync(options);
  }

  modern_sqlite::Result<modern_sqlite::FileSize> DoSize() override { return delegate_->Size(); }

  modern_sqlite::Status DoLock(modern_sqlite::DatabaseLock lock) override {
    auto locked = delegate_->Lock(lock);
    if (locked.has_value() && lock == modern_sqlite::DatabaseLock::kShared) {
      state_->shared_lock_held = true;
    }
    return locked;
  }

  modern_sqlite::Status DoUnlock(modern_sqlite::DatabaseLock lock) override {
    auto unlocked = delegate_->Unlock(lock);
    if (unlocked.has_value() && lock == modern_sqlite::DatabaseLock::kNone) {
      state_->shared_lock_held = false;
    }
    return unlocked;
  }

  modern_sqlite::Result<bool> DoHasReservedLock() override { return delegate_->HasReservedLock(); }

  [[nodiscard]] modern_sqlite::FileProperties DoProperties() const noexcept override {
    auto properties = delegate_->Properties();
    if (properties.has_value()) {
      return *properties;
    }
    return modern_sqlite::FileProperties{
        .sector_size = modern_sqlite::ByteCount{1},
        .device_characteristics = {},
    };
  }

  modern_sqlite::Result<std::optional<modern_sqlite::MutableByteView>> DoMapSharedMemory(
      modern_sqlite::SharedMemoryRegionIndex region, modern_sqlite::ByteCount region_size,
      modern_sqlite::SharedMemoryMapMode mode) override {
    return delegate_->MapSharedMemory(region, region_size, mode);
  }

  modern_sqlite::Status DoLockSharedMemory(modern_sqlite::SharedMemoryLockRange range,
                                           modern_sqlite::SharedMemoryLockOperation operation,
                                           modern_sqlite::SharedMemoryLockMode mode) override {
    return delegate_->LockSharedMemory(range, operation, mode);
  }

  void DoSharedMemoryBarrier() noexcept override { delegate_->SharedMemoryBarrier(); }

  modern_sqlite::Status DoUnmapSharedMemory(modern_sqlite::SharedMemoryUnmapMode mode) override {
    return delegate_->UnmapSharedMemory(mode);
  }

  std::unique_ptr<modern_sqlite::File> delegate_;
  std::shared_ptr<LockTrackingState> state_;
};

class LockTrackingVfs final : public modern_sqlite::Vfs {
 public:
  explicit LockTrackingVfs(std::shared_ptr<LockTrackingState> state) : state_(std::move(state)) {}

 private:
  modern_sqlite::Result<modern_sqlite::OpenedFile> DoOpen(
      std::optional<std::string_view> path, modern_sqlite::FileOpenOptions options) override {
    auto opened = delegate_.Open(path, options);
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    const modern_sqlite::FileAccessMode access = opened->access;
    auto file = std::make_unique<LockTrackingFile>(std::move(opened->file), state_);
    return modern_sqlite::OpenedFile{
        .file = std::move(file),
        .access = access,
    };
  }

  modern_sqlite::Status DoDelete(std::string_view path,
                                 modern_sqlite::DirectorySync directory_sync) override {
    return delegate_.Delete(path, directory_sync);
  }

  modern_sqlite::Result<bool> DoAccess(std::string_view path,
                                       modern_sqlite::FileAccessQuery query) override {
    return delegate_.Access(path, query);
  }

  modern_sqlite::Result<std::string> DoFullPath(std::string_view path) override {
    return delegate_.FullPath(path);
  }

  modern_sqlite::Result<modern_sqlite::ByteCount> DoRandomBytes(
      modern_sqlite::MutableByteView output) override {
    auto random = delegate_.RandomBytes(output);
    if (!random.has_value()) {
      return std::unexpected(std::move(random.error()));
    }
    return modern_sqlite::ByteCount{output.size()};
  }

  modern_sqlite::Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) override {
    return delegate_.SleepFor(duration);
  }

  modern_sqlite::Result<modern_sqlite::WallClockTime> DoCurrentTime() override {
    return delegate_.CurrentTime();
  }

  [[nodiscard]] modern_sqlite::ByteCount DoMaximumPathLength() const noexcept override {
    return delegate_.MaximumPathLength();
  }

  modern_sqlite::PosixVfs delegate_;
  std::shared_ptr<LockTrackingState> state_;
};

[[nodiscard]] modern_sqlite::ReadStatement Prepare(modern_sqlite::ReadSession& session,
                                                   std::string_view sql) {
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    throw std::runtime_error{"unable to prepare session OOM statement"};
  }
  return std::move(*prepared->statement);
}

[[nodiscard]] bool IsOutOfMemory(const modern_sqlite::Error& error) noexcept {
  return error.code() == modern_sqlite::ErrorCode::kOutOfMemory;
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

  const std::vector<char> empty_bytes;
  const std::vector<char> v1 = ReadBytes(FixturePath("sqlite-3.54.0-session-v1.db"));
  const std::vector<char> v2 = ReadBytes(FixturePath("sqlite-3.54.0-session-v2.db"));

  const TemporaryDatabase open_database(empty_bytes);
  const std::string open_path = open_database.path().string();
  allocation_index.store(0, std::memory_order_relaxed);
  {
    const auto opened = ReadSession::Open(open_path);
    if (!opened.has_value()) {
      return 1;
    }
  }
  const std::size_t open_allocations = allocation_index.load(std::memory_order_relaxed);
  if (open_allocations == 0U || open_allocations > 64U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < open_allocations; ++failure) {
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool returned_oom = false;
    try {
      const auto opened = ReadSession::Open(open_path);
      returned_oom = !opened.has_value() && IsOutOfMemory(opened.error());
    } catch (...) {
      inject_failure = false;
      return 1;
    }
    inject_failure = false;
    if (!returned_oom) {
      return 1;
    }
  }

  const TemporaryDatabase prepare_database(empty_bytes);
  const std::string prepare_path = prepare_database.path().string();
  std::size_t prepare_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    {
      auto prepared = session->Prepare(Utf8View{"SELECT ?1"});
      if (!prepared.has_value() || !prepared->statement.has_value()) {
        return 1;
      }
    }
    prepare_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (prepare_allocations == 0U || prepare_allocations > 256U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < prepare_allocations; ++failure) {
    const auto locks = std::make_shared<LockTrackingState>();
    auto session = ReadSession::Open(std::make_unique<LockTrackingVfs>(locks), prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool returned_oom = false;
    try {
      auto prepared = session->Prepare(Utf8View{"SELECT ?1"});
      returned_oom = !prepared.has_value() && IsOutOfMemory(prepared.error());
    } catch (...) {
      inject_failure = false;
      return 1;
    }
    inject_failure = false;
    if (!returned_oom || locks->shared_lock_held) {
      return 1;
    }
  }

  {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement finalized = Prepare(*session, "SELECT 1");
    if (!finalized.Finalize().has_value()) {
      return 1;
    }

    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    bool step_returned_oom = false;
    try {
      const auto stepped = finalized.Step();
      step_returned_oom = !stepped.has_value() && IsOutOfMemory(stepped.error());
    } catch (...) {
      inject_failure = false;
      return 1;
    }
    inject_failure = false;
    if (!step_returned_oom || !finalized.Finalize().has_value()) {
      return 1;
    }

    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = 0;
    inject_failure = true;
    bool reset_returned_oom = false;
    try {
      const auto reset = finalized.Reset();
      reset_returned_oom = !reset.has_value() && IsOutOfMemory(reset.error());
    } catch (...) {
      inject_failure = false;
      return 1;
    }
    inject_failure = false;
    if (!reset_returned_oom || !finalized.Finalize().has_value()) {
      return 1;
    }
  }

  std::size_t reprepare_allocations = 0;
  {
    TemporaryDatabase database(v1);
    auto session = ReadSession::Open(database.path().string());
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT * FROM items WHERE id=?1");
    const SqlValue one = SqlValue::Integer(1);
    if (!statement.Bind(1, one).has_value()) {
      return 1;
    }
    database.Rewrite(v2);
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow) {
      return 1;
    }
    reprepare_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (reprepare_allocations == 0U || reprepare_allocations > 256U) {
    return 1;
  }

  for (std::size_t failure = 0; failure < reprepare_allocations; ++failure) {
    TemporaryDatabase database(v1);
    auto session = ReadSession::Open(database.path().string());
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT * FROM items WHERE id=?1");
    const SqlValue one = SqlValue::Integer(1);
    if (!statement.Bind(1, one).has_value()) {
      return 1;
    }
    database.Rewrite(v2);
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool returned_oom = false;
    try {
      const auto stepped = statement.Step();
      returned_oom = !stepped.has_value() && IsOutOfMemory(stepped.error());
    } catch (...) {
      inject_failure = false;
      return 1;
    }
    inject_failure = false;
    if (!returned_oom) {
      return 1;
    }

    const Status reset = statement.Reset();
    if (reset.has_value() || !IsOutOfMemory(reset.error())) {
      return 1;
    }
    const auto recovered = statement.Step();
    if (!recovered.has_value() || *recovered != ReadStep::kRow) {
      return 1;
    }
  }

  return 0;
} catch (...) {
  inject_failure = false;
  return 1;
}
