#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/session/write_session.hpp"
#include "modern_sqlite/text/text.hpp"

namespace modern_sqlite {
namespace {

constexpr std::uint64_t kModelSeed = UINT64_C(0x4d53514c57524954);
constexpr std::size_t kModelActions = 256;
constexpr std::size_t kMaximumRows = 128;
constexpr std::size_t kMaximumSavepoints = 8;
constexpr std::size_t kHistoryLimit = 16;
constexpr std::string_view kGeneratorVersion = "splitmix64-v1";

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t Next() noexcept {
    state_ += UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
  }

 private:
  std::uint64_t state_;
};

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename T>
[[nodiscard]] T TakeOptional(std::optional<T> value, std::string_view message) {
  if (!value.has_value()) {
    throw std::runtime_error{std::string{message}};
  }
  return std::move(value).value();
}

void RequireStatus(Status status) {
  if (!status.has_value()) {
    throw std::runtime_error(status.error().ToString());
  }
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("modern-sqlite-write-model-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path DatabasePath() const { return path_ / "model.sqlite"; }

 private:
  std::filesystem::path path_;
};

enum class ActionKind : std::uint8_t {
  kCreateMain,
  kCreateMainNoOp,
  kCreateTemp,
  kCreateTempNoOp,
  kInsertExplicit,
  kInsertDefault,
  kInsertDuplicate,
  kInsertNotNull,
  kInsertFractional,
  kUpdateExact,
  kUpdateScan,
  kMoveRowId,
  kDeleteExact,
  kDeleteScan,
  kBegin,
  kCommit,
  kRollback,
  kSavepoint,
  kRelease,
  kRollbackTo,
  kSelectSnapshot,
  kReopen,
};

struct Action {
  ActionKind kind = ActionKind::kSelectSnapshot;
  std::int64_t first = 0;
  std::int64_t second = 0;
  std::string text;
};

struct GeneratedAction {
  Action action;
  std::array<std::uint64_t, 4> words{};
};

struct ModelRow {
  std::string name;
  std::optional<double> score;
  std::optional<std::vector<std::byte>> payload;

  bool operator==(const ModelRow&) const = default;
};

struct DatabaseModel {
  bool items_visible = false;
  bool temp_visible = false;
  std::map<std::int64_t, ModelRow> rows;

  bool operator==(const DatabaseModel&) const = default;
};

struct SavepointModel {
  std::string name;
  DatabaseModel snapshot;
};

enum class ModelTransactionKind : std::uint8_t {
  kNone,
  kExplicit,
  kSavepoint,
};

struct ExpectedOutcome {
  std::optional<ErrorCode> error;
  bool finalize_repeats_error = false;
};

struct EngineOutcome {
  std::optional<ErrorCode> error;
  std::optional<ErrorCode> finalize_error;
};

[[nodiscard]] char FoldAscii(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool EqualNoCase(std::string_view left, std::string_view right) noexcept {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](char first, char second) {
           return FoldAscii(first) == FoldAscii(second);
         });
}

[[nodiscard]] std::vector<std::byte> PayloadFor(std::int64_t rowid, std::int64_t score) {
  return {
      static_cast<std::byte>(static_cast<std::uint8_t>(rowid)),
      static_cast<std::byte>(static_cast<std::uint8_t>(score)),
  };
}

class ReferenceModel final {
 public:
  [[nodiscard]] bool autocommit() const noexcept {
    return transaction_kind_ == ModelTransactionKind::kNone;
  }

  [[nodiscard]] std::size_t savepoint_count() const noexcept { return savepoints_.size(); }
  [[nodiscard]] const DatabaseModel& database() const noexcept { return database_; }
  [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }
  [[nodiscard]] std::int64_t last_insert_rowid() const noexcept { return last_insert_rowid_; }

  void ResetConnectionState() noexcept {
    changes_ = 0;
    last_insert_rowid_ = 0;
  }

  [[nodiscard]] std::optional<std::int64_t> ExistingRow(std::uint64_t selector) const {
    if (database_.rows.empty()) {
      return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(selector % database_.rows.size());
    auto found = database_.rows.begin();
    std::advance(found, static_cast<std::ptrdiff_t>(offset));
    return found->first;
  }

  [[nodiscard]] std::int64_t FreeRowId(std::uint64_t selector) const {
    std::int64_t candidate = static_cast<std::int64_t>(selector % 257U) - 64;
    for (std::size_t attempt = 0; attempt < 512U; ++attempt, ++candidate) {
      if (!database_.rows.contains(candidate)) {
        return candidate;
      }
    }
    throw std::runtime_error{"write model could not find a free rowid"};
  }

  [[nodiscard]] std::string ExistingSavepoint(std::uint64_t selector) const {
    if (savepoints_.empty()) {
      return "missing";
    }
    const auto offset = static_cast<std::size_t>(selector % savepoints_.size());
    return savepoints_[offset].name;
  }

  [[nodiscard]] ExpectedOutcome Apply(const Action& action) {
    switch (action.kind) {
      case ActionKind::kCreateMain:
        if (database_.items_visible) {
          return PrepareError(ErrorCode::kGeneric);
        }
        database_.items_visible = true;
        return {};
      case ActionKind::kCreateMainNoOp:
        return {};
      case ActionKind::kCreateTemp:
        if (database_.temp_visible) {
          return PrepareError(ErrorCode::kGeneric);
        }
        database_.temp_visible = true;
        return {};
      case ActionKind::kCreateTempNoOp:
        return {};
      case ActionKind::kInsertExplicit:
        return InsertExplicit(action.first, action.second, action.text);
      case ActionKind::kInsertDefault:
        return InsertDefault();
      case ActionKind::kInsertDuplicate:
      case ActionKind::kInsertNotNull:
        changes_ = 0;
        return StepError(ErrorCode::kConstraint);
      case ActionKind::kInsertFractional:
        changes_ = 0;
        return StepError(ErrorCode::kTypeMismatch);
      case ActionKind::kUpdateExact:
        return UpdateExact(action);
      case ActionKind::kUpdateScan:
        return UpdateScan(action.first, action.text);
      case ActionKind::kMoveRowId:
        return MoveRowId(action.first, action.second);
      case ActionKind::kDeleteExact:
        return DeleteExact(action.first);
      case ActionKind::kDeleteScan:
        return DeleteScan(action.first);
      case ActionKind::kBegin:
        return Begin();
      case ActionKind::kCommit:
        return Commit();
      case ActionKind::kRollback:
        return Rollback();
      case ActionKind::kSavepoint:
        return Savepoint(action.text);
      case ActionKind::kRelease:
        return Release(action.text);
      case ActionKind::kRollbackTo:
        return RollbackTo(action.text);
      case ActionKind::kSelectSnapshot:
      case ActionKind::kReopen:
        return {};
    }
    throw std::runtime_error{"write model action kind is invalid"};
  }

 private:
  [[nodiscard]] static ExpectedOutcome PrepareError(ErrorCode code) noexcept {
    return ExpectedOutcome{.error = code, .finalize_repeats_error = false};
  }

  [[nodiscard]] static ExpectedOutcome StepError(ErrorCode code) noexcept {
    return ExpectedOutcome{.error = code, .finalize_repeats_error = true};
  }

  [[nodiscard]] ExpectedOutcome InsertExplicit(std::int64_t rowid, std::int64_t score,
                                               std::string_view name) {
    if (database_.rows.contains(rowid)) {
      changes_ = 0;
      return StepError(ErrorCode::kConstraint);
    }
    database_.rows.emplace(rowid, ModelRow{
                                      .name = std::string{name},
                                      .score = static_cast<double>(score),
                                      .payload = PayloadFor(rowid, score),
                                  });
    changes_ = 1;
    last_insert_rowid_ = rowid;
    return {};
  }

  [[nodiscard]] ExpectedOutcome InsertDefault() {
    const std::int64_t rowid = database_.rows.empty() ? 1 : database_.rows.rbegin()->first + 1;
    database_.rows.emplace(rowid, ModelRow{
                                      .name = "seed",
                                      .score = std::nullopt,
                                      .payload = std::nullopt,
                                  });
    changes_ = 1;
    last_insert_rowid_ = rowid;
    return {};
  }

  [[nodiscard]] ExpectedOutcome UpdateExact(const Action& action) {
    const auto found = database_.rows.find(action.first);
    if (found == database_.rows.end()) {
      changes_ = 0;
      return {};
    }
    found->second.name = action.text;
    found->second.score = static_cast<double>(action.second);
    changes_ = 1;
    return {};
  }

  [[nodiscard]] ExpectedOutcome UpdateScan(std::int64_t threshold, std::string_view suffix) {
    std::uint64_t changed = 0;
    for (auto& [rowid, row] : database_.rows) {
      if (rowid >= threshold) {
        row.name.append(suffix);
        ++changed;
      }
    }
    changes_ = changed;
    return {};
  }

  [[nodiscard]] ExpectedOutcome MoveRowId(std::int64_t source, std::int64_t destination) {
    const auto found = database_.rows.find(source);
    if (found == database_.rows.end()) {
      changes_ = 0;
      return {};
    }
    if (source != destination && database_.rows.contains(destination)) {
      changes_ = 0;
      return StepError(ErrorCode::kConstraint);
    }
    if (source != destination) {
      ModelRow row = std::move(found->second);
      database_.rows.erase(found);
      database_.rows.emplace(destination, std::move(row));
    }
    changes_ = 1;
    return {};
  }

  [[nodiscard]] ExpectedOutcome DeleteExact(std::int64_t rowid) {
    changes_ = database_.rows.erase(rowid);
    return {};
  }

  [[nodiscard]] ExpectedOutcome DeleteScan(std::int64_t threshold) {
    std::uint64_t changed = 0;
    for (auto iterator = database_.rows.lower_bound(threshold); iterator != database_.rows.end();) {
      iterator = database_.rows.erase(iterator);
      ++changed;
    }
    changes_ = changed;
    return {};
  }

  [[nodiscard]] ExpectedOutcome Begin() {
    if (!autocommit()) {
      return StepError(ErrorCode::kGeneric);
    }
    transaction_base_ = database_;
    transaction_kind_ = ModelTransactionKind::kExplicit;
    return {};
  }

  [[nodiscard]] ExpectedOutcome Commit() {
    if (autocommit()) {
      return StepError(ErrorCode::kGeneric);
    }
    transaction_base_.reset();
    savepoints_.clear();
    transaction_kind_ = ModelTransactionKind::kNone;
    return {};
  }

  [[nodiscard]] ExpectedOutcome Rollback() {
    if (autocommit()) {
      return StepError(ErrorCode::kGeneric);
    }
    database_ = TakeOptional(std::move(transaction_base_), "model transaction base is missing");
    transaction_base_.reset();
    savepoints_.clear();
    transaction_kind_ = ModelTransactionKind::kNone;
    return {};
  }

  [[nodiscard]] ExpectedOutcome Savepoint(std::string_view name) {
    if (autocommit()) {
      transaction_base_ = database_;
      transaction_kind_ = ModelTransactionKind::kSavepoint;
    }
    savepoints_.push_back(SavepointModel{
        .name = std::string{name},
        .snapshot = database_,
    });
    return {};
  }

  [[nodiscard]] ExpectedOutcome Release(std::string_view name) {
    const auto found = FindSavepoint(name);
    if (!found.has_value()) {
      return StepError(ErrorCode::kGeneric);
    }
    const std::size_t found_index = found.value();
    const bool commits_transaction =
        transaction_kind_ == ModelTransactionKind::kSavepoint && found_index == 0U;
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(found_index),
                      savepoints_.end());
    if (commits_transaction) {
      transaction_base_.reset();
      transaction_kind_ = ModelTransactionKind::kNone;
    }
    return {};
  }

  [[nodiscard]] ExpectedOutcome RollbackTo(std::string_view name) {
    const auto found = FindSavepoint(name);
    if (!found.has_value()) {
      return StepError(ErrorCode::kGeneric);
    }
    const std::size_t found_index = found.value();
    database_ = savepoints_[found_index].snapshot;
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(found_index + 1U),
                      savepoints_.end());
    return {};
  }

  [[nodiscard]] std::optional<std::size_t> FindSavepoint(std::string_view name) const {
    for (std::size_t index = savepoints_.size(); index > 0U; --index) {
      if (EqualNoCase(savepoints_[index - 1U].name, name)) {
        return index - 1U;
      }
    }
    return std::nullopt;
  }

  DatabaseModel database_;
  std::optional<DatabaseModel> transaction_base_;
  std::vector<SavepointModel> savepoints_;
  ModelTransactionKind transaction_kind_ = ModelTransactionKind::kNone;
  std::uint64_t changes_ = 0;
  std::int64_t last_insert_rowid_ = 0;
};

[[nodiscard]] std::string_view ActionKindName(ActionKind kind) noexcept {
  switch (kind) {
    case ActionKind::kCreateMain:
      return "create-main";
    case ActionKind::kCreateMainNoOp:
      return "create-main-noop";
    case ActionKind::kCreateTemp:
      return "create-temp";
    case ActionKind::kCreateTempNoOp:
      return "create-temp-noop";
    case ActionKind::kInsertExplicit:
      return "insert-explicit";
    case ActionKind::kInsertDefault:
      return "insert-default";
    case ActionKind::kInsertDuplicate:
      return "insert-duplicate";
    case ActionKind::kInsertNotNull:
      return "insert-not-null";
    case ActionKind::kInsertFractional:
      return "insert-fractional";
    case ActionKind::kUpdateExact:
      return "update-exact";
    case ActionKind::kUpdateScan:
      return "update-scan";
    case ActionKind::kMoveRowId:
      return "move-rowid";
    case ActionKind::kDeleteExact:
      return "delete-exact";
    case ActionKind::kDeleteScan:
      return "delete-scan";
    case ActionKind::kBegin:
      return "begin";
    case ActionKind::kCommit:
      return "commit";
    case ActionKind::kRollback:
      return "rollback";
    case ActionKind::kSavepoint:
      return "savepoint";
    case ActionKind::kRelease:
      return "release";
    case ActionKind::kRollbackTo:
      return "rollback-to";
    case ActionKind::kSelectSnapshot:
      return "select";
    case ActionKind::kReopen:
      return "reopen";
  }
  return "unknown";
}

[[nodiscard]] std::string DescribeAction(const Action& action) {
  std::ostringstream output;
  output << ActionKindName(action.kind) << "(first=" << action.first << ",second=" << action.second
         << ",text=" << action.text << ")";
  return output.str();
}

[[nodiscard]] std::vector<GeneratedAction> FixedPrelude() {
  const std::vector<Action> actions{
      {.kind = ActionKind::kCreateMain, .text = {}},
      {.kind = ActionKind::kCreateMainNoOp, .text = {}},
      {.kind = ActionKind::kInsertExplicit, .first = 1, .second = 1, .text = "alpha"},
      {.kind = ActionKind::kInsertDefault, .text = {}},
      {.kind = ActionKind::kInsertDuplicate, .first = 1, .second = 9, .text = "duplicate"},
      {.kind = ActionKind::kInsertNotNull, .first = 3, .text = {}},
      {.kind = ActionKind::kInsertFractional, .first = 7, .text = {}},
      {.kind = ActionKind::kUpdateExact, .first = 1, .second = 2, .text = "updated"},
      {.kind = ActionKind::kUpdateScan, .first = 1, .text = "x"},
      {.kind = ActionKind::kMoveRowId, .first = 1, .second = 5, .text = {}},
      {.kind = ActionKind::kDeleteExact, .first = 2, .text = {}},
      {.kind = ActionKind::kBegin, .text = {}},
      {.kind = ActionKind::kInsertExplicit, .first = 9, .second = 9, .text = "kept"},
      {.kind = ActionKind::kInsertDuplicate, .first = 9, .second = 10, .text = "duplicate"},
      {.kind = ActionKind::kSavepoint, .text = "MiXeD"},
      {.kind = ActionKind::kInsertExplicit, .first = 10, .second = 10, .text = "temporary"},
      {.kind = ActionKind::kRollbackTo, .text = "mixed"},
      {.kind = ActionKind::kRelease, .text = "MIXED"},
      {.kind = ActionKind::kCreateTemp, .text = {}},
      {.kind = ActionKind::kRollback, .text = {}},
      {.kind = ActionKind::kSavepoint, .text = "outer"},
      {.kind = ActionKind::kCreateTemp, .text = {}},
      {.kind = ActionKind::kInsertExplicit, .first = 11, .second = 11, .text = "savepoint"},
      {.kind = ActionKind::kRollbackTo, .text = "OUTER"},
      {.kind = ActionKind::kRelease, .text = "outer"},
      {.kind = ActionKind::kCommit, .text = {}},
      {.kind = ActionKind::kBegin, .text = {}},
      {.kind = ActionKind::kCreateTemp, .text = {}},
      {.kind = ActionKind::kCommit, .text = {}},
      {.kind = ActionKind::kReopen, .text = {}},
      {.kind = ActionKind::kBegin, .text = {}},
      {.kind = ActionKind::kBegin, .text = {}},
      {.kind = ActionKind::kRollback, .text = {}},
      {.kind = ActionKind::kSavepoint, .text = "dup"},
      {.kind = ActionKind::kSavepoint, .text = "DUP"},
      {.kind = ActionKind::kInsertExplicit, .first = 12, .second = 12, .text = "duplicate-name"},
      {.kind = ActionKind::kRollbackTo, .text = "dup"},
      {.kind = ActionKind::kRelease, .text = "DuP"},
      {.kind = ActionKind::kRelease, .text = "dup"},
      {.kind = ActionKind::kSelectSnapshot, .text = {}},
  };

  std::vector<GeneratedAction> generated;
  generated.reserve(actions.size());
  for (std::size_t index = 0; index < actions.size(); ++index) {
    generated.push_back(GeneratedAction{
        .action = actions[index],
        .words = {index, 0, 0, 0},
    });
  }
  return generated;
}

[[nodiscard]] std::string MixedCase(std::string value, std::uint64_t selector) {
  if ((selector & 1U) == 0U) {
    return value;
  }
  for (char& character : value) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    } else if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return value;
}

[[nodiscard]] Action GenerateAction(const ReferenceModel& model,
                                    const std::array<std::uint64_t, 4>& words) {
  constexpr std::array<std::string_view, 5> kSavepointNames{
      "alpha", "ALPHA", "beta", "BETA", "gamma",
  };
  const std::uint64_t selector = words[0] % 18U;
  if (model.database().rows.size() >= kMaximumRows && selector <= 3U) {
    return Action{
        .kind = ActionKind::kDeleteScan,
        .first = static_cast<std::int64_t>(words[1] % 129U) - 32,
        .text = {},
    };
  }

  switch (selector) {
    case 0:
      return Action{
          .kind = ActionKind::kInsertExplicit,
          .first = model.FreeRowId(words[1]),
          .second = static_cast<std::int64_t>(words[2] % 101U) - 50,
          .text = "row-" + std::to_string(words[3] % 1000U),
      };
    case 1:
      return Action{.kind = ActionKind::kInsertDefault, .text = {}};
    case 2: {
      const std::optional<std::int64_t> existing = model.ExistingRow(words[1]);
      return existing.has_value()
                 ? Action{
                       .kind = ActionKind::kInsertDuplicate,
                       .first = existing.value(),
                       .second = static_cast<std::int64_t>(words[2] % 101U),
                       .text = "duplicate",
                   }
                 : Action{
                       .kind = ActionKind::kInsertExplicit,
                       .first = model.FreeRowId(words[1]),
                       .second = 1,
                       .text = "first",
                   };
    }
    case 3:
      return Action{
          .kind =
              (words[1] & 1U) == 0U ? ActionKind::kInsertNotNull : ActionKind::kInsertFractional,
          .first = static_cast<std::int64_t>(words[2] % 257U) - 64,
          .text = {},
      };
    case 4:
      return Action{
          .kind = ActionKind::kUpdateExact,
          .first =
              model.ExistingRow(words[1]).value_or(static_cast<std::int64_t>(words[1] % 257U) - 64),
          .second = static_cast<std::int64_t>(words[2] % 101U) - 50,
          .text = "update-" + std::to_string(words[3] % 1000U),
      };
    case 5:
      return Action{
          .kind = ActionKind::kUpdateScan,
          .first = static_cast<std::int64_t>(words[1] % 257U) - 64,
          .text = (words[2] & 1U) == 0U ? "a" : "z",
      };
    case 6:
      return Action{
          .kind = ActionKind::kMoveRowId,
          .first =
              model.ExistingRow(words[1]).value_or(static_cast<std::int64_t>(words[1] % 257U) - 64),
          .second = (words[2] & 1U) == 0U
                        ? model.FreeRowId(words[3])
                        : model.ExistingRow(words[3]).value_or(model.FreeRowId(words[3])),
          .text = {},
      };
    case 7:
      return Action{
          .kind = ActionKind::kDeleteExact,
          .first =
              model.ExistingRow(words[1]).value_or(static_cast<std::int64_t>(words[1] % 257U) - 64),
          .text = {},
      };
    case 8:
      return Action{
          .kind = ActionKind::kDeleteScan,
          .first = static_cast<std::int64_t>(words[1] % 257U) - 64,
          .text = {},
      };
    case 9:
      return Action{.kind = ActionKind::kBegin, .text = {}};
    case 10:
      return Action{.kind = ActionKind::kCommit, .text = {}};
    case 11:
      return Action{.kind = ActionKind::kRollback, .text = {}};
    case 12:
      if (model.savepoint_count() >= kMaximumSavepoints) {
        return Action{
            .kind = ActionKind::kRelease,
            .text = MixedCase(model.ExistingSavepoint(words[1]), words[2]),
        };
      }
      return Action{
          .kind = ActionKind::kSavepoint,
          .text = std::string{kSavepointNames[words[1] % kSavepointNames.size()]},
      };
    case 13:
      return Action{
          .kind = ActionKind::kRelease,
          .text = (words[1] % 4U) == 0U ? "missing"
                                        : MixedCase(model.ExistingSavepoint(words[2]), words[3]),
      };
    case 14:
      return Action{
          .kind = ActionKind::kRollbackTo,
          .text = (words[1] % 4U) == 0U ? "missing"
                                        : MixedCase(model.ExistingSavepoint(words[2]), words[3]),
      };
    case 15:
      return Action{.kind = ActionKind::kSelectSnapshot, .text = {}};
    case 16:
      return model.autocommit() ? Action{.kind = ActionKind::kReopen, .text = {}}
                                : Action{.kind = ActionKind::kSelectSnapshot, .text = {}};
    case 17:
      return Action{.kind = ActionKind::kCreateTempNoOp, .text = {}};
    default:
      break;
  }
  throw std::runtime_error{"write model generator selected an invalid action"};
}

[[nodiscard]] WriteStatement PrepareOne(WriteSession& session, std::string_view sql) {
  WritePrepareOutput prepared = TakeValue(session.Prepare(Utf8View{sql}));
  if (!prepared.statement.has_value() || prepared.next_offset.value() != sql.size()) {
    throw std::runtime_error{"write model failed to prepare exactly one statement"};
  }
  return std::move(*prepared.statement);
}

[[nodiscard]] EngineOutcome ExecuteSql(WriteSession& session, std::string_view sql,
                                       std::vector<SqlValue> bindings = {}) {
  auto prepared = session.Prepare(Utf8View{sql});
  if (!prepared.has_value()) {
    return EngineOutcome{
        .error = prepared.error().code(),
        .finalize_error = std::nullopt,
    };
  }
  if (!prepared->statement.has_value() || prepared->next_offset.value() != sql.size()) {
    throw std::runtime_error{"write model SQL did not produce exactly one statement"};
  }
  WriteStatement statement = std::move(*prepared->statement);
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    RequireStatus(statement.Bind(index + 1U, bindings[index]));
  }

  EngineOutcome outcome;
  while (true) {
    const Result<WriteStep> stepped = statement.Step();
    if (!stepped.has_value()) {
      outcome.error = stepped.error().code();
      break;
    }
    if (*stepped == WriteStep::kDone) {
      break;
    }
  }
  const Status finalized = statement.Finalize();
  if (!finalized.has_value()) {
    outcome.finalize_error = finalized.error().code();
  }
  return outcome;
}

[[nodiscard]] ByteBuffer BlobFor(const Action& action) {
  const std::vector<std::byte> payload = PayloadFor(action.first, action.second);
  return ByteBuffer::CopyOf(payload);
}

[[nodiscard]] EngineOutcome ExecuteAction(WriteSession& session, const Action& action) {
  switch (action.kind) {
    case ActionKind::kCreateMain:
      return ExecuteSql(session,
                        "CREATE TABLE Items("
                        "id INTEGER PRIMARY KEY,"
                        "Name TEXT NOT NULL DEFAULT 'seed',"
                        "Score REAL,"
                        "Payload BLOB"
                        ")");
    case ActionKind::kCreateMainNoOp:
      return ExecuteSql(session, "CREATE TABLE IF NOT EXISTS Items(ignored TEXT)");
    case ActionKind::kCreateTemp:
      return ExecuteSql(session, "CREATE TABLE Temp(id INTEGER PRIMARY KEY)");
    case ActionKind::kCreateTempNoOp:
      return ExecuteSql(session, "CREATE TABLE IF NOT EXISTS Temp(id INTEGER PRIMARY KEY)");
    case ActionKind::kInsertExplicit: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.first));
      bindings.push_back(SqlValue::Text(action.text));
      bindings.push_back(SqlValue::Integer(action.second));
      bindings.push_back(SqlValue::Blob(BlobFor(action)));
      return ExecuteSql(session, "INSERT INTO Items(id,Name,Score,Payload) VALUES(?1,?2,?3,?4)",
                        std::move(bindings));
    }
    case ActionKind::kInsertDefault:
      return ExecuteSql(session, "INSERT INTO Items DEFAULT VALUES");
    case ActionKind::kInsertDuplicate: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.first));
      bindings.push_back(SqlValue::Text(action.text));
      bindings.push_back(SqlValue::Integer(action.second));
      bindings.push_back(SqlValue::Blob(BlobFor(action)));
      return ExecuteSql(session, "INSERT INTO Items(id,Name,Score,Payload) VALUES(?1,?2,?3,?4)",
                        std::move(bindings));
    }
    case ActionKind::kInsertNotNull: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.first));
      bindings.emplace_back();
      return ExecuteSql(session, "INSERT INTO Items(id,Name) VALUES(?1,?2)", std::move(bindings));
    }
    case ActionKind::kInsertFractional: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Real(static_cast<double>(action.first) + 0.5));
      return ExecuteSql(session, "INSERT INTO Items(id,Name) VALUES(?1,'fractional')",
                        std::move(bindings));
    }
    case ActionKind::kUpdateExact: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Text(action.text));
      bindings.push_back(SqlValue::Integer(action.second));
      bindings.push_back(SqlValue::Integer(action.first));
      return ExecuteSql(session, "UPDATE Items SET Name=?1,Score=?2 WHERE id=?3",
                        std::move(bindings));
    }
    case ActionKind::kUpdateScan: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Text(action.text));
      bindings.push_back(SqlValue::Integer(action.first));
      return ExecuteSql(session, "UPDATE Items SET Name=Name||?1 WHERE id>=?2",
                        std::move(bindings));
    }
    case ActionKind::kMoveRowId: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.second));
      bindings.push_back(SqlValue::Integer(action.first));
      return ExecuteSql(session, "UPDATE Items SET id=?1 WHERE id=?2", std::move(bindings));
    }
    case ActionKind::kDeleteExact: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.first));
      return ExecuteSql(session, "DELETE FROM Items WHERE id=?1", std::move(bindings));
    }
    case ActionKind::kDeleteScan: {
      std::vector<SqlValue> bindings;
      bindings.push_back(SqlValue::Integer(action.first));
      return ExecuteSql(session, "DELETE FROM Items WHERE id>=?1", std::move(bindings));
    }
    case ActionKind::kBegin:
      return ExecuteSql(session, "BEGIN");
    case ActionKind::kCommit:
      return ExecuteSql(session, "COMMIT");
    case ActionKind::kRollback:
      return ExecuteSql(session, "ROLLBACK");
    case ActionKind::kSavepoint:
      return ExecuteSql(session, "SAVEPOINT " + action.text);
    case ActionKind::kRelease:
      return ExecuteSql(session, "RELEASE " + action.text);
    case ActionKind::kRollbackTo:
      return ExecuteSql(session, "ROLLBACK TO " + action.text);
    case ActionKind::kSelectSnapshot:
      return ExecuteSql(session, "SELECT id,Name,Score,Payload FROM Items");
    case ActionKind::kReopen:
      return {};
  }
  throw std::runtime_error{"write model cannot execute an invalid action"};
}

[[nodiscard]] std::vector<std::vector<SqlValue>> QueryRows(WriteSession& session,
                                                           std::string_view sql) {
  WriteStatement statement = PrepareOne(session, sql);
  std::vector<std::vector<SqlValue>> rows;
  while (true) {
    const WriteStep stepped = TakeValue(statement.Step());
    if (stepped == WriteStep::kDone) {
      RequireStatus(statement.Finalize());
      return rows;
    }
    std::vector<SqlValue> row;
    row.reserve(statement.row().size());
    for (const SqlValue& value : statement.row()) {
      row.push_back(value.Clone());
    }
    rows.push_back(std::move(row));
  }
}

[[nodiscard]] std::string Trace(std::size_t index, const GeneratedAction& generated,
                                const std::vector<std::string>& history) {
  std::ostringstream output;
  output << "generator=" << kGeneratorVersion << " seed=0x" << std::hex << kModelSeed << std::dec
         << " action=" << index << " words=[";
  for (std::size_t word = 0; word < generated.words.size(); ++word) {
    if (word != 0U) {
      output << ",";
    }
    output << "0x" << std::hex << generated.words[word] << std::dec;
  }
  output << "] selected=" << DescribeAction(generated.action) << " history=[";
  const std::size_t begin = history.size() > kHistoryLimit ? history.size() - kHistoryLimit : 0U;
  for (std::size_t item = begin; item < history.size(); ++item) {
    if (item != begin) {
      output << ",";
    }
    output << history[item];
  }
  output << "]";
  return output.str();
}

[[noreturn]] void Fail(std::string_view trace, std::string_view detail) {
  throw std::runtime_error{std::string{trace} + ": " + std::string{detail}};
}

void CompareOutcome(const ExpectedOutcome& expected, const EngineOutcome& actual,
                    std::string_view trace) {
  if (expected.error != actual.error) {
    Fail(trace, "primary action result differs");
  }
  const std::optional<ErrorCode> expected_finalize =
      expected.finalize_repeats_error ? expected.error : std::nullopt;
  if (expected_finalize != actual.finalize_error) {
    Fail(trace, "finalize result differs");
  }
}

void CompareCatalogVisibility(WriteSession& session, std::string_view name, bool expected,
                              std::string_view trace) {
  const auto rows =
      QueryRows(session, "SELECT name FROM sqlite_schema WHERE name='" + std::string{name} + "'");
  if ((rows.size() == 1U) != expected) {
    Fail(trace, "catalog visibility differs");
  }
  if (expected) {
    if (rows[0].size() != 1U || rows[0][0].type() != SqlValueType::kText ||
        TakeOptional(rows[0][0].text_value(), "catalog name is not text").bytes() != name) {
      Fail(trace, "catalog row differs");
    }
  }
}

void CompareRows(WriteSession& session, const DatabaseModel& expected, std::string_view trace) {
  if (!expected.items_visible) {
    return;
  }
  const auto rows = QueryRows(session, "SELECT id,Name,Score,Payload FROM Items");
  if (rows.size() != expected.rows.size()) {
    Fail(trace, "row count differs");
  }
  auto expected_row = expected.rows.begin();
  for (std::size_t index = 0; index < rows.size(); ++index, ++expected_row) {
    const std::vector<SqlValue>& row = rows[index];
    if (row.size() != 4U || row[0].type() != SqlValueType::kInteger ||
        TakeOptional(row[0].integer_value(), "rowid is not integer") != expected_row->first ||
        row[1].type() != SqlValueType::kText ||
        TakeOptional(row[1].text_value(), "name is not text").bytes() !=
            expected_row->second.name) {
      Fail(trace, "row identity or name differs");
    }

    if (!expected_row->second.score.has_value()) {
      if (row[2].type() != SqlValueType::kNull) {
        Fail(trace, "NULL score differs");
      }
    } else if (row[2].type() != SqlValueType::kReal ||
               TakeOptional(row[2].real_value(), "score is not real") !=
                   expected_row->second.score.value()) {
      Fail(trace, "REAL score differs");
    }

    if (!expected_row->second.payload.has_value()) {
      if (row[3].type() != SqlValueType::kNull) {
        Fail(trace, "NULL payload differs");
      }
    } else {
      if (row[3].type() != SqlValueType::kBlob) {
        Fail(trace, "payload storage class differs");
      }
      const ByteView actual = TakeOptional(row[3].blob_value(), "payload is not blob");
      if (!std::ranges::equal(actual, expected_row->second.payload.value())) {
        Fail(trace, "payload bytes differ");
      }
    }
  }
}

void CompareState(WriteSession& session, const ReferenceModel& model, std::string_view trace) {
  CompareCatalogVisibility(session, "Items", model.database().items_visible, trace);
  CompareCatalogVisibility(session, "Temp", model.database().temp_visible, trace);
  CompareRows(session, model.database(), trace);
  if (session.changes() != model.changes()) {
    Fail(trace, "changes differs");
  }
  if (session.last_insert_rowid() != model.last_insert_rowid()) {
    Fail(trace, "last_insert_rowid differs");
  }
  if (session.autocommit() != model.autocommit()) {
    Fail(trace, "autocommit differs");
  }
}

void RunDeterministicWriteSessionModel() {
  const TemporaryDirectory directory;
  const std::filesystem::path database_path = directory.DatabasePath();
  WriteSession session = TakeValue(WriteSession::Open(database_path.string()));
  ReferenceModel model;
  SplitMix64 generator{kModelSeed};
  std::vector<GeneratedAction> actions = FixedPrelude();
  actions.reserve(kModelActions);
  for (const GeneratedAction& generated : actions) {
    if (generated.action.kind == ActionKind::kReopen) {
      model.ResetConnectionState();
    } else {
      static_cast<void>(model.Apply(generated.action));
    }
  }
  while (actions.size() < kModelActions) {
    const std::array<std::uint64_t, 4> words{
        generator.Next(),
        generator.Next(),
        generator.Next(),
        generator.Next(),
    };
    actions.push_back(GeneratedAction{
        .action = GenerateAction(model, words),
        .words = words,
    });
    const Action& action = actions.back().action;
    if (action.kind == ActionKind::kReopen) {
      model.ResetConnectionState();
    } else {
      static_cast<void>(model.Apply(action));
    }
  }

  model = ReferenceModel{};
  std::vector<std::string> history;
  history.reserve(actions.size());
  std::array<bool, static_cast<std::size_t>(ActionKind::kReopen) + 1U> covered{};
  for (std::size_t index = 0; index < actions.size(); ++index) {
    const GeneratedAction& generated = actions[index];
    covered[static_cast<std::size_t>(generated.action.kind)] = true;
    const std::string trace = Trace(index, generated, history);
    ExpectedOutcome expected;
    EngineOutcome actual;
    if (generated.action.kind == ActionKind::kReopen) {
      if (!model.autocommit()) {
        Fail(trace, "generator attempted reopen during a transaction");
      }
      session = TakeValue(WriteSession::Open(database_path.string()));
      model.ResetConnectionState();
    } else {
      expected = model.Apply(generated.action);
      actual = ExecuteAction(session, generated.action);
      CompareOutcome(expected, actual, trace);
    }
    CompareState(session, model, trace);
    if (model.database().rows.size() > kMaximumRows ||
        model.savepoint_count() > kMaximumSavepoints) {
      Fail(trace, "model resource bound was exceeded");
    }
    history.push_back(DescribeAction(generated.action));
  }

  if (!model.autocommit()) {
    const Action rollback{.kind = ActionKind::kRollback, .text = {}};
    const ExpectedOutcome expected = model.Apply(rollback);
    const EngineOutcome actual = ExecuteAction(session, rollback);
    CompareOutcome(expected, actual, "final model rollback");
    CompareState(session, model, "final model rollback");
  }
  for (std::size_t kind = 0; kind < covered.size(); ++kind) {
    if (!covered[kind]) {
      throw std::runtime_error{"write model did not cover action kind " + std::to_string(kind)};
    }
  }
}

TEST(WriteSessionModel, MatchesDeterministicMixedActions) {
  EXPECT_NO_THROW(RunDeterministicWriteSessionModel());
}

}  // namespace
}  // namespace modern_sqlite
