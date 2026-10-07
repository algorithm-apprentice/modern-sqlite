#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "write_fuzz.hpp"

namespace {

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

[[nodiscard]] bool ReplayCorpus(const std::filesystem::path& directory) {
  const auto files = CorpusFiles(directory);
  if (!files.has_value()) {
    return false;
  }
  for (const std::filesystem::path& path : files.value()) {
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    modern_sqlite::fuzz::RunWriteSqlInput(bytes.value());
  }

  constexpr std::array<std::uint8_t, 4> invalid_utf8{0xff, 0x80, 0xc0, 0x00};
  constexpr std::array<std::uint8_t, 9> embedded_nul{'S', 'E', 'L', 'E', 'C', 'T', 0x00, ' ', '1'};
  modern_sqlite::fuzz::RunWriteSqlInput({});
  modern_sqlite::fuzz::RunWriteSqlInput(invalid_utf8);
  modern_sqlite::fuzz::RunWriteSqlInput(embedded_nul);

  std::vector<std::uint8_t> oversized(std::size_t{64} * 1024U + 1U, static_cast<std::uint8_t>('x'));
  modern_sqlite::fuzz::RunWriteSqlInput(oversized);

  constexpr std::string_view statement = "SELECT id FROM fuzz_target;";
  std::vector<std::uint8_t> many_statements;
  many_statements.reserve(statement.size() * 40U);
  for (std::size_t index = 0; index < 40U; ++index) {
    for (const char byte : statement) {
      many_statements.push_back(static_cast<std::uint8_t>(byte));
    }
  }
  modern_sqlite::fuzz::RunWriteSqlInput(many_statements);
  return true;
}

struct DatabaseCorpusInputs {
  const std::filesystem::path& directory;
  const std::filesystem::path& valid_fixture;
};

[[nodiscard]] bool ReplayDatabaseCorpus(const DatabaseCorpusInputs& inputs) {
  const auto files = CorpusFiles(inputs.directory);
  if (!files.has_value()) {
    return false;
  }
  for (const std::filesystem::path& path : files.value()) {
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    modern_sqlite::fuzz::RunWriteDatabaseImageInput(bytes.value());
  }
  const auto valid = ReadFile(inputs.valid_fixture);
  if (!valid.has_value()) {
    return false;
  }
  modern_sqlite::fuzz::RunWriteDatabaseImageInput({});
  modern_sqlite::fuzz::RunWriteDatabaseImageInput(valid.value());
  std::vector<std::uint8_t> oversized(std::size_t{1024} * 1024U + 1U,
                                      static_cast<std::uint8_t>(0xffU));
  modern_sqlite::fuzz::RunWriteDatabaseImageInput(oversized);
  return true;
}

[[nodiscard]] int Run(int argument_count, char* const* arguments) {
  return argument_count == 4 && ReplayCorpus(arguments[1]) &&
                 ReplayDatabaseCorpus({
                     .directory = arguments[2],
                     .valid_fixture = arguments[3],
                 })
             ? 0
             : 1;
}

}  // namespace

int main(int argument_count, char* const* arguments) {
  try {
    return Run(argument_count, arguments);
  } catch (const std::exception&) {
    return 1;
  }
}
