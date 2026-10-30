#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "tests/support/windows_process.hpp"
#include "tests/support/windows_temporary_directory.hpp"

namespace {

using modern_sqlite::test_support::OwnedWindowsProcess;
using modern_sqlite::test_support::OwnWindowsHandle;
using modern_sqlite::test_support::RunWindowsProcess;
using modern_sqlite::test_support::WindowsProcessExecutable;
using modern_sqlite::test_support::WindowsTemporaryDirectory;

TEST(BtreeWriterProcess, RejectsInvalidWorkerArguments) {
  const auto executable = WindowsProcessExecutable().parent_path() /
                          L"modern_sqlite_btree_writer_sqlite_compatibility.exe";
  const WindowsTemporaryDirectory directory;
  const auto leaf = directory.RootPath().filename().native();
  const auto invalid = [&](std::initializer_list<std::wstring_view> arguments) {
    EXPECT_EQ(2U, RunWindowsProcess(executable, arguments));
  };
  invalid({L"--geometry-worker", L"invalid", leaf});
  invalid({L"--geometry-worker", L"8", leaf});
  invalid({L"--geometry-worker", L"-1", leaf});
  invalid({L"--geometry-worker", L"0", L".."});
  invalid({L"--geometry-worker", L"0", L"modern-sqlite-windows-0-0"});
  invalid({L"--unknown-worker"});
}

TEST(BtreeWriterProcess, PreservesTheDefaultWaitAndJoinsRetiredOwnedChildren) {
  const auto executable = WindowsProcessExecutable();
  EXPECT_EQ(17U, RunWindowsProcess(executable, {L"--probe-exit"}));
  EXPECT_EQ(2U, RunWindowsProcess(executable, {L"--probe-reporting-failure"}));
  modern_sqlite::test_support::WindowsHandle unwaited;
  {
    const OwnedWindowsProcess child{executable, {L"--probe-sleep"}};
    unwaited = OwnWindowsHandle(::OpenProcess(SYNCHRONIZE, FALSE, child.Id()));
    ASSERT_TRUE(unwaited);
    EXPECT_EQ(WAIT_TIMEOUT, ::WaitForSingleObject(unwaited.get(), 0));
  }
  EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(unwaited.get(), 0));

  OwnedWindowsProcess child{executable, {L"--probe-sleep"}};
  const auto retired = OwnWindowsHandle(::OpenProcess(SYNCHRONIZE, FALSE, child.Id()));
  ASSERT_TRUE(retired);
  try {
    static_cast<void>(child.WaitForExit(1));
    FAIL() << "the short owned-child wait unexpectedly completed";
  } catch (const std::system_error& error) {
    EXPECT_EQ(static_cast<int>(ERROR_TIMEOUT), error.code().value());
  }
  EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(retired.get(), 0));
}

[[nodiscard]] DWORD ReadChildId(const std::filesystem::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream input{path};
    DWORD id = 0;
    if (input >> id && id != 0) {
      return id;
    }
    ::Sleep(1);
  }
  throw std::runtime_error{"probe parent did not publish its owned child"};
}

TEST(BtreeWriterProcess, RetiresChildrenAfterAbruptParentExitWhileTheOuterJobStaysOpen) {
  const WindowsTemporaryDirectory directory;
  const auto leaf = directory.RootPath().filename().native();
  OwnedWindowsProcess parent{WindowsProcessExecutable(), {L"--probe-parent", leaf}};
  const auto child =
      OwnWindowsHandle(::OpenProcess(SYNCHRONIZE, FALSE, ReadChildId(directory.NativePath("pid"))));
  ASSERT_TRUE(child);
  ASSERT_EQ(WAIT_TIMEOUT, ::WaitForSingleObject(child.get(), 0));
  {
    std::ofstream signal{directory.NativePath("exit")};
    signal << "exit\n";
    ASSERT_TRUE(signal);
  }
  EXPECT_EQ(0U, parent.WaitForExit(3000));
  EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(child.get(), 3000));
}

[[nodiscard]] bool ReportProbeError(std::string_view message) noexcept {
  const HANDLE output = ::GetStdHandle(STD_ERROR_HANDLE);
  while (!message.empty()) {
    const auto count = static_cast<DWORD>(std::min<std::size_t>(message.size(), 32'768));
    DWORD written = 0;
    if (::WriteFile(output, message.data(), count, &written, nullptr) == 0 || written == 0) {
      return false;
    }
    message.remove_prefix(written);
  }
  DWORD written = 0;
  return ::WriteFile(output, "\n", 1, &written, nullptr) != 0 && written == 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view{argv[1]} == "--probe-exit") {
      ::Sleep(25);
      return 17;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--probe-sleep") {
      ::Sleep(10'000);
      return 17;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--probe-reporting-failure") {
      std::cerr.exceptions(std::ios::badbit);
      std::cerr.setstate(std::ios::badbit);
      return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--probe-parent") {
      const auto directory = std::filesystem::temp_directory_path() / argv[2];
      const OwnedWindowsProcess child{WindowsProcessExecutable(), {L"--probe-sleep"}};
      {
        std::ofstream output{directory / "pid"};
        output << child.Id() << '\n';
        if (!output) {
          throw std::runtime_error{"probe parent could not publish its child"};
        }
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
      while (!std::filesystem::exists(directory / "exit")) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error{"probe parent did not receive its exit request"};
        }
        ::Sleep(1);
      }
      ::ExitProcess(0);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
  } catch (const std::exception& error) {
    return ReportProbeError(error.what()) ? 2 : 3;
  }
}
