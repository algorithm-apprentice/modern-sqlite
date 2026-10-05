#include <array>
#include <bit>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/read_session.hpp"
#include "modern_sqlite/text/text.hpp"

namespace {

constexpr std::size_t kMaximumProtocolBytes = std::size_t{4} * 1024U * 1024U;
constexpr std::size_t kMaximumSqlBytes = std::size_t{64} * 1024U;
constexpr std::size_t kMaximumOperations = 1024U;
constexpr std::size_t kMaximumBindingOperations = 256U;
constexpr std::size_t kMaximumColumns = 256U;
constexpr std::size_t kMaximumRows = 4096U;
constexpr std::size_t kMaximumValueBytes = std::size_t{1024} * 1024U;
constexpr std::size_t kMaximumOutputBytes = std::size_t{16} * 1024U * 1024U;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr std::string_view kHexDigits = "0123456789abcdef";

enum class OperationKind : std::uint8_t {
  kBind,
  kStep,
  kReset,
  kFinalize,
};

struct Operation {
  OperationKind kind = OperationKind::kStep;
  std::size_t parameter_index = 0;
  modern_sqlite::SqlValue value{};
};

struct TraceRequest {
  std::string sql;
  std::vector<Operation> operations;
};

class JsonBuffer {
 public:
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& text() const noexcept { return text_; }

  void Append(std::string_view value) {
    if (!ok_ || value.size() > kMaximumOutputBytes - text_.size()) {
      ok_ = false;
      return;
    }
    text_.append(value);
  }

  void AppendUnsigned(std::size_t value) {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 2U> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
      ok_ = false;
      return;
    }
    Append(std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())});
  }

  void AppendSigned(std::int64_t value) {
    std::array<char, std::numeric_limits<std::int64_t>::digits10 + 3U> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
      ok_ = false;
      return;
    }
    Append(std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())});
  }

  void AppendQuotedHex(std::string_view value) {
    Append("\"");
    AppendHexBytes(std::as_bytes(std::span<const char>{value.data(), value.size()}));
    Append("\"");
  }

  void AppendQuotedHex(modern_sqlite::ByteView value) {
    Append("\"");
    AppendHexBytes(value);
    Append("\"");
  }

  void AppendRealBits(double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    Append("\"");
    for (unsigned int shift = 60U;; shift -= 4U) {
      const auto digit = static_cast<std::size_t>((bits >> shift) & 0x0fU);
      Append(std::string_view{kHexDigits.data() + digit, 1U});
      if (shift == 0U) {
        break;
      }
    }
    Append("\"");
  }

  void Invalidate() noexcept { ok_ = false; }

 private:
  void AppendHexBytes(modern_sqlite::ByteView value) {
    if (!ok_ || value.size() > (kMaximumOutputBytes - text_.size()) / 2U) {
      ok_ = false;
      return;
    }
    for (const std::byte byte : value) {
      const auto octet = std::to_integer<unsigned int>(byte);
      text_.push_back(kHexDigits[(octet >> 4U) & 0x0fU]);
      text_.push_back(kHexDigits[octet & 0x0fU]);
    }
  }

  std::string text_;
  bool ok_ = true;
};

[[nodiscard]] std::optional<unsigned int> HexValue(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned int>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned int>(value - 'a') + 10U;
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned int>(value - 'A') + 10U;
  }
  return std::nullopt;
}

[[nodiscard]] std::expected<std::string, std::string> DecodeHex(std::string_view encoded,
                                                                std::size_t maximum_bytes,
                                                                std::string_view description) {
  if ((encoded.size() % 2U) != 0U || encoded.size() / 2U > maximum_bytes) {
    return std::unexpected(std::string{description});
  }

  std::string decoded(encoded.size() / 2U, '\0');
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    const std::optional<unsigned int> high = HexValue(encoded[index * 2U]);
    const std::optional<unsigned int> low = HexValue(encoded[index * 2U + 1U]);
    if (!high.has_value() || !low.has_value()) {
      return std::unexpected(std::string{description});
    }
    decoded[index] = static_cast<char>((high.value() << 4U) | low.value());
  }
  return decoded;
}

[[nodiscard]] std::expected<std::string, std::string> ReadProtocol() {
  std::string input;
  std::array<char, 4096> buffer{};
  while (true) {
    const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), stdin);
    if (count > kMaximumProtocolBytes - input.size()) {
      return std::unexpected("input exceeds the protocol byte limit");
    }
    input.append(buffer.data(), count);
    if (count < buffer.size()) {
      if (std::ferror(stdin) != 0) {
        return std::unexpected("unable to read standard input");
      }
      break;
    }
  }
  return input;
}

[[nodiscard]] std::vector<std::string_view> SplitLines(std::string_view input) {
  std::vector<std::string_view> lines;
  while (!input.empty()) {
    const std::size_t newline = input.find('\n');
    std::string_view line = input;
    if (newline != std::string_view::npos) {
      line.remove_suffix(input.size() - newline);
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1U);
    }
    lines.push_back(line);
    if (newline == std::string_view::npos) {
      input = {};
    } else {
      input.remove_prefix(newline + 1U);
    }
  }
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  return lines;
}

[[nodiscard]] std::vector<std::string_view> SplitFields(std::string_view line) {
  std::vector<std::string_view> fields;
  while (!line.empty()) {
    const std::size_t begin = line.find_first_not_of(' ');
    if (begin == std::string_view::npos) {
      break;
    }
    line.remove_prefix(begin);
    const std::size_t end = line.find(' ');
    if (end == std::string_view::npos) {
      fields.push_back(line);
      break;
    }
    fields.emplace_back(line.data(), end);
    line.remove_prefix(end + 1U);
  }
  return fields;
}

template <typename Integer>
[[nodiscard]] bool ParseInteger(std::string_view text, Integer& output, int base = 10) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output, base);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::expected<Operation, std::string> ParseBind(
    std::span<const std::string_view> fields) {
  if (fields.size() < 3U || fields.size() > 4U) {
    return std::unexpected("BIND requires an index, type, and optional payload");
  }

  std::size_t parameter_index = 0;
  if (!ParseInteger(fields[1], parameter_index)) {
    return std::unexpected("BIND index is not an unsigned decimal integer");
  }
  if (parameter_index > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected("BIND index exceeds SQLite C int range");
  }

  const std::string_view type = fields[2];
  if (type == "null") {
    if (fields.size() != 3U) {
      return std::unexpected("NULL BIND must not contain a payload");
    }
    return Operation{
        .kind = OperationKind::kBind,
        .parameter_index = parameter_index,
        .value = modern_sqlite::SqlValue{},
    };
  }
  if (fields.size() != 4U) {
    return std::unexpected("non-NULL BIND requires a payload");
  }

  if (type == "integer") {
    std::int64_t value = 0;
    if (!ParseInteger(fields[3], value) || std::to_string(value) != fields[3]) {
      return std::unexpected("INTEGER payload is not canonical signed decimal");
    }
    return Operation{
        .kind = OperationKind::kBind,
        .parameter_index = parameter_index,
        .value = modern_sqlite::SqlValue::Integer(value),
    };
  }
  if (type == "real") {
    if (fields[3].size() != 16U) {
      return std::unexpected("REAL payload is not a 16-digit hexadecimal bit pattern");
    }
    std::uint64_t bits = 0;
    if (!ParseInteger(fields[3], bits, 16)) {
      return std::unexpected("REAL payload is not a 16-digit hexadecimal bit pattern");
    }
    return Operation{
        .kind = OperationKind::kBind,
        .parameter_index = parameter_index,
        .value = modern_sqlite::SqlValue::Real(std::bit_cast<double>(bits)),
    };
  }
  if (type == "text") {
    auto decoded = DecodeHex(fields[3], kMaximumValueBytes, "TEXT payload is not hexadecimal");
    if (!decoded.has_value()) {
      return std::unexpected(std::move(decoded.error()));
    }
    if (!modern_sqlite::ValidateUtf8(modern_sqlite::Utf8View{*decoded}).has_value()) {
      return std::unexpected("TEXT payload is not valid UTF-8");
    }
    return Operation{
        .kind = OperationKind::kBind,
        .parameter_index = parameter_index,
        .value = modern_sqlite::SqlValue::Text(std::move(*decoded)),
    };
  }
  if (type == "blob") {
    auto decoded = DecodeHex(fields[3], kMaximumValueBytes, "BLOB payload is not hexadecimal");
    if (!decoded.has_value()) {
      return std::unexpected(std::move(decoded.error()));
    }
    return Operation{
        .kind = OperationKind::kBind,
        .parameter_index = parameter_index,
        .value = modern_sqlite::SqlValue::Blob(
            modern_sqlite::ByteBuffer::CopyOf(modern_sqlite::AsBytes(*decoded))),
    };
  }
  return std::unexpected("BIND type is unknown");
}

[[nodiscard]] std::expected<TraceRequest, std::string> ParseTrace(std::string_view input) {
  const std::vector<std::string_view> lines = SplitLines(input);
  if (lines.empty() || lines.front() != "MSRT1") {
    return std::unexpected("missing MSRT1 header");
  }
  if (lines.size() < 2U || !lines[1].starts_with("SQL ")) {
    return std::unexpected("missing SQL line");
  }

  const std::string_view encoded_sql{lines[1].data() + 4U, lines[1].size() - 4U};
  auto decoded_sql = DecodeHex(encoded_sql, kMaximumSqlBytes, "SQL payload is not hexadecimal");
  if (!decoded_sql.has_value()) {
    return std::unexpected(std::move(decoded_sql.error()));
  }

  TraceRequest request{
      .sql = std::move(*decoded_sql),
      .operations = {},
  };
  request.operations.reserve(lines.size() - 2U);
  std::size_t binding_count = 0;
  bool saw_finalize = false;
  for (std::size_t line_index = 2U; line_index < lines.size(); ++line_index) {
    if (request.operations.size() == kMaximumOperations) {
      return std::unexpected("operation count exceeds the protocol limit");
    }
    const std::vector<std::string_view> fields = SplitFields(lines[line_index]);
    if (fields.empty()) {
      return std::unexpected("blank operation line");
    }
    if (saw_finalize) {
      return std::unexpected("FINALIZE must be the last operation");
    }

    if (fields.front() == "BIND") {
      if (binding_count == kMaximumBindingOperations) {
        return std::unexpected("binding operation count exceeds the protocol limit");
      }
      auto operation = ParseBind(fields);
      if (!operation.has_value()) {
        return std::unexpected(std::move(operation.error()));
      }
      request.operations.push_back(std::move(*operation));
      ++binding_count;
      continue;
    }
    if (fields.size() != 1U) {
      return std::unexpected("non-BIND operation has unexpected fields");
    }
    if (fields.front() == "STEP") {
      request.operations.push_back(Operation{.kind = OperationKind::kStep});
    } else if (fields.front() == "RESET") {
      request.operations.push_back(Operation{.kind = OperationKind::kReset});
    } else if (fields.front() == "FINALIZE") {
      request.operations.push_back(Operation{.kind = OperationKind::kFinalize});
      saw_finalize = true;
    } else {
      return std::unexpected("unknown operation");
    }
  }

  if (!request.operations.empty() && !saw_finalize) {
    return std::unexpected("statement transcript must end with FINALIZE");
  }
  return request;
}

void AppendStatus(JsonBuffer& json, const modern_sqlite::Error* error) {
  json.Append(R"({"primary_code":")");
  if (error == nullptr) {
    json.Append(R"(ok","return_code":0,"extended_code":0})");
    return;
  }

  json.Append(modern_sqlite::ErrorCodeName(error->code()));
  json.Append(R"(","return_code":)");
  json.AppendSigned(error->sqlite_code());
  json.Append(",\"extended_code\":");
  json.AppendSigned(error->sqlite_code());
  if (!error->message().empty()) {
    json.Append(",\"message_hex\":");
    json.AppendQuotedHex(error->message());
  }
  json.Append("}");
}

void AppendColumns(JsonBuffer& json, std::span<const modern_sqlite::ResultColumnMetadata> columns) {
  json.Append("[");
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index != 0U) {
      json.Append(",");
    }
    json.Append("{\"name_hex\":");
    json.AppendQuotedHex(columns[index].name);
    json.Append(",\"declared_type_hex\":");
    const std::optional<std::string> declared_type = columns[index].declared_type;
    if (declared_type.has_value()) {
      json.AppendQuotedHex(declared_type.value());
    } else {
      json.Append("null");
    }
    json.Append("}");
  }
  json.Append("]");
}

[[nodiscard]] bool ValueWithinLimits(const modern_sqlite::SqlValue& value) noexcept {
  if (const auto text = value.text_value(); text.has_value()) {
    return text->size_bytes() <= kMaximumValueBytes;
  }
  if (const auto blob = value.blob_value(); blob.has_value()) {
    return blob->size() <= kMaximumValueBytes;
  }
  return true;
}

void AppendValue(JsonBuffer& json, const modern_sqlite::SqlValue& value) {
  using modern_sqlite::SqlValueType;
  switch (value.type()) {
    case SqlValueType::kNull:
      json.Append(R"({"type":"null"})");
      return;
    case SqlValueType::kInteger: {
      const std::optional<std::int64_t> integer = value.integer_value();
      if (!integer.has_value()) {
        json.Invalidate();
        return;
      }
      json.Append(R"({"type":"integer","value":")");
      json.AppendSigned(*integer);
      json.Append("\"}");
      return;
    }
    case SqlValueType::kReal: {
      const std::optional<double> real = value.real_value();
      if (!real.has_value()) {
        json.Invalidate();
        return;
      }
      json.Append(R"({"type":"real","bits":)");
      json.AppendRealBits(*real);
      json.Append("}");
      return;
    }
    case SqlValueType::kText: {
      const std::optional<modern_sqlite::Utf8View> text = value.text_value();
      if (!text.has_value()) {
        json.Invalidate();
        return;
      }
      json.Append(R"({"type":"text","hex":)");
      json.AppendQuotedHex(text->bytes());
      json.Append("}");
      return;
    }
    case SqlValueType::kBlob: {
      const std::optional<modern_sqlite::ByteView> blob = value.blob_value();
      if (!blob.has_value()) {
        json.Invalidate();
        return;
      }
      json.Append(R"({"type":"blob","hex":)");
      json.AppendQuotedHex(*blob);
      json.Append("}");
      return;
    }
  }
  json.Invalidate();
}

void AppendRow(JsonBuffer& json, std::span<const modern_sqlite::SqlValue> row) {
  json.Append("[");
  for (std::size_t index = 0; index < row.size(); ++index) {
    if (index != 0U) {
      json.Append(",");
    }
    AppendValue(json, row[index]);
  }
  json.Append("]");
}

[[nodiscard]] std::string LimitOutcome(std::string_view resource) {
  std::string output = R"({"format_version":1,"kind":"limit","resource":")";
  output.append(resource);
  output.append(R"(","completed_observations":[]})");
  return output;
}

[[nodiscard]] bool WriteJson(std::string_view json) {
  return std::fwrite(json.data(), 1, json.size(), stdout) == json.size() &&
         std::fputc('\n', stdout) != EOF && std::fflush(stdout) == 0;
}

[[nodiscard]] std::optional<std::string_view> RowLimitResource(
    const modern_sqlite::ReadStatement& statement) noexcept {
  if (statement.result_columns().size() > kMaximumColumns ||
      statement.row().size() > kMaximumColumns) {
    return "result_columns";
  }
  for (const modern_sqlite::SqlValue& value : statement.row()) {
    if (!ValueWithinLimits(value)) {
      return "value_bytes";
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::expected<std::string, std::string_view> ExecuteTrace(
    modern_sqlite::ReadStatement& statement, const TraceRequest& request,
    modern_sqlite::ByteOffset next_offset) {
  if (statement.parameter_count() > kMaximumBindingOperations ||
      statement.result_columns().size() > kMaximumColumns) {
    return std::unexpected("result_metadata");
  }

  JsonBuffer json;
  json.Append(R"({"format_version":1,"kind":"statement","preparation":{"next_offset":)");
  json.AppendUnsigned(next_offset.value());
  json.Append(",\"parameter_count\":");
  json.AppendUnsigned(statement.parameter_count());
  json.Append(",\"parameter_names\":[");
  for (std::size_t index = 1U; index <= statement.parameter_count(); ++index) {
    if (index != 1U) {
      json.Append(",");
    }
    const std::optional<std::string_view> name = statement.parameter_name(index);
    if (name.has_value()) {
      json.AppendQuotedHex(*name);
    } else {
      json.Append("null");
    }
  }
  json.Append("],\"columns\":");
  AppendColumns(json, statement.result_columns());
  json.Append("},\"observations\":[");

  std::size_t row_count = 0;
  for (std::size_t operation_index = 0; operation_index < request.operations.size();
       ++operation_index) {
    if (operation_index != 0U) {
      json.Append(",");
    }
    const Operation& operation = request.operations[operation_index];
    json.Append("{\"operation_index\":");
    json.AppendUnsigned(operation_index);

    switch (operation.kind) {
      case OperationKind::kBind: {
        json.Append(R"(,"op":"bind","index":)");
        json.AppendUnsigned(operation.parameter_index);
        const modern_sqlite::Status status =
            statement.Bind(operation.parameter_index, operation.value);
        json.Append(",\"status\":");
        AppendStatus(json, status.has_value() ? nullptr : &status.error());
        json.Append("}");
        break;
      }
      case OperationKind::kStep: {
        json.Append(R"(,"op":"step")");
        const modern_sqlite::Result<modern_sqlite::ReadStep> result = statement.Step();
        if (!result.has_value()) {
          json.Append(R"(,"result":"error","status":)");
          AppendStatus(json, &result.error());
          json.Append("}");
          break;
        }
        if (*result == modern_sqlite::ReadStep::kDone) {
          json.Append(R"(,"result":"done","return_code":)");
          json.AppendSigned(kSqliteDone);
          json.Append("}");
          break;
        }

        if (row_count == kMaximumRows) {
          return std::unexpected("result_rows");
        }
        ++row_count;
        if (const std::optional<std::string_view> limit = RowLimitResource(statement);
            limit.has_value()) {
          return std::unexpected(*limit);
        }
        json.Append(R"(,"result":"row","return_code":)");
        json.AppendSigned(kSqliteRow);
        json.Append(",\"columns\":");
        AppendColumns(json, statement.result_columns());
        json.Append(",\"row\":");
        AppendRow(json, statement.row());
        json.Append("}");
        break;
      }
      case OperationKind::kReset: {
        json.Append(R"(,"op":"reset","status":)");
        const modern_sqlite::Status status = statement.Reset();
        AppendStatus(json, status.has_value() ? nullptr : &status.error());
        json.Append("}");
        break;
      }
      case OperationKind::kFinalize: {
        json.Append(R"(,"op":"finalize","status":)");
        const modern_sqlite::Status status = statement.Finalize();
        AppendStatus(json, status.has_value() ? nullptr : &status.error());
        json.Append("}");
        break;
      }
    }
    if (!json.ok()) {
      return std::unexpected("canonical_output");
    }
  }

  json.Append("]}");
  if (!json.ok()) {
    return std::unexpected("canonical_output");
  }
  return json.text();
}

[[nodiscard]] int Run(std::string_view database_path) {
  auto protocol = ReadProtocol();
  if (!protocol.has_value()) {
    std::fputs("invalid read trace protocol: ", stderr);
    std::fputs(protocol.error().c_str(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }
  auto request = ParseTrace(*protocol);
  if (!request.has_value()) {
    std::fputs("invalid read trace protocol: ", stderr);
    std::fputs(request.error().c_str(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }

  auto opened = modern_sqlite::ReadSession::Open(database_path);
  if (!opened.has_value()) {
    JsonBuffer json;
    json.Append(R"({"format_version":1,"kind":"open_error","status":)");
    AppendStatus(json, &opened.error());
    json.Append("}");
    return json.ok() && WriteJson(json.text()) ? 0 : 1;
  }

  auto prepared = opened->Prepare(modern_sqlite::Utf8View{request->sql});
  if (!prepared.has_value()) {
    JsonBuffer json;
    json.Append(R"({"format_version":1,"kind":"prepare_error","status":)");
    AppendStatus(json, &prepared.error());
    json.Append("}");
    return json.ok() && WriteJson(json.text()) ? 0 : 1;
  }

  if (!prepared->statement.has_value()) {
    if (!request->operations.empty()) {
      std::fputs("invalid read trace protocol: empty statement has operations\n", stderr);
      return 1;
    }
    JsonBuffer json;
    json.Append(R"({"format_version":1,"kind":"empty","next_offset":)");
    json.AppendUnsigned(prepared->next_offset.value());
    json.Append("}");
    return json.ok() && WriteJson(json.text()) ? 0 : 1;
  }

  if (request->operations.empty()) {
    std::fputs("invalid read trace protocol: statement transcript must end with FINALIZE\n",
               stderr);
    return 1;
  }

  auto output = ExecuteTrace(*prepared->statement, *request, prepared->next_offset);
  if (!output.has_value()) {
    const std::string limit = LimitOutcome(output.error());
    return WriteJson(limit) ? 0 : 1;
  }
  return WriteJson(*output) ? 0 : 1;
}

}  // namespace

int main(int argc, char* const* argv) {
#if defined(SIGPIPE)
  if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    return 1;
  }
#endif
  if (argc != 2) {
    std::fputs("usage: modern_sqlite_read_trace DATABASE\n", stderr);
    return 1;
  }

  try {
    return Run(argv[1]);
  } catch (const std::exception& error) {
    std::fputs("read trace failure: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }
}
