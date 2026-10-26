#ifndef MODERN_SQLITE_PLATFORM_WINDOWS_VFS_HPP_
#define MODERN_SQLITE_PLATFORM_WINDOWS_VFS_HPP_

#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {

class WindowsVfs final : public Vfs {
 public:
  WindowsVfs() = default;
  ~WindowsVfs() override = default;

 protected:
  [[nodiscard]] Result<OpenedFile> DoOpen(std::optional<std::string_view> path,
                                          FileOpenOptions options) override;
  [[nodiscard]] Status DoDelete(std::string_view path, DirectorySync directory_sync) override;
  [[nodiscard]] Result<bool> DoAccess(std::string_view path, FileAccessQuery query) override;
  [[nodiscard]] Result<std::string> DoFullPath(std::string_view path) override;
  [[nodiscard]] Result<ByteCount> DoRandomBytes(MutableByteView output) override;
  [[nodiscard]] Result<std::chrono::microseconds> DoSleepFor(
      std::chrono::microseconds duration) override;
  [[nodiscard]] Result<WallClockTime> DoCurrentTime() override;
  [[nodiscard]] ByteCount DoMaximumPathLength() const noexcept override;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PLATFORM_WINDOWS_VFS_HPP_
