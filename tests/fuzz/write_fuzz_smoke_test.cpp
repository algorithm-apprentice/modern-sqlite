#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include "../support/phase_timing.hpp"
#include "write_fuzz.hpp"

namespace {

class WriteSmokeProgress final {
 public:
  ~WriteSmokeProgress() { Report("scope-exit", "driver", 0); }

  void Report(std::string_view phase, std::string_view input, std::size_t bytes) noexcept {
    const auto times = timing_.Mark();
    static_cast<void>(std::fprintf(
        stderr,
        "native-contract owner=write-smoke phase=%.*s input=%.*s bytes=%zu elapsed-ms=%lld "
        "previous-phase-ms=%lld\n",
        static_cast<int>(phase.size()), phase.data(), static_cast<int>(input.size()), input.data(),
        bytes, times.elapsed_ms, times.previous_phase_ms));
    static_cast<void>(std::fflush(stderr));
  }

 private:
  modern_sqlite::test_support::PhaseTiming timing_;
};

void ReplaySqlInput(std::span<const std::uint8_t> input, std::string_view name,
                    WriteSmokeProgress& progress) {
  progress.Report("sql-input.begin", name, input.size());
  modern_sqlite::fuzz::RunWriteSqlInput(input);
  progress.Report("sql-input.end", name, input.size());
}

void ReplayDatabaseInput(std::span<const std::uint8_t> input, std::string_view name,
                         WriteSmokeProgress& progress) {
  progress.Report("database-input.begin", name, input.size());
  modern_sqlite::fuzz::RunWriteDatabaseImageInput(input);
  progress.Report("database-input.end", name, input.size());
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::nullopt;
  }
  const std::streamoff end = input.tellg();
  if (end < 0) {
    return std::nullopt;
  }
  const auto size = static_cast<std::size_t>(end);
  std::vector<std::uint8_t> bytes(size);
  input.seekg(0);
  if (size != 0U) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  }
  return input ? std::optional<std::vector<std::uint8_t>>{std::move(bytes)} : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::filesystem::path>> CorpusFiles(
    const std::filesystem::path& directory) {
  std::error_code error;
  const std::filesystem::directory_iterator iterator{directory, error};
  if (error) {
    return std::nullopt;
  }
  std::vector<std::filesystem::path> files;
  for (const std::filesystem::directory_entry& entry : iterator) {
    if (entry.is_regular_file(error)) {
      files.push_back(entry.path());
    }
    if (error) {
      return std::nullopt;
    }
  }
  std::ranges::sort(files);
  return files.empty() ? std::nullopt
                       : std::optional<std::vector<std::filesystem::path>>{std::move(files)};
}

[[nodiscard]] bool ReplayCorpus(const std::filesystem::path& directory,
                                WriteSmokeProgress& progress) {
  progress.Report("sql-corpus.enumerate.begin", "corpus", 0);
  const auto files = CorpusFiles(directory);
  if (!files.has_value()) {
    return false;
  }
  progress.Report("sql-corpus.enumerate.end", "corpus", 0);
  for (const std::filesystem::path& path : files.value()) {
    const auto name = path.filename().u8string();
    const std::string_view input_name{reinterpret_cast<const char*>(name.data()), name.size()};
    progress.Report("sql-file.read.begin", input_name, 0);
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    progress.Report("sql-file.read.end", input_name, bytes->size());
    ReplaySqlInput(bytes.value(), input_name, progress);
  }

  constexpr std::array<std::uint8_t, 4> invalid_utf8{0xff, 0x80, 0xc0, 0x00};
  constexpr std::array<std::uint8_t, 9> embedded_nul{'S', 'E', 'L', 'E', 'C', 'T', 0x00, ' ', '1'};
  ReplaySqlInput({}, "generated:empty", progress);
  ReplaySqlInput(invalid_utf8, "generated:invalid-utf8", progress);
  ReplaySqlInput(embedded_nul, "generated:embedded-nul", progress);

  std::vector<std::uint8_t> oversized(std::size_t{64} * 1024U + 1U, static_cast<std::uint8_t>('x'));
  ReplaySqlInput(oversized, "generated:oversized-sql", progress);

  constexpr std::string_view statement = "SELECT id FROM fuzz_target;";
  std::vector<std::uint8_t> many_statements;
  many_statements.reserve(statement.size() * 40U);
  for (std::size_t index = 0; index < 40U; ++index) {
    for (const char byte : statement) {
      many_statements.push_back(static_cast<std::uint8_t>(byte));
    }
  }
  ReplaySqlInput(many_statements, "generated:many-statements", progress);
  return true;
}

struct DatabaseCorpusInputs {
  const std::filesystem::path& directory;
  const std::filesystem::path& valid_fixture;
};

[[nodiscard]] bool ReplayDatabaseCorpus(const DatabaseCorpusInputs& inputs,
                                        WriteSmokeProgress& progress) {
  progress.Report("database-corpus.enumerate.begin", "corpus", 0);
  const auto files = CorpusFiles(inputs.directory);
  if (!files.has_value()) {
    return false;
  }
  progress.Report("database-corpus.enumerate.end", "corpus", 0);
  for (const std::filesystem::path& path : files.value()) {
    const auto name = path.filename().u8string();
    const std::string_view input_name{reinterpret_cast<const char*>(name.data()), name.size()};
    progress.Report("database-file.read.begin", input_name, 0);
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    progress.Report("database-file.read.end", input_name, bytes->size());
    ReplayDatabaseInput(bytes.value(), input_name, progress);
  }
  progress.Report("database-fixture.read.begin", "fixture", 0);
  const auto valid = ReadFile(inputs.valid_fixture);
  if (!valid.has_value()) {
    return false;
  }
  progress.Report("database-fixture.read.end", "fixture", valid->size());
  ReplayDatabaseInput({}, "generated:empty", progress);
  ReplayDatabaseInput(valid.value(), "generated:valid-fixture", progress);
  std::vector<std::uint8_t> oversized(std::size_t{1024} * 1024U + 1U,
                                      static_cast<std::uint8_t>(0xffU));
  ReplayDatabaseInput(oversized, "generated:oversized-database", progress);
  return true;
}

[[nodiscard]] int Run(int argument_count, char* const* arguments) {
  WriteSmokeProgress progress;
  progress.Report("driver.begin", "arguments", 0);
  const bool completed = argument_count == 4 && ReplayCorpus(arguments[1], progress) &&
                         ReplayDatabaseCorpus(
                             {
                                 .directory = arguments[2],
                                 .valid_fixture = arguments[3],
                             },
                             progress);
  progress.Report(completed ? "driver.complete" : "driver.error", "arguments", 0);
  return completed ? 0 : 1;
}

}  // namespace

int main(int argument_count, char* const* arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception&) {
    return 1;
  }
}
