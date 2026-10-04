#ifndef MODERN_SQLITE_SESSION_READ_SESSION_HPP_
#define MODERN_SQLITE_SESSION_READ_SESSION_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/bytecode/program.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

enum class ReadStep : std::uint8_t {
  kRow,
  kDone,
};

class ReadSession;

class ReadStatement final {
 public:
  ReadStatement(const ReadStatement&) = delete;
  ReadStatement& operator=(const ReadStatement&) = delete;
  ReadStatement(ReadStatement&&) noexcept;
  ReadStatement& operator=(ReadStatement&&) noexcept;
  ~ReadStatement();

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] std::size_t parameter_count() const noexcept;
  [[nodiscard]] std::optional<std::string_view> parameter_name(
      std::size_t parameter_index) const noexcept;
  [[nodiscard]] std::size_t parameter_index(std::string_view parameter_name) const noexcept;

  [[nodiscard]] std::span<const ResultColumnMetadata> result_columns() const noexcept;
  [[nodiscard]] std::span<const SqlValue> row() const noexcept;

  [[nodiscard]] Status Bind(std::size_t parameter_index, const SqlValue& value);
  [[nodiscard]] Result<ReadStep> Step();
  [[nodiscard]] Status Reset();
  [[nodiscard]] Status Finalize();

 private:
  friend class ReadSession;

  struct Impl;

  explicit ReadStatement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct ReadPrepareOutput {
  std::optional<ReadStatement> statement;
  ByteOffset next_offset;
};

class ReadSession final {
 public:
  [[nodiscard]] static Result<ReadSession> Open(std::string_view path);
  [[nodiscard]] static Result<ReadSession> Open(std::unique_ptr<Vfs> vfs, std::string_view path);

  ReadSession(const ReadSession&) = delete;
  ReadSession& operator=(const ReadSession&) = delete;
  ReadSession(ReadSession&&) noexcept;
  ReadSession& operator=(ReadSession&&) noexcept;
  ~ReadSession();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<ReadPrepareOutput> Prepare(Utf8View source);

 private:
  friend class ReadStatement;

  struct State;

  explicit ReadSession(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SESSION_READ_SESSION_HPP_
