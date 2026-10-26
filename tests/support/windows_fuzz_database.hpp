#ifndef MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_FUZZ_DATABASE_HPP_
#define MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_FUZZ_DATABASE_HPP_

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "modern_sqlite/platform/native_vfs.hpp"
#include "windows_handle.hpp"

namespace modern_sqlite::fuzz::detail {

class PrivateDatabase final {
 public:
  PrivateDatabase() = default;
  PrivateDatabase(const PrivateDatabase&) = delete;
  PrivateDatabase& operator=(const PrivateDatabase&) = delete;

  ~PrivateDatabase() {
    for (const auto& child : children_) {
      if (!child.empty()) {
        static_cast<void>(::DeleteFileW(child.c_str()));
      }
    }
    directory_handle_.reset();
    if (directory_created_) {
      static_cast<void>(::RemoveDirectoryW(directory_.c_str()));
    }
  }

  [[nodiscard]] bool Initialize(std::span<const std::uint8_t> input) {
    std::error_code error;
    const auto parent = std::filesystem::temp_directory_path(error);
    if (error) {
      return false;
    }
    constexpr std::wstring_view hexadecimal = L"0123456789abcdef";
    ByteBuffer entropy{ByteCount{15}};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      if (!vfs_.RandomBytes(entropy.mutable_view()).has_value()) {
        return false;
      }
      std::wstring name{L"modern-sqlite-fuzz-"};
      for (const std::byte value : entropy.view()) {
        const auto byte = std::to_integer<unsigned>(value);
        name += hexadecimal[byte >> 4U];
        name += hexadecimal[byte & 0x0fU];
      }
      directory_ = parent / name;
      if (::CreateDirectoryW(directory_.c_str(), nullptr) != 0) {
        directory_created_ = true;
        break;
      }
      if (::GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
    }
    if (!directory_created_) {
      return false;
    }
    directory_handle_ = test_support::OwnWindowsHandle(::CreateFileW(
        directory_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory_handle_) {
      return false;
    }
    FILE_ATTRIBUTE_TAG_INFO information{};
    if (::GetFileInformationByHandleEx(directory_handle_.get(), FileAttributeTagInfo, &information,
                                       sizeof(information)) == 0 ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
      return false;
    }
    constexpr std::array names{L"input.db", L"input.db-journal", L"input.db-wal", L"input.db-shm"};
    for (std::size_t index = 0; index < names.size(); ++index) {
      children_[index] = directory_ / names[index];
    }
    const auto utf8 = children_.front().u8string();
    path_.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    auto opened = vfs_.Open(path_, FileOpenOptions{
                                       .access = FileAccessMode::kReadWrite,
                                       .create = true,
                                       .exclusive_create = true,
                                       .no_follow = true,
                                   });
    return opened.has_value() &&
           opened->file->WriteAt(std::as_bytes(input), FileOffset{0}).has_value();
  }

  [[nodiscard]] std::string_view path() const noexcept { return path_; }

  void Unlink() noexcept {
    if (!children_.front().empty()) {
      static_cast<void>(::DeleteFileW(children_.front().c_str()));
    }
  }

 private:
  NativeVfs vfs_;
  std::filesystem::path directory_;
  std::array<std::filesystem::path, 4> children_;
  std::string path_;
  test_support::WindowsHandle directory_handle_;
  bool directory_created_ = false;
};

}  // namespace modern_sqlite::fuzz::detail

#endif  // MODERN_SQLITE_TESTS_SUPPORT_WINDOWS_FUZZ_DATABASE_HPP_
