#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/native_vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "tests/support/aligned_allocation.hpp"

#if defined(_WIN32)
#include "tests/support/windows_temporary_directory.hpp"
#endif

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
std::size_t minimum_failing_size = std::numeric_limits<std::size_t>::max();
bool inject_failure = false;

constexpr std::string_view kPathlessTemporaryPrefix = "modern-sqlite-";
#if defined(_WIN32)
constexpr std::size_t kPathlessTemporarySuffixLength = 15;
#else
constexpr std::size_t kPathlessTemporarySuffixLength = 6;
#endif

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && (index == failing_allocation || size >= minimum_failing_size)) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && (index == failing_allocation || size >= minimum_failing_size)) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] std::filesystem::path FixturePath(std::string_view name) {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "session" / std::string{name};
}

[[nodiscard]] std::filesystem::path ReadCompatibilityFixturePath() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "fixtures" /
         "read_compatibility" / "sqlite-3.54.0-read-compatibility.db";
}

[[nodiscard]] std::vector<char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error{"unable to read session OOM fixture"};
  }
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

#if defined(_WIN32)
[[nodiscard]] std::optional<std::wstring> CaptureNativeTemporaryEnvironment() {
  const DWORD required = ::GetEnvironmentVariableW(L"TMP", nullptr, 0);
  if (required != 0) {
    std::wstring current(required, L'\0');
    ::SetLastError(ERROR_SUCCESS);
    const DWORD copied = ::GetEnvironmentVariableW(L"TMP", current.data(), required);
    if ((copied == 0 && ::GetLastError() != ERROR_SUCCESS) || copied >= required) {
      throw std::runtime_error{"unable to capture the session OOM temporary environment"};
    }
    current.resize(copied);
    return current;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_SUCCESS) {
    return std::wstring{};
  }
  if (error != ERROR_ENVVAR_NOT_FOUND) {
    throw std::runtime_error{"unable to capture the session OOM temporary environment"};
  }
  return std::nullopt;
}
#endif

class IsolatedTemporaryDirectory final {
 public:
#if defined(_WIN32)
  explicit IsolatedTemporaryDirectory(
      std::filesystem::path base = std::filesystem::temp_directory_path())
      : native_directory_(std::move(base)) {
    previous_tmpdir_ = CaptureNativeTemporaryEnvironment();
    path_ = native_directory_.NativePath("");
    if (::SetEnvironmentVariableW(L"TMP", path_.c_str()) == 0) {
      throw std::runtime_error{"unable to select isolated session OOM directory"};
    }
#else
  IsolatedTemporaryDirectory() {
    if (const char* current = std::getenv("TMPDIR"); current != nullptr) {
      previous_tmpdir_ = current;
    }
    std::string pattern =
        (std::filesystem::temp_directory_path() / "modern-sqlite-session-oom-XXXXXX").string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const char* created = ::mkdtemp(mutable_pattern.data());
    if (created == nullptr) {
      throw std::runtime_error{"unable to create isolated session OOM directory"};
    }
    path_ = created;
    if (::setenv("TMPDIR", path_.c_str(), 1) != 0) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
      throw std::runtime_error{"unable to select isolated session OOM directory"};
    }
#endif
  }

  IsolatedTemporaryDirectory(const IsolatedTemporaryDirectory&) = delete;
  IsolatedTemporaryDirectory& operator=(const IsolatedTemporaryDirectory&) = delete;
  IsolatedTemporaryDirectory(IsolatedTemporaryDirectory&&) = delete;
  IsolatedTemporaryDirectory& operator=(IsolatedTemporaryDirectory&&) = delete;

  ~IsolatedTemporaryDirectory() {
#if defined(_WIN32)
    if (::SetEnvironmentVariableW(
            L"TMP", previous_tmpdir_.has_value() ? previous_tmpdir_->c_str() : nullptr) == 0) {
      std::fputs("unable to restore the session OOM temporary environment\n", stderr);
    }
#else
    if (previous_tmpdir_.has_value()) {
      static_cast<void>(::setenv("TMPDIR", previous_tmpdir_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv("TMPDIR"));
    }
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
#endif
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
#if defined(_WIN32)
  modern_sqlite::test_support::WindowsTemporaryDirectory native_directory_;
#endif
  std::optional<std::filesystem::path::string_type> previous_tmpdir_;
  std::filesystem::path path_;
};

#if defined(_WIN32)
[[nodiscard]] int VerifyNativeTemporaryEnvironment() {
  const modern_sqlite::test_support::WindowsTemporaryDirectory parent;
  const std::array<std::optional<std::wstring>, 3> values{std::nullopt, std::wstring{},
                                                          parent.NativePath("").native()};
  for (const auto& expected : values) {
    if (::SetEnvironmentVariableW(L"TMP", expected.has_value() ? expected->c_str() : nullptr) ==
        0) {
      throw std::runtime_error{"unable to prepare the native temporary environment control"};
    }
    if (CaptureNativeTemporaryEnvironment() != expected) {
      throw std::runtime_error{"native temporary environment capture changed its value"};
    }
    std::filesystem::path isolated;
    {
      const IsolatedTemporaryDirectory directory{parent.NativePath("")};
      isolated = directory.path();
      if (CaptureNativeTemporaryEnvironment() !=
          std::optional<std::wstring>{directory.path().native()}) {
        throw std::runtime_error{"the native temporary fixture did not select its owned directory"};
      }
    }
    if (CaptureNativeTemporaryEnvironment() != expected || std::filesystem::exists(isolated)) {
      throw std::runtime_error{"the native temporary fixture did not restore and retire its state"};
    }
  }
  return 0;
}
#endif

[[nodiscard]] std::size_t CountPathlessTemporaryFiles(const std::filesystem::path& directory) {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator{directory}) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && name.starts_with(kPathlessTemporaryPrefix) &&
        name.size() == kPathlessTemporaryPrefix.size() + kPathlessTemporarySuffixLength) {
      ++count;
    }
  }
  return count;
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

  modern_sqlite::NativeVfs delegate_;
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

struct TemporaryConsumerCase {
  std::string_view name;
  std::string_view sql;
  bool joined_index_fixture = false;
  std::size_t warmup_rows = 0;
};

constexpr std::string_view kOrderedBlocksSql =
    "SELECT id FROM key_rows WHERE id<=?1 "
    "ORDER BY text_key COLLATE NOCASE,numeric_key+0,id LIMIT ?2";

constexpr std::array<TemporaryConsumerCase, 37> kTemporaryConsumers{
    TemporaryConsumerCase{.name = "scalar_membership", .sql = "SELECT ?1 IN (?1,?2,NULL)"},
    TemporaryConsumerCase{.name = "correlated_membership",
                          .sql = "SELECT (SELECT o.id IN (?1,?2,NULL)) "
                                 "FROM model_rows o WHERE o.id<=2"},
    TemporaryConsumerCase{.name = "compound_membership",
                          .sql = "SELECT id IN (?1,?2,NULL) FROM model_rows WHERE id<=2 "
                                 "UNION SELECT id IN (?2,?1,3) FROM model_rows WHERE id<=2"},
    TemporaryConsumerCase{.name = "aggregate_union_all",
                          .sql =
                              "SELECT count() FROM model_rows WHERE id<=?1 "
                              "UNION ALL SELECT sum(id) FROM model_rows WHERE id<=?2 ORDER BY 1"},
    TemporaryConsumerCase{.name = "aggregate_demand_one",
                          .sql = "SELECT count(value) FROM model_rows WHERE id<=?1 GROUP BY id "
                                 "UNION SELECT sum(id) FROM model_rows WHERE id<=?2 LIMIT 1"},
    TemporaryConsumerCase{.name = "cross_order",
                          .sql =
                              "SELECT a.label||b.label FROM model_rows a CROSS JOIN model_rows b "
                              "WHERE a.id<=?1/8 AND b.id<=?2/8 ORDER BY 1"},
    TemporaryConsumerCase{
        .name = "cross_demand_one",
        .sql = "SELECT b.label FROM model_rows a,model_rows b WHERE a.id=1 AND b.id>=?1 "
               "UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{
        .name = "joined_ordered_aggregate",
        .sql = "SELECT group_concat(a.label ORDER BY b.id,a.id) "
               "FROM model_rows a CROSS JOIN model_rows b WHERE a.id<=?1/8 AND b.id<=?2/8"},
    TemporaryConsumerCase{
        .name = "joined_grouping",
        .sql = "SELECT group_concat(a.label ORDER BY a.id) "
               "FROM model_rows a CROSS JOIN model_rows b WHERE a.id<=?1/8 AND b.id<=?2/8 "
               "GROUP BY b.id ORDER BY 1"},
    TemporaryConsumerCase{
        .name = "joined_distinct_set",
        .sql = "SELECT count(DISTINCT a.value) FROM model_rows a CROSS JOIN model_rows b "
               "WHERE a.id<=?1/8 AND b.id<=?2/8 UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{
        .name = "left_grouping",
        .sql =
            "SELECT group_concat(b.label ORDER BY b.id) FROM model_rows a LEFT JOIN model_rows b "
            "ON b.id=a.id+1 AND b.id<=?2/8 WHERE a.id<=?1/8 GROUP BY a.id ORDER BY a.id"},
    TemporaryConsumerCase{
        .name = "left_demand_one",
        .sql = "SELECT count(b.id) FROM model_rows a LEFT JOIN model_rows b ON b.id=a.id+1 "
               "WHERE a.id<=?1/8 AND b.id IS NULL UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{.name = "using_grouping",
                          .sql = "SELECT group_concat(b.name ORDER BY b.id) FROM model_rows a LEFT "
                                 "JOIN items b USING(id) "
                                 "WHERE a.id<=?1 GROUP BY id ORDER BY id LIMIT ?2"},
    TemporaryConsumerCase{.name = "natural_left_demand_one",
                          .sql =
                              "SELECT count(b.id) FROM model_rows a NATURAL LEFT JOIN model_rows b "
                              "WHERE a.id<=?1 UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{.name = "right_grouping",
                          .sql = "SELECT group_concat(a.name ORDER BY id) FROM items a RIGHT JOIN "
                                 "model_rows b USING(id) "
                                 "WHERE id<=?1 GROUP BY id ORDER BY id LIMIT ?2"},
    TemporaryConsumerCase{
        .name = "natural_right_demand_one",
        .sql = "SELECT count(a.id) FROM model_rows a NATURAL RIGHT JOIN model_rows b "
               "WHERE b.id<=?1 UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{
        .name = "full_grouping",
        .sql =
            "SELECT group_concat(a.name ORDER BY id) FROM items a FULL JOIN model_rows b USING(id) "
            "WHERE id<=?1 GROUP BY id ORDER BY id LIMIT ?2"},
    TemporaryConsumerCase{.name = "natural_full_demand_one",
                          .sql =
                              "SELECT count(a.id) FROM model_rows a NATURAL FULL JOIN model_rows b "
                              "WHERE id<=?1 UNION SELECT ?2 LIMIT 1"},
    TemporaryConsumerCase{.name = "scalar_order",
                          .sql = "SELECT (SELECT label FROM model_rows WHERE id<=?1 "
                                 "ORDER BY label DESC LIMIT 2 OFFSET ?2/8)"},
    TemporaryConsumerCase{.name = "scalar_grouped",
                          .sql = "SELECT (SELECT group_concat(label ORDER BY id DESC) "
                                 "FROM model_rows WHERE id<=?1 GROUP BY id%3 ORDER BY 1 LIMIT ?2)"},
    TemporaryConsumerCase{.name = "exists_intersect",
                          .sql = "SELECT EXISTS(SELECT label FROM model_rows WHERE id<=?1 "
                                 "INTERSECT SELECT label FROM model_rows WHERE id<=?2 "
                                 "LIMIT -1 OFFSET 1)"},
    TemporaryConsumerCase{
        .name = "correlated_order",
        .sql = "SELECT (SELECT i.label FROM model_rows i WHERE i.id<=o.id AND i.id<=?1 "
               "ORDER BY i.label DESC LIMIT ?2) FROM model_rows o WHERE o.id<=2"},
    TemporaryConsumerCase{
        .name = "correlated_grouped",
        .sql = "SELECT (SELECT group_concat(i.label ORDER BY i.id DESC) FROM model_rows i "
               "WHERE i.id<=o.id+?1/8 GROUP BY i.id%3 ORDER BY 1 LIMIT ?2) FROM items o"},
    TemporaryConsumerCase{
        .name = "correlated_set",
        .sql = "SELECT EXISTS(SELECT i.label FROM model_rows i WHERE i.id<=o.id+?1/8 "
               "INTERSECT SELECT j.label FROM model_rows j WHERE j.id<=?2/8) FROM items o"},
    TemporaryConsumerCase{
        .name = "correlated_joined_aggregate",
        .sql = "SELECT (SELECT group_concat(b.name||o.name ORDER BY b.id) FROM items a "
               "LEFT JOIN items b ON b.id=a.id WHERE a.id<=?1/8) FROM items o LIMIT ?2"},
    TemporaryConsumerCase{.name = "joined_index_full_aggregate",
                          .sql = "SELECT group_concat(b.id ORDER BY b.id DESC) FROM probe_rows p "
                                 "FULL JOIN key_rows b ON b.text_key=p.text_key "
                                 "WHERE p.id<=?1 OR p.id IS NULL LIMIT ?2",
                          .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "joined_index_scalar_group",
        .sql = "SELECT (SELECT group_concat(b.id ORDER BY b.id DESC) FROM probe_rows p "
               "LEFT JOIN key_rows b ON b.text_key=p.text_key "
               "WHERE p.id<=?1 GROUP BY p.id ORDER BY p.id LIMIT ?2)",
        .joined_index_fixture = true},
    TemporaryConsumerCase{.name = "joined_index_distinct_set",
                          .sql = "SELECT count(DISTINCT b.numeric_key) FROM probe_rows p "
                                 "JOIN key_rows b ON b.text_key=p.text_key WHERE p.id<=?1 "
                                 "UNION SELECT ?2 LIMIT 1",
                          .joined_index_fixture = true},
    TemporaryConsumerCase{.name = "joined_noncovering_full_aggregate",
                          .sql =
                              "SELECT group_concat(b.payload ORDER BY b.id DESC) FROM probe_rows p "
                              "FULL JOIN key_rows b ON b.text_key=p.text_key "
                              "WHERE p.id<=?1 OR p.id IS NULL LIMIT ?2",
                          .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "joined_noncovering_scalar_group",
        .sql = "SELECT (SELECT group_concat(b.payload ORDER BY b.id DESC) FROM probe_rows p "
               "LEFT JOIN key_rows b ON b.text_key=p.text_key "
               "WHERE p.id<=?1 GROUP BY p.id ORDER BY p.id LIMIT ?2)",
        .joined_index_fixture = true},
    TemporaryConsumerCase{.name = "joined_noncovering_distinct_set",
                          .sql =
                              "SELECT count(DISTINCT b.payload) FROM probe_rows p JOIN key_rows b "
                              "ON b.text_key=p.text_key WHERE p.id<=?1 UNION SELECT ?2 LIMIT 1",
                          .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "joined_range_full_aggregate",
        .sql = "SELECT group_concat(b.payload ORDER BY b.id DESC) FROM probe_rows p "
               "FULL JOIN key_rows b ON b.numeric_key>=p.numeric_key AND b.numeric_key<=5 "
               "WHERE p.id<=?1 OR p.id IS NULL LIMIT ?2",
        .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "joined_range_scalar_group",
        .sql = "SELECT (SELECT group_concat(b.payload ORDER BY b.id DESC) FROM probe_rows p "
               "LEFT JOIN key_rows b ON b.text_key IS p.text_key AND b.numeric_key>=p.numeric_key "
               "WHERE p.id<=?1 GROUP BY p.id ORDER BY p.id LIMIT ?2)",
        .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "joined_range_distinct_set",
        .sql = "SELECT count(DISTINCT b.payload) FROM probe_rows p JOIN key_rows b "
               "ON b.numeric_key>p.numeric_key WHERE p.id<=?1 UNION SELECT ?2 LIMIT 1",
        .joined_index_fixture = true},
    TemporaryConsumerCase{
        .name = "ordered_blocks", .sql = kOrderedBlocksSql, .joined_index_fixture = true},
    TemporaryConsumerCase{.name = "ordered_blocks_resume",
                          .sql = kOrderedBlocksSql,
                          .joined_index_fixture = true,
                          .warmup_rows = 3},
    TemporaryConsumerCase{.name = "ordered_inner",
                          .sql = "SELECT b.id FROM model_rows a CROSS JOIN model_rows b "
                                 "WHERE a.id<=?1/8 ORDER BY b.id LIMIT ?2/8"},
};

inline constexpr std::size_t kAllocationPartitions = 4;

void ReportAllocationBoundaries(std::string_view phase, std::size_t count,
                                std::optional<std::size_t> partition) {
  std::cout << "OOM allocation sweep " << phase << ": " << count
            << " boundaries, first=" << partition.value_or(0U)
            << ", stride=" << (partition.has_value() ? kAllocationPartitions : 1U) << '\n'
            << std::flush;
}

[[nodiscard]] int RunTemporaryConsumer(const TemporaryConsumerCase& test_case,
                                       std::optional<std::size_t> partition) {
  using namespace modern_sqlite;
  std::cout << "OOM temporary consumer " << test_case.name << ": start\n" << std::flush;
  const ReadSessionOptions options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{1U << 20U},
          },
  };
  const auto failed = [&](std::string_view phase) {
    std::cerr << "OOM temporary consumer " << test_case.name << ": " << phase << '\n';
    return 1;
  };
  const auto warmup = [&](ReadStatement& statement) {
    for (std::size_t row = 0; row < test_case.warmup_rows; ++row) {
      const auto stepped = statement.Step();
      if (!stepped.has_value() || *stepped != ReadStep::kRow || statement.row().size() != 1U) {
        return false;
      }
    }
    return true;
  };
  const TemporaryDatabase input_database{ReadBytes(
      test_case.joined_index_fixture ? ReadCompatibilityFixturePath().parent_path().parent_path() /
                                           "joined_index" / "sqlite-3.54.0-joined-index.db"
                                     : ReadCompatibilityFixturePath())};
  const std::string fixture = input_database.path().string();
  std::size_t allocations = 0;
  {
    auto session = ReadSession::Open(fixture, options);
    if (!session.has_value()) {
      return failed("baseline open failed");
    }
    ReadStatement statement = Prepare(*session, test_case.sql);
    if (!statement.Bind(1, SqlValue::Integer(16)).has_value() ||
        !statement.Bind(2, SqlValue::Integer(16)).has_value()) {
      return failed("baseline bind failed");
    }
    if (!warmup(statement)) {
      return failed("baseline warmup failed");
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow || statement.row().size() != 1U) {
      return failed("baseline step failed");
    }
    allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (allocations == 0U || allocations > 1024U) {
    return failed("baseline allocation range is invalid");
  }
  std::cout << "OOM temporary consumer " << test_case.name << ": " << allocations
            << " allocation boundaries\n"
            << std::flush;
  if (partition.has_value()) {
    std::cout << "OOM temporary consumer " << test_case.name << ": partition " << *partition << '/'
              << kAllocationPartitions << '\n'
              << std::flush;
  }
  const std::size_t stride = partition.has_value() ? kAllocationPartitions : 1U;
  for (std::size_t failure = partition.value_or(0U); failure < allocations; failure += stride) {
    auto session = ReadSession::Open(fixture, options);
    if (!session.has_value()) {
      return failed("injection setup open failed");
    }
    ReadStatement statement = Prepare(*session, test_case.sql);
    if (!statement.Bind(1, SqlValue::Integer(16)).has_value() ||
        !statement.Bind(2, SqlValue::Integer(16)).has_value()) {
      return failed("injection setup bind failed");
    }
    if (!warmup(statement)) {
      return failed("injection setup warmup failed");
    }
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool returned_oom = false;
    try {
      const auto stepped = statement.Step();
      returned_oom = !stepped.has_value() && IsOutOfMemory(stepped.error());
    } catch (...) {
      inject_failure = false;
      return failed("injected step escaped its typed result");
    }
    inject_failure = false;
    if (!returned_oom) {
      return failed("injected step did not report OOM");
    }
    const Status reset = statement.Reset();
    if (reset.has_value() || !IsOutOfMemory(reset.error())) {
      return failed("reset did not preserve the injected OOM");
    }
    const auto recovered = statement.Step();
    if (!recovered.has_value() || *recovered != ReadStep::kRow || statement.row().size() != 1U) {
      return failed("reset recovery did not reproduce a row");
    }
  }
  std::cout << "OOM temporary consumer " << test_case.name << ": complete\n" << std::flush;
  return 0;
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

int main(int argc, char** argv) try {
  using namespace modern_sqlite;

#if defined(_WIN32)
  if (argc == 2 && std::string_view{argv[1]} == "--verify-native-environment") {
    return VerifyNativeTemporaryEnvironment();
  }
#endif

  const bool combined_partition =
      argc == 3 && std::string_view{argv[1]} == "--allocation-partition";
  const TemporaryConsumerCase* temporary_consumer = nullptr;
  if (argc != 1 && !combined_partition) {
    if ((argc != 3 && argc != 5) || std::string_view{argv[1]} != "--temporary-consumer") {
      std::cerr << "usage: read_session_oom_test "
                   "[--allocation-partition 0|1|2|3 | "
                   "--temporary-consumer NAME [--allocation-partition 0|1|2|3]]\n";
      return 2;
    }
    const auto selected = std::ranges::find(kTemporaryConsumers, std::string_view{argv[2]},
                                            &TemporaryConsumerCase::name);
    if (selected == kTemporaryConsumers.end()) {
      std::cerr << "unknown temporary consumer: " << argv[2] << '\n';
      return 2;
    }
    temporary_consumer = &*selected;
  }
  std::optional<std::size_t> partition;
  if (combined_partition || argc == 5) {
    const std::string_view option{combined_partition ? argv[1] : argv[3]};
    const std::string_view number{combined_partition ? argv[2] : argv[4]};
    if (option != "--allocation-partition" || number.size() != 1U || number[0] < '0' ||
        number[0] > '3') {
      std::cerr << "allocation partition must be one of 0, 1, 2, 3\n";
      return 2;
    }
    partition = static_cast<std::size_t>(number[0] - '0');
  }
  const IsolatedTemporaryDirectory temporary_directory;
  const std::size_t initial_pathless_temporary_files =
      CountPathlessTemporaryFiles(temporary_directory.path());
  if (temporary_consumer != nullptr) {
    const int result = RunTemporaryConsumer(*temporary_consumer, partition);
    return result == 0 && CountPathlessTemporaryFiles(temporary_directory.path()) !=
                              initial_pathless_temporary_files
               ? 1
               : result;
  }
  const std::size_t first_failure = partition.value_or(0U);
  const std::size_t failure_stride = partition.has_value() ? kAllocationPartitions : 1U;
  const TemporaryDatabase joined_database{ReadBytes(ReadCompatibilityFixturePath())};
  const std::string joined_path = joined_database.path().string();

  if (first_failure == 0U) {
    for (const std::string_view sql :
         {"SELECT (SELECT coalesce(?1,o.value)) FROM storage_values o WHERE o.id=11",
          "SELECT (SELECT iif(?1 IS NULL,o.value,1)) FROM storage_values o WHERE o.id=11",
          "SELECT EXISTS(SELECT o.value AS x FROM storage_values o "
          "WHERE o.id=11 AND (SELECT coalesce(?1,x)) IS 1)",
          "SELECT EXISTS(SELECT o.value AS x FROM storage_values o "
          "WHERE o.id=11 AND (SELECT iif(?1 IS NULL,x,1)) IS 1)"}) {
      const bool existence = sql.starts_with("SELECT EXISTS(");
      for (const bool taken : {false, true}) {
        const auto locks = std::make_shared<LockTrackingState>();
        auto session = ReadSession::Open(std::make_unique<LockTrackingVfs>(locks), joined_path);
        if (!session.has_value()) {
          return 1;
        }
        ReadStatement statement = Prepare(*session, sql);
        if (!statement.Bind(1, taken ? SqlValue{} : SqlValue::Integer(1)).has_value()) {
          return 1;
        }
        failing_allocation = std::numeric_limits<std::size_t>::max();
        minimum_failing_size = 2000U;
        inject_failure = true;
        const auto stepped = statement.Step();
        inject_failure = false;
        minimum_failing_size = std::numeric_limits<std::size_t>::max();
        if (taken) {
          if (stepped.has_value() || !IsOutOfMemory(stepped.error()) || locks->shared_lock_held) {
            std::cerr << "taken correlated overflow read did not report and clean up OOM\n";
            return 1;
          }
          const Status reset = statement.Reset();
          if (reset.has_value() || !IsOutOfMemory(reset.error())) {
            return 1;
          }
        } else {
          if (!stepped.has_value() || *stepped != ReadStep::kRow ||
              statement.row()[0].integer_value() != 1) {
            std::cerr << "skipped correlated overflow read allocated its unused source value\n";
            return 1;
          }
          const auto done = statement.Step();
          if (!done.has_value() || *done != ReadStep::kDone || !statement.Reset().has_value()) {
            return 1;
          }
        }
        const auto recovered = statement.Step();
        if (!recovered.has_value() || *recovered != ReadStep::kRow) {
          return 1;
        }
        if (taken && !existence) {
          const auto blob = statement.row()[0].blob_value();
          if (!blob.has_value() || blob->size() != 2000U) {
            return 1;
          }
        } else if (taken && statement.row()[0].integer_value() != 0) {
          return 1;
        }
        const auto done = statement.Step();
        if (!done.has_value() || *done != ReadStep::kDone || locks->shared_lock_held) {
          return 1;
        }
      }
    }
  }

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

  ReportAllocationBoundaries("open", open_allocations, partition);
  for (std::size_t failure = first_failure; failure < open_allocations; failure += failure_stride) {
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

  ReportAllocationBoundaries("prepare", prepare_allocations, partition);
  for (std::size_t failure = first_failure; failure < prepare_allocations;
       failure += failure_stride) {
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

  constexpr std::array kArenaPrepareSql{
      std::string_view{"SELECT o.id IN (?1,?2,NULL),(SELECT o.id IN (1,2,3)) "
                       "FROM model_rows o ORDER BY 1"},
      std::string_view{"SELECT ?9 IN (),TRUE IN (TRUE),1 IN (\"A\" COLLATE NOCASE)"},
      std::string_view{"SELECT DISTINCT a.label,b.label FROM model_rows a CROSS JOIN model_rows b "
                       "WHERE a.id<=?1 AND b.id<=?2 ORDER BY a.label,b.label LIMIT ?3"},
      std::string_view{
          "SELECT (SELECT (SELECT label FROM model_rows ORDER BY label DESC LIMIT ?1)),"
          "EXISTS(SELECT sum(id) FROM model_rows HAVING sum(id)>?2)"},
      std::string_view{"SELECT o.id,(SELECT i.label FROM model_rows i WHERE i.id<=o.id "
                       "ORDER BY i.label DESC LIMIT ?1) AS x FROM model_rows o WHERE x IS NOT NULL "
                       "ORDER BY x LIMIT ?2"},
      std::string_view{
          "SELECT count(*),EXISTS(SELECT (SELECT o.label) ORDER BY o.value) FROM model_rows o"},
      std::string_view{"SELECT 1 AS x FROM model_rows WHERE (SELECT (SELECT x))>0"},
      std::string_view{"SELECT o.label AS x FROM model_rows o WHERE o.id<=?1 "
                       "AND EXISTS(SELECT x ORDER BY (SELECT x) LIMIT 1) ORDER BY (SELECT x)"},
      std::string_view{"SELECT o.nullable IS NULL AS x,count(*) FROM model_rows o "
                       "GROUP BY o.nullable IS NULL HAVING (SELECT x)>=0 ORDER BY x"},
      std::string_view{"SELECT a.id,b.label FROM model_rows a FULL JOIN model_rows b "
                       "ON b.id>a.id AND b.id<=a.id+?1 ORDER BY b.id DESC LIMIT ?2"},
  };
  for (const std::string_view prepare_sql : kArenaPrepareSql) {
    std::size_t joined_prepare_allocations = 0;
    {
      auto session = ReadSession::Open(joined_path);
      if (!session.has_value()) {
        return 1;
      }
      allocation_index.store(0, std::memory_order_relaxed);
      const auto prepared = session->Prepare(Utf8View{prepare_sql});
      if (!prepared.has_value() || !prepared->statement.has_value()) {
        return 1;
      }
      joined_prepare_allocations = allocation_index.load(std::memory_order_relaxed);
    }
    if (joined_prepare_allocations == 0U || joined_prepare_allocations > 1024U) {
      return 1;
    }
    ReportAllocationBoundaries(prepare_sql, joined_prepare_allocations, partition);
    for (std::size_t failure = first_failure; failure < joined_prepare_allocations;
         failure += failure_stride) {
      const auto locks = std::make_shared<LockTrackingState>();
      auto session = ReadSession::Open(std::make_unique<LockTrackingVfs>(locks), joined_path);
      if (!session.has_value()) {
        return 1;
      }
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool returned_oom = false;
      try {
        const auto prepared = session->Prepare(Utf8View{prepare_sql});
        returned_oom = !prepared.has_value() && IsOutOfMemory(prepared.error());
      } catch (...) {
        inject_failure = false;
        return 1;
      }
      inject_failure = false;
      if (!returned_oom || locks->shared_lock_held) {
        return 1;
      }
      const auto recovered = session->Prepare(Utf8View{prepare_sql});
      if (!recovered.has_value() || !recovered->statement.has_value()) {
        return 1;
      }
    }
  }

  constexpr std::string_view kAggregateSyntax =
      "SELECT abs(1 ORDER BY 2) FILTER (WHERE 1) GROUP BY 1 HAVING 1";
  std::size_t aggregate_prepare_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto prepared = session->Prepare(Utf8View{kAggregateSyntax});
    if (prepared.has_value() || prepared.error().code() != ErrorCode::kGeneric) {
      return 1;
    }
    aggregate_prepare_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (aggregate_prepare_allocations == 0U || aggregate_prepare_allocations > 256U) {
    return 1;
  }

  ReportAllocationBoundaries("aggregate syntax", aggregate_prepare_allocations, partition);
  for (std::size_t failure = first_failure; failure < aggregate_prepare_allocations;
       failure += failure_stride) {
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
      const auto prepared = session->Prepare(Utf8View{kAggregateSyntax});
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

  const ReadSessionOptions sorter_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{4096},
          },
  };
  std::size_t sorter_step_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path, sorter_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT ?1 AS value ORDER BY value");
    if (!statement.Bind(1, SqlValue::Integer(7)).has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow) {
      return 1;
    }
    sorter_step_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (sorter_step_allocations == 0U || sorter_step_allocations > 128U) {
    return 1;
  }

  ReportAllocationBoundaries("sorter step", sorter_step_allocations, partition);
  for (std::size_t failure = first_failure; failure < sorter_step_allocations;
       failure += failure_stride) {
    auto session = ReadSession::Open(prepare_path, sorter_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT ?1 AS value ORDER BY value");
    if (!statement.Bind(1, SqlValue::Integer(7)).has_value()) {
      return 1;
    }
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
    if (!recovered.has_value() || *recovered != ReadStep::kRow || statement.row().size() != 1U ||
        statement.row()[0].integer_value() != std::optional<std::int64_t>{7}) {
      return 1;
    }
  }

  std::size_t compound_step_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path, sorter_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "VALUES(1),(2) UNION SELECT 3 ORDER BY 1");
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow ||
        statement.row()[0].integer_value() != std::optional<std::int64_t>{1}) {
      return 1;
    }
    compound_step_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (compound_step_allocations == 0U || compound_step_allocations > 512U) {
    return 1;
  }

  ReportAllocationBoundaries("compound step", compound_step_allocations, partition);
  for (std::size_t failure = first_failure; failure < compound_step_allocations;
       failure += failure_stride) {
    auto session = ReadSession::Open(prepare_path, sorter_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "VALUES(1),(2) UNION SELECT 3 ORDER BY 1");
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
    if (!recovered.has_value() || *recovered != ReadStep::kRow ||
        statement.row()[0].integer_value() != std::optional<std::int64_t>{1}) {
      return 1;
    }
  }

  std::size_t aggregate_step_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT group_concat(?1)");
    if (!statement.Bind(1, SqlValue::Text("aggregate")).has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow || statement.row().size() != 1U) {
      return 1;
    }
    aggregate_step_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (aggregate_step_allocations == 0U || aggregate_step_allocations > 128U) {
    return 1;
  }

  ReportAllocationBoundaries("aggregate step", aggregate_step_allocations, partition);
  for (std::size_t failure = first_failure; failure < aggregate_step_allocations;
       failure += failure_stride) {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, "SELECT group_concat(?1)");
    if (!statement.Bind(1, SqlValue::Text("aggregate")).has_value()) {
      return 1;
    }
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
    if (!recovered.has_value() || *recovered != ReadStep::kRow || statement.row().size() != 1U) {
      return 1;
    }
  }

  constexpr std::string_view kModifierSql =
      "SELECT group_concat(?1 ORDER BY ?2),count(DISTINCT ?3)";
  std::size_t modifier_step_allocations = 0;
  {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, kModifierSql);
    if (!statement.Bind(1, SqlValue::Text("modifier")).has_value() ||
        !statement.Bind(2, SqlValue::Integer(1)).has_value() ||
        !statement.Bind(3, SqlValue::Text("distinct")).has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow || statement.row().size() != 2U) {
      return 1;
    }
    modifier_step_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (modifier_step_allocations == 0U || modifier_step_allocations > 512U) {
    return 1;
  }

  ReportAllocationBoundaries("modifier step", modifier_step_allocations, partition);
  for (std::size_t failure = first_failure; failure < modifier_step_allocations;
       failure += failure_stride) {
    auto session = ReadSession::Open(prepare_path);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, kModifierSql);
    if (!statement.Bind(1, SqlValue::Text("modifier")).has_value() ||
        !statement.Bind(2, SqlValue::Integer(1)).has_value() ||
        !statement.Bind(3, SqlValue::Text("distinct")).has_value()) {
      return 1;
    }
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
    if (!recovered.has_value() || *recovered != ReadStep::kRow || statement.row().size() != 2U) {
      return 1;
    }
  }

  constexpr std::string_view kIntegratedAggregateSql =
      "SELECT nullable IS NULL,"
      "count(),"
      "min(label),"
      "group_concat(label ORDER BY id DESC) FILTER (WHERE value>=?1),"
      "count(DISTINCT value<0) "
      "FROM model_rows "
      "GROUP BY nullable IS NULL "
      "HAVING count()>?2 "
      "ORDER BY 1";
  const ReadSessionOptions aggregate_options{
      .temporary_storage =
          TemporaryStorageOptions{
              .mode = TemporaryStoreMode::kMemory,
              .sorter_memory_threshold = ByteCount{1U << 20U},
          },
  };
  std::size_t integrated_aggregate_allocations = 0;
  {
    auto session = ReadSession::Open(joined_path, aggregate_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, kIntegratedAggregateSql);
    if (!statement.Bind(1, SqlValue::Integer(0)).has_value() ||
        !statement.Bind(2, SqlValue::Integer(0)).has_value()) {
      return 1;
    }
    allocation_index.store(0, std::memory_order_relaxed);
    const auto stepped = statement.Step();
    if (!stepped.has_value() || *stepped != ReadStep::kRow || statement.row().size() != 5U) {
      return 1;
    }
    integrated_aggregate_allocations = allocation_index.load(std::memory_order_relaxed);
  }
  if (integrated_aggregate_allocations == 0U || integrated_aggregate_allocations > 1024U) {
    return 1;
  }

  ReportAllocationBoundaries("integrated aggregate step", integrated_aggregate_allocations,
                             partition);
  for (std::size_t failure = first_failure; failure < integrated_aggregate_allocations;
       failure += failure_stride) {
    auto session = ReadSession::Open(joined_path, aggregate_options);
    if (!session.has_value()) {
      return 1;
    }
    ReadStatement statement = Prepare(*session, kIntegratedAggregateSql);
    if (!statement.Bind(1, SqlValue::Integer(0)).has_value() ||
        !statement.Bind(2, SqlValue::Integer(0)).has_value()) {
      return 1;
    }
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
    if (!recovered.has_value() || *recovered != ReadStep::kRow || statement.row().size() != 5U) {
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

  ReportAllocationBoundaries("schema reprepare", reprepare_allocations, partition);
  for (std::size_t failure = first_failure; failure < reprepare_allocations;
       failure += failure_stride) {
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

  return CountPathlessTemporaryFiles(temporary_directory.path()) == initial_pathless_temporary_files
             ? 0
             : 1;
} catch (const std::exception& error) {
  inject_failure = false;
  std::fprintf(stderr, "read-session OOM fixture: %s\n", error.what());
  return 1;
} catch (...) {
  inject_failure = false;
  std::fputs("read-session OOM fixture: unknown exception\n", stderr);
  return 1;
}
