#ifndef MODERN_SQLITE_SESSION_WRITE_SESSION_HPP_
#define MODERN_SQLITE_SESSION_WRITE_SESSION_HPP_

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
#include "modern_sqlite/temporary_storage/temporary_storage.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {

enum class WriteStep : std::uint8_t {
  kRow,
  kDone,
};

class WriteSession;

class WriteStatement final {
 public:
  WriteStatement(const WriteStatement&) = delete;
  WriteStatement& operator=(const WriteStatement&) = delete;
  WriteStatement(WriteStatement&&) noexcept;
  WriteStatement& operator=(WriteStatement&&) noexcept;
  ~WriteStatement();

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] std::size_t parameter_count() const noexcept;
  [[nodiscard]] std::optional<std::string_view> parameter_name(
      std::size_t parameter_index) const noexcept;
  [[nodiscard]] std::size_t parameter_index(std::string_view parameter_name) const noexcept;

  [[nodiscard]] std::span<const ResultColumnMetadata> result_columns() const noexcept;
  [[nodiscard]] std::span<const SqlValue> row() const noexcept;

  [[nodiscard]] Status Bind(std::size_t parameter_index, const SqlValue& value);
  [[nodiscard]] Result<WriteStep> Step();
  [[nodiscard]] Status Reset();
  [[nodiscard]] Status Finalize();

 private:
  friend class WriteSession;

  struct Impl;

  explicit WriteStatement(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct WritePrepareOutput {
  std::optional<WriteStatement> statement;
  ByteOffset next_offset;
};

struct WriteSessionOptions {
  TemporaryStorageOptions temporary_storage{};
};

class WriteSession final {
 public:
  [[nodiscard]] static Result<WriteSession> Open(std::string_view path,
                                                 WriteSessionOptions options = {});
  [[nodiscard]] static Result<WriteSession> Open(std::unique_ptr<Vfs> vfs, std::string_view path,
                                                 WriteSessionOptions options = {});

  WriteSession(const WriteSession&) = delete;
  WriteSession& operator=(const WriteSession&) = delete;
  WriteSession(WriteSession&&) noexcept;
  WriteSession& operator=(WriteSession&&) noexcept;
  ~WriteSession();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<WritePrepareOutput> Prepare(Utf8View source);
  [[nodiscard]] std::uint64_t changes() const noexcept;
  [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
  [[nodiscard]] bool autocommit() const noexcept;

 private:
  friend class WriteStatement;

  struct State;

  explicit WriteSession(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_SESSION_WRITE_SESSION_HPP_
