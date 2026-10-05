#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>

#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
#include "read_fuzz.hpp"

namespace modern_sqlite::fuzz {
namespace {

constexpr std::size_t kMaximumDatabaseBytes = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumRows = 256;
constexpr auto kDatabaseName = std::to_array("input.db");

class OwnedFileDescriptor {
 public:
  OwnedFileDescriptor() = default;
  explicit OwnedFileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
  OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
  OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;
  ~OwnedFileDescriptor() { Reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

  void Reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
  }

 private:
  int descriptor_ = -1;
};

class PrivateDatabase {
 public:
  PrivateDatabase() = default;
  PrivateDatabase(const PrivateDatabase&) = delete;
  PrivateDatabase& operator=(const PrivateDatabase&) = delete;

  ~PrivateDatabase() {
    Unlink();
    directory_descriptor_.Reset();
    if (directory_created_) {
      static_cast<void>(::rmdir(directory_.data()));
    }
  }

  [[nodiscard]] bool Initialize(std::span<const std::uint8_t> input) {
    constexpr auto directory_template = std::to_array("/tmp/modern-sqlite-read-fuzz-XXXXXX");
    std::ranges::copy(directory_template, directory_.begin());
    if (::mkdtemp(directory_.data()) == nullptr) {
      return false;
    }
    directory_created_ = true;

    directory_descriptor_.Reset(::open(directory_.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_descriptor_.valid()) {
      return false;
    }
    OwnedFileDescriptor writer{::openat(directory_descriptor_.get(), kDatabaseName.data(),
                                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                        0600)};
    if (!writer.valid()) {
      return false;
    }
    file_created_ = true;

    std::size_t written = 0;
    while (written < input.size()) {
      const ssize_t count = ::write(writer.get(), input.data() + written, input.size() - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      written += static_cast<std::size_t>(count);
    }
    writer.Reset();

    const int count =
        std::snprintf(path_.data(), path_.size(), "%s/%s", directory_.data(), kDatabaseName.data());
    return count > 0 && static_cast<std::size_t>(count) < path_.size();
  }

  [[nodiscard]] std::string_view path() const noexcept { return path_.data(); }

  void Unlink() noexcept {
    if (!file_created_ || !directory_descriptor_.valid()) {
      return;
    }
    if (::unlinkat(directory_descriptor_.get(), kDatabaseName.data(), 0) == 0 || errno == ENOENT) {
      file_created_ = false;
    }
  }

 private:
  std::array<char, 64> directory_{};
  std::array<char, PATH_MAX> path_{};
  OwnedFileDescriptor directory_descriptor_;
  bool directory_created_ = false;
  bool file_created_ = false;
};

void ConsumePrepared(Result<ReadPrepareOutput> prepared) {
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    return;
  }
  ReadStatement statement = std::move(*prepared->statement);
  for (std::size_t row = 0; row < kMaximumRows; ++row) {
    const Result<ReadStep> step = statement.Step();
    if (!step.has_value() || *step == ReadStep::kDone) {
      break;
    }
  }
  [[maybe_unused]] const Status finalized = statement.Finalize();
}

}  // namespace

void RunDatabaseImageInput(std::span<const std::uint8_t> input) {
  if (input.size() > kMaximumDatabaseBytes) {
    return;
  }
  PrivateDatabase database;
  if (!database.Initialize(input)) {
    return;
  }

  Result<ReadSession> opened = ReadSession::Open(database.path());
  if (!opened.has_value()) {
    return;
  }
  ReadSession session = std::move(*opened);
  Result<ReadPrepareOutput> schema = session.Prepare(Utf8View{"SELECT name FROM sqlite_schema"});
  database.Unlink();
  ConsumePrepared(std::move(schema));
  ConsumePrepared(session.Prepare(Utf8View{"SELECT 1"}));
}

}  // namespace modern_sqlite::fuzz
