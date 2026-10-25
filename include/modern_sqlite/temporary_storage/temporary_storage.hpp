#ifndef MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_
#define MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/format/record_codec.hpp"
#include "modern_sqlite/platform/vfs.hpp"

namespace modern_sqlite {

class Pager;
class BoundedTopN;
class EphemeralRelation;
class RecordSorter;
class TemporaryStorageFactory;
class TemporaryMemoryAccount;
class TemporaryFileLease;
class StatementTemporaryStorage;
struct RecordSorterDescriptor;
struct EphemeralRelationDescriptor;

namespace temporary_storage_detail {
struct MemoryRegistration;
struct StatementState;
class StatementStorageAccess;
class RelationArenaAccess;
class BoundedTopNAccess;
}  // namespace temporary_storage_detail

enum class TemporaryStoreMode : std::uint8_t {
  kFile,
  kMemory,
};

struct TemporaryStorageOptions {
  TemporaryStoreMode mode = TemporaryStoreMode::kFile;
  std::optional<ByteCount> sorter_memory_threshold{};

  constexpr auto operator<=>(const TemporaryStorageOptions&) const noexcept = default;
};

[[nodiscard]] Result<void> ValidateTemporaryStorageOptions(TemporaryStorageOptions options);

class TemporaryStorageSpillTarget {
 public:
  virtual ~TemporaryStorageSpillTarget() = default;

  [[nodiscard]] virtual ByteCount reclaimable_bytes() const noexcept = 0;
  // The registered account and target must remain alive throughout this callback.
  [[nodiscard]] virtual Status SpillForPressure() = 0;
};

class TemporaryMemoryAccount final {
 public:
  TemporaryMemoryAccount(const TemporaryMemoryAccount&) = delete;
  TemporaryMemoryAccount& operator=(const TemporaryMemoryAccount&) = delete;
  TemporaryMemoryAccount(TemporaryMemoryAccount&&) noexcept;
  TemporaryMemoryAccount& operator=(TemporaryMemoryAccount&&) noexcept;
  ~TemporaryMemoryAccount();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] ByteCount retained_bytes() const noexcept;
  [[nodiscard]] Status Reserve(ByteCount amount);
  // Tracks a bounded file-mode drain working set that may exceed the spill budget.
  [[nodiscard]] Status ReserveWorkingSet(ByteCount amount);
  [[nodiscard]] Status Release(ByteCount amount);

 private:
  friend class RecordSorter;
  friend class StatementTemporaryStorage;
  friend struct temporary_storage_detail::StatementState;

  explicit TemporaryMemoryAccount(
      std::unique_ptr<temporary_storage_detail::MemoryRegistration> registration) noexcept;
  [[nodiscard]] Status ReserveExcludingSelf(ByteCount amount);

  std::unique_ptr<temporary_storage_detail::MemoryRegistration> registration_;
};

class TemporaryFileLease final {
 public:
  TemporaryFileLease(const TemporaryFileLease&) = delete;
  TemporaryFileLease& operator=(const TemporaryFileLease&) = delete;
  TemporaryFileLease(TemporaryFileLease&& other) noexcept;
  TemporaryFileLease& operator=(TemporaryFileLease&& other) noexcept;
  ~TemporaryFileLease();

  [[nodiscard]] bool valid() const noexcept;

 private:
  friend class StatementTemporaryStorage;
  friend struct temporary_storage_detail::StatementState;

  explicit TemporaryFileLease(temporary_storage_detail::StatementState& state) noexcept;
  void Release() noexcept;

  temporary_storage_detail::StatementState* state_;
};

class StatementTemporaryStorage final {
 public:
  StatementTemporaryStorage(const StatementTemporaryStorage&) = delete;
  StatementTemporaryStorage& operator=(const StatementTemporaryStorage&) = delete;
  StatementTemporaryStorage(StatementTemporaryStorage&&) noexcept;
  StatementTemporaryStorage& operator=(StatementTemporaryStorage&&) = delete;
  ~StatementTemporaryStorage();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool file_spill_enabled() const noexcept;
  [[nodiscard]] ByteCount memory_budget() const noexcept;
  [[nodiscard]] ByteCount retained_bytes() const noexcept;
  [[nodiscard]] ByteCount peak_retained_bytes() const noexcept;
  [[nodiscard]] ByteCount overhead_bytes() const noexcept;
  [[nodiscard]] std::size_t live_file_count() const noexcept;
  [[nodiscard]] std::size_t peak_live_file_count() const noexcept;
  [[nodiscard]] std::size_t relation_cache_capacity_pages() const noexcept;
  [[nodiscard]] std::size_t relation_cache_page_count() const noexcept;
  [[nodiscard]] std::size_t relation_cache_allocated_pages() const noexcept;
  [[nodiscard]] std::size_t relation_cache_peak_allocated_pages() const noexcept;
  [[nodiscard]] std::size_t relation_cache_pin_count() const noexcept;

  [[nodiscard]] Result<TemporaryMemoryAccount> RegisterMemoryOwner();
  // The target is borrowed and must outlive the returned account.
  [[nodiscard]] Result<TemporaryMemoryAccount> RegisterMemoryOwner(
      TemporaryStorageSpillTarget& target);
  [[nodiscard]] Result<TemporaryFileLease> AcquireFileLease();
  // The returned sorter borrows the underlying state, which must outlive it.
  [[nodiscard]] Result<RecordSorter> CreateRecordSorter(const RecordSorterDescriptor& descriptor);
  // The returned top-N relation borrows the underlying state, which must outlive it.
  [[nodiscard]] Result<BoundedTopN> CreateTopN(const RecordSorterDescriptor& descriptor,
                                               std::size_t bound);
  // The returned relation borrows the underlying state, which must outlive it.
  [[nodiscard]] Result<EphemeralRelation> CreateEphemeralRelation(
      const EphemeralRelationDescriptor& descriptor);

 private:
  friend class TemporaryStorageFactory;
  friend class temporary_storage_detail::StatementStorageAccess;

  explicit StatementTemporaryStorage(
      std::unique_ptr<temporary_storage_detail::StatementState> state) noexcept;

  std::unique_ptr<temporary_storage_detail::StatementState> state_;
};

struct RecordSorterDescriptor {
  std::size_t field_count = 0;
  std::size_t key_field_count = 0;
  // Referenced collations must outlive every sorter created from the descriptor.
  std::vector<IndexColumnOrder> key_columns{};
  RecordCodecOptions record_options{};
};

enum class EphemeralRelationPurpose : std::uint8_t {
  kMembership,
  kLookup,
  ReusableMembership,
};

struct EphemeralRelationDescriptor {
  std::size_t field_count = 0;
  std::size_t key_field_count = 0;
  // Referenced collations must outlive every relation created from the descriptor.
  std::vector<IndexColumnOrder> key_columns{};
  RecordCodecOptions record_options{};
  EphemeralRelationPurpose purpose = EphemeralRelationPurpose::kMembership;
  bool track_nulls = false;
};

enum class EphemeralInsertMode : std::uint8_t {
  kKeepExisting,
  kReplaceExisting,
};

enum class EphemeralInsertResult : std::uint8_t {
  kInserted,
  kDuplicate,
  kReplaced,
};

enum class EphemeralRelationState : std::uint8_t {
  kWriting,
  kPositioned,
  kExhausted,
  kClosed,
  kLookupReady,
  kLookupPositioned,
};

[[nodiscard]] std::string_view EphemeralRelationStateName(EphemeralRelationState state) noexcept;

class EphemeralRelation final {
 public:
  EphemeralRelation(const EphemeralRelation&) = delete;
  EphemeralRelation& operator=(const EphemeralRelation&) = delete;
  EphemeralRelation(EphemeralRelation&&) noexcept;
  EphemeralRelation& operator=(EphemeralRelation&&) noexcept;
  ~EphemeralRelation();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] EphemeralRelationState state() const noexcept;
  [[nodiscard]] std::size_t record_count() const noexcept;
  [[nodiscard]] ByteCount memory_usage() const noexcept;
  [[nodiscard]] bool file_backed() const noexcept;

  [[nodiscard]] Result<EphemeralInsertResult> Insert(ByteBuffer record, EphemeralInsertMode mode);
  [[nodiscard]] Result<bool> Contains(ByteView key);
  [[nodiscard]] Result<bool> Contains(std::span<const SqlValue> key);
  [[nodiscard]] Result<bool> HasNullKey() const;
  [[nodiscard]] Result<bool> Erase(ByteView key);
  [[nodiscard]] Status SealForLookup();
  [[nodiscard]] Result<bool> SeekPrefix(std::span<const SqlValue> key);
  [[nodiscard]] Result<bool> NextPrefix(std::span<const SqlValue> key);
  [[nodiscard]] Status ClearPosition();
  [[nodiscard]] Status Rewind();
  // The view is invalidated by seek, advance, clear, reset, close, move
  // assignment, or destruction.
  [[nodiscard]] Result<RecordView> current_record() const;
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Status Reset();
  void Close() noexcept;

 private:
  friend class StatementTemporaryStorage;

  struct Impl;

  explicit EphemeralRelation(std::unique_ptr<Impl> impl) noexcept;
  [[nodiscard]] static Result<EphemeralRelation> Create(
      const EphemeralRelationDescriptor& descriptor, StatementTemporaryStorage& storage);

  std::unique_ptr<Impl> impl_;
};

enum class RecordSorterState : std::uint8_t {
  kWriting,
  kPositioned,
  kExhausted,
  kClosed,
};

[[nodiscard]] std::string_view RecordSorterStateName(RecordSorterState state) noexcept;

class RecordSorter final {
 public:
  RecordSorter(const RecordSorter&) = delete;
  RecordSorter& operator=(const RecordSorter&) = delete;
  RecordSorter(RecordSorter&&) noexcept;
  RecordSorter& operator=(RecordSorter&&) noexcept;
  ~RecordSorter();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] RecordSorterState state() const noexcept;
  [[nodiscard]] std::size_t record_count() const noexcept;
  [[nodiscard]] ByteCount memory_usage() const noexcept;
  [[nodiscard]] bool has_spilled() const noexcept;
  [[nodiscard]] std::size_t spilled_run_count() const noexcept;
  [[nodiscard]] std::size_t merge_level_count() const noexcept;

  [[nodiscard]] Status Insert(std::span<const SqlValue> values);
  [[nodiscard]] Status Insert(ByteBuffer record);
  // Borrows input only until return; retained records own their bytes.
  [[nodiscard]] Status InsertEncoded(ByteView record);
  [[nodiscard]] Status Rewind();
  // The returned view remains valid until Next(), Reset(), Close(), move
  // assignment, or destruction.
  [[nodiscard]] Result<RecordView> current_record() const;
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Result<std::weak_ordering> CompareCurrent(const RecordView& record) const;
  [[nodiscard]] Status Reset();
  void Close() noexcept;

 private:
  friend class StatementTemporaryStorage;

  struct Impl;

  explicit RecordSorter(std::unique_ptr<Impl> impl) noexcept;
  [[nodiscard]] static Result<RecordSorter> Create(const RecordSorterDescriptor& descriptor,
                                                   StatementTemporaryStorage& storage);

  std::unique_ptr<Impl> impl_;
};

enum class BoundedTopNState : std::uint8_t {
  kWriting,
  kCandidatePending,
  kPositioned,
  kExhausted,
  kClosed,
};

enum class TopNCheckResult : std::uint8_t {
  kRejected,
  kAccepted,
};

[[nodiscard]] std::string_view BoundedTopNStateName(BoundedTopNState state) noexcept;

class BoundedTopN final {
 public:
  BoundedTopN(const BoundedTopN&) = delete;
  BoundedTopN& operator=(const BoundedTopN&) = delete;
  BoundedTopN(BoundedTopN&&) noexcept;
  BoundedTopN& operator=(BoundedTopN&&) noexcept;
  ~BoundedTopN();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] BoundedTopNState state() const noexcept;
  [[nodiscard]] std::size_t bound() const noexcept;
  [[nodiscard]] std::size_t record_count() const noexcept;
  [[nodiscard]] ByteCount memory_usage() const noexcept;
  [[nodiscard]] bool has_pending_candidate() const noexcept;

  [[nodiscard]] Result<TopNCheckResult> CheckCandidate(ByteBuffer key);
  [[nodiscard]] Status Insert(ByteBuffer record);
  // Borrows input only until return; queue records own their bytes.
  [[nodiscard]] Status InsertEncoded(ByteView record);
  [[nodiscard]] Status Rewind();
  [[nodiscard]] Result<RecordView> current_record() const;
  [[nodiscard]] Result<bool> Next();
  [[nodiscard]] Status Reset();
  // Clears one block without reopening its root or restarting its sequence.
  [[nodiscard]] Status ResetBlock(std::size_t residual_bound);
  void Close() noexcept;

 private:
  friend class StatementTemporaryStorage;
  friend class temporary_storage_detail::BoundedTopNAccess;

  struct Impl;

  explicit BoundedTopN(std::unique_ptr<Impl> impl) noexcept;

  [[nodiscard]] Status ResetContents(std::optional<std::size_t> residual_bound);
  [[nodiscard]] static Result<BoundedTopN> Create(const RecordSorterDescriptor& descriptor,
                                                  std::size_t bound,
                                                  StatementTemporaryStorage& storage);

  mutable std::unique_ptr<Impl> impl_;
};

// Borrows the VFS and pager. Both must outlive the factory, statement storage,
// and every temporary capability created through them.
class TemporaryStorageFactory final {
 public:
  [[nodiscard]] static Result<TemporaryStorageFactory> Create(Vfs& vfs, const Pager& pager,
                                                              TemporaryStorageOptions options = {});

  TemporaryStorageFactory(const TemporaryStorageFactory&) = delete;
  TemporaryStorageFactory& operator=(const TemporaryStorageFactory&) = delete;
  TemporaryStorageFactory(TemporaryStorageFactory&&) noexcept = default;
  TemporaryStorageFactory& operator=(TemporaryStorageFactory&&) noexcept = default;
  ~TemporaryStorageFactory() = default;

  [[nodiscard]] const TemporaryStorageOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool attached_to(const Pager& pager) const noexcept { return pager_ == &pager; }
  [[nodiscard]] bool file_spill_enabled() const noexcept;
  [[nodiscard]] ByteCount sorter_memory_threshold() const noexcept;
  [[nodiscard]] Result<StatementTemporaryStorage> CreateStatementStorage() const;

 private:
  friend struct temporary_storage_detail::StatementState;

  TemporaryStorageFactory(Vfs& vfs, const Pager& pager, TemporaryStorageOptions options) noexcept
      : vfs_(&vfs), pager_(&pager), options_(options) {}

  Vfs* vfs_;
  const Pager* pager_;
  TemporaryStorageOptions options_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_TEMPORARY_STORAGE_TEMPORARY_STORAGE_HPP_
