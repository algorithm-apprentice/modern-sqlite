#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/instrumentation/counters.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "write_performance_build_config.hpp"

#if !defined(NDEBUG)
#error "The write performance benchmark requires NDEBUG"
#endif

#if !defined(_MSC_VER) && !defined(__OPTIMIZE__)
#error "The write performance benchmark requires compiler optimization"
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(__SANITIZE_UNDEFINED__) || defined(__COVERAGE__) || defined(__GCOV__)
#error "The write performance benchmark forbids sanitizers and coverage"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#error "The write performance benchmark forbids sanitizers"
#endif
#endif

#ifndef MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
#define MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS 0
#endif

#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS != 0 && \
    MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS != 1
#error "MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS must be 0 or 1"
#endif

namespace {

constexpr int kSqliteOpenFlags = static_cast<int>(static_cast<unsigned int>(SQLITE_OPEN_READWRITE) |
                                                  static_cast<unsigned int>(SQLITE_OPEN_CREATE) |
                                                  static_cast<unsigned int>(SQLITE_OPEN_NOMUTEX));
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kKeyOrderSeed = 0xD1B54A32D192ED03ULL;
constexpr std::size_t kValueSize = 256;
constexpr std::size_t kTimingRepetitions = 3;
constexpr std::uint64_t kMinimumWallNanoseconds = 20'000'000ULL;
constexpr std::int64_t kPopulatedRows = 65'536;
constexpr std::string_view kInsertSql = "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)";
constexpr std::string_view kUpdatePointSql = "UPDATE kv SET v=?1,version=version+1 WHERE k=?2";
constexpr std::string_view kDeletePointSql = "DELETE FROM kv WHERE k=?1";
constexpr std::string_view kSqliteVersion = "3.54.0";
constexpr std::string_view kSqliteSourceId =
    "2026-10-02 20:18:07 "
    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2";

[[nodiscard]] sqlite3_destructor_type SqliteTransient() noexcept {
  // SQLite defines SQLITE_TRANSIENT as the function-pointer sentinel -1.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return reinterpret_cast<sqlite3_destructor_type>(static_cast<std::intptr_t>(-1));
}

class HarnessFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class BenchmarkMismatch final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class EngineKind : std::uint8_t {
  kModern,
  kSqlite,
};

enum class ProfileKind : std::uint8_t {
  kEngineDefault,
  kMatchedDurable,
};

enum class CaseKind : std::uint8_t {
  kCreate,
  kInsertPoint,
  kInsertBatch,
  kUpdatePoint,
  kUpdateScan,
  kDeletePoint,
  kDeleteScan,
  kMixedCommit,
  kMixedRollback,
};

enum class RunKind : std::uint8_t {
  kSmoke,
  kBaseline,
};

struct WorkResult {
  std::uint64_t changed_rows = 0;
  std::int64_t last_insert_rowid = 0;
};

struct WorkloadScale {
  std::size_t operations = 0;
  std::uint64_t transactions = 0;
  std::uint64_t dml_operations = 0;
  std::uint64_t row_mutations = 0;
};

struct EffectiveConfiguration {
  std::int64_t page_size = 0;
  std::int64_t cache_size = 0;
  std::int64_t mmap_bytes = 0;
  std::string temp_store;
  std::string journal_mode;
  std::string synchronous;
  std::string locking_mode;
  std::string thread_mode;

  bool operator==(const EffectiveConfiguration&) const = default;
};

enum class DiagnosticFileGroup : std::uint8_t {
  kMainDatabase,
  kMainJournal,
  kSubjournal,
  kWriteAheadLog,
};

constexpr std::size_t kDiagnosticFileGroupCount = 4;

struct FileDiagnosticCounters {
  std::uint64_t open_calls = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_calls = 0;
  std::uint64_t write_bytes = 0;
  std::uint64_t sync_calls = 0;
  std::uint64_t truncate_calls = 0;
  std::uint64_t delete_calls = 0;
  std::uint64_t directory_sync_requests = 0;
  std::uint64_t lock_calls = 0;
  std::uint64_t unlock_calls = 0;
  std::uint64_t access_calls = 0;
  std::uint64_t full_path_calls = 0;
};

struct VfsDiagnosticCounters {
  std::array<FileDiagnosticCounters, kDiagnosticFileGroupCount> files{};
  std::uint64_t random_byte_calls = 0;
  std::uint64_t delegated_vfs_calls = 0;
};

struct ModernCounterValues {
  std::array<std::uint64_t, modern_sqlite::instrumentation::kCounterCount> values{};
};

struct SqliteCounterValues {
  std::uint64_t cache_bytes_current = 0;
  std::uint64_t cache_hits = 0;
  std::uint64_t cache_misses = 0;
  std::uint64_t cache_writes = 0;
  std::uint64_t changes = 0;
  std::uint64_t fullscan_steps = 0;
  std::uint64_t malloc_count_current = 0;
  std::uint64_t malloc_count_highwater = 0;
  std::uint64_t malloc_size_highwater = 0;
  std::uint64_t reprepares = 0;
  std::uint64_t statement_runs = 0;
  std::uint64_t total_changes = 0;
  std::uint64_t vm_steps = 0;
};

struct ExpectedRecord {
  std::int64_t rowid;
  std::int64_t value_seed;
  std::int64_t version;
};

struct Verification {
  std::size_t rows = 0;
  std::size_t schema_objects = 0;
  std::string digest;

  bool operator==(const Verification&) const = default;
};

struct DatabaseFingerprint {
  std::string sha256;
  std::uint64_t size_bytes = 0;
  std::uint32_t page_count = 0;
  std::uint32_t freelist_count = 0;
  std::uint32_t schema_cookie = 0;

  bool operator==(const DatabaseFingerprint&) const = default;
};

template <typename T>
[[nodiscard]] T TakeValue(modern_sqlite::Result<T> result) {
  if (!result.has_value()) {
    throw HarnessFailure{result.error().ToString()};
  }
  return std::move(*result);
}

void RequireStatus(modern_sqlite::Status status) {
  if (!status.has_value()) {
    throw HarnessFailure{status.error().ToString()};
  }
}

class SqliteDatabase final {
 public:
  explicit SqliteDatabase(const std::filesystem::path& path) {
    const int result =
        sqlite3_open_v2(path.string().c_str(), &database_, kSqliteOpenFlags, nullptr);
    if (result != SQLITE_OK) {
      const std::string message =
          database_ == nullptr ? "sqlite3_open_v2 failed" : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        static_cast<void>(sqlite3_close(database_));
        database_ = nullptr;
      }
      throw HarnessFailure{message};
    }
  }

  SqliteDatabase(const SqliteDatabase&) = delete;
  SqliteDatabase& operator=(const SqliteDatabase&) = delete;

  ~SqliteDatabase() noexcept {
    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close(database_));
    }
  }

  [[nodiscard]] sqlite3* get() const noexcept { return database_; }

  void Close() {
    if (database_ != nullptr && sqlite3_close(database_) != SQLITE_OK) {
      throw HarnessFailure{"sqlite3_close failed"};
    }
    database_ = nullptr;
  }

 private:
  sqlite3* database_ = nullptr;
};

class SqliteStatement final {
 public:
  SqliteStatement(sqlite3* database, std::string_view sql) {
    const std::string owned{sql};
    const int result = sqlite3_prepare_v3(database, owned.c_str(), -1, SQLITE_PREPARE_PERSISTENT,
                                          &statement_, nullptr);
    if (result != SQLITE_OK) {
      throw HarnessFailure{sqlite3_errmsg(database)};
    }
  }

  SqliteStatement(const SqliteStatement&) = delete;
  SqliteStatement& operator=(const SqliteStatement&) = delete;

  SqliteStatement(SqliteStatement&& other) noexcept
      : statement_(std::exchange(other.statement_, nullptr)) {}

  SqliteStatement& operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
      if (statement_ != nullptr) {
        static_cast<void>(sqlite3_finalize(statement_));
      }
      statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
  }

  ~SqliteStatement() noexcept {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

  void Finalize() {
    if (statement_ != nullptr && sqlite3_finalize(statement_) != SQLITE_OK) {
      throw HarnessFailure{"sqlite3_finalize failed"};
    }
    statement_ = nullptr;
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

[[nodiscard]] constexpr std::size_t DiagnosticFileGroupIndex(DiagnosticFileGroup group) noexcept {
  return static_cast<std::size_t>(group);
}

[[nodiscard]] std::optional<DiagnosticFileGroup> DiagnosticGroupFor(modern_sqlite::FileKind kind) {
  switch (kind) {
    case modern_sqlite::FileKind::kMainDatabase:
      return DiagnosticFileGroup::kMainDatabase;
    case modern_sqlite::FileKind::kMainJournal:
      return DiagnosticFileGroup::kMainJournal;
    case modern_sqlite::FileKind::kSubjournal:
      return DiagnosticFileGroup::kSubjournal;
    case modern_sqlite::FileKind::kWriteAheadLog:
      return DiagnosticFileGroup::kWriteAheadLog;
    case modern_sqlite::FileKind::kTemporaryDatabase:
    case modern_sqlite::FileKind::kTransientDatabase:
    case modern_sqlite::FileKind::kTemporaryJournal:
    case modern_sqlite::FileKind::kSuperJournal:
      return std::nullopt;
  }
  throw HarnessFailure{"invalid diagnostic file kind"};
}

[[nodiscard]] DiagnosticFileGroup DiagnosticGroupForPath(std::string_view path) noexcept {
  if (path.ends_with("-journal")) {
    return DiagnosticFileGroup::kMainJournal;
  }
  if (path.ends_with("-wal")) {
    return DiagnosticFileGroup::kWriteAheadLog;
  }
  return DiagnosticFileGroup::kMainDatabase;
}

class CountingFile final : public modern_sqlite::File {
 public:
  CountingFile(std::unique_ptr<modern_sqlite::File> delegate, VfsDiagnosticCounters& counters,
               FileDiagnosticCounters* file_counters, bool delete_on_close) noexcept
      : delegate_(std::move(delegate)),
        counters_(&counters),
        file_counters_(file_counters),
        delete_on_close_(delete_on_close) {}

  ~CountingFile() override {
    if (file_counters_ != nullptr) {
      ++FileCounters().close_calls;
      if (delete_on_close_) {
        ++FileCounters().delete_calls;
      }
    }
  }

 protected:
  [[nodiscard]] modern_sqlite::Result<modern_sqlite::ByteCount> DoReadAt(
      modern_sqlite::MutableByteView destination, modern_sqlite::FileOffset offset) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().read_calls;
    }
    auto result = delegate_->ReadAt(destination, offset);
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    if (file_counters_ != nullptr) {
      FileCounters().read_bytes += static_cast<std::uint64_t>(result->bytes_read().value());
    }
    return result->bytes_read();
  }

  [[nodiscard]] modern_sqlite::Status DoWriteAt(modern_sqlite::ByteView source,
                                                modern_sqlite::FileOffset offset) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().write_calls;
      FileCounters().write_bytes += static_cast<std::uint64_t>(source.size());
    }
    return delegate_->WriteAt(source, offset);
  }

  [[nodiscard]] modern_sqlite::Status DoTruncate(modern_sqlite::FileSize size) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().truncate_calls;
    }
    return delegate_->Truncate(size);
  }

  [[nodiscard]] modern_sqlite::Status DoSync(modern_sqlite::SyncOptions options) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().sync_calls;
    }
    return delegate_->Sync(options);
  }

  [[nodiscard]] modern_sqlite::Result<modern_sqlite::FileSize> DoSize() override {
    CountDelegatedCall();
    return delegate_->Size();
  }

  [[nodiscard]] modern_sqlite::Status DoLock(modern_sqlite::DatabaseLock lock) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().lock_calls;
    }
    return delegate_->Lock(lock);
  }

  [[nodiscard]] modern_sqlite::Status DoUnlock(modern_sqlite::DatabaseLock lock) override {
    CountDelegatedCall();
    if (file_counters_ != nullptr) {
      ++FileCounters().unlock_calls;
    }
    return delegate_->Unlock(lock);
  }

  [[nodiscard]] modern_sqlite::Result<bool> DoHasReservedLock() override {
    CountDelegatedCall();
    return delegate_->HasReservedLock();
  }

  [[nodiscard]] modern_sqlite::FileProperties DoProperties() const noexcept override {
    CountDelegatedCall();
    auto result = delegate_->Properties();
    if (!result.has_value()) {
      std::terminate();
    }
    return *result;
  }

  [[nodiscard]] modern_sqlite::Result<std::optional<modern_sqlite::MutableByteView>>
  DoMapSharedMemory(modern_sqlite::SharedMemoryRegionIndex region,
                    modern_sqlite::ByteCount region_size,
                    modern_sqlite::SharedMemoryMapMode mode) override {
    CountDelegatedCall();
    return delegate_->MapSharedMemory(region, region_size, mode);
  }

  [[nodiscard]] modern_sqlite::Status DoLockSharedMemory(
      modern_sqlite::SharedMemoryLockRange range,
      modern_sqlite::SharedMemoryLockOperation operation,
      modern_sqlite::SharedMemoryLockMode mode) override {
    CountDelegatedCall();
    return delegate_->LockSharedMemory(range, operation, mode);
  }

  void DoSharedMemoryBarrier() noexcept override {
    CountDelegatedCall();
    delegate_->SharedMemoryBarrier();
  }

  [[nodiscard]] modern_sqlite::Status DoUnmapSharedMemory(
      modern_sqlite::SharedMemoryUnmapMode mode) override {
    CountDelegatedCall();
    return delegate_->UnmapSharedMemory(mode);
  }

 private:
  [[nodiscard]] FileDiagnosticCounters& FileCounters() const noexcept { return *file_counters_; }

  void CountDelegatedCall() const noexcept { ++counters_->delegated_vfs_calls; }

  std::unique_ptr<modern_sqlite::File> delegate_;
  VfsDiagnosticCounters* counters_;
  FileDiagnosticCounters* file_counters_;
  bool delete_on_close_;
};

class CountingVfs final : public modern_sqlite::Vfs {
 public:
  explicit CountingVfs(VfsDiagnosticCounters& counters)
      : delegate_(std::make_unique<modern_sqlite::PosixVfs>()), counters_(&counters) {}

 protected:
  [[nodiscard]] modern_sqlite::Result<modern_sqlite::OpenedFile> DoOpen(
      std::optional<std::string_view> path, modern_sqlite::FileOpenOptions options) override {
    CountDelegatedCall();
    auto opened = delegate_->Open(path, options);
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    const std::optional<DiagnosticFileGroup> group = DiagnosticGroupFor(options.kind);
    if (group.has_value()) {
      ++FileCounters(*group).open_calls;
    }
    FileDiagnosticCounters* file_counters = group.has_value() ? &FileCounters(*group) : nullptr;
    try {
      return modern_sqlite::OpenedFile{
          .file = std::make_unique<CountingFile>(std::move(opened->file), *counters_, file_counters,
                                                 options.delete_on_close),
          .access = opened->access,
      };
    } catch (const std::bad_alloc&) {
      return std::unexpected(modern_sqlite::Error::OutOfMemory());
    }
  }

  [[nodiscard]] modern_sqlite::Status DoDelete(
      std::string_view path, modern_sqlite::DirectorySync directory_sync) override {
    CountDelegatedCall();
    FileDiagnosticCounters& counters = FileCounters(DiagnosticGroupForPath(path));
    ++counters.delete_calls;
    if (directory_sync == modern_sqlite::DirectorySync::kYes) {
      ++counters.directory_sync_requests;
    }
    return delegate_->Delete(path, directory_sync);
  }

  [[nodiscard]] modern_sqlite::Result<bool> DoAccess(
      std::string_view path, modern_sqlite::FileAccessQuery query) override {
    CountDelegatedCall();
    ++FileCounters(DiagnosticGroupForPath(path)).access_calls;
    return delegate_->Access(path, query);
  }

  [[nodiscard]] modern_sqlite::Result<std::string> DoFullPath(std::string_view path) override {
    CountDelegatedCall();
    ++FileCounters(DiagnosticGroupForPath(path)).full_path_calls;
    return delegate_->FullPath(path);
  }

  [[nodiscard]] modern_sqlite::Result<modern_sqlite::ByteCount> DoRandomBytes(
      modern_sqlite::MutableByteView output) override {
    CountDelegatedCall();
    ++counters_->random_byte_calls;
    modern_sqlite::Status status = delegate_->RandomBytes(output);
    if (!status.has_value()) {
      return std::unexpected(std::move(status.error()));
    }
    return modern_sqlite::ByteCount{output.size()};
  }

  [[nodiscard]] modern_sqlite::Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) override {
    CountDelegatedCall();
    return delegate_->SleepFor(duration);
  }

  [[nodiscard]] modern_sqlite::Result<modern_sqlite::WallClockTime> DoCurrentTime() override {
    CountDelegatedCall();
    return delegate_->CurrentTime();
  }

  [[nodiscard]] modern_sqlite::ByteCount DoMaximumPathLength() const noexcept override {
    return delegate_->MaximumPathLength();
  }

 private:
  [[nodiscard]] FileDiagnosticCounters& FileCounters(DiagnosticFileGroup group) const noexcept {
    return counters_->files[DiagnosticFileGroupIndex(group)];
  }

  void CountDelegatedCall() const noexcept { ++counters_->delegated_vfs_calls; }

  std::unique_ptr<modern_sqlite::Vfs> delegate_;
  VfsDiagnosticCounters* counters_;
};

class Digest final {
 public:
  void AddByte(std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= kFnvPrime;
  }

  void AddInteger(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
      AddByte(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU));
    }
  }

  void AddBytes(modern_sqlite::ByteView bytes) noexcept {
    for (const std::byte byte : bytes) {
      AddByte(std::to_integer<std::uint8_t>(byte));
    }
  }

  [[nodiscard]] std::string Hex() const {
    std::array<char, 17> output{};
    const auto [end, error] = std::to_chars(output.data(), output.data() + 16, value_, 16);
    if (error != std::errc{}) {
      throw HarnessFailure{"cannot render result digest"};
    }
    const auto digits = static_cast<std::size_t>(end - output.data());
    std::string result(16U - digits, '0');
    result.append(output.data(), digits);
    return result;
  }

 private:
  std::uint64_t value_ = kFnvOffset;
};

class Sha256 final {
 public:
  void Update(modern_sqlite::ByteView bytes) {
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - total_bytes_) {
      throw HarnessFailure{"SHA-256 input length overflow"};
    }
    total_bytes_ += static_cast<std::uint64_t>(bytes.size());
    for (const std::byte byte : bytes) {
      block_[block_size_++] = byte;
      if (block_size_ == block_.size()) {
        Transform();
        block_size_ = 0;
      }
    }
  }

  [[nodiscard]] std::string Hex() const {
    Sha256 finished = *this;
    if (finished.total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8U) {
      throw HarnessFailure{"SHA-256 bit length overflow"};
    }
    const std::uint64_t bit_length = finished.total_bytes_ * 8U;
    finished.block_[finished.block_size_++] = std::byte{0x80};
    if (finished.block_size_ > 56U) {
      while (finished.block_size_ < finished.block_.size()) {
        finished.block_[finished.block_size_++] = std::byte{0};
      }
      finished.Transform();
      finished.block_size_ = 0;
    }
    while (finished.block_size_ < 56U) {
      finished.block_[finished.block_size_++] = std::byte{0};
    }
    for (std::size_t index = 0; index < 8U; ++index) {
      const std::size_t shift = (7U - index) * 8U;
      finished.block_[finished.block_size_++] =
          static_cast<std::byte>((bit_length >> shift) & 0xffU);
    }
    finished.Transform();

    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const std::uint32_t word : finished.state_) {
      for (std::size_t index = 0; index < 4U; ++index) {
        const std::size_t shift = (3U - index) * 8U;
        const auto byte = static_cast<std::uint8_t>((word >> shift) & 0xffU);
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
      }
    }
    return result;
  }

 private:
  [[nodiscard]] static std::uint32_t LoadWord(std::span<const std::byte, 4> bytes) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]));
  }

  void Transform() noexcept {
    static constexpr std::array<std::uint32_t, 64> round_constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };
    std::array<std::uint32_t, 64> schedule{};
    const std::span<const std::byte> block{block_};
    for (std::size_t index = 0; index < 16U; ++index) {
      schedule[index] = LoadWord(std::span<const std::byte, 4>{block.subspan(index * 4U, 4U)});
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
      const std::uint32_t first = std::rotr(schedule[index - 15U], 7) ^
                                  std::rotr(schedule[index - 15U], 18) ^
                                  (schedule[index - 15U] >> 3U);
      const std::uint32_t second = std::rotr(schedule[index - 2U], 17) ^
                                   std::rotr(schedule[index - 2U], 19) ^
                                   (schedule[index - 2U] >> 10U);
      schedule[index] = schedule[index - 16U] + first + schedule[index - 7U] + second;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const std::uint32_t temporary1 = h + sum1 + choose + round_constants[index] + schedule[index];
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::byte, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] modern_sqlite::ByteBuffer ValueFor(std::int64_t seed) {
  modern_sqlite::ByteBuffer value{modern_sqlite::ByteCount{kValueSize}};
  std::ranges::fill(value.mutable_view(), std::byte{'0'});
  std::array<char, 16> digits{};
  const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), seed, 16);
  if (error != std::errc{}) {
    throw HarnessFailure{"cannot render benchmark value"};
  }
  const auto count = static_cast<std::size_t>(end - digits.data());
  if (count > 8U) {
    throw HarnessFailure{"benchmark value seed exceeds its prefix"};
  }
  const std::size_t offset = 8U - count;
  for (std::size_t index = 0; index < count; ++index) {
    value.mutable_view()[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(digits[index]));
  }
  return value;
}

[[nodiscard]] std::string TableName(std::size_t index) {
  std::array<char, 4> digits{};
  const auto [end, error] = std::to_chars(digits.data(), digits.data() + 3, index);
  if (error != std::errc{}) {
    throw HarnessFailure{"cannot render CREATE table index"};
  }
  const auto count = static_cast<std::size_t>(end - digits.data());
  std::string name{"t"};
  name.append(3U - count, '0');
  name.append(digits.data(), count);
  return name;
}

void ExecuteSqlite(sqlite3* database, std::string_view sql) {
  const std::string owned{sql};
  char* error = nullptr;
  if (sqlite3_exec(database, owned.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(database) : std::string{error};
    sqlite3_free(error);
    throw HarnessFailure{message};
  }
}

[[nodiscard]] std::int64_t SqliteSingleInteger(sqlite3* database, std::string_view sql) {
  SqliteStatement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
    throw HarnessFailure{"SQLite configuration query returned no integer"};
  }
  const std::int64_t value = sqlite3_column_int64(statement.get(), 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw HarnessFailure{"SQLite configuration query returned extra rows"};
  }
  statement.Finalize();
  return value;
}

[[nodiscard]] std::string SqliteSingleText(sqlite3* database, std::string_view sql) {
  SqliteStatement statement{database, sql};
  if (sqlite3_step(statement.get()) != SQLITE_ROW ||
      sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT) {
    throw HarnessFailure{"SQLite configuration query returned no text"};
  }
  const unsigned char* text = sqlite3_column_text(statement.get(), 0);
  const int bytes = sqlite3_column_bytes(statement.get(), 0);
  if (text == nullptr || bytes < 0) {
    throw HarnessFailure{"SQLite configuration query returned invalid text"};
  }
  const std::string value{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw HarnessFailure{"SQLite configuration query returned extra rows"};
  }
  statement.Finalize();
  return value;
}

void VerifySqliteIdentity() {
  if (std::string_view{sqlite3_libversion()} != kSqliteVersion ||
      std::string_view{sqlite3_sourceid()} != kSqliteSourceId) {
    throw HarnessFailure{"write benchmark SQLite identity is not pinned"};
  }
}

[[nodiscard]] std::string_view SqliteSynchronousName(std::int64_t value) {
  switch (value) {
    case 0:
      return "off";
    case 1:
      return "normal";
    case 2:
      return "full";
    case 3:
      return "extra";
    default:
      throw HarnessFailure{"SQLite synchronous mode is invalid"};
  }
}

[[nodiscard]] std::string_view SqliteTempStoreName(std::int64_t value) {
  switch (value) {
    case 0:
      return "default";
    case 1:
      return "file";
    case 2:
      return "memory";
    default:
      throw HarnessFailure{"SQLite temp-store mode is invalid"};
  }
}

[[nodiscard]] EffectiveConfiguration ReadSqliteConfiguration(sqlite3* database) {
  return EffectiveConfiguration{
      .page_size = SqliteSingleInteger(database, "PRAGMA page_size"),
      .cache_size = SqliteSingleInteger(database, "PRAGMA cache_size"),
      .mmap_bytes = SqliteSingleInteger(database, "PRAGMA mmap_size"),
      .temp_store =
          std::string{SqliteTempStoreName(SqliteSingleInteger(database, "PRAGMA temp_store"))},
      .journal_mode = SqliteSingleText(database, "PRAGMA journal_mode"),
      .synchronous =
          std::string{SqliteSynchronousName(SqliteSingleInteger(database, "PRAGMA synchronous"))},
      .locking_mode = SqliteSingleText(database, "PRAGMA locking_mode"),
      .thread_mode = "single",
  };
}

[[nodiscard]] EffectiveConfiguration ConfigureSqlite(sqlite3* database, ProfileKind profile) {
  VerifySqliteIdentity();
  if (profile == ProfileKind::kMatchedDurable) {
    ExecuteSqlite(database, "PRAGMA cache_size=512");
    ExecuteSqlite(database, "PRAGMA mmap_size=0");
    ExecuteSqlite(database, "PRAGMA temp_store=MEMORY");
    ExecuteSqlite(database, "PRAGMA journal_mode=DELETE");
    ExecuteSqlite(database, "PRAGMA synchronous=FULL");
    ExecuteSqlite(database, "PRAGMA locking_mode=NORMAL");
  }
  EffectiveConfiguration configuration = ReadSqliteConfiguration(database);
  if (profile == ProfileKind::kMatchedDurable && configuration != EffectiveConfiguration{
                                                                      .page_size = 4096,
                                                                      .cache_size = 512,
                                                                      .mmap_bytes = 0,
                                                                      .temp_store = "memory",
                                                                      .journal_mode = "delete",
                                                                      .synchronous = "full",
                                                                      .locking_mode = "normal",
                                                                      .thread_mode = "single",
                                                                  }) {
    throw HarnessFailure{"write benchmark SQLite matched configuration differs"};
  }
  return configuration;
}

[[nodiscard]] EffectiveConfiguration ModernConfiguration() {
  return EffectiveConfiguration{
      .page_size = 4096,
      .cache_size = 512,
      .mmap_bytes = 0,
      .temp_store = "memory",
      .journal_mode = "delete",
      .synchronous = "full",
      .locking_mode = "normal",
      .thread_mode = "single",
  };
}

[[nodiscard]] modern_sqlite::WriteStatement PrepareModern(modern_sqlite::WriteSession& session,
                                                          std::string_view sql) {
  modern_sqlite::WritePrepareOutput prepared =
      TakeValue(session.Prepare(modern_sqlite::Utf8View{sql}));
  if (!prepared.statement.has_value()) {
    throw HarnessFailure{"Modern preparation produced no statement"};
  }
  return std::move(*prepared.statement);
}

void StepModernCommand(modern_sqlite::WriteStatement& statement) {
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone) {
    throw BenchmarkMismatch{"Modern transaction command produced a row"};
  }
}

void StepSqliteCommand(const SqliteStatement& statement) {
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    throw BenchmarkMismatch{"SQLite transaction command completion differs"};
  }
}

void VerifyModernLastInsertRowid(const modern_sqlite::WriteSession& session,
                                 std::int64_t expected) {
  if (session.last_insert_rowid() != expected) {
    throw BenchmarkMismatch{"Modern last-insert-rowid differs"};
  }
}

void VerifySqliteLastInsertRowid(sqlite3* database, sqlite3_int64 expected) {
  if (sqlite3_last_insert_rowid(database) != expected) {
    throw BenchmarkMismatch{"SQLite last-insert-rowid differs"};
  }
}

[[nodiscard]] std::uint64_t StepModern(modern_sqlite::WriteStatement& statement,
                                       const modern_sqlite::WriteSession& session,
                                       std::uint64_t expected_changes) {
  if (TakeValue(statement.Step()) != modern_sqlite::WriteStep::kDone ||
      session.changes() != expected_changes) {
    throw BenchmarkMismatch{"Modern DML completion differs"};
  }
  RequireStatus(statement.Reset());
  return expected_changes;
}

[[nodiscard]] std::uint64_t StepSqlite(const SqliteStatement& statement, sqlite3* database,
                                       std::uint64_t expected_changes) {
  if (sqlite3_step(statement.get()) != SQLITE_DONE ||
      sqlite3_changes64(database) != static_cast<sqlite3_int64>(expected_changes) ||
      sqlite3_reset(statement.get()) != SQLITE_OK ||
      sqlite3_clear_bindings(statement.get()) != SQLITE_OK) {
    throw BenchmarkMismatch{"SQLite DML completion differs"};
  }
  return expected_changes;
}

struct BlobBinding {
  std::size_t index;
  std::int64_t seed;
};

void BindModernBlob(modern_sqlite::WriteStatement& statement, BlobBinding binding) {
  const modern_sqlite::SqlValue value = modern_sqlite::SqlValue::Blob(ValueFor(binding.seed));
  RequireStatus(statement.Bind(binding.index, value));
}

void BindSqliteBlob(const SqliteStatement& statement, BlobBinding binding) {
  if (!std::in_range<int>(binding.index)) {
    throw HarnessFailure{"SQLite BLOB binding index is out of range"};
  }
  const modern_sqlite::ByteBuffer value = ValueFor(binding.seed);
  if (sqlite3_bind_blob(statement.get(), static_cast<int>(binding.index), value.view().data(),
                        static_cast<int>(value.view().size()), SqliteTransient()) != SQLITE_OK) {
    throw HarnessFailure{"SQLite BLOB binding failed"};
  }
}

[[nodiscard]] std::uint64_t RunCreateModern(modern_sqlite::WriteSession& session,
                                            std::size_t operations,
                                            std::vector<modern_sqlite::WriteStatement>& completed) {
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    modern_sqlite::WriteStatement statement = PrepareModern(session, sql);
    StepModernCommand(statement);
    if (session.changes() != 0U) {
      throw BenchmarkMismatch{"Modern CREATE change count differs"};
    }
    VerifyModernLastInsertRowid(session, 0);
    completed.push_back(std::move(statement));
  }
  return 0;
}

[[nodiscard]] std::uint64_t RunCreateSqlite(sqlite3* database, std::size_t operations,
                                            std::vector<SqliteStatement>& completed) {
  for (std::size_t index = 0; index < operations; ++index) {
    const std::string sql =
        "CREATE TABLE " + TableName(index) + "(id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')";
    SqliteStatement statement{database, sql};
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      throw BenchmarkMismatch{"SQLite CREATE completion differs"};
    }
    if (sqlite3_changes64(database) != 0) {
      throw BenchmarkMismatch{"SQLite CREATE change count differs"};
    }
    VerifySqliteLastInsertRowid(database, 0);
    completed.push_back(std::move(statement));
  }
  return 0;
}

[[nodiscard]] std::uint64_t RunInsertModern(const modern_sqlite::WriteSession& session,
                                            modern_sqlite::WriteStatement& statement,
                                            modern_sqlite::WriteStatement* begin,
                                            modern_sqlite::WriteStatement* commit,
                                            std::size_t operations) {
  const bool explicit_transaction = begin != nullptr;
  if (explicit_transaction) {
    if (commit == nullptr) {
      throw HarnessFailure{"Modern explicit INSERT has no COMMIT statement"};
    }
    StepModernCommand(*begin);
  }
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<std::int64_t>(index) + 1;
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  if (explicit_transaction) {
    StepModernCommand(*commit);
  }
  VerifyModernLastInsertRowid(session, static_cast<std::int64_t>(operations));
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunInsertSqlite(sqlite3* database, const SqliteStatement& statement,
                                            const SqliteStatement* begin,
                                            const SqliteStatement* commit, std::size_t operations) {
  const bool explicit_transaction = begin != nullptr;
  if (explicit_transaction) {
    if (commit == nullptr) {
      throw HarnessFailure{"SQLite explicit INSERT has no COMMIT statement"};
    }
    StepSqliteCommand(*begin);
  }
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto rowid = static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    BindSqliteBlob(statement, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, rowid);
  }
  if (explicit_transaction) {
    StepSqliteCommand(*commit);
  }
  VerifySqliteLastInsertRowid(database, static_cast<sqlite3_int64>(operations));
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointModern(const modern_sqlite::WriteSession& session,
                                                 modern_sqlite::WriteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    BindModernBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdatePointSqlite(sqlite3* database,
                                                 const SqliteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    BindSqliteBlob(statement, {.index = 1, .seed = rowid + 1'000'000});
    if (sqlite3_bind_int64(statement.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanModern(const modern_sqlite::WriteSession& session,
                                                modern_sqlite::WriteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  BindModernBlob(statement, {.index = 1, .seed = 9'000'000});
  if (maximum_rowid.has_value()) {
    RequireStatus(statement.Bind(2, modern_sqlite::SqlValue::Integer(*maximum_rowid)));
  }
  const std::uint64_t changed_rows = StepModern(statement, session, expected_changes);
  VerifyModernLastInsertRowid(session, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunUpdateScanSqlite(sqlite3* database, const SqliteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  BindSqliteBlob(statement, {.index = 1, .seed = 9'000'000});
  if (maximum_rowid.has_value() &&
      sqlite3_bind_int64(statement.get(), 2, *maximum_rowid) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database, expected_changes);
  VerifySqliteLastInsertRowid(database, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointModern(const modern_sqlite::WriteSession& session,
                                                 modern_sqlite::WriteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statement, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeletePointSqlite(sqlite3* database,
                                                 const SqliteStatement& statement,
                                                 std::span<const std::int64_t> rowids) {
  std::uint64_t changed_rows = 0;
  for (const std::int64_t rowid : rowids) {
    if (sqlite3_bind_int64(statement.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite rowid binding failed"};
    }
    changed_rows += StepSqlite(statement, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanModern(const modern_sqlite::WriteSession& session,
                                                modern_sqlite::WriteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  if (maximum_rowid.has_value()) {
    RequireStatus(statement.Bind(1, modern_sqlite::SqlValue::Integer(*maximum_rowid)));
  }
  const std::uint64_t changed_rows = StepModern(statement, session, expected_changes);
  VerifyModernLastInsertRowid(session, 0);
  return changed_rows;
}

[[nodiscard]] std::uint64_t RunDeleteScanSqlite(sqlite3* database, const SqliteStatement& statement,
                                                std::optional<std::int64_t> maximum_rowid,
                                                std::uint64_t expected_changes) {
  if (maximum_rowid.has_value() &&
      sqlite3_bind_int64(statement.get(), 1, *maximum_rowid) != SQLITE_OK) {
    throw HarnessFailure{"SQLite scan bound failed"};
  }
  const std::uint64_t changed_rows = StepSqlite(statement, database, expected_changes);
  VerifySqliteLastInsertRowid(database, 0);
  return changed_rows;
}

struct MixedCounts {
  std::size_t updates;
  std::size_t deletes;
  std::size_t inserts;
};

[[nodiscard]] MixedCounts SplitMixed(std::size_t operations) {
  const std::size_t updates = operations / 3U;
  const std::size_t deletes = operations / 3U;
  return MixedCounts{
      .updates = updates,
      .deletes = deletes,
      .inserts = operations - updates - deletes,
  };
}

struct ModernMixedStatements {
  modern_sqlite::WriteStatement& begin;
  modern_sqlite::WriteStatement& update;
  modern_sqlite::WriteStatement& remove;
  modern_sqlite::WriteStatement& insert;
  modern_sqlite::WriteStatement& terminal;
};

[[nodiscard]] std::uint64_t RunMixedModern(const modern_sqlite::WriteSession& session,
                                           ModernMixedStatements statements,
                                           std::span<const std::int64_t> permutation,
                                           std::size_t operations) {
  const MixedCounts counts = SplitMixed(operations);
  StepModernCommand(statements.begin);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const std::int64_t rowid = permutation[index];
    BindModernBlob(statements.update, {.index = 1, .seed = rowid + 2'000'000});
    RequireStatus(statements.update.Bind(2, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statements.update, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const std::int64_t rowid = permutation[counts.updates + index];
    RequireStatus(statements.remove.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    changed_rows += StepModern(statements.remove, session, 1);
    VerifyModernLastInsertRowid(session, 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<std::int64_t>(index) + 1;
    RequireStatus(statements.insert.Bind(1, modern_sqlite::SqlValue::Integer(rowid)));
    BindModernBlob(statements.insert, {.index = 2, .seed = rowid});
    changed_rows += StepModern(statements.insert, session, 1);
    VerifyModernLastInsertRowid(session, rowid);
  }
  StepModernCommand(statements.terminal);
  const std::int64_t expected_last_insert_rowid =
      kPopulatedRows + static_cast<std::int64_t>(counts.inserts);
  VerifyModernLastInsertRowid(session, expected_last_insert_rowid);
  return changed_rows;
}

struct SqliteMixedStatements {
  const SqliteStatement& begin;
  const SqliteStatement& update;
  const SqliteStatement& remove;
  const SqliteStatement& insert;
  const SqliteStatement& terminal;
};

[[nodiscard]] std::uint64_t RunMixedSqlite(sqlite3* database, SqliteMixedStatements statements,
                                           std::span<const std::int64_t> permutation,
                                           std::size_t operations) {
  const MixedCounts counts = SplitMixed(operations);
  StepSqliteCommand(statements.begin);
  std::uint64_t changed_rows = 0;
  for (std::size_t index = 0; index < counts.updates; ++index) {
    const std::int64_t rowid = permutation[index];
    BindSqliteBlob(statements.update, {.index = 1, .seed = rowid + 2'000'000});
    if (sqlite3_bind_int64(statements.update.get(), 2, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed update binding failed"};
    }
    changed_rows += StepSqlite(statements.update, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  for (std::size_t index = 0; index < counts.deletes; ++index) {
    const std::int64_t rowid = permutation[counts.updates + index];
    if (sqlite3_bind_int64(statements.remove.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed delete binding failed"};
    }
    changed_rows += StepSqlite(statements.remove, database, 1);
    VerifySqliteLastInsertRowid(database, 0);
  }
  for (std::size_t index = 0; index < counts.inserts; ++index) {
    const auto rowid = kPopulatedRows + static_cast<sqlite3_int64>(index) + 1;
    if (sqlite3_bind_int64(statements.insert.get(), 1, rowid) != SQLITE_OK) {
      throw HarnessFailure{"SQLite mixed insert binding failed"};
    }
    BindSqliteBlob(statements.insert, {.index = 2, .seed = rowid});
    changed_rows += StepSqlite(statements.insert, database, 1);
    VerifySqliteLastInsertRowid(database, rowid);
  }
  StepSqliteCommand(statements.terminal);
  const sqlite3_int64 expected_last_insert_rowid =
      kPopulatedRows + static_cast<sqlite3_int64>(counts.inserts);
  VerifySqliteLastInsertRowid(database, expected_last_insert_rowid);
  return changed_rows;
}

[[nodiscard]] EngineKind ParseEngine(std::string_view value) {
  if (value == "modern") {
    return EngineKind::kModern;
  }
  if (value == "sqlite") {
    return EngineKind::kSqlite;
  }
  throw HarnessFailure{"unsupported write benchmark engine"};
}

[[nodiscard]] ProfileKind ParseProfile(std::string_view value) {
  if (value == "engine-default") {
    return ProfileKind::kEngineDefault;
  }
  if (value == "matched-durable") {
    return ProfileKind::kMatchedDurable;
  }
  throw HarnessFailure{"unsupported write benchmark profile"};
}

[[nodiscard]] CaseKind ParseCase(std::string_view value) {
  if (value == "create-table-implicit") {
    return CaseKind::kCreate;
  }
  if (value == "insert-point-implicit") {
    return CaseKind::kInsertPoint;
  }
  if (value == "insert-batch-explicit") {
    return CaseKind::kInsertBatch;
  }
  if (value == "update-point-implicit") {
    return CaseKind::kUpdatePoint;
  }
  if (value == "update-scan-implicit") {
    return CaseKind::kUpdateScan;
  }
  if (value == "delete-point-implicit") {
    return CaseKind::kDeletePoint;
  }
  if (value == "delete-scan-implicit") {
    return CaseKind::kDeleteScan;
  }
  if (value == "mixed-batch-commit") {
    return CaseKind::kMixedCommit;
  }
  if (value == "mixed-batch-rollback") {
    return CaseKind::kMixedRollback;
  }
  throw HarnessFailure{"unsupported write benchmark case"};
}

[[nodiscard]] std::string_view ProfileName(ProfileKind profile) noexcept {
  return profile == ProfileKind::kEngineDefault ? "engine-default" : "matched-durable";
}

[[maybe_unused, nodiscard]] RunKind ParseRunKind(std::string_view value) {
  if (value == "smoke") {
    return RunKind::kSmoke;
  }
  if (value == "baseline") {
    return RunKind::kBaseline;
  }
  throw HarnessFailure{"run kind must be smoke or baseline"};
}

[[maybe_unused, nodiscard]] std::string_view RunKindName(RunKind kind) noexcept {
  return kind == RunKind::kSmoke ? "smoke" : "baseline";
}

[[nodiscard]] WorkloadScale ScaleFor(CaseKind kind, RunKind run_kind) {
  const bool smoke = run_kind == RunKind::kSmoke;
  switch (kind) {
    case CaseKind::kCreate:
      return WorkloadScale{
          .operations = smoke ? 1U : 256U,
          .transactions = smoke ? 1U : 256U,
          .dml_operations = smoke ? 1U : 256U,
          .row_mutations = 0,
      };
    case CaseKind::kInsertPoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kInsertBatch:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = smoke ? 8U : 65'536U,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kUpdatePoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kUpdateScan:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = 1,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kDeletePoint:
      return WorkloadScale{
          .operations = smoke ? 1U : 512U,
          .transactions = smoke ? 1U : 512U,
          .dml_operations = smoke ? 1U : 512U,
          .row_mutations = smoke ? 1U : 512U,
      };
    case CaseKind::kDeleteScan:
      return WorkloadScale{
          .operations = smoke ? 8U : 65'536U,
          .transactions = 1,
          .dml_operations = 1,
          .row_mutations = smoke ? 8U : 65'536U,
      };
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback:
      return WorkloadScale{
          .operations = smoke ? 8U : 12'288U,
          .transactions = 1,
          .dml_operations = smoke ? 8U : 12'288U,
          .row_mutations = smoke ? 8U : 12'288U,
      };
  }
  throw HarnessFailure{"invalid write benchmark case"};
}

[[nodiscard]] std::vector<std::int64_t> GenerateKeyOrder() {
  std::vector<std::int64_t> result(static_cast<std::size_t>(kPopulatedRows));
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::int64_t>(index) + 1;
  }
  std::uint64_t state = kKeyOrderSeed;
  const auto next = [&state] {
    state += kSplitMixIncrement;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  };
  for (std::size_t index = result.size() - 1U; index > 0; --index) {
    const std::uint64_t bound = static_cast<std::uint64_t>(index) + 1U;
    const std::uint64_t threshold = (0U - bound) % bound;
    std::uint64_t random = 0;
    do {
      random = next();
    } while (random < threshold);
    const auto selected = static_cast<std::size_t>(random % bound);
    std::swap(result[index], result[selected]);
  }
  return result;
}

[[nodiscard]] std::uint64_t ProcessCpuNanoseconds() {
  timespec value{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0) {
    throw HarnessFailure{"cannot read CLOCK_PROCESS_CPUTIME_ID"};
  }
  const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / 1'000'000'000ULL) {
    throw HarnessFailure{"process CPU clock overflow"};
  }
  return seconds * 1'000'000'000ULL + nanoseconds;
}

struct TimedWork {
  std::size_t index = 0;
  std::uint64_t wall_ns = 0;
  std::uint64_t cpu_ns = 0;
  WorkResult work;
};

template <typename Callable>
[[nodiscard]] TimedWork RunMeasured(std::size_t index, bool measured, Callable&& callable) {
  if (!measured) {
    return TimedWork{.index = index, .work = std::forward<Callable>(callable)()};
  }
  const std::uint64_t cpu_started = ProcessCpuNanoseconds();
  const auto wall_started = std::chrono::steady_clock::now();
  const WorkResult work = std::forward<Callable>(callable)();
  const auto wall_finished = std::chrono::steady_clock::now();
  const std::uint64_t cpu_finished = ProcessCpuNanoseconds();
  if (cpu_finished < cpu_started) {
    throw HarnessFailure{"process CPU clock moved backward"};
  }
  const auto wall_duration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_finished - wall_started).count();
  if (wall_duration <= 0) {
    throw HarnessFailure{"steady clock produced a nonpositive duration"};
  }
  return TimedWork{
      .index = index,
      .wall_ns = static_cast<std::uint64_t>(wall_duration),
      .cpu_ns = cpu_finished - cpu_started,
      .work = work,
  };
}

struct Execution {
  TimedWork timed;
  EffectiveConfiguration configuration;
};

[[nodiscard]] std::optional<std::int64_t> ScanMaximum(const WorkloadScale& scale) {
  if (scale.operations == static_cast<std::size_t>(kPopulatedRows)) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(scale.operations);
}

[[nodiscard]] std::string_view UpdateScanSql(const WorkloadScale& scale) {
  return ScanMaximum(scale).has_value() ? "UPDATE kv SET v=?1,version=version+1 WHERE k<=?2"
                                        : "UPDATE kv SET v=?1,version=version+1 WHERE k>=1";
}

[[nodiscard]] std::string_view DeleteScanSql(const WorkloadScale& scale) {
  return ScanMaximum(scale).has_value() ? "DELETE FROM kv WHERE k<=?1"
                                        : "DELETE FROM kv WHERE k>=1";
}

void FinalizeModern(modern_sqlite::WriteStatement& statement) {
  RequireStatus(statement.Finalize());
}

[[nodiscard]] Execution ExecuteModernWork(CaseKind kind, const std::filesystem::path& path,
                                          const WorkloadScale& scale,
                                          std::span<const std::int64_t> key_order, bool measured,
                                          std::size_t index, VfsDiagnosticCounters* vfs_counters) {
  modern_sqlite::Result<modern_sqlite::WriteSession> opened =
      vfs_counters == nullptr ? modern_sqlite::WriteSession::Open(path.string())
                              : modern_sqlite::WriteSession::Open(
                                    std::make_unique<CountingVfs>(*vfs_counters), path.string());
  modern_sqlite::WriteSession session = TakeValue(std::move(opened));
  const EffectiveConfiguration configuration = ModernConfiguration();
  TimedWork timed;
  switch (kind) {
    case CaseKind::kCreate: {
      std::vector<modern_sqlite::WriteStatement> completed;
      completed.reserve(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunCreateModern(session, scale.operations, completed),
            .last_insert_rowid = 0,
        };
      });
      for (modern_sqlite::WriteStatement& statement : completed) {
        FinalizeModern(statement);
      }
      break;
    }
    case CaseKind::kInsertPoint:
    case CaseKind::kInsertBatch: {
      const bool explicit_transaction = kind == CaseKind::kInsertBatch;
      std::optional<modern_sqlite::WriteStatement> begin;
      std::optional<modern_sqlite::WriteStatement> commit;
      if (explicit_transaction) {
        begin.emplace(PrepareModern(session, "BEGIN"));
        commit.emplace(PrepareModern(session, "COMMIT"));
      }
      modern_sqlite::WriteStatement statement = PrepareModern(session, kInsertSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunInsertModern(session, statement, begin.has_value() ? &*begin : nullptr,
                                commit.has_value() ? &*commit : nullptr, scale.operations),
            .last_insert_rowid = static_cast<std::int64_t>(scale.operations),
        };
      });
      FinalizeModern(statement);
      if (begin.has_value() && commit.has_value()) {
        FinalizeModern(*begin);
        FinalizeModern(*commit);
      }
      break;
    }
    case CaseKind::kUpdatePoint: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, kUpdatePointSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdatePointModern(session, statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kUpdateScan: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, UpdateScanSql(scale));
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdateScanModern(session, statement, ScanMaximum(scale), scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kDeletePoint: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, kDeletePointSql);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeletePointModern(session, statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kDeleteScan: {
      modern_sqlite::WriteStatement statement = PrepareModern(session, DeleteScanSql(scale));
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeleteScanModern(session, statement, ScanMaximum(scale), scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      FinalizeModern(statement);
      break;
    }
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const bool rollback = kind == CaseKind::kMixedRollback;
      modern_sqlite::WriteStatement begin = PrepareModern(session, "BEGIN");
      modern_sqlite::WriteStatement update = PrepareModern(session, kUpdatePointSql);
      modern_sqlite::WriteStatement remove = PrepareModern(session, kDeletePointSql);
      modern_sqlite::WriteStatement insert = PrepareModern(session, kInsertSql);
      modern_sqlite::WriteStatement terminal =
          PrepareModern(session, rollback ? "ROLLBACK" : "COMMIT");
      const MixedCounts counts = SplitMixed(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunMixedModern(session,
                                           ModernMixedStatements{
                                               .begin = begin,
                                               .update = update,
                                               .remove = remove,
                                               .insert = insert,
                                               .terminal = terminal,
                                           },
                                           key_order, scale.operations),
            .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
        };
      });
      FinalizeModern(update);
      FinalizeModern(remove);
      FinalizeModern(insert);
      FinalizeModern(begin);
      FinalizeModern(terminal);
      break;
    }
  }
  return Execution{.timed = timed, .configuration = configuration};
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> SqliteDbStatus(sqlite3* database,
                                                                     int operation, bool reset) {
  int current = 0;
  int highwater = 0;
  const int result = sqlite3_db_status(database, operation, &current, &highwater, reset ? 1 : 0);
  if (result != SQLITE_OK || current < 0 || highwater < 0) {
    throw BenchmarkMismatch{"SQLite diagnostic db status failed"};
  }
  return {
      static_cast<std::uint64_t>(current),
      static_cast<std::uint64_t>(highwater),
  };
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> SqliteGlobalStatus(int operation,
                                                                         bool reset) {
  sqlite3_int64 current = 0;
  sqlite3_int64 highwater = 0;
  const int result = sqlite3_status64(operation, &current, &highwater, reset ? 1 : 0);
  if (result != SQLITE_OK || current < 0 || highwater < 0) {
    throw BenchmarkMismatch{"SQLite diagnostic global status failed"};
  }
  return {
      static_cast<std::uint64_t>(current),
      static_cast<std::uint64_t>(highwater),
  };
}

void ResetSqliteDiagnosticCounters(sqlite3* database) {
  static_cast<void>(SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_HIT, true));
  static_cast<void>(SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_MISS, true));
  static_cast<void>(SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_WRITE, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, true));
  static_cast<void>(SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, true));
}

void AccumulateSqliteStatementCounters(const SqliteStatement& statement,
                                       SqliteCounterValues& counters) {
  const auto read = [&statement](int operation) {
    const int value = sqlite3_stmt_status(statement.get(), operation, 0);
    if (value < 0) {
      throw BenchmarkMismatch{"SQLite diagnostic statement status failed"};
    }
    return static_cast<std::uint64_t>(value);
  };
  counters.vm_steps += read(SQLITE_STMTSTATUS_VM_STEP);
  counters.fullscan_steps += read(SQLITE_STMTSTATUS_FULLSCAN_STEP);
  counters.statement_runs += read(SQLITE_STMTSTATUS_RUN);
  counters.reprepares += read(SQLITE_STMTSTATUS_REPREPARE);
}

void CaptureSqliteDiagnosticCounters(sqlite3* database, SqliteCounterValues& counters) {
  counters.cache_hits = SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_HIT, false).first;
  counters.cache_misses = SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_MISS, false).first;
  counters.cache_writes = SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_WRITE, false).first;
  counters.cache_bytes_current = SqliteDbStatus(database, SQLITE_DBSTATUS_CACHE_USED, false).first;
  const auto malloc_count = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_COUNT, false);
  counters.malloc_count_current = malloc_count.first;
  counters.malloc_count_highwater = malloc_count.second;
  counters.malloc_size_highwater = SqliteGlobalStatus(SQLITE_STATUS_MALLOC_SIZE, false).second;
  const sqlite3_int64 changes = sqlite3_changes64(database);
  const sqlite3_int64 total_changes = sqlite3_total_changes64(database);
  if (changes < 0 || total_changes < 0) {
    throw BenchmarkMismatch{"SQLite diagnostic change count is negative"};
  }
  counters.changes = static_cast<std::uint64_t>(changes);
  counters.total_changes = static_cast<std::uint64_t>(total_changes);
}

[[nodiscard]] Execution ExecuteSqliteWork(CaseKind kind, ProfileKind profile,
                                          const std::filesystem::path& path,
                                          const WorkloadScale& scale,
                                          std::span<const std::int64_t> key_order, bool measured,
                                          std::size_t index, SqliteCounterValues* sqlite_counters) {
  SqliteDatabase database{path};
  const EffectiveConfiguration configuration = ConfigureSqlite(database.get(), profile);
  if (sqlite_counters != nullptr) {
    ResetSqliteDiagnosticCounters(database.get());
  }
  TimedWork timed;
  switch (kind) {
    case CaseKind::kCreate: {
      std::vector<SqliteStatement> completed;
      completed.reserve(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunCreateSqlite(database.get(), scale.operations, completed),
            .last_insert_rowid = 0,
        };
      });
      for (SqliteStatement& statement : completed) {
        if (sqlite_counters != nullptr) {
          AccumulateSqliteStatementCounters(statement, *sqlite_counters);
        }
        statement.Finalize();
      }
      break;
    }
    case CaseKind::kInsertPoint:
    case CaseKind::kInsertBatch: {
      const bool explicit_transaction = kind == CaseKind::kInsertBatch;
      std::optional<SqliteStatement> begin;
      std::optional<SqliteStatement> commit;
      if (explicit_transaction) {
        begin.emplace(database.get(), "BEGIN");
        commit.emplace(database.get(), "COMMIT");
      }
      SqliteStatement statement{database.get(), kInsertSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunInsertSqlite(database.get(), statement, begin.has_value() ? &*begin : nullptr,
                                commit.has_value() ? &*commit : nullptr, scale.operations),
            .last_insert_rowid = static_cast<std::int64_t>(scale.operations),
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(statement, *sqlite_counters);
      }
      statement.Finalize();
      if (begin.has_value() && commit.has_value()) {
        if (sqlite_counters != nullptr) {
          AccumulateSqliteStatementCounters(*begin, *sqlite_counters);
          AccumulateSqliteStatementCounters(*commit, *sqlite_counters);
        }
        begin->Finalize();
        commit->Finalize();
      }
      break;
    }
    case CaseKind::kUpdatePoint: {
      SqliteStatement statement{database.get(), kUpdatePointSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunUpdatePointSqlite(database.get(), statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(statement, *sqlite_counters);
      }
      statement.Finalize();
      break;
    }
    case CaseKind::kUpdateScan: {
      SqliteStatement statement{database.get(), UpdateScanSql(scale)};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunUpdateScanSqlite(database.get(), statement, ScanMaximum(scale),
                                                scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(statement, *sqlite_counters);
      }
      statement.Finalize();
      break;
    }
    case CaseKind::kDeletePoint: {
      SqliteStatement statement{database.get(), kDeletePointSql};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows =
                RunDeletePointSqlite(database.get(), statement, key_order.first(scale.operations)),
            .last_insert_rowid = 0,
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(statement, *sqlite_counters);
      }
      statement.Finalize();
      break;
    }
    case CaseKind::kDeleteScan: {
      SqliteStatement statement{database.get(), DeleteScanSql(scale)};
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunDeleteScanSqlite(database.get(), statement, ScanMaximum(scale),
                                                scale.row_mutations),
            .last_insert_rowid = 0,
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(statement, *sqlite_counters);
      }
      statement.Finalize();
      break;
    }
    case CaseKind::kMixedCommit:
    case CaseKind::kMixedRollback: {
      const bool rollback = kind == CaseKind::kMixedRollback;
      SqliteStatement begin{database.get(), "BEGIN"};
      SqliteStatement update{database.get(), kUpdatePointSql};
      SqliteStatement remove{database.get(), kDeletePointSql};
      SqliteStatement insert{database.get(), kInsertSql};
      SqliteStatement terminal{database.get(), rollback ? "ROLLBACK" : "COMMIT"};
      const MixedCounts counts = SplitMixed(scale.operations);
      timed = RunMeasured(index, measured, [&] {
        return WorkResult{
            .changed_rows = RunMixedSqlite(database.get(),
                                           SqliteMixedStatements{
                                               .begin = begin,
                                               .update = update,
                                               .remove = remove,
                                               .insert = insert,
                                               .terminal = terminal,
                                           },
                                           key_order, scale.operations),
            .last_insert_rowid = kPopulatedRows + static_cast<std::int64_t>(counts.inserts),
        };
      });
      if (sqlite_counters != nullptr) {
        AccumulateSqliteStatementCounters(begin, *sqlite_counters);
        AccumulateSqliteStatementCounters(update, *sqlite_counters);
        AccumulateSqliteStatementCounters(remove, *sqlite_counters);
        AccumulateSqliteStatementCounters(insert, *sqlite_counters);
        AccumulateSqliteStatementCounters(terminal, *sqlite_counters);
      }
      update.Finalize();
      remove.Finalize();
      insert.Finalize();
      begin.Finalize();
      terminal.Finalize();
      break;
    }
  }
  if (sqlite_counters != nullptr) {
    CaptureSqliteDiagnosticCounters(database.get(), *sqlite_counters);
  }
  database.Close();
  return Execution{.timed = timed, .configuration = configuration};
}

[[nodiscard]] std::vector<ExpectedRecord> ExpectedRecords(CaseKind kind, std::size_t operations,
                                                          std::span<const std::int64_t> key_order) {
  std::vector<ExpectedRecord> records;
  if (kind == CaseKind::kCreate) {
    return records;
  }
  if (kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch) {
    records.reserve(operations);
    for (std::size_t index = 0; index < operations; ++index) {
      const auto rowid = static_cast<std::int64_t>(index) + 1;
      records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
    }
    return records;
  }

  const bool rollback = kind == CaseKind::kMixedRollback;
  const MixedCounts mixed = SplitMixed(operations);
  std::vector<std::uint8_t> mutations(static_cast<std::size_t>(kPopulatedRows) + 1U, 0);
  if (!rollback && (kind == CaseKind::kUpdatePoint || kind == CaseKind::kDeletePoint)) {
    const std::uint8_t mutation = kind == CaseKind::kUpdatePoint ? 1U : 2U;
    for (const std::int64_t rowid : key_order.first(operations)) {
      mutations[static_cast<std::size_t>(rowid)] = mutation;
    }
  } else if (!rollback && kind == CaseKind::kMixedCommit) {
    for (std::size_t index = 0; index < mixed.updates; ++index) {
      mutations[static_cast<std::size_t>(key_order[index])] = 3U;
    }
    for (std::size_t index = 0; index < mixed.deletes; ++index) {
      mutations[static_cast<std::size_t>(key_order[mixed.updates + index])] = 2U;
    }
  }
  records.reserve(static_cast<std::size_t>(kPopulatedRows) + mixed.inserts);
  for (std::int64_t rowid = 1; rowid <= kPopulatedRows; ++rowid) {
    const std::uint8_t mutation = mutations[static_cast<std::size_t>(rowid)];
    if ((!rollback && kind == CaseKind::kDeleteScan && std::cmp_less_equal(rowid, operations)) ||
        mutation == 2U) {
      continue;
    }
    std::int64_t seed = rowid;
    std::int64_t version = 0;
    if (mutation == 1U) {
      seed = rowid + 1'000'000;
      version = 1;
    } else if (!rollback && kind == CaseKind::kUpdateScan &&
               std::cmp_less_equal(rowid, operations)) {
      seed = 9'000'000;
      version = 1;
    } else if (mutation == 3U) {
      seed = rowid + 2'000'000;
      version = 1;
    }
    records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = seed, .version = version});
  }
  if (!rollback && kind == CaseKind::kMixedCommit) {
    for (std::size_t index = 0; index < mixed.inserts; ++index) {
      const auto rowid = kPopulatedRows + static_cast<std::int64_t>(index) + 1;
      records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
    }
  }
  return records;
}

[[nodiscard]] Verification VerifyCreateWithSqlite(const std::filesystem::path& path,
                                                  std::size_t operations) {
  SqliteDatabase database{path};
  if (SqliteSingleText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw BenchmarkMismatch{"SQLite CREATE integrity check failed"};
  }
  SqliteStatement statement{database.get(),
                            "SELECT name FROM sqlite_schema WHERE type='table' ORDER BY rowid"};
  Digest digest;
  std::size_t count = 0;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(statement.get(), 0);
    const int bytes = sqlite3_column_bytes(statement.get(), 0);
    const std::string expected = TableName(count);
    if (text == nullptr || std::cmp_not_equal(bytes, expected.size()) ||
        std::string_view{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)} !=
            expected) {
      throw BenchmarkMismatch{"SQLite CREATE schema differs"};
    }
    digest.AddBytes(modern_sqlite::AsBytes(expected));
    ++count;
  }
  statement.Finalize();
  database.Close();
  if (count != operations) {
    throw BenchmarkMismatch{"SQLite CREATE object count differs"};
  }
  return Verification{.rows = 0, .schema_objects = count, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyCreateWithModern(const std::filesystem::path& path,
                                                  std::size_t operations) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement =
      PrepareModern(session, "SELECT name FROM sqlite_schema");
  Digest digest;
  std::size_t count = 0;
  while (true) {
    const modern_sqlite::WriteStep step = TakeValue(statement.Step());
    if (step == modern_sqlite::WriteStep::kDone) {
      break;
    }
    const std::optional<modern_sqlite::Utf8View> text = statement.row()[0].text_value();
    const std::string expected = TableName(count);
    if (!text.has_value() || text.value().bytes() != expected) {
      throw BenchmarkMismatch{"Modern CREATE schema differs"};
    }
    digest.AddBytes(modern_sqlite::AsBytes(expected));
    ++count;
  }
  RequireStatus(statement.Finalize());
  if (count != operations) {
    throw BenchmarkMismatch{"Modern CREATE object count differs"};
  }
  return Verification{.rows = 0, .schema_objects = count, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyKvWithSqlite(const std::filesystem::path& path,
                                              const std::vector<ExpectedRecord>& expected) {
  SqliteDatabase database{path};
  if (SqliteSingleText(database.get(), "PRAGMA integrity_check") != "ok") {
    throw BenchmarkMismatch{"SQLite integrity_check failed"};
  }
  SqliteStatement statement{database.get(), "SELECT k,v,version FROM kv"};
  Digest digest;
  std::size_t index = 0;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW || index >= expected.size()) {
      throw BenchmarkMismatch{"SQLite final scan differs"};
    }
    const ExpectedRecord& record = expected[index];
    const std::int64_t rowid = sqlite3_column_int64(statement.get(), 0);
    const void* pointer = sqlite3_column_blob(statement.get(), 1);
    const int bytes = sqlite3_column_bytes(statement.get(), 1);
    const std::int64_t version = sqlite3_column_int64(statement.get(), 2);
    if (pointer == nullptr || std::cmp_not_equal(bytes, kValueSize) || rowid != record.rowid ||
        version != record.version) {
      throw BenchmarkMismatch{"SQLite final row differs"};
    }
    const modern_sqlite::ByteBuffer expected_value = ValueFor(record.value_seed);
    const modern_sqlite::ByteView actual{static_cast<const std::byte*>(pointer), kValueSize};
    if (!std::ranges::equal(actual, expected_value.view())) {
      throw BenchmarkMismatch{"SQLite final BLOB differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(actual);
    digest.AddInteger(version);
    ++index;
  }
  statement.Finalize();
  database.Close();
  if (index != expected.size()) {
    throw BenchmarkMismatch{"SQLite final row count differs"};
  }
  return Verification{.rows = index, .schema_objects = 1, .digest = digest.Hex()};
}

[[nodiscard]] Verification VerifyKvWithModern(const std::filesystem::path& path,
                                              const std::vector<ExpectedRecord>& expected) {
  modern_sqlite::WriteSession session = TakeValue(modern_sqlite::WriteSession::Open(path.string()));
  modern_sqlite::WriteStatement statement = PrepareModern(session, "SELECT k,v,version FROM kv");
  Digest digest;
  std::size_t index = 0;
  while (true) {
    const modern_sqlite::WriteStep step = TakeValue(statement.Step());
    if (step == modern_sqlite::WriteStep::kDone) {
      break;
    }
    if (index >= expected.size() || statement.row().size() != 3U) {
      throw BenchmarkMismatch{"Modern final scan differs"};
    }
    const ExpectedRecord& record = expected[index];
    const std::int64_t rowid =
        statement.row()[0].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    const std::optional<modern_sqlite::ByteView> blob = statement.row()[1].blob_value();
    const std::int64_t version =
        statement.row()[2].integer_value().value_or(std::numeric_limits<std::int64_t>::min());
    if (!blob.has_value() || blob.value().size() != kValueSize || rowid != record.rowid ||
        version != record.version) {
      throw BenchmarkMismatch{"Modern final row differs"};
    }
    const modern_sqlite::ByteBuffer expected_value = ValueFor(record.value_seed);
    if (!std::ranges::equal(blob.value(), expected_value.view())) {
      throw BenchmarkMismatch{"Modern final BLOB differs"};
    }
    digest.AddInteger(rowid);
    digest.AddBytes(blob.value());
    digest.AddInteger(version);
    ++index;
  }
  RequireStatus(statement.Finalize());
  if (index != expected.size()) {
    throw BenchmarkMismatch{"Modern final row count differs"};
  }
  return Verification{.rows = index, .schema_objects = 1, .digest = digest.Hex()};
}

[[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    throw HarnessFailure{"cannot open benchmark database"};
  }
  const std::streamoff end = input.tellg();
  if (end < 0) {
    throw HarnessFailure{"cannot size benchmark database"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) {
    throw HarnessFailure{"cannot read benchmark database"};
  }
  return bytes;
}

[[nodiscard]] std::uint32_t ReadBigEndian32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4U) {
    throw HarnessFailure{"database header field is truncated"};
  }
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]));
}

[[nodiscard]] DatabaseFingerprint FingerprintDatabase(const std::filesystem::path& path) {
  const std::vector<std::byte> bytes = ReadFile(path);
  Sha256 sha256;
  sha256.Update(bytes);
  DatabaseFingerprint fingerprint{
      .sha256 = sha256.Hex(),
      .size_bytes = static_cast<std::uint64_t>(bytes.size()),
  };
  if (bytes.empty()) {
    return fingerprint;
  }
  constexpr std::string_view header{"SQLite format 3\0", 16};
  if (bytes.size() < 100U ||
      !std::ranges::equal(std::span<const std::byte>{bytes}.first(header.size()),
                          modern_sqlite::AsBytes(header))) {
    throw BenchmarkMismatch{"database fingerprint input has an invalid header"};
  }
  fingerprint.page_count = ReadBigEndian32(bytes, 28U);
  fingerprint.freelist_count = ReadBigEndian32(bytes, 36U);
  fingerprint.schema_cookie = ReadBigEndian32(bytes, 40U);
  return fingerprint;
}

void EnsureNoSidecars(const std::filesystem::path& path) {
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(path.string() + std::string{suffix})) {
      throw BenchmarkMismatch{"write benchmark left a sidecar"};
    }
  }
}

[[nodiscard]] std::vector<ExpectedRecord> OriginalRecords() {
  std::vector<ExpectedRecord> records;
  records.reserve(static_cast<std::size_t>(kPopulatedRows));
  for (std::int64_t rowid = 1; rowid <= kPopulatedRows; ++rowid) {
    records.push_back(ExpectedRecord{.rowid = rowid, .value_seed = rowid, .version = 0});
  }
  return records;
}

void VerifyInitialInput(EngineKind engine, CaseKind kind, const std::filesystem::path& path) {
  EnsureNoSidecars(path);
  if (kind == CaseKind::kCreate) {
    if (!ReadFile(path).empty()) {
      throw BenchmarkMismatch{"CREATE input is not a zero-byte database"};
    }
    return;
  }
  const std::vector<ExpectedRecord> expected =
      kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch
          ? std::vector<ExpectedRecord>{}
          : OriginalRecords();
  const Verification verification = engine == EngineKind::kModern
                                        ? VerifyKvWithModern(path, expected)
                                        : VerifyKvWithSqlite(path, expected);
  if (verification.rows != expected.size() || verification.schema_objects != 1U) {
    throw BenchmarkMismatch{"write benchmark input verification differs"};
  }
}

struct FreshDatabaseRequest {
  const std::filesystem::path& input;
  const std::filesystem::path& scratch;
  std::string_view name;
};

[[nodiscard]] std::filesystem::path FreshDatabasePath(FreshDatabaseRequest request) {
  const std::filesystem::path output = request.scratch / std::string{request.name};
  if (std::filesystem::exists(output)) {
    throw HarnessFailure{"scratch database already exists"};
  }
  for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
    if (std::filesystem::exists(output.string() + std::string{suffix})) {
      throw HarnessFailure{"scratch database sidecar already exists"};
    }
  }
  std::error_code error;
  if (!std::filesystem::copy_file(request.input, output, std::filesystem::copy_options::none,
                                  error)) {
    throw HarnessFailure{"cannot copy write benchmark input: " + error.message()};
  }
  return output;
}

void RemoveFreshDatabase(const std::filesystem::path& path) {
  EnsureNoSidecars(path);
  std::error_code error;
  if (!std::filesystem::remove(path, error) || error) {
    throw HarnessFailure{"cannot remove write benchmark scratch database"};
  }
}

struct VerifiedWork {
  WorkResult work;
  Verification verification;
  DatabaseFingerprint final_database;
};

struct VerifiedRepetition {
  TimedWork timed;
  Verification verification;
  DatabaseFingerprint final_database;
};

struct TimingRun {
  WorkloadScale scale;
  EffectiveConfiguration configuration;
  DatabaseFingerprint initial_database;
  VerifiedWork warmup;
  std::vector<VerifiedRepetition> repetitions;
};

[[nodiscard]] Execution ExecuteWork(EngineKind engine, ProfileKind profile, CaseKind kind,
                                    const std::filesystem::path& path, const WorkloadScale& scale,
                                    std::span<const std::int64_t> key_order, bool measured,
                                    std::size_t index,
                                    VfsDiagnosticCounters* vfs_counters = nullptr,
                                    SqliteCounterValues* sqlite_counters = nullptr) {
  return engine == EngineKind::kModern
             ? ExecuteModernWork(kind, path, scale, key_order, measured, index, vfs_counters)
             : ExecuteSqliteWork(kind, profile, path, scale, key_order, measured, index,
                                 sqlite_counters);
}

[[nodiscard]] std::int64_t ExpectedLastInsertRowid(CaseKind kind, const WorkloadScale& scale) {
  if (kind == CaseKind::kInsertPoint || kind == CaseKind::kInsertBatch) {
    return static_cast<std::int64_t>(scale.operations);
  }
  if (kind == CaseKind::kMixedCommit || kind == CaseKind::kMixedRollback) {
    return kPopulatedRows + static_cast<std::int64_t>(SplitMixed(scale.operations).inserts);
  }
  return 0;
}

[[nodiscard]] Verification VerifyFinalOutput(CaseKind kind, const WorkloadScale& scale,
                                             const std::filesystem::path& path,
                                             std::span<const std::int64_t> key_order) {
  const std::vector<ExpectedRecord> expected = ExpectedRecords(kind, scale.operations, key_order);
  const Verification sqlite_verification = kind == CaseKind::kCreate
                                               ? VerifyCreateWithSqlite(path, scale.operations)
                                               : VerifyKvWithSqlite(path, expected);
  const Verification modern_verification = kind == CaseKind::kCreate
                                               ? VerifyCreateWithModern(path, scale.operations)
                                               : VerifyKvWithModern(path, expected);
  if (sqlite_verification != modern_verification) {
    throw BenchmarkMismatch{"write benchmark final verification differs"};
  }
  return sqlite_verification;
}

void ValidateExecutedWork(CaseKind kind, const WorkloadScale& scale, const WorkResult& work) {
  const std::uint64_t expected_changes = kind == CaseKind::kCreate ? 0U : scale.row_mutations;
  if (work.changed_rows != expected_changes ||
      work.last_insert_rowid != ExpectedLastInsertRowid(kind, scale)) {
    throw BenchmarkMismatch{"write benchmark completion counts differ"};
  }
}

[[maybe_unused, nodiscard]] TimingRun RunTiming(EngineKind engine, ProfileKind profile,
                                                CaseKind kind, RunKind run_kind,
                                                const std::filesystem::path& input,
                                                const std::filesystem::path& scratch) {
  VerifyInitialInput(engine, kind, input);
  const DatabaseFingerprint initial_database = FingerprintDatabase(input);
  const WorkloadScale scale = ScaleFor(kind, run_kind);
  const std::vector<std::int64_t> key_order = GenerateKeyOrder();
  const std::vector<std::byte> rollback_input =
      kind == CaseKind::kMixedRollback ? ReadFile(input) : std::vector<std::byte>{};
  std::vector<std::filesystem::path> paths;
  paths.reserve(1U + kTimingRepetitions);

  const std::filesystem::path warmup_path = FreshDatabasePath(
      FreshDatabaseRequest{.input = input, .scratch = scratch, .name = "warmup.db"});
  paths.push_back(warmup_path);
  Execution warmup = ExecuteWork(engine, profile, kind, warmup_path, scale, key_order, false, 0);

  const std::size_t repetition_count = run_kind == RunKind::kSmoke ? 1U : kTimingRepetitions;
  std::vector<Execution> executions;
  executions.reserve(repetition_count);
  for (std::size_t index = 0; index < repetition_count; ++index) {
    const std::string name = "repetition-" + std::to_string(index) + ".db";
    const std::filesystem::path path =
        FreshDatabasePath(FreshDatabaseRequest{.input = input, .scratch = scratch, .name = name});
    paths.push_back(path);
    Execution execution = ExecuteWork(engine, profile, kind, path, scale, key_order, true, index);
    if (run_kind == RunKind::kBaseline && execution.timed.wall_ns < kMinimumWallNanoseconds) {
      throw HarnessFailure{"baseline repetition did not reach the minimum wall time: " +
                           std::to_string(execution.timed.wall_ns) + " ns"};
    }
    if (execution.configuration != warmup.configuration) {
      throw HarnessFailure{"effective write configuration changed between repetitions"};
    }
    executions.push_back(std::move(execution));
  }

  ValidateExecutedWork(kind, scale, warmup.timed.work);
  const Verification warmup_verification = VerifyFinalOutput(kind, scale, warmup_path, key_order);
  const DatabaseFingerprint warmup_fingerprint = FingerprintDatabase(warmup_path);
  if (kind == CaseKind::kMixedRollback && rollback_input != ReadFile(warmup_path)) {
    throw BenchmarkMismatch{"write benchmark rollback changed warmup database bytes"};
  }

  std::vector<VerifiedRepetition> repetitions;
  repetitions.reserve(repetition_count);
  for (std::size_t index = 0; index < executions.size(); ++index) {
    const Execution& execution = executions[index];
    ValidateExecutedWork(kind, scale, execution.timed.work);
    const Verification verification = VerifyFinalOutput(kind, scale, paths[index + 1U], key_order);
    const DatabaseFingerprint fingerprint = FingerprintDatabase(paths[index + 1U]);
    if (verification != warmup_verification) {
      throw BenchmarkMismatch{"write benchmark repetition result differs from warmup"};
    }
    if (kind == CaseKind::kMixedRollback && rollback_input != ReadFile(paths[index + 1U])) {
      throw BenchmarkMismatch{"write benchmark rollback changed repetition database bytes"};
    }
    repetitions.push_back(VerifiedRepetition{
        .timed = execution.timed,
        .verification = verification,
        .final_database = fingerprint,
    });
  }

  for (const std::filesystem::path& path : paths) {
    RemoveFreshDatabase(path);
  }
  return TimingRun{
      .scale = scale,
      .configuration = std::move(warmup.configuration),
      .initial_database = initial_database,
      .warmup =
          VerifiedWork{
              .work = warmup.timed.work,
              .verification = warmup_verification,
              .final_database = warmup_fingerprint,
          },
      .repetitions = std::move(repetitions),
  };
}

#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
struct DiagnosticRun {
  WorkloadScale scale;
  EffectiveConfiguration configuration;
  DatabaseFingerprint initial_database;
  VerifiedWork work;
  std::optional<ModernCounterValues> modern;
  std::optional<SqliteCounterValues> sqlite;
  std::optional<VfsDiagnosticCounters> vfs;
};

[[nodiscard]] ModernCounterValues ReadModernCounters(
    const modern_sqlite::instrumentation::CounterCollection& collection,
    const VfsDiagnosticCounters& vfs) {
  ModernCounterValues result;
  for (std::size_t index = 0; index < result.values.size(); ++index) {
    result.values[index] =
        collection.Value(static_cast<modern_sqlite::instrumentation::Counter>(index));
  }
  const auto vfs_index =
      static_cast<std::size_t>(modern_sqlite::instrumentation::Counter::kVfsCalls);
  if (vfs.delegated_vfs_calls > std::numeric_limits<std::uint64_t>::max() / 2U ||
      result.values[vfs_index] != vfs.delegated_vfs_calls * 2U) {
    throw BenchmarkMismatch{"Modern diagnostic VFS delegation count differs"};
  }
  result.values[vfs_index] -= vfs.delegated_vfs_calls;
  return result;
}

[[nodiscard]] DiagnosticRun RunDiagnostic(EngineKind engine, ProfileKind profile, CaseKind kind,
                                          const std::filesystem::path& input,
                                          const std::filesystem::path& scratch) {
  VerifyInitialInput(engine, kind, input);
  const DatabaseFingerprint initial_database = FingerprintDatabase(input);
  const WorkloadScale scale = ScaleFor(kind, RunKind::kBaseline);
  const std::vector<std::int64_t> key_order = GenerateKeyOrder();
  const std::vector<std::byte> rollback_input =
      kind == CaseKind::kMixedRollback ? ReadFile(input) : std::vector<std::byte>{};
  const std::filesystem::path warmup_path = FreshDatabasePath(
      FreshDatabaseRequest{.input = input, .scratch = scratch, .name = "warmup.db"});
  const std::filesystem::path diagnostic_path = FreshDatabasePath(
      FreshDatabaseRequest{.input = input, .scratch = scratch, .name = "diagnostic.db"});

  const Execution warmup =
      ExecuteWork(engine, profile, kind, warmup_path, scale, key_order, false, 0);
  Execution diagnostic;
  std::optional<ModernCounterValues> modern;
  std::optional<SqliteCounterValues> sqlite;
  std::optional<VfsDiagnosticCounters> vfs;
  if (engine == EngineKind::kModern) {
    modern_sqlite::instrumentation::CounterCollection collection;
    vfs.emplace();
    {
      const modern_sqlite::instrumentation::ScopedCounterCollection scope{collection};
      diagnostic =
          ExecuteWork(engine, profile, kind, diagnostic_path, scale, key_order, false, 0, &*vfs);
    }
    modern = ReadModernCounters(collection, *vfs);
  } else {
    sqlite.emplace();
    diagnostic = ExecuteWork(engine, profile, kind, diagnostic_path, scale, key_order, false, 0,
                             nullptr, &*sqlite);
  }
  if (diagnostic.configuration != warmup.configuration) {
    throw HarnessFailure{"effective write configuration changed for diagnostics"};
  }

  ValidateExecutedWork(kind, scale, warmup.timed.work);
  ValidateExecutedWork(kind, scale, diagnostic.timed.work);
  const Verification warmup_verification = VerifyFinalOutput(kind, scale, warmup_path, key_order);
  const Verification diagnostic_verification =
      VerifyFinalOutput(kind, scale, diagnostic_path, key_order);
  const DatabaseFingerprint diagnostic_fingerprint = FingerprintDatabase(diagnostic_path);
  if (warmup_verification != diagnostic_verification) {
    throw BenchmarkMismatch{"write diagnostic result differs from warmup"};
  }
  if (kind == CaseKind::kMixedRollback &&
      (rollback_input != ReadFile(warmup_path) || rollback_input != ReadFile(diagnostic_path))) {
    throw BenchmarkMismatch{"write diagnostic rollback changed database bytes"};
  }
  RemoveFreshDatabase(warmup_path);
  RemoveFreshDatabase(diagnostic_path);
  return DiagnosticRun{
      .scale = scale,
      .configuration = std::move(diagnostic.configuration),
      .initial_database = initial_database,
      .work =
          VerifiedWork{
              .work = diagnostic.timed.work,
              .verification = diagnostic_verification,
              .final_database = diagnostic_fingerprint,
          },
      .modern = modern,
      .sqlite = sqlite,
      .vfs = vfs,
  };
}
#endif

void PrintJsonString(std::ostream& output, std::string_view value) {
  output << '"';
  for (const char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << character;
        break;
    }
  }
  output << '"';
}

[[nodiscard]] std::vector<std::string> SqliteCompileOptions() {
  std::vector<std::string> options;
  for (int index = 0;; ++index) {
    const char* option = sqlite3_compileoption_get(index);
    if (option == nullptr) {
      break;
    }
    const std::string_view value{option};
    if (!value.starts_with("COMPILER=")) {
      options.emplace_back(value);
    }
  }
  std::ranges::sort(options);
  return options;
}

void PrintCompilerIdentity(std::ostream& output) {
#if defined(__clang__)
  output << R"({"id":"clang","version":)";
  PrintJsonString(output, __clang_version__);
#elif defined(__GNUC__)
  output << R"({"id":"gcc","version":)";
  PrintJsonString(output, __VERSION__);
#elif defined(_MSC_VER)
  output << R"({"id":"msvc","version":)";
  PrintJsonString(output, std::to_string(_MSC_FULL_VER));
#else
#error "The write performance benchmark requires a recognized compiler"
#endif
  output << '}';
}

void PrintStandardLibraryIdentity(std::ostream& output) {
#if defined(_LIBCPP_VERSION)
  output << R"({"id":"libc++","version":)";
  PrintJsonString(output, std::to_string(_LIBCPP_VERSION));
#elif defined(__GLIBCXX__)
  output << R"({"id":"libstdc++","version":)";
  PrintJsonString(output, std::to_string(__GLIBCXX__));
#elif defined(_MSVC_STL_VERSION)
  output << R"({"id":"msvc-stl","version":)";
  PrintJsonString(output, std::to_string(_MSVC_STL_VERSION));
#else
#error "The write performance benchmark requires a recognized standard library"
#endif
  output << '}';
}

[[nodiscard]] constexpr std::string_view TargetArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
#error "The write performance benchmark requires a recognized target architecture"
#endif
}

void PrintBuildIdentity(std::ostream& output) {
  output << R"({"architecture":)";
  PrintJsonString(output, TargetArchitecture());
  output << R"(,"build_type":"Release","compiler":)";
  PrintCompilerIdentity(output);
  output << R"(,"cplusplus":)" << __cplusplus << R"(,"coverage":false,"instrumentation":)"
         << (MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS ? "true" : "false")
         << R"(,"sanitizers":false,"standard_library":)";
  PrintStandardLibraryIdentity(output);
  output << '}';
}

void PrintSourceIdentity(std::ostream& output) {
  output << R"({"revision":)";
  PrintJsonString(output, modern_sqlite::write_performance_build_config::kGitRevision);
  output << R"(,"tree":)";
  PrintJsonString(output, modern_sqlite::write_performance_build_config::kGitTree);
  output << '}';
}

void PrintSqliteIdentity(std::ostream& output) {
  const std::vector<std::string> options = SqliteCompileOptions();
  output << R"({"compile_options":[)";
  for (std::size_t index = 0; index < options.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    PrintJsonString(output, options[index]);
  }
  output << R"(],"source_id":)";
  PrintJsonString(output, sqlite3_sourceid());
  output << R"(,"version":)";
  PrintJsonString(output, sqlite3_libversion());
  output << '}';
}

void PrintIdentityReport() {
  std::cout << R"({"build":)";
  PrintBuildIdentity(std::cout);
  std::cout << R"(,"mode":"identity","schema_version":1,"source":)";
  PrintSourceIdentity(std::cout);
  std::cout << R"(,"sqlite":)";
  PrintSqliteIdentity(std::cout);
  std::cout << "}\n";
}

void PrintConfiguration(std::ostream& output, const EffectiveConfiguration& configuration) {
  output << R"({"cache_size":)" << configuration.cache_size << R"(,"journal_mode":)";
  PrintJsonString(output, configuration.journal_mode);
  output << R"(,"locking_mode":)";
  PrintJsonString(output, configuration.locking_mode);
  output << R"(,"mmap_bytes":)" << configuration.mmap_bytes << R"(,"page_size":)"
         << configuration.page_size << R"(,"synchronous":)";
  PrintJsonString(output, configuration.synchronous);
  output << R"(,"temp_store":)";
  PrintJsonString(output, configuration.temp_store);
  output << R"(,"thread_mode":)";
  PrintJsonString(output, configuration.thread_mode);
  output << '}';
}

void PrintDatabaseFingerprint(std::ostream& output, const DatabaseFingerprint& fingerprint) {
  output << R"({"freelist_count":)" << fingerprint.freelist_count << R"(,"page_count":)"
         << fingerprint.page_count << R"(,"schema_cookie":)" << fingerprint.schema_cookie
         << R"(,"sha256":)";
  PrintJsonString(output, fingerprint.sha256);
  output << R"(,"size_bytes":)" << fingerprint.size_bytes << '}';
}

void PrintWork(std::ostream& output, const WorkloadScale& scale, const WorkResult& work,
               const Verification& verification, const DatabaseFingerprint& final_database) {
  output << R"("changed_rows":)" << work.changed_rows << R"(,"digest":)";
  PrintJsonString(output, verification.digest);
  output << R"(,"dml_operations":)" << scale.dml_operations << R"(,"final_database":)";
  PrintDatabaseFingerprint(output, final_database);
  output << R"(,"final_rows":)" << verification.rows << R"(,"last_insert_rowid":)"
         << work.last_insert_rowid << R"(,"row_mutations":)" << scale.row_mutations
         << R"(,"schema_objects":)" << verification.schema_objects << R"(,"transactions":)"
         << scale.transactions;
}

#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
void PrintModernCounters(std::ostream& output, const ModernCounterValues& counters) {
  output << '{';
  for (std::size_t index = 0; index < counters.values.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto counter = static_cast<modern_sqlite::instrumentation::Counter>(index);
    PrintJsonString(output, modern_sqlite::instrumentation::CounterName(counter));
    output << ':' << counters.values[index];
  }
  output << '}';
}

void PrintSqliteCounters(std::ostream& output, const SqliteCounterValues& counters) {
  output << R"({"cache_bytes_current":)" << counters.cache_bytes_current << R"(,"cache_hits":)"
         << counters.cache_hits << R"(,"cache_misses":)" << counters.cache_misses
         << R"(,"cache_writes":)" << counters.cache_writes << R"(,"changes":)" << counters.changes
         << R"(,"fullscan_steps":)" << counters.fullscan_steps << R"(,"malloc_count_current":)"
         << counters.malloc_count_current << R"(,"malloc_count_highwater":)"
         << counters.malloc_count_highwater << R"(,"malloc_size_highwater":)"
         << counters.malloc_size_highwater << R"(,"reprepares":)" << counters.reprepares
         << R"(,"statement_runs":)" << counters.statement_runs << R"(,"total_changes":)"
         << counters.total_changes << R"(,"vm_steps":)" << counters.vm_steps << '}';
}

void PrintFileCounters(std::ostream& output, const FileDiagnosticCounters& counters) {
  output << R"({"access_calls":)" << counters.access_calls << R"(,"close_calls":)"
         << counters.close_calls << R"(,"delete_calls":)" << counters.delete_calls
         << R"(,"directory_sync_requests":)" << counters.directory_sync_requests
         << R"(,"full_path_calls":)" << counters.full_path_calls << R"(,"lock_calls":)"
         << counters.lock_calls << R"(,"open_calls":)" << counters.open_calls << R"(,"read_bytes":)"
         << counters.read_bytes << R"(,"read_calls":)" << counters.read_calls << R"(,"sync_calls":)"
         << counters.sync_calls << R"(,"truncate_calls":)" << counters.truncate_calls
         << R"(,"unlock_calls":)" << counters.unlock_calls << R"(,"write_bytes":)"
         << counters.write_bytes << R"(,"write_calls":)" << counters.write_calls << '}';
}

void PrintVfsCounters(std::ostream& output, const VfsDiagnosticCounters& counters) {
  output << R"({"global":{"random_byte_calls":)" << counters.random_byte_calls
         << R"(},"main_database":)";
  PrintFileCounters(output,
                    counters.files[DiagnosticFileGroupIndex(DiagnosticFileGroup::kMainDatabase)]);
  output << R"(,"main_journal":)";
  PrintFileCounters(output,
                    counters.files[DiagnosticFileGroupIndex(DiagnosticFileGroup::kMainJournal)]);
  output << R"(,"subjournal":)";
  PrintFileCounters(output,
                    counters.files[DiagnosticFileGroupIndex(DiagnosticFileGroup::kSubjournal)]);
  output << R"(,"write_ahead_log":)";
  PrintFileCounters(output,
                    counters.files[DiagnosticFileGroupIndex(DiagnosticFileGroup::kWriteAheadLog)]);
  output << '}';
}

void PrintDiagnosticReport(std::string_view engine, ProfileKind profile, std::string_view case_id,
                           const DiagnosticRun& run) {
  std::cout << R"({"build":)";
  PrintBuildIdentity(std::cout);
  std::cout << R"(,"case":)";
  PrintJsonString(std::cout, case_id);
  std::cout << R"(,"completion":{"diagnostic_runs":1,"fresh_databases":2,)"
               R"("post_verifications":2,"pre_verifications":1,"status":"complete",)"
               R"("warmups":1},"counters":{"modern":)";
  if (run.modern.has_value()) {
    PrintModernCounters(std::cout, *run.modern);
  } else {
    std::cout << "{}";
  }
  std::cout << R"(,"sqlite":)";
  if (run.sqlite.has_value()) {
    PrintSqliteCounters(std::cout, *run.sqlite);
  } else {
    std::cout << "{}";
  }
  std::cout << R"(,"vfs":)";
  if (run.vfs.has_value()) {
    PrintVfsCounters(std::cout, *run.vfs);
  } else {
    std::cout << "{}";
  }
  std::cout << R"(},"diagnostic_schema_version":1,"effective_configuration":)";
  PrintConfiguration(std::cout, run.configuration);
  std::cout << R"(,"engine":)";
  PrintJsonString(std::cout, engine);
  std::cout << R"(,"mode":"diagnostic","profile":)";
  PrintJsonString(std::cout, ProfileName(profile));
  std::cout << R"(,"initial_database":)";
  PrintDatabaseFingerprint(std::cout, run.initial_database);
  std::cout << R"(,"schema_version":1,"source":)";
  PrintSourceIdentity(std::cout);
  std::cout << R"(,"sqlite":)";
  PrintSqliteIdentity(std::cout);
  std::cout << R"(,"work":{)";
  PrintWork(std::cout, run.scale, run.work.work, run.work.verification, run.work.final_database);
  std::cout << R"(},"workload_semantics_version":1})" << '\n';
}
#endif

[[maybe_unused]] void PrintReport(std::string_view engine, ProfileKind profile,
                                  std::string_view case_id, RunKind run_kind,
                                  const TimingRun& run) {
  std::cout << R"({"build":)";
  PrintBuildIdentity(std::cout);
  std::cout << R"(,"case":)";
  PrintJsonString(std::cout, case_id);
  std::cout << R"(,"completion":{"fresh_databases":)" << run.repetitions.size() + 1U
            << R"(,"measured_repetitions":)" << run.repetitions.size()
            << R"(,"post_verifications":)" << run.repetitions.size() + 1U
            << R"(,"pre_verifications":1,"status":"complete","warmups":1},)"
               R"("effective_configuration":)";
  PrintConfiguration(std::cout, run.configuration);
  std::cout << R"(,"engine":)";
  PrintJsonString(std::cout, engine);
  std::cout << R"(,"mode":"timing","profile":)";
  PrintJsonString(std::cout, ProfileName(profile));
  std::cout << R"(,"initial_database":)";
  PrintDatabaseFingerprint(std::cout, run.initial_database);
  std::cout << R"(,"repetitions":[)";
  for (std::size_t index = 0; index < run.repetitions.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const VerifiedRepetition& repetition = run.repetitions[index];
    std::cout << '{';
    PrintWork(std::cout, run.scale, repetition.timed.work, repetition.verification,
              repetition.final_database);
    std::cout << R"(,"cpu_ns":)" << repetition.timed.cpu_ns << R"(,"index":)"
              << repetition.timed.index << R"(,"wall_ns":)" << repetition.timed.wall_ns << '}';
  }
  std::cout << R"(],"run_kind":)";
  PrintJsonString(std::cout, RunKindName(run_kind));
  std::cout << R"(,"schema_version":1,"source":)";
  PrintSourceIdentity(std::cout);
  std::cout << R"(,"sqlite":)";
  PrintSqliteIdentity(std::cout);
  std::cout << R"(,"timer":{"cpu":"CLOCK_PROCESS_CPUTIME_ID",)"
               R"("wall":"steady_clock"},"warmup":{)";
  PrintWork(std::cout, run.scale, run.warmup.work, run.warmup.verification,
            run.warmup.final_database);
  std::cout << R"(},"workload_semantics_version":1})" << '\n';
}

int Run(int argument_count, char* const* arguments) {
  if (argument_count == 2 && std::string_view{arguments[1]} == "identity") {
    VerifySqliteIdentity();
    PrintIdentityReport();
    return 0;
  }
  if (argument_count != 8 || std::string_view{arguments[1]} != "run") {
#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
    throw HarnessFailure{
        "usage: write diagnostics identity | "
        "run ENGINE PROFILE CASE INPUT SCRATCH diagnostic"};
#else
    throw HarnessFailure{
        "usage: write benchmark identity | "
        "run ENGINE PROFILE CASE INPUT SCRATCH <smoke|baseline>"};
#endif
  }
  const EngineKind engine = ParseEngine(arguments[2]);
  const ProfileKind profile = ParseProfile(arguments[3]);
  const CaseKind benchmark_case = ParseCase(arguments[4]);
  const std::filesystem::path input = arguments[5];
  const std::filesystem::path scratch = arguments[6];
  if (!std::filesystem::is_regular_file(input)) {
    throw HarnessFailure{"write benchmark input is not a file"};
  }
  if (!std::filesystem::is_directory(scratch)) {
    throw HarnessFailure{"write benchmark scratch path is not a directory"};
  }
#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
  if (std::string_view{arguments[7]} != "diagnostic") {
    throw HarnessFailure{"write diagnostic binary accepts only diagnostic runs"};
  }
  const DiagnosticRun run = RunDiagnostic(engine, profile, benchmark_case, input, scratch);
  PrintDiagnosticReport(arguments[2], profile, arguments[4], run);
#else
  const RunKind run_kind = ParseRunKind(arguments[7]);
  const TimingRun run = RunTiming(engine, profile, benchmark_case, run_kind, input, scratch);
  PrintReport(arguments[2], profile, arguments[4], run_kind, run);
#endif
  return 0;
}

}  // namespace

#if MODERN_SQLITE_WRITE_PERFORMANCE_DIAGNOSTICS
void* operator new(std::size_t size) {
  MODERN_SQLITE_RECORD_COUNTER(modern_sqlite::instrumentation::Counter::kAllocations, 1U);
  if (void* allocation = std::malloc(size == 0 ? 1U : size); allocation != nullptr) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  MODERN_SQLITE_RECORD_COUNTER(modern_sqlite::instrumentation::Counter::kAllocations, 1U);
  void* allocation = nullptr;
  const std::size_t aligned_size = size == 0 ? static_cast<std::size_t>(alignment) : size;
  if (posix_memalign(&allocation, static_cast<std::size_t>(alignment), aligned_size) == 0) {
    return allocation;
  }
  throw std::bad_alloc{};
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::align_val_t) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}
void operator delete[](void* allocation, std::size_t, std::align_val_t) noexcept {
  std::free(allocation);
}
#endif

int main(int argument_count, char* const* arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const BenchmarkMismatch& error) {
    std::cerr << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
