#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "read_fuzz.hpp"

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
  if (!input) {
    return std::nullopt;
  }
  return bytes;
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
  if (files.empty()) {
    return std::nullopt;
  }
  return files;
}

[[nodiscard]] bool ReplaySqlCorpus(const std::filesystem::path& directory,
                                   std::string_view fixture) {
  const auto files = CorpusFiles(directory);
  if (!files.has_value()) {
    return false;
  }
  for (const std::filesystem::path& path : *files) {
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    modern_sqlite::fuzz::RunReadSqlInput(*bytes, fixture);
  }
  constexpr std::array<std::uint8_t, 4> invalid_utf8{0xff, 0x80, 0xc0, 0x00};
  constexpr std::array<std::uint8_t, 9> embedded_nul{'S', 'E', 'L', 'E', 'C', 'T', 0x00, ' ', '1'};
  modern_sqlite::fuzz::RunReadSqlInput(invalid_utf8, fixture);
  modern_sqlite::fuzz::RunReadSqlInput(embedded_nul, fixture);
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
  for (const std::filesystem::path& path : *files) {
    const auto bytes = ReadFile(path);
    if (!bytes.has_value()) {
      return false;
    }
    modern_sqlite::fuzz::RunDatabaseImageInput(*bytes);
  }
  const auto valid = ReadFile(inputs.valid_fixture);
  if (!valid.has_value()) {
    return false;
  }
  modern_sqlite::fuzz::RunDatabaseImageInput(*valid);
  return true;
}

[[nodiscard]] int Run(int argument_count, char* const* arguments) {
  if (argument_count != 4) {
    return 1;
  }
  const std::filesystem::path fixture = arguments[1];
  const std::filesystem::path sql_corpus = arguments[2];
  const std::filesystem::path database_corpus = arguments[3];
  return ReplaySqlCorpus(sql_corpus, fixture.string()) &&
                 ReplayDatabaseCorpus({.directory = database_corpus, .valid_fixture = fixture})
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
