#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/diagnostics/storage_inspector.hpp"
#include "modern_sqlite/platform/posix_vfs.hpp"

namespace {

constexpr std::string_view kOutOfMemoryJson =
    R"({"format_version":1,"ok":false,"fatal_error":{"code":"out_of_memory"}})";

[[nodiscard]] bool WriteJson(std::string_view json) {
  return std::fwrite(json.data(), 1, json.size(), stdout) == json.size() &&
         std::fputc('\n', stdout) != EOF && std::fflush(stdout) == 0;
}

[[nodiscard]] bool WriteFatal(modern_sqlite::ErrorCode code) {
  auto rendered = modern_sqlite::RenderStorageInspectionErrorJson(code);
  if (!rendered.has_value()) {
    return WriteJson(kOutOfMemoryJson);
  }
  return WriteJson(*rendered);
}

}  // namespace

int main(int argc, char* const* argv) {
#if defined(SIGPIPE)
  if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    return 1;
  }
#endif
  if (argc != 2) {
    std::fputs("usage: modern_sqlite_inspect DATABASE\n", stderr);
    return 1;
  }

  modern_sqlite::PosixVfs vfs;
  auto report = modern_sqlite::InspectDatabase(vfs, argv[1]);
  if (!report.has_value()) {
    static_cast<void>(WriteFatal(report.error().code()));
    return 1;
  }

  auto rendered = modern_sqlite::RenderStorageInspectionJson(*report);
  if (!rendered.has_value()) {
    static_cast<void>(WriteFatal(rendered.error().code()));
    return 1;
  }
  if (!WriteJson(*rendered)) {
    return 1;
  }
  return report->ok() ? 0 : 2;
}
