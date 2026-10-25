#ifndef MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_
#define MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "modern_sqlite/catalog/catalog_loader.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/syntax/parser.hpp"

namespace modern_sqlite::session_detail {

enum class CatalogGenerationError {
  kOverflow,
};

[[nodiscard]] constexpr std::expected<std::uint64_t, CatalogGenerationError> NextCatalogGeneration(
    std::uint64_t current) noexcept {
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(CatalogGenerationError::kOverflow);
  }
  return current + 1U;
}

[[nodiscard]] inline Error CombineErrors(const Error& primary, const Error& cleanup,
                                         std::string_view context) {
  std::string message{primary.message()};
  message.append("; ");
  message.append(context);
  message.append(" failed with ");
  message.append(ErrorCodeName(cleanup.code()));
  message.append(": ");
  message.append(cleanup.message());
  auto combined = Error::FromSqliteCode(primary.sqlite_code(), std::move(message));
  return combined.has_value() ? std::move(*combined)
                              : Error::Create(primary.code(), std::string{primary.message()});
}

[[nodiscard]] inline Error ParseFailure(const ParseError& error) {
  ErrorCode code = ErrorCode::kGeneric;
  switch (error.code) {
    case ParseErrorCode::kResourceLimitExceeded:
    case ParseErrorCode::kParserDepthExceeded:
      code = ErrorCode::kTooLarge;
      break;
    case ParseErrorCode::kInternalInvariant:
      code = ErrorCode::kInternal;
      break;
    case ParseErrorCode::kIllegalToken:
    case ParseErrorCode::kUnexpectedToken:
    case ParseErrorCode::kUnsupportedSyntax:
    case ParseErrorCode::kExpressionDepthExceeded:
      break;
  }
  std::string message{"parse failed with "};
  message.append(ParseErrorCodeName(error.code));
  message.append(" at byte ");
  message.append(std::to_string(error.span.begin().value()));
  message.append(": ");
  message.append(ParseErrorMessage(error));
  return Error::Create(code, std::move(message));
}

class PrepareReadScope final {
 public:
  PrepareReadScope() noexcept = default;
  PrepareReadScope(const PrepareReadScope&) = delete;
  PrepareReadScope& operator=(const PrepareReadScope&) = delete;
  PrepareReadScope(PrepareReadScope&&) = delete;
  PrepareReadScope& operator=(PrepareReadScope&&) = delete;

  [[nodiscard]] Status Begin(Pager& pager) {
    if (owned_) {
      return std::unexpected(
          Error::Create(ErrorCode::kMisuse, "preparation read cleanup is unfinished"));
    }
    if (pager.in_read_transaction()) {
      return {};
    }
    owned_ = true;
    return pager.BeginRead();
  }

  [[nodiscard]] Status Finish(Pager& pager) {
    if (!owned_) {
      return {};
    }
    auto cleaned = pager.CleanupReadState();
    if (cleaned.has_value()) {
      owned_ = false;
    }
    return cleaned;
  }

 private:
  bool owned_ = false;
};

[[nodiscard]] inline Status RefreshCatalogSnapshot(BtreeDatabase& database,
                                                   CatalogSnapshotPtr& catalog, bool& initialized,
                                                   std::uint64_t& generation) {
  Pager& pager = database.pager();
  if (!pager.in_read_transaction()) {
    return std::unexpected(
        Error::Create(ErrorCode::kMisuse, "catalog refresh requires an active read transaction"));
  }
  bool reload = !initialized || catalog == nullptr;
  if (!reload) {
    auto required = CatalogRequiresReload(pager, *catalog);
    if (!required.has_value()) {
      return std::unexpected(std::move(required.error()));
    }
    reload = *required;
  }
  if (!reload) {
    return {};
  }
  initialized = false;
  const auto next = NextCatalogGeneration(generation);
  if (!next.has_value()) {
    return std::unexpected(Error::Create(ErrorCode::kTooLarge, "catalog generation is exhausted"));
  }
  auto loaded = LoadCatalog(database, CatalogLoadOptions{.generation = *next});
  if (!loaded.has_value()) {
    return std::unexpected(std::move(loaded.error()));
  }
  catalog = std::move(*loaded);
  generation = *next;
  initialized = true;
  return {};
}

[[nodiscard]] inline Result<bool> CheckPrepareSchema(Pager& pager,
                                                     const CatalogSnapshotPtr& catalog,
                                                     bool& initialized, PrepareReadScope& scope) {
  if (catalog == nullptr) {
    return std::unexpected(
        Error::Create(ErrorCode::kInternal, "schema validation requires a retained catalog"));
  }
  auto begun = scope.Begin(pager);
  if (!begun.has_value()) {
    auto ended = scope.Finish(pager);
    return !ended.has_value() ? Result<bool>{std::unexpected(CombineErrors(
                                    begun.error(), ended.error(), "schema probe cleanup"))}
                              : Result<bool>{std::unexpected(std::move(begun.error()))};
  }
  auto changed = CatalogRequiresReload(pager, *catalog);
  if (changed.has_value() && *changed) {
    initialized = false;
  }
  auto ended = scope.Finish(pager);
  if (!ended.has_value()) {
    return !changed.has_value() ? Result<bool>{std::unexpected(CombineErrors(
                                      changed.error(), ended.error(), "schema probe cleanup"))}
                                : Result<bool>{std::unexpected(std::move(ended.error()))};
  }
  return changed;
}

template <typename State>
[[nodiscard]] auto CompilePreparedStatement(State& state, SyntaxTree tree, Utf8View retained_sql)
    -> decltype(state.Compile(std::move(tree), nullptr)) {
  using CompilationResult = decltype(state.Compile(std::move(tree), nullptr));
  constexpr std::size_t kMaximumSchemaRetries = 1U;
  for (std::size_t attempt = 0U;; ++attempt) {
    auto initialized = state.InitializePrepareCatalog();
    if (!initialized.has_value()) {
      auto ended = state.prepare_read.Finish(*state.pager);
      return !ended.has_value()
                 ? CompilationResult{std::unexpected(CombineErrors(
                       initialized.error(), ended.error(), "read transaction cleanup"))}
                 : CompilationResult{std::unexpected(std::move(initialized.error()))};
    }
    bool schema_check = false;
    auto compiled = state.Compile(std::move(tree), &schema_check);
    auto ended = state.prepare_read.Finish(*state.pager);
    if (!ended.has_value()) {
      return !compiled.has_value()
                 ? CompilationResult{std::unexpected(
                       CombineErrors(compiled.error(), ended.error(), "read transaction cleanup"))}
                 : CompilationResult{std::unexpected(std::move(ended.error()))};
    }
    if (compiled.has_value()) {
      return compiled;
    }
    Error failure = std::move(compiled.error());
    if (!schema_check) {
      return std::unexpected(std::move(failure));
    }
    auto changed = CheckPrepareSchema(*state.pager, state.catalog, state.catalog_initialized,
                                      state.prepare_read);
    if (!changed.has_value()) {
      return changed.error().code() == ErrorCode::kOutOfMemory
                 ? CompilationResult{std::unexpected(std::move(changed.error()))}
                 : CompilationResult{std::unexpected(
                       CombineErrors(failure, changed.error(), "schema validation"))};
    }
    if (!*changed) {
      return std::unexpected(std::move(failure));
    }
    if (attempt == kMaximumSchemaRetries) {
      return std::unexpected(
          Error::Create(ErrorCode::kSchemaChanged, "database schema changed during preparation"));
    }
    auto retried = ParseOne(retained_sql);
    if (!retried.has_value()) {
      return std::unexpected(ParseFailure(retried.error()));
    }
    if (!retried->tree.has_value() || retried->next_offset.value() != retained_sql.size_bytes()) {
      return std::unexpected(
          Error::Create(ErrorCode::kInternal, "retained preparation SQL did not reparse exactly"));
    }
    tree = std::move(*retried->tree);
  }
}

template <typename State>
void CleanupPreparationAfterAllocationFailure(State& state) noexcept {
  try {
    [[maybe_unused]] const auto cleanup = state.CleanupPreparation();
  } catch (const std::bad_alloc&) {
    return;
  } catch (const std::length_error&) {
    return;
  }
}

}  // namespace modern_sqlite::session_detail

#endif  // MODERN_SQLITE_SRC_SESSION_SESSION_INTERNAL_HPP_
