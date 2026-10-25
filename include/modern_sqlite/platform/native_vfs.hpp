#ifndef MODERN_SQLITE_PLATFORM_NATIVE_VFS_HPP_
#define MODERN_SQLITE_PLATFORM_NATIVE_VFS_HPP_

#if defined(_WIN32)
#include "modern_sqlite/platform/windows_vfs.hpp"
#else
#include "modern_sqlite/platform/posix_vfs.hpp"
#endif

namespace modern_sqlite {

#if defined(_WIN32)
using NativeVfs = WindowsVfs;
#else
using NativeVfs = PosixVfs;
#endif

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_PLATFORM_NATIVE_VFS_HPP_
