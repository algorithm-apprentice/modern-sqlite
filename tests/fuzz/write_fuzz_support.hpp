#ifndef MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_SUPPORT_HPP_
#define MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_SUPPORT_HPP_

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
#include <ranges>
#include <span>
#include <string_view>

namespace modern_sqlite::fuzz::detail {

inline constexpr auto kDatabaseName = std::to_array("input.db");
inline constexpr auto kJournalName = std::to_array("input.db-journal");
inline constexpr auto kWalName = std::to_array("input.db-wal");
inline constexpr auto kSharedMemoryName = std::to_array("input.db-shm");

class OwnedFileDescriptor final {
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

class PrivateDatabase final {
 public:
  PrivateDatabase() = default;
  PrivateDatabase(const PrivateDatabase&) = delete;
  PrivateDatabase& operator=(const PrivateDatabase&) = delete;

  ~PrivateDatabase() {
    if (directory_descriptor_.valid()) {
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kSharedMemoryName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kWalName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kJournalName.data(), 0));
      static_cast<void>(::unlinkat(directory_descriptor_.get(), kDatabaseName.data(), 0));
    }
    directory_descriptor_.Reset();
    if (directory_created_) {
      static_cast<void>(::rmdir(directory_.data()));
    }
  }

  [[nodiscard]] bool Initialize(std::span<const std::uint8_t> input) {
    constexpr auto directory_template = std::to_array("/tmp/modern-sqlite-write-fuzz-XXXXXX");
    std::ranges::copy(directory_template, directory_.begin());
    if (::mkdtemp(directory_.data()) == nullptr) {
      return false;
    }
    directory_created_ = true;

    directory_descriptor_.Reset(::open(directory_.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory_descriptor_.valid()) {
      return false;
    }
    OwnedFileDescriptor database{::openat(directory_descriptor_.get(), kDatabaseName.data(),
                                          O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                          0600)};
    if (!database.valid()) {
      return false;
    }

    std::size_t written = 0;
    while (written < input.size()) {
      const ssize_t count = ::write(database.get(), input.data() + written, input.size() - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      written += static_cast<std::size_t>(count);
    }
    database.Reset();

    const int count =
        std::snprintf(path_.data(), path_.size(), "%s/%s", directory_.data(), kDatabaseName.data());
    return count > 0 && static_cast<std::size_t>(count) < path_.size();
  }

  [[nodiscard]] std::string_view path() const noexcept { return path_.data(); }

 private:
  std::array<char, 64> directory_{};
  std::array<char, PATH_MAX> path_{};
  OwnedFileDescriptor directory_descriptor_;
  bool directory_created_ = false;
};

}  // namespace modern_sqlite::fuzz::detail

#endif  // MODERN_SQLITE_TESTS_FUZZ_WRITE_FUZZ_SUPPORT_HPP_
