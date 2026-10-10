#ifndef MODERN_SQLITE_BYTECODE_PROGRAM_HPP_
#define MODERN_SQLITE_BYTECODE_PROGRAM_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"

namespace modern_sqlite {

template <typename Tag>
class BytecodeId final {
 public:
  constexpr explicit BytecodeId(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const BytecodeId&) const noexcept = default;

 private:
  std::uint32_t value_;
};

struct InstructionAddressTag;
struct RegisterIdTag;
struct CursorIdTag;
struct WriteCursorIdTag;
struct SorterIdTag;
struct TopNIdTag;
struct RelationIdTag;
struct RecordComparisonIdTag;
struct ParameterIdTag;
struct ConstantIdTag;
struct SymbolIdTag;
struct RootPageNumberTag;
struct CursorFieldIdTag;

using InstructionAddress = BytecodeId<InstructionAddressTag>;
using RegisterId = BytecodeId<RegisterIdTag>;
using CursorId = BytecodeId<CursorIdTag>;
using WriteCursorId = BytecodeId<WriteCursorIdTag>;
using SorterId = BytecodeId<SorterIdTag>;
using TopNId = BytecodeId<TopNIdTag>;
using RelationId = BytecodeId<RelationIdTag>;
using RecordComparisonId = BytecodeId<RecordComparisonIdTag>;
using ParameterId = BytecodeId<ParameterIdTag>;
using ConstantId = BytecodeId<ConstantIdTag>;
using SymbolId = BytecodeId<SymbolIdTag>;
using RootPageNumber = BytecodeId<RootPageNumberTag>;
using CursorFieldId = BytecodeId<CursorFieldIdTag>;

enum class CursorStorageKind : std::uint8_t {
  kRowIdTable,
  kIndex,
};

enum class WriteCursorStorageKind : std::uint8_t {
  kRowIdTable,
  kIndex,
};

enum class ProgramStatementKind : std::uint8_t {
  kSelect,
  kInsert,
  kUpdate,
  kDelete,
  kCreateTable,
  kCreateIndex,
  kAnalyze,
};

enum class ProgramTransactionAccess : std::uint8_t {
  kRead,
  kWrite,
};

enum class ProgramRollbackMode : std::uint8_t {
  kTransaction,
  kStatement,
};

enum class CursorFieldSourceKind : std::uint8_t {
  kRecordField,
  kRowId,
};

enum class MissingFieldValueKind : std::uint8_t {
  kNull,
  kConstant,
  kUnsupported,
};

enum class BytecodeSortOrder : std::uint8_t {
  kAscending,
  kDescending,
};

enum class BytecodeNullPlacement : std::uint8_t {
  kFirst,
  kLast,
};

enum class UnaryOperation : std::uint8_t {
  kNegate,
  kBitwiseNot,
  kLogicalNot,
};

enum class BinaryOperation : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kRemainder,
  kConcatenate,
  kBitwiseAnd,
  kBitwiseOr,
  kShiftLeft,
  kShiftRight,
  kLogicalAnd,
  kLogicalOr,
};

enum class JumpCondition : std::uint8_t {
  kIfTrue,
  kIfFalse,
  kIfNull,
  kIfNotNull,
};

enum class Stat1ClearScope : std::uint8_t {
  kDatabase,
  kTable,
  kIndex,
};

enum class RelationInsertMode : std::uint8_t {
  kKeepExisting,
  kReplaceExisting,
};

struct CursorFieldSource {
  CursorFieldSourceKind kind;
  std::uint32_t record_field;
  MissingFieldValueKind missing_value_kind = MissingFieldValueKind::kNull;
  std::optional<ConstantId> missing_value{};

  constexpr auto operator<=>(const CursorFieldSource&) const noexcept = default;
};

struct IndexColumnMetadata {
  SymbolId collation;
  BytecodeSortOrder order;

  constexpr auto operator<=>(const IndexColumnMetadata&) const noexcept = default;
};

struct OrderingColumnMetadata {
  SymbolId collation;
  BytecodeSortOrder order = BytecodeSortOrder::kAscending;
  BytecodeNullPlacement null_placement = BytecodeNullPlacement::kFirst;

  constexpr auto operator<=>(const OrderingColumnMetadata&) const noexcept = default;
};

struct OrderingRecordDescriptor {
  std::uint32_t field_count = 0;
  std::uint32_t key_field_count = 0;
  std::vector<OrderingColumnMetadata> key_columns;
};

struct ReadCursorDescriptor {
  RootPageNumber root_page;
  CursorStorageKind storage;
  std::uint32_t record_field_count;
  std::vector<CursorFieldSource> fields;
  std::vector<IndexColumnMetadata> index_columns;
};

struct WriteColumnDescriptor {
  TypeAffinity affinity = TypeAffinity::kNone;
  bool not_null = false;
  bool rowid_alias = false;
  std::optional<ConstantId> default_value{};
};

struct WriteCursorDescriptor {
  RootPageNumber root_page;
  std::vector<WriteColumnDescriptor> columns;
  std::optional<std::uint32_t> rowid_alias{};
  std::vector<IndexColumnMetadata> index_columns;
  std::uint32_t key_term_count = 0;
  bool unique = false;
  bool unique_not_null = false;
  bool pending_root = false;
  WriteCursorStorageKind storage = WriteCursorStorageKind::kRowIdTable;
};

struct MutationResultMetadata {
  bool publishes_changes = false;
  bool publishes_last_insert_rowid = false;

  constexpr auto operator<=>(const MutationResultMetadata&) const noexcept = default;
};

struct ResultColumnMetadata {
  std::string name;
  std::optional<std::string> declared_type;
  TypeAffinity affinity;
};

struct SchemaVersionRequirement {
  std::uint32_t schema_cookie = 0;
  std::uint64_t generation = 0;

  constexpr auto operator<=>(const SchemaVersionRequirement&) const noexcept = default;
};

struct ProgramResourceCounts {
  std::uint32_t registers = 0;
  std::uint32_t parameters = 0;

  constexpr auto operator<=>(const ProgramResourceCounts&) const noexcept = default;
};

struct HaltInstruction {};

struct LoadConstantInstruction {
  ConstantId constant;
  RegisterId output;
};

struct LoadParameterInstruction {
  ParameterId parameter;
  RegisterId output;
};

struct CopyInstruction {
  RegisterId input;
  RegisterId output;
};

struct UnaryInstruction {
  UnaryOperation operation;
  RegisterId input;
  RegisterId output;
};

struct BinaryInstruction {
  BinaryOperation operation;
  RegisterId left;
  RegisterId right;
  RegisterId output;
};

struct ApplyAffinityInstruction {
  RegisterId input;
  TypeAffinity affinity;
  RegisterId output;
};

struct MustBeIntegerInstruction {
  RegisterId input;
  RegisterId output;
};

struct RealAffinityInstruction {
  RegisterId input;
  RegisterId output;
};

struct RealStorageAffinityInstruction {
  RegisterId input;
  RegisterId output;
};

struct CastInstruction {
  RegisterId input;
  CastTarget target;
  RegisterId output;
};

struct OpenReadCursorInstruction {
  CursorId cursor;
};

struct OpenMutationCursorInstruction {
  CursorId cursor;
};

struct OpenWriteCursorInstruction {
  WriteCursorId cursor;
};

struct CloseCursorInstruction {
  CursorId cursor;
};

struct CloseWriteCursorInstruction {
  WriteCursorId cursor;
};

struct RewindInstruction {
  CursorId cursor;
  InstructionAddress empty_target;
};

struct NextInstruction {
  CursorId cursor;
  InstructionAddress next_target;
};

struct ClearRowIdListInstruction {};

struct AppendRowIdListInstruction {
  RegisterId input;
};

struct RewindRowIdListInstruction {
  RegisterId output;
  InstructionAddress empty_target;
};

struct NextRowIdListInstruction {
  RegisterId output;
  InstructionAddress next_target;
};

enum class RowIdSeekMode : std::uint8_t {
  kEqual,
  kGreater,
};

struct SeekRowIdInstruction {
  CursorId cursor;
  RegisterId key;
  InstructionAddress missing_target;
  RowIdSeekMode mode = RowIdSeekMode::kEqual;
};

struct SeekTableRowIdInstruction {
  CursorId cursor;
  RegisterId key;
};

enum class IndexSeekMode : std::uint8_t {
  kEqual,
  kGreaterOrEqual,
  kGreater,
  kLessOrEqual,
  kLess,
};

struct SeekIndexInstruction {
  CursorId cursor;
  RegisterId first_key;
  std::uint32_t key_count;
  InstructionAddress missing_target;
  IndexSeekMode mode = IndexSeekMode::kEqual;
};

enum class IndexRangeEndMode : std::uint8_t {
  kInclusive,
  kExclusive,
};

struct CheckIndexRangeInstruction {
  CursorId cursor;
  RegisterId first_key;
  std::uint32_t key_count;
  InstructionAddress end_target;
  IndexRangeEndMode mode = IndexRangeEndMode::kInclusive;
};

struct ReadFieldInstruction {
  CursorId cursor;
  CursorFieldId field;
  RegisterId output;
};

struct ReadRowIdInstruction {
  CursorId cursor;
  RegisterId output;
};

struct ResolveInsertRowIdInstruction {
  WriteCursorId cursor;
  RegisterId input;
  RegisterId output;
};

struct CheckInsertRowIdInstruction {
  WriteCursorId cursor;
  RegisterId rowid;
};

struct CheckUpdateRowIdInstruction {
  WriteCursorId cursor;
  RegisterId old_rowid;
  RegisterId new_rowid;
};

struct BuildTableRecordInstruction {
  WriteCursorId cursor;
  RegisterId first_value;
  std::uint32_t value_count;
  RegisterId output;
};

struct CheckUniqueIndexInstruction {
  WriteCursorId cursor;
  RegisterId first_key;
  std::uint32_t key_count;
  std::optional<RegisterId> ignored_rowid{};
};

struct InsertIndexInstruction {
  WriteCursorId cursor;
  RegisterId first_value;
  std::uint32_t value_count;
};

struct DeleteIndexInstruction {
  WriteCursorId cursor;
  RegisterId first_value;
  std::uint32_t value_count;
};

struct InsertTableInstruction {
  WriteCursorId cursor;
  RegisterId rowid;
  RegisterId record;
};

struct DeleteTableInstruction {
  WriteCursorId cursor;
  RegisterId rowid;
};

struct DeleteCurrentTableInstruction {
  CursorId cursor;
  InstructionAddress exhausted_target;
};

struct UpdateCurrentTableInstruction {
  CursorId cursor;
  RegisterId record;
};

struct UpdateTableInstruction {
  WriteCursorId cursor;
  RegisterId old_rowid;
  RegisterId new_rowid;
  RegisterId record;
};

struct EnsureDatabaseInitializedInstruction {};

struct CreateTableRootInstruction {
  std::optional<WriteCursorId> cursor{};
  RegisterId output;
};

struct CreateIndexRootInstruction {
  WriteCursorId cursor;
  RegisterId output;
};

struct ClearStat1Instruction {
  WriteCursorId cursor;
  Stat1ClearScope scope = Stat1ClearScope::kDatabase;
  std::optional<RegisterId> name{};
};

struct ComputeIndexStat1Instruction {
  CursorId cursor;
  std::uint32_t key_term_count;
  bool emit_empty = false;
  RegisterId output;
};

struct ComputeTableStat1Instruction {
  CursorId cursor;
  RegisterId output;
};

struct IncrementSchemaCookieInstruction {
  RegisterId output;
};

struct CompareInstruction {
  SqlComparison comparison;
  TypeAffinity affinity;
  SymbolId collation;
  RegisterId left;
  RegisterId right;
  RegisterId output;
};

struct CallScalarInstruction {
  SymbolId function;
  SymbolId collation;
  RegisterId first_argument;
  std::uint32_t argument_count;
  RegisterId output;
};

struct JumpInstruction {
  InstructionAddress target;
};

struct JumpIfInstruction {
  JumpCondition condition;
  RegisterId input;
  InstructionAddress target;
};

struct ResultRowInstruction {
  RegisterId first;
  std::uint32_t count;
};

struct OpenSorterInstruction {
  SorterId sorter;
};

struct InsertSorterInstruction {
  SorterId sorter;
  RegisterId first_value;
  std::uint32_t value_count;
};

struct RewindSorterInstruction {
  SorterId sorter;
  InstructionAddress empty_target;
};

struct ReadSorterFieldInstruction {
  SorterId sorter;
  std::uint32_t field;
  RegisterId output;
};

struct NextSorterInstruction {
  SorterId sorter;
  InstructionAddress next_target;
};

struct ResetSorterInstruction {
  SorterId sorter;
};

struct CloseSorterInstruction {
  SorterId sorter;
};

struct OpenTopNInstruction {
  TopNId top_n;
  RegisterId bound;
};

struct CheckTopNInstruction {
  TopNId top_n;
  RegisterId first_key;
  std::uint32_t key_count;
  InstructionAddress rejected_target;
};

struct InsertTopNInstruction {
  TopNId top_n;
  RegisterId first_value;
  std::uint32_t value_count;
};

struct RewindTopNInstruction {
  TopNId top_n;
  InstructionAddress empty_target;
};

struct ReadTopNFieldInstruction {
  TopNId top_n;
  std::uint32_t field;
  RegisterId output;
};

struct NextTopNInstruction {
  TopNId top_n;
  InstructionAddress next_target;
};

struct ResetTopNInstruction {
  TopNId top_n;
};

struct CloseTopNInstruction {
  TopNId top_n;
};

struct OpenRelationInstruction {
  RelationId relation;
};

struct InsertRelationInstruction {
  RelationId relation;
  RegisterId first_value;
  std::uint32_t value_count;
  RelationInsertMode mode = RelationInsertMode::kKeepExisting;
  InstructionAddress duplicate_target;
};

struct ContainsRelationInstruction {
  RelationId relation;
  RegisterId first_key;
  std::uint32_t key_count;
  InstructionAddress found_target;
};

struct DeleteRelationInstruction {
  RelationId relation;
  RegisterId first_key;
  std::uint32_t key_count;
};

struct RewindRelationInstruction {
  RelationId relation;
  InstructionAddress empty_target;
};

struct ReadRelationFieldInstruction {
  RelationId relation;
  std::uint32_t field;
  RegisterId output;
};

struct NextRelationInstruction {
  RelationId relation;
  InstructionAddress next_target;
};

struct ResetRelationInstruction {
  RelationId relation;
};

struct CloseRelationInstruction {
  RelationId relation;
};

struct CompareRecordsInstruction {
  RecordComparisonId comparison;
  RegisterId left_first;
  RegisterId right_first;
  RegisterId output;
};

using Instruction = std::variant<
    HaltInstruction, LoadConstantInstruction, LoadParameterInstruction, CopyInstruction,
    UnaryInstruction, BinaryInstruction, ApplyAffinityInstruction, MustBeIntegerInstruction,
    RealAffinityInstruction, RealStorageAffinityInstruction, CastInstruction,
    OpenReadCursorInstruction, OpenMutationCursorInstruction, OpenWriteCursorInstruction,
    CloseCursorInstruction, CloseWriteCursorInstruction, RewindInstruction, NextInstruction,
    ClearRowIdListInstruction, AppendRowIdListInstruction, RewindRowIdListInstruction,
    NextRowIdListInstruction, SeekRowIdInstruction, SeekTableRowIdInstruction, SeekIndexInstruction,
    CheckIndexRangeInstruction, ReadFieldInstruction, ReadRowIdInstruction,
    ResolveInsertRowIdInstruction, CheckInsertRowIdInstruction, CheckUpdateRowIdInstruction,
    BuildTableRecordInstruction, CheckUniqueIndexInstruction, InsertIndexInstruction,
    DeleteIndexInstruction, InsertTableInstruction, DeleteTableInstruction,
    DeleteCurrentTableInstruction, UpdateCurrentTableInstruction, UpdateTableInstruction,
    EnsureDatabaseInitializedInstruction, CreateTableRootInstruction, CreateIndexRootInstruction,
    ClearStat1Instruction, ComputeIndexStat1Instruction, ComputeTableStat1Instruction,
    IncrementSchemaCookieInstruction, CompareInstruction, CallScalarInstruction, JumpInstruction,
    JumpIfInstruction, ResultRowInstruction, OpenSorterInstruction, InsertSorterInstruction,
    RewindSorterInstruction, ReadSorterFieldInstruction, NextSorterInstruction,
    ResetSorterInstruction, CloseSorterInstruction, OpenTopNInstruction, CheckTopNInstruction,
    InsertTopNInstruction, RewindTopNInstruction, ReadTopNFieldInstruction, NextTopNInstruction,
    ResetTopNInstruction, CloseTopNInstruction, OpenRelationInstruction, InsertRelationInstruction,
    ContainsRelationInstruction, DeleteRelationInstruction, RewindRelationInstruction,
    ReadRelationFieldInstruction, NextRelationInstruction, ResetRelationInstruction,
    CloseRelationInstruction, CompareRecordsInstruction>;

static_assert(sizeof(Instruction) <= 32);

enum class InstructionKind : std::uint8_t {
  kHalt,
  kLoadConstant,
  kLoadParameter,
  kCopy,
  kUnary,
  kBinary,
  kApplyAffinity,
  kMustBeInteger,
  kRealAffinity,
  kRealStorageAffinity,
  kCast,
  kOpenRead,
  kOpenMutation,
  kOpenWrite,
  kClose,
  kCloseWrite,
  kRewind,
  kNext,
  kClearRowIdList,
  kAppendRowIdList,
  kRewindRowIdList,
  kNextRowIdList,
  kSeekRowId,
  kSeekTableRowId,
  kSeekIndex,
  kCheckIndexRange,
  kReadField,
  kReadRowId,
  kResolveInsertRowId,
  kCheckInsertRowId,
  kCheckUpdateRowId,
  kBuildTableRecord,
  kCheckUniqueIndex,
  kInsertIndex,
  kDeleteIndex,
  kInsertTable,
  kDeleteTable,
  kDeleteCurrentTable,
  kUpdateCurrentTable,
  kUpdateTable,
  kEnsureDatabaseInitialized,
  kCreateTableRoot,
  kCreateIndexRoot,
  kClearStat1,
  kComputeIndexStat1,
  kComputeTableStat1,
  kIncrementSchemaCookie,
  kCompare,
  kCallScalar,
  kJump,
  kJumpIf,
  kResultRow,
  kOpenSorter,
  kInsertSorter,
  kRewindSorter,
  kReadSorterField,
  kNextSorter,
  kResetSorter,
  kCloseSorter,
  kOpenTopN,
  kCheckTopN,
  kInsertTopN,
  kRewindTopN,
  kReadTopNField,
  kNextTopN,
  kResetTopN,
  kCloseTopN,
  kOpenRelation,
  kInsertRelation,
  kContainsRelation,
  kDeleteRelation,
  kRewindRelation,
  kReadRelationField,
  kNextRelation,
  kResetRelation,
  kCloseRelation,
  kCompareRecords,
};

[[nodiscard]] InstructionKind InstructionKindOf(const Instruction& instruction) noexcept;
[[nodiscard]] std::string_view InstructionKindName(InstructionKind kind) noexcept;

struct ProgramInput {
  SchemaVersionRequirement schema_version;
  ProgramStatementKind statement_kind = ProgramStatementKind::kSelect;
  ProgramTransactionAccess transaction_access = ProgramTransactionAccess::kRead;
  ProgramRollbackMode rollback_mode = ProgramRollbackMode::kTransaction;
  MutationResultMetadata mutation_result{};
  std::uint32_t register_count = 0;
  std::uint32_t parameter_count = 0;
  bool requires_database_snapshot = false;
  std::vector<SqlValue> constants;
  std::vector<std::string> symbols;
  std::vector<ReadCursorDescriptor> cursors;
  std::vector<WriteCursorDescriptor> write_cursors;
  std::vector<OrderingRecordDescriptor> sorters;
  std::vector<OrderingRecordDescriptor> top_ns;
  std::vector<OrderingRecordDescriptor> relations;
  std::vector<OrderingRecordDescriptor> record_comparisons;
  std::vector<ResultColumnMetadata> result_columns;
  std::vector<Instruction> instructions;
};

struct ProgramLimits {
  std::size_t maximum_instructions = 250'000'000;
  std::size_t maximum_registers = 1'000'000;
  std::size_t maximum_cursors = 100'000;
  std::size_t maximum_sorters = 100'000;
  std::size_t maximum_top_ns = 100'000;
  std::size_t maximum_relations = 100'000;
  std::size_t maximum_record_comparisons = 100'000;
  std::size_t maximum_parameters = 32'766;
  std::size_t maximum_constants = 1'000'000;
  std::size_t maximum_symbols = 1'000'000;
  std::size_t maximum_result_columns = 2'000;
  std::size_t maximum_labels = 10'000'000;
  std::size_t maximum_owned_bytes = 1'000'000'000;
  std::size_t maximum_analysis_words = 16'000'000;
  std::size_t maximum_analysis_edge_words = 250'000'000;
};

enum class ProgramErrorCode : std::uint8_t {
  kEmptyProgram,
  kInstructionLimitExceeded,
  kRegisterLimitExceeded,
  kCursorLimitExceeded,
  kSorterLimitExceeded,
  kTopNLimitExceeded,
  kRelationLimitExceeded,
  kRecordComparisonLimitExceeded,
  kParameterLimitExceeded,
  kConstantLimitExceeded,
  kSymbolLimitExceeded,
  kResultColumnLimitExceeded,
  kLabelLimitExceeded,
  kOwnedBytesLimitExceeded,
  kAnalysisLimitExceeded,
  kAnalysisWorkLimitExceeded,
  kInvalidRootPage,
  kInvalidCursorDescriptor,
  kInvalidExecutionMetadata,
  kInvalidRegister,
  kInvalidRegisterRange,
  kInvalidParameter,
  kInvalidConstant,
  kInvalidSymbol,
  kInvalidCursor,
  kInvalidSorter,
  kInvalidTopN,
  kInvalidRelation,
  kInvalidRecordComparison,
  kInvalidOrderingDescriptor,
  kInvalidField,
  kInvalidBranchTarget,
  kInvalidEnumValue,
  kInvalidText,
  kResultShapeMismatch,
  kBranchRequiresLabel,
  kInvalidBuilder,
  kForeignLabel,
  kInvalidLabel,
  kLabelAlreadyBound,
  kUnboundLabel,
  kLabelTargetOutOfRange,
  kUninitializedRegister,
  kCursorAlreadyOpen,
  kCursorAlreadyClosed,
  kCursorNotOpen,
  kCursorNotPositioned,
  kCursorStateConflict,
  kCapabilityAlreadyOpen,
  kCapabilityAlreadyClosed,
  kCapabilityNotOpen,
  kCapabilityNotWriting,
  kCapabilityNotPositioned,
  kCapabilityStateConflict,
  kTopNCandidatePending,
  kTopNCandidateRequired,
  kRowIdOperationRequiresRowIdTable,
  kIndexOperationRequiresIndex,
  kFallthroughPastEnd,
  kUnreachableInstruction,
};

struct ProgramError {
  static constexpr std::size_t kNoInstruction = static_cast<std::size_t>(-1);

  ProgramErrorCode code;
  std::size_t instruction = kNoInstruction;
  std::uint64_t detail = 0;

  [[nodiscard]] ErrorCode base_error_code() const noexcept;

  constexpr auto operator<=>(const ProgramError&) const noexcept = default;
};

template <typename T>
using ProgramResult = std::expected<T, ProgramError>;

struct ProgramVerificationMetrics {
  std::size_t owned_bytes = 0;
  std::size_t analysis_words = 0;
  std::size_t processed_edge_words = 0;
  std::size_t reachable_instruction_count = 0;

  constexpr auto operator<=>(const ProgramVerificationMetrics&) const noexcept = default;
};

[[nodiscard]] ProgramResult<ProgramVerificationMetrics> VerifyProgram(const ProgramInput& input,
                                                                      ProgramLimits limits = {});

class BytecodeProgram final {
 public:
  BytecodeProgram(const BytecodeProgram&) = delete;
  BytecodeProgram& operator=(const BytecodeProgram&) = delete;
  BytecodeProgram(BytecodeProgram&&) noexcept = default;
  BytecodeProgram& operator=(BytecodeProgram&&) noexcept = default;
  ~BytecodeProgram() = default;

  [[nodiscard]] static ProgramResult<BytecodeProgram> Create(const ProgramInput& input,
                                                             ProgramLimits limits = {});

  [[nodiscard]] const SchemaVersionRequirement& schema_version() const noexcept {
    return input_.schema_version;
  }
  [[nodiscard]] ProgramStatementKind statement_kind() const noexcept {
    return input_.statement_kind;
  }
  [[nodiscard]] ProgramTransactionAccess transaction_access() const noexcept {
    return input_.transaction_access;
  }
  [[nodiscard]] ProgramRollbackMode rollback_mode() const noexcept { return input_.rollback_mode; }
  [[nodiscard]] const MutationResultMetadata& mutation_result() const noexcept {
    return input_.mutation_result;
  }
  [[nodiscard]] std::uint32_t register_count() const noexcept { return input_.register_count; }
  [[nodiscard]] std::uint32_t parameter_count() const noexcept { return input_.parameter_count; }
  [[nodiscard]] bool requires_database_snapshot() const noexcept {
    return input_.requires_database_snapshot;
  }
  [[nodiscard]] std::span<const SqlValue> constants() const noexcept { return input_.constants; }
  [[nodiscard]] std::span<const std::string> symbols() const noexcept { return input_.symbols; }
  [[nodiscard]] std::span<const ReadCursorDescriptor> cursors() const noexcept {
    return input_.cursors;
  }
  [[nodiscard]] std::span<const WriteCursorDescriptor> write_cursors() const noexcept {
    return input_.write_cursors;
  }
  [[nodiscard]] std::span<const OrderingRecordDescriptor> sorters() const noexcept {
    return input_.sorters;
  }
  [[nodiscard]] std::span<const OrderingRecordDescriptor> top_ns() const noexcept {
    return input_.top_ns;
  }
  [[nodiscard]] std::span<const OrderingRecordDescriptor> relations() const noexcept {
    return input_.relations;
  }
  [[nodiscard]] std::span<const OrderingRecordDescriptor> record_comparisons() const noexcept {
    return input_.record_comparisons;
  }
  [[nodiscard]] std::span<const ResultColumnMetadata> result_columns() const noexcept {
    return input_.result_columns;
  }
  [[nodiscard]] std::span<const Instruction> instructions() const noexcept {
    return input_.instructions;
  }
  [[nodiscard]] const ProgramVerificationMetrics& verification_metrics() const noexcept {
    return metrics_;
  }

  [[nodiscard]] const SqlValue& constant(ConstantId id) const noexcept;
  [[nodiscard]] std::string_view symbol(SymbolId id) const noexcept;
  [[nodiscard]] const ReadCursorDescriptor& cursor(CursorId id) const noexcept;
  [[nodiscard]] const WriteCursorDescriptor& write_cursor(WriteCursorId id) const noexcept;
  [[nodiscard]] const OrderingRecordDescriptor& sorter(SorterId id) const noexcept;
  [[nodiscard]] const OrderingRecordDescriptor& top_n(TopNId id) const noexcept;
  [[nodiscard]] const OrderingRecordDescriptor& relation(RelationId id) const noexcept;
  [[nodiscard]] const OrderingRecordDescriptor& record_comparison(
      RecordComparisonId id) const noexcept;
  [[nodiscard]] const Instruction& instruction(InstructionAddress address) const noexcept;

 private:
  BytecodeProgram(ProgramInput input, ProgramVerificationMetrics metrics) noexcept
      : input_(std::move(input)), metrics_(metrics) {}

  ProgramInput input_;
  ProgramVerificationMetrics metrics_;
};

class Label final {
 public:
  constexpr auto operator<=>(const Label&) const noexcept = default;

 private:
  friend class ProgramBuilder;

  constexpr Label(std::uint64_t owner, std::uint32_t index) noexcept
      : owner_(owner), index_(index) {}

  std::uint64_t owner_;
  std::uint32_t index_;
};

class ProgramBuilder final {
 public:
  ProgramBuilder(const ProgramBuilder&) = delete;
  ProgramBuilder& operator=(const ProgramBuilder&) = delete;
  ProgramBuilder(ProgramBuilder&& other) noexcept;
  ProgramBuilder& operator=(ProgramBuilder&& other) noexcept;
  ~ProgramBuilder() = default;

  [[nodiscard]] static ProgramResult<ProgramBuilder> Create(SchemaVersionRequirement schema_version,
                                                            ProgramResourceCounts resources,
                                                            ProgramLimits limits = {});

  [[nodiscard]] ProgramResult<ConstantId> AddConstant(SqlValue value);
  [[nodiscard]] ProgramResult<SymbolId> AddSymbol(std::string symbol);
  [[nodiscard]] ProgramResult<CursorId> AddCursor(ReadCursorDescriptor cursor);
  [[nodiscard]] ProgramResult<WriteCursorId> AddWriteCursor(WriteCursorDescriptor cursor);
  [[nodiscard]] ProgramResult<SorterId> AddSorter(OrderingRecordDescriptor sorter);
  [[nodiscard]] ProgramResult<TopNId> AddTopN(OrderingRecordDescriptor top_n);
  [[nodiscard]] ProgramResult<RelationId> AddRelation(OrderingRecordDescriptor relation);
  [[nodiscard]] ProgramResult<RecordComparisonId> AddRecordComparison(
      OrderingRecordDescriptor comparison);
  [[nodiscard]] ProgramResult<void> SetExecutionMetadata(
      ProgramStatementKind statement_kind, ProgramTransactionAccess transaction_access,
      ProgramRollbackMode rollback_mode, MutationResultMetadata mutation_result = {});
  [[nodiscard]] ProgramResult<void> RequireDatabaseSnapshot();

  [[nodiscard]] ProgramResult<Label> CreateLabel();
  [[nodiscard]] ProgramResult<void> BindLabel(Label label);

  [[nodiscard]] ProgramResult<InstructionAddress> Append(Instruction instruction);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitRewind(CursorId cursor, Label empty_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitNext(CursorId cursor, Label next_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitRewindRowIdList(RegisterId output,
                                                                      Label empty_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitNextRowIdList(RegisterId output,
                                                                    Label next_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitRewindSorter(SorterId sorter,
                                                                   Label empty_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitNextSorter(SorterId sorter,
                                                                 Label next_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitCheckTopN(TopNId top_n, RegisterId first_key,
                                                                std::uint32_t key_count,
                                                                Label rejected_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitRewindTopN(TopNId top_n, Label empty_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitNextTopN(TopNId top_n, Label next_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitInsertRelation(RelationId relation,
                                                                     RegisterId first_value,
                                                                     std::uint32_t value_count,
                                                                     RelationInsertMode mode,
                                                                     Label duplicate_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitContainsRelation(RelationId relation,
                                                                       RegisterId first_key,
                                                                       std::uint32_t key_count,
                                                                       Label found_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitRewindRelation(RelationId relation,
                                                                     Label empty_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitNextRelation(RelationId relation,
                                                                   Label next_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitSeekRowId(
      CursorId cursor, RegisterId key, Label missing_target,
      RowIdSeekMode mode = RowIdSeekMode::kEqual);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitSeekIndex(
      CursorId cursor, RegisterId first_key, std::uint32_t key_count, Label missing_target,
      IndexSeekMode mode = IndexSeekMode::kEqual);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitCheckIndexRange(
      CursorId cursor, RegisterId first_key, std::uint32_t key_count, Label end_target,
      IndexRangeEndMode mode = IndexRangeEndMode::kInclusive);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitDeleteCurrentTable(CursorId cursor,
                                                                         Label exhausted_target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitJump(Label target);
  [[nodiscard]] ProgramResult<InstructionAddress> EmitJumpIf(RegisterId input,
                                                             JumpCondition condition, Label target);

  [[nodiscard]] ProgramResult<BytecodeProgram> Build(
      std::vector<ResultColumnMetadata> result_columns) &&;

 private:
  struct PendingRewind {
    CursorId cursor;
    Label target;
  };
  struct PendingNext {
    CursorId cursor;
    Label target;
  };
  struct PendingRewindRowIdList {
    RegisterId output;
    Label target;
  };
  struct PendingNextRowIdList {
    RegisterId output;
    Label target;
  };
  struct PendingRewindSorter {
    SorterId sorter;
    Label target;
  };
  struct PendingNextSorter {
    SorterId sorter;
    Label target;
  };
  struct PendingCheckTopN {
    TopNId top_n;
    RegisterId first_key;
    std::uint32_t key_count;
    Label target;
  };
  struct PendingRewindTopN {
    TopNId top_n;
    Label target;
  };
  struct PendingNextTopN {
    TopNId top_n;
    Label target;
  };
  struct PendingInsertRelation {
    RelationId relation;
    RegisterId first_value;
    std::uint32_t value_count;
    RelationInsertMode mode;
    Label target;
  };
  struct PendingContainsRelation {
    RelationId relation;
    RegisterId first_key;
    std::uint32_t key_count;
    Label target;
  };
  struct PendingRewindRelation {
    RelationId relation;
    Label target;
  };
  struct PendingNextRelation {
    RelationId relation;
    Label target;
  };
  struct PendingSeekRowId {
    CursorId cursor;
    RegisterId key;
    Label target;
    RowIdSeekMode mode;
  };
  struct PendingSeekIndex {
    CursorId cursor;
    RegisterId first_key;
    std::uint32_t key_count;
    Label target;
    IndexSeekMode mode;
  };
  struct PendingCheckIndexRange {
    CursorId cursor;
    RegisterId first_key;
    std::uint32_t key_count;
    Label target;
    IndexRangeEndMode mode;
  };
  struct PendingDeleteCurrentTable {
    CursorId cursor;
    Label target;
  };
  struct PendingJump {
    Label target;
  };
  struct PendingJumpIf {
    RegisterId input;
    JumpCondition condition;
    Label target;
  };

  using PendingInstruction =
      std::variant<Instruction, PendingRewind, PendingNext, PendingRewindRowIdList,
                   PendingNextRowIdList, PendingRewindSorter, PendingNextSorter, PendingCheckTopN,
                   PendingRewindTopN, PendingNextTopN, PendingInsertRelation,
                   PendingContainsRelation, PendingRewindRelation, PendingNextRelation,
                   PendingSeekRowId, PendingSeekIndex, PendingCheckIndexRange,
                   PendingDeleteCurrentTable, PendingJump, PendingJumpIf>;

  ProgramBuilder(std::uint64_t owner, SchemaVersionRequirement schema_version,
                 ProgramResourceCounts resources, ProgramLimits limits) noexcept;

  [[nodiscard]] ProgramResult<void> CheckLabel(Label label) const noexcept;
  [[nodiscard]] ProgramResult<void> CheckUsable() const noexcept;
  [[nodiscard]] ProgramResult<InstructionAddress> AppendPending(PendingInstruction instruction);
  [[nodiscard]] ProgramResult<void> ReserveOwnedBytes(std::size_t bytes) noexcept;

  std::uint64_t owner_;
  ProgramLimits limits_;
  std::size_t owned_bytes_ = 0;
  ProgramInput input_;
  std::vector<PendingInstruction> pending_;
  std::vector<std::size_t> label_addresses_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BYTECODE_PROGRAM_HPP_
