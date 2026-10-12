#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"
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

[[nodiscard]] bool ExecuteSql(modern_sqlite::ReadSession& session, std::string_view sql) {
  constexpr std::size_t kMaximumGeneratedRows = 256;
  auto prepared = session.Prepare(modern_sqlite::Utf8View{sql});
  if (!prepared.has_value() || !prepared->statement.has_value()) {
    return false;
  }
  modern_sqlite::ReadStatement statement = std::move(*prepared->statement);
  for (std::size_t row = 0; row < kMaximumGeneratedRows; ++row) {
    const auto stepped = statement.Step();
    if (!stepped.has_value()) {
      return false;
    }
    if (*stepped == modern_sqlite::ReadStep::kDone) {
      return statement.Finalize().has_value();
    }
  }
  return false;
}

[[nodiscard]] bool ReplayGeneratedSql(std::string_view fixture) {
  auto session = modern_sqlite::ReadSession::Open(fixture);
  if (!session.has_value()) {
    return false;
  }
  constexpr std::array<std::uint8_t, 8> global_aggregate_input{
      0x00, 0x00, 0x1f, 0x80, 0x01, 0x00, 0x00, 0x01,
  };
  constexpr std::array<std::uint8_t, 10> grouped_aggregate_input{
      0x00, 0x01, 0x1f, 0x80, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01,
  };
  constexpr std::string_view kAggregateCompoundSql =
      "SELECT DISTINCT nullable IS NULL,count(DISTINCT value<0),"
      "group_concat(label ORDER BY id DESC) FILTER (WHERE value>=0) "
      "FROM model_rows GROUP BY nullable IS NULL "
      "UNION ALL SELECT 2,count(),group_concat(DISTINCT nullable ORDER BY nullable DESC) "
      "FROM model_rows ORDER BY 1 LIMIT 2 OFFSET 1";
  const std::string global_aggregate_sql =
      modern_sqlite::fuzz::GenerateReadSqlInput(global_aggregate_input);
  const std::string grouped_aggregate_sql =
      modern_sqlite::fuzz::GenerateReadSqlInput(grouped_aggregate_input);
  if (global_aggregate_sql.find("GROUP BY") != std::string::npos ||
      grouped_aggregate_sql.find("GROUP BY") == std::string::npos ||
      global_aggregate_sql.find("FILTER") == std::string::npos ||
      grouped_aggregate_sql.find("DISTINCT") == std::string::npos ||
      !grouped_aggregate_sql.ends_with(" LIMIT 1 OFFSET 1") ||
      !ExecuteSql(*session, global_aggregate_sql) || !ExecuteSql(*session, grouped_aggregate_sql) ||
      !ExecuteSql(*session, kAggregateCompoundSql)) {
    return false;
  }
  constexpr std::array<std::array<std::uint8_t, 4>, 4> inputs{{
      {0x01, 0x03, 0x05, 0x07},
      {0x02, 0x04, 0x06, 0x08},
      {0x7f, 0x80, 0xfe, 0xff},
      {0x11, 0x22, 0x33, 0x44},
  }};
  for (const auto& input : inputs) {
    const std::string sql = modern_sqlite::fuzz::GenerateReadSqlInput(input);
    if (!ExecuteSql(*session, sql)) {
      return false;
    }
  }
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
  return ReplaySqlCorpus(sql_corpus, fixture.string()) && ReplayGeneratedSql(fixture.string()) &&
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
