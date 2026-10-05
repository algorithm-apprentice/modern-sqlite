#include "modern_sqlite/storage/journal/rollback_journal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_sqlite/base/coding.hpp"

namespace modern_sqlite {
namespace {

constexpr std::array<std::byte, 8> kJournalMagic{
    std::byte{0xd9}, std::byte{0xd5}, std::byte{0x05}, std::byte{0xf9},
    std::byte{0x20}, std::byte{0xa1}, std::byte{0x63}, std::byte{0xd7},
};
constexpr std::size_t kPageRecordOverhead = 8;
constexpr std::size_t kSubjournalRecordOverhead = 4;
constexpr std::size_t kMinimumPageSize = 512;
constexpr std::size_t kMaximumPageSize = 65536;
constexpr std::size_t kMinimumSectorSize = 32;
constexpr std::size_t kMaximumSectorSize = 65536;

[[nodiscard]] Error Misuse(std::string message) {
  return Error::Create(ErrorCode::kMisuse, std::move(message));
}

[[nodiscard]] Error Internal(std::string message) {
  return Error::Create(ErrorCode::kInternal, std::move(message));
}

[[nodiscard]] Error TooLarge(std::string message) {
  return Error::Create(ErrorCode::kTooLarge, std::move(message));
}

[[nodiscard]] bool IsValidPageSize(ByteCount page_size) noexcept {
  return page_size.value() >= kMinimumPageSize && page_size.value() <= kMaximumPageSize &&
         std::has_single_bit(page_size.value());
}

[[nodiscard]] Result<std::uint64_t> CheckedAdd(std::uint64_t lhs, std::uint64_t rhs,
                                               std::string_view context) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return std::unexpected(TooLarge(std::string{context}));
  }
  return lhs + rhs;
}

[[nodiscard]] Result<std::uint64_t> AlignUp(std::uint64_t value, ByteCount alignment) {
  const auto mask = static_cast<std::uint64_t>(alignment.value() - 1U);
  auto added = CheckedAdd(value, mask, "rollback-journal alignment overflows the file range");
  if (!added.has_value()) {
    return std::unexpected(std::move(added.error()));
  }
  return *added & ~mask;
}

void StoreU32(MutableByteView output, std::size_t offset, std::uint32_t value) noexcept {
  StoreBigEndian<std::uint32_t>(
      std::span<std::byte, sizeof(std::uint32_t)>{output.data() + offset, sizeof(std::uint32_t)},
      value);
}

[[nodiscard]] std::uint32_t LockingPage(ByteCount page_size) noexcept {
  constexpr std::uint64_t kPendingByte = 0x40000000ULL;
  return static_cast<std::uint32_t>(kPendingByte / page_size.value() + 1U);
}

}  // namespace

Result<ByteCount> ResolveRollbackJournalSectorSize(const FileProperties& properties) {
  std::size_t sector_size = properties.sector_size.value();
  if (properties.device_characteristics.Has(DeviceCapability::kPowersafeOverwrite) ||
      sector_size < kMinimumSectorSize) {
    sector_size = 512;
  } else if (sector_size > kMaximumSectorSize) {
    sector_size = kMaximumSectorSize;
  }
  if (!std::has_single_bit(sector_size)) {
    return std::unexpected(Internal("rollback-journal sector size must resolve to a power of two"));
  }
  return ByteCount{sector_size};
}

namespace {

struct JournalCohort {
  std::uint64_t header_offset = 0;
  std::uint64_t first_record_offset = 0;
  std::uint32_t checksum_seed = 0;
  std::uint32_t record_count = 0;
  bool published = false;
};

[[nodiscard]] Result<bool> ReadComplete(File& file, MutableByteView destination,
                                        std::uint64_t offset) {
  auto read = file.ReadAt(destination, FileOffset{offset});
  if (!read.has_value()) {
    return std::unexpected(std::move(read.error()));
  }
  return read->complete();
}

[[nodiscard]] bool IsHexDigit(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

[[nodiscard]] bool IsSuperJournalName(std::string_view name) noexcept {
  if (name.size() < 12U) {
    return false;
  }
  const std::size_t marker = name.size() - 12U;
  if (name[marker] != '-' || name[marker + 1U] != 'm' || name[marker + 2U] != 'j' ||
      name[name.size() - 3U] != '9') {
    return false;
  }
  return std::all_of(name.end() - 9, name.end(), IsHexDigit);
}

[[nodiscard]] Result<bool> SuppressForMissingSuperJournal(File& file, Vfs& vfs,
                                                          std::uint64_t file_size) {
  constexpr std::size_t kTrailerTailSize = 16;
  if (file_size < kTrailerTailSize) {
    return false;
  }

  std::array<std::byte, kTrailerTailSize> tail{};
  auto complete = ReadComplete(file, tail, file_size - kTrailerTailSize);
  if (!complete.has_value()) {
    return std::unexpected(std::move(complete.error()));
  }
  if (!*complete ||
      !std::ranges::equal(kJournalMagic, ByteView{tail}.subspan(8, kJournalMagic.size()))) {
    return false;
  }

  const auto name_size = LoadBigEndian<std::uint32_t>(
      std::span<const std::byte, sizeof(std::uint32_t)>{tail.data(), sizeof(std::uint32_t)});
  const auto expected_checksum = LoadBigEndian<std::uint32_t>(
      std::span<const std::byte, sizeof(std::uint32_t)>{tail.data() + 4, sizeof(std::uint32_t)});
  const std::size_t maximum_path = vfs.MaximumPathLength().value();
  if (maximum_path == 0U) {
    return std::unexpected(Internal("the VFS reported a zero maximum pathname length"));
  }
  if (name_size == 0U || name_size > maximum_path ||
      static_cast<std::uint64_t>(name_size) > file_size - kTrailerTailSize) {
    return false;
  }

  std::vector<std::byte> name_bytes;
  try {
    name_bytes.resize(name_size);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
  complete = ReadComplete(file, name_bytes, file_size - kTrailerTailSize - name_size);
  if (!complete.has_value()) {
    return std::unexpected(std::move(complete.error()));
  }
  if (!*complete) {
    return false;
  }

  std::uint32_t checksum = 0;
  for (const std::byte value : name_bytes) {
    const auto unsigned_value = std::to_integer<std::uint8_t>(value);
    const auto signed_value = std::bit_cast<std::int8_t>(unsigned_value);
    checksum += static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_value));
  }
  if (checksum != expected_checksum) {
    return false;
  }

  std::string_view name = AsStringView(name_bytes);
  const std::size_t nul = name.find('\0');
  if (nul != std::string_view::npos) {
    name = name.substr(0, nul);
  }
  if (!IsSuperJournalName(name)) {
    return false;
  }

  auto exists = vfs.Access(name, FileAccessQuery::kExists);
  if (!exists.has_value()) {
    return std::unexpected(std::move(exists.error()));
  }
  return !*exists;
}

struct MainPlaybackOptions {
  ByteCount initial_sector;
  ByteCount legacy_page_size;
  std::optional<std::uint64_t> live_header_offset;
};

class RollbackMainPlayback final : public JournalPlayback {
 public:
  [[nodiscard]] static Result<std::unique_ptr<JournalPlayback>> Open(File& file, Vfs& vfs,
                                                                     JournalPlaybackKind kind,
                                                                     MainPlaybackOptions options) {
    try {
      auto playback =
          std::unique_ptr<RollbackMainPlayback>(new RollbackMainPlayback(file, vfs, kind, options));
      auto initialized = playback->Initialize();
      if (!initialized.has_value()) {
        return std::unexpected(std::move(initialized.error()));
      }
      return std::unique_ptr<JournalPlayback>{std::move(playback)};
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] std::optional<JournalPlaybackInfo> info() const noexcept override { return info_; }

  [[nodiscard]] Result<std::optional<JournalPageImage>> Next() override {
    while (info_.has_value() && !ended_) {
      if (records_remaining_ == 0U) {
        if (terminal_cohort_) {
          ended_ = true;
          break;
        }
        auto loaded = LoadNextHeader();
        if (!loaded.has_value()) {
          return std::unexpected(std::move(loaded.error()));
        }
        if (!*loaded) {
          ended_ = true;
          break;
        }
        continue;
      }

      const std::uint64_t record_size = page_size_.value() + kPageRecordOverhead;
      if (record_offset_ > file_size_ || record_size > file_size_ - record_offset_) {
        ended_ = true;
        break;
      }
      auto complete = ReadComplete(*file_, record_buffer_.mutable_view(), record_offset_);
      if (!complete.has_value()) {
        return std::unexpected(std::move(complete.error()));
      }
      if (!*complete) {
        ended_ = true;
        break;
      }
      record_offset_ += record_size;
      --records_remaining_;

      const ByteView record = record_buffer_.view();
      const auto page_number = LoadBigEndian<std::uint32_t>(record.first<sizeof(std::uint32_t)>());
      if (page_number == 0U || page_number == LockingPage(page_size_)) {
        ended_ = true;
        break;
      }
      if (page_number > info_->original_page_count) {
        continue;
      }
      const ByteView page = record.subspan(4, page_size_.value());
      const auto stored_checksum =
          LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
              record.data() + page_size_.value() + 4U, sizeof(std::uint32_t)});
      if (stored_checksum != ComputeRollbackJournalChecksum(page, checksum_seed_)) {
        ended_ = true;
        break;
      }
      return std::optional<JournalPageImage>{JournalPageImage{
          .page_number = PageNumber{page_number},
          .bytes = page,
      }};
    }
    return std::optional<JournalPageImage>{};
  }

 private:
  RollbackMainPlayback(File& file, Vfs& vfs, JournalPlaybackKind kind,
                       MainPlaybackOptions options) noexcept
      : file_(&file),
        vfs_(&vfs),
        kind_(kind),
        initial_sector_(options.initial_sector),
        legacy_page_size_(options.legacy_page_size),
        live_header_offset_(options.live_header_offset) {}

  [[nodiscard]] Status Initialize() {
    auto size = file_->Size();
    if (!size.has_value()) {
      return std::unexpected(std::move(size.error()));
    }
    file_size_ = size->value();

    auto suppressed = SuppressForMissingSuperJournal(*file_, *vfs_, file_size_);
    if (!suppressed.has_value()) {
      return std::unexpected(std::move(suppressed.error()));
    }
    if (*suppressed || file_size_ < initial_sector_.value()) {
      ended_ = true;
      return {};
    }

    std::array<std::byte, 28> header{};
    auto complete = ReadComplete(*file_, header, 0);
    if (!complete.has_value()) {
      return std::unexpected(std::move(complete.error()));
    }
    if (!*complete ||
        !std::ranges::equal(kJournalMagic, ByteView{header}.first(kJournalMagic.size()))) {
      ended_ = true;
      return {};
    }

    const std::uint32_t record_count = ReadHeaderU32(header, 8);
    checksum_seed_ = ReadHeaderU32(header, 12);
    const std::uint32_t original_page_count = ReadHeaderU32(header, 16);
    const std::uint32_t sector_size = ReadHeaderU32(header, 20);
    std::uint32_t page_size = ReadHeaderU32(header, 24);
    if (page_size == 0U) {
      page_size = static_cast<std::uint32_t>(legacy_page_size_.value());
    }
    if (page_size < kMinimumPageSize || page_size > kMaximumPageSize ||
        !std::has_single_bit(page_size) || sector_size < kMinimumSectorSize ||
        sector_size > kMaximumSectorSize || !std::has_single_bit(sector_size)) {
      ended_ = true;
      return {};
    }

    page_size_ = ByteCount{page_size};
    sector_size_ = ByteCount{sector_size};
    record_buffer_ = ByteBuffer{ByteCount{page_size_.value() + kPageRecordOverhead}};
    record_offset_ = sector_size_.value();
    ConfigureRecordCount(record_count, 0);
    info_ = JournalPlaybackInfo{
        .kind = kind_,
        .page_size = page_size_,
        .original_page_count = original_page_count,
    };
    return {};
  }

  [[nodiscard]] static std::uint32_t ReadHeaderU32(const std::array<std::byte, 28>& header,
                                                   std::size_t offset) noexcept {
    return LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
        header.data() + offset, sizeof(std::uint32_t)});
  }

  void ConfigureRecordCount(std::uint32_t record_count, std::uint64_t header_offset) noexcept {
    terminal_cohort_ = false;
    if (record_count == std::numeric_limits<std::uint32_t>::max()) {
      records_remaining_ =
          record_offset_ <= file_size_
              ? (file_size_ - record_offset_) / (page_size_.value() + kPageRecordOverhead)
              : 0U;
      terminal_cohort_ = true;
      return;
    }
    if (record_count == 0U && kind_ == JournalPlaybackKind::kTransactionRollback &&
        live_header_offset_.has_value() && *live_header_offset_ == header_offset) {
      records_remaining_ =
          record_offset_ <= file_size_
              ? (file_size_ - record_offset_) / (page_size_.value() + kPageRecordOverhead)
              : 0U;
      terminal_cohort_ = true;
      return;
    }
    records_remaining_ = record_count;
  }

  [[nodiscard]] Result<bool> LoadNextHeader() {
    auto aligned = AlignUp(record_offset_, sector_size_);
    if (!aligned.has_value()) {
      return false;
    }
    if (*aligned > file_size_ || sector_size_.value() > file_size_ - *aligned) {
      return false;
    }

    std::array<std::byte, 20> header{};
    auto complete = ReadComplete(*file_, header, *aligned);
    if (!complete.has_value()) {
      return std::unexpected(std::move(complete.error()));
    }
    if (!*complete ||
        !std::ranges::equal(kJournalMagic, ByteView{header}.first(kJournalMagic.size()))) {
      return false;
    }

    const auto record_count =
        LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
            header.data() + 8, sizeof(std::uint32_t)});
    checksum_seed_ = LoadBigEndian<std::uint32_t>(std::span<const std::byte, sizeof(std::uint32_t)>{
        header.data() + 12, sizeof(std::uint32_t)});
    record_offset_ = *aligned + sector_size_.value();
    ConfigureRecordCount(record_count, *aligned);
    return true;
  }

  File* file_;
  Vfs* vfs_;
  JournalPlaybackKind kind_;
  ByteCount initial_sector_;
  ByteCount legacy_page_size_;
  std::optional<std::uint64_t> live_header_offset_;
  std::optional<JournalPlaybackInfo> info_;
  ByteCount page_size_{0};
  ByteCount sector_size_{0};
  ByteBuffer record_buffer_;
  std::uint64_t file_size_ = 0;
  std::uint64_t record_offset_ = 0;
  std::uint64_t records_remaining_ = 0;
  std::uint32_t checksum_seed_ = 0;
  bool terminal_cohort_ = false;
  bool ended_ = false;
};

struct SavepointPlaybackRange {
  std::uint64_t main_boundary;
  std::uint64_t subjournal_boundary;
  std::uint64_t subjournal_record_count;
};

class RollbackSavepointPlayback final : public JournalPlayback {
 public:
  [[nodiscard]] static Result<std::unique_ptr<JournalPlayback>> Open(
      File& main_file, File* subjournal_file, const std::vector<JournalCohort>& cohorts,
      JournalPlaybackInfo info, SavepointPlaybackRange range) {
    try {
      return std::unique_ptr<JournalPlayback>(
          new RollbackSavepointPlayback(main_file, subjournal_file, cohorts, info, range));
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }

  [[nodiscard]] std::optional<JournalPlaybackInfo> info() const noexcept override { return info_; }

  [[nodiscard]] Result<std::optional<JournalPageImage>> Next() override {
    const std::uint64_t main_record_size = info_.page_size.value() + kPageRecordOverhead;
    while (cohort_index_ < cohorts_->size()) {
      const JournalCohort& cohort = (*cohorts_)[cohort_index_];
      if (cohort_record_index_ >= cohort.record_count) {
        ++cohort_index_;
        cohort_record_index_ = 0;
        continue;
      }
      const std::uint64_t offset =
          cohort.first_record_offset + main_record_size * cohort_record_index_++;
      if (offset < main_boundary_) {
        continue;
      }
      auto image = ReadRecord(*main_file_, FileOffset{offset}, ByteCount{kPageRecordOverhead});
      if (!image.has_value()) {
        return std::unexpected(std::move(image.error()));
      }
      return std::optional<JournalPageImage>{*image};
    }

    if (subjournal_index_ >= subjournal_record_count_) {
      return std::optional<JournalPageImage>{};
    }
    if (subjournal_file_ == nullptr) {
      return std::unexpected(Internal("savepoint playback is missing its subjournal file"));
    }
    const std::uint64_t subjournal_record_size =
        info_.page_size.value() + kSubjournalRecordOverhead;
    const std::uint64_t offset = subjournal_record_size * subjournal_index_++;
    auto image =
        ReadRecord(*subjournal_file_, FileOffset{offset}, ByteCount{kSubjournalRecordOverhead});
    if (!image.has_value()) {
      return std::unexpected(std::move(image.error()));
    }
    return std::optional<JournalPageImage>{*image};
  }

 private:
  RollbackSavepointPlayback(File& main_file, File* subjournal_file,
                            const std::vector<JournalCohort>& cohorts, JournalPlaybackInfo info,
                            SavepointPlaybackRange range)
      : main_file_(&main_file),
        subjournal_file_(subjournal_file),
        cohorts_(&cohorts),
        info_(info),
        main_boundary_(range.main_boundary),
        subjournal_index_(range.subjournal_boundary),
        subjournal_record_count_(range.subjournal_record_count),
        record_buffer_(ByteCount{info.page_size.value() + kPageRecordOverhead}) {}

  [[nodiscard]] Result<JournalPageImage> ReadRecord(File& file, FileOffset offset,
                                                    ByteCount overhead) {
    const std::size_t record_size = info_.page_size.value() + overhead.value();
    auto complete =
        ReadComplete(file, record_buffer_.mutable_view().first(record_size), offset.value());
    if (!complete.has_value()) {
      return std::unexpected(std::move(complete.error()));
    }
    if (!*complete) {
      return std::unexpected(Internal("live savepoint journal contains a short record"));
    }
    const ByteView record = record_buffer_.view();
    const auto page_number = LoadBigEndian<std::uint32_t>(record.first<sizeof(std::uint32_t)>());
    if (page_number == 0U || page_number == LockingPage(info_.page_size)) {
      return std::unexpected(Internal("live savepoint journal contains an invalid page number"));
    }
    return JournalPageImage{
        .page_number = PageNumber{page_number},
        .bytes = record.subspan(4, info_.page_size.value()),
    };
  }

  File* main_file_;
  File* subjournal_file_;
  const std::vector<JournalCohort>* cohorts_;
  JournalPlaybackInfo info_;
  std::uint64_t main_boundary_;
  std::uint64_t subjournal_index_;
  std::uint64_t subjournal_record_count_;
  ByteBuffer record_buffer_;
  std::size_t cohort_index_ = 0;
  std::uint64_t cohort_record_index_ = 0;
};

}  // namespace

class RollbackJournal::Impl final {
 public:
  Impl(Vfs& vfs, std::string database_path, std::string journal_path,
       FileProperties database_properties, ByteCount effective_sector,
       RollbackJournalOptions options) noexcept
      : vfs_(&vfs),
        database_path_(std::move(database_path)),
        journal_path_(std::move(journal_path)),
        effective_sector_(effective_sector),
        options_(options),
        safe_append_(database_properties.device_characteristics.Has(DeviceCapability::kSafeAppend)),
        sequential_(database_properties.device_characteristics.Has(DeviceCapability::kSequential)) {
  }

  [[nodiscard]] std::string_view database_path() const noexcept { return database_path_; }
  [[nodiscard]] std::string_view journal_path() const noexcept { return journal_path_; }

  [[nodiscard]] Status Begin(JournalTransactionInfo info) {
    if (terminal_failure_) {
      return std::unexpected(Internal("rollback journal is in a terminal failure state"));
    }
    if (active_ || recovering_ || main_file_ != nullptr) {
      return std::unexpected(Misuse("rollback journal is already active"));
    }
    if (info.sector_size != effective_sector_) {
      return std::unexpected(
          Misuse("journal transaction sector size does not match database properties"));
    }
    if (!IsValidPageSize(info.page_size)) {
      return std::unexpected(Misuse("rollback-journal page size is invalid"));
    }

    try {
      header_buffer_ = ByteBuffer{effective_sector_};
      record_buffer_ = ByteBuffer{ByteCount{info.page_size.value() + kPageRecordOverhead}};
      cohorts_.clear();
      cohorts_.reserve(4);
      savepoints_.clear();
      savepoints_.reserve(4);
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }

    auto opened = vfs_->Open(journal_path_, FileOpenOptions{
                                                .kind = FileKind::kMainJournal,
                                                .access = FileAccessMode::kReadWrite,
                                                .create = true,
                                                .exclusive_create = false,
                                                .delete_on_close = false,
                                                .allow_read_only_fallback = false,
                                                .no_follow = false,
                                            });
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    main_file_ = std::move(opened->file);
    auto truncated = main_file_->Truncate(FileSize{0});
    if (!truncated.has_value()) {
      main_file_.reset();
      return std::unexpected(std::move(truncated.error()));
    }

    info_ = info;
    journal_offset_ = 0;
    subjournal_record_count_ = 0;
    auto started = StartCohort(0);
    if (!started.has_value()) {
      main_file_.reset();
      return started;
    }
    active_ = true;
    return {};
  }

  [[nodiscard]] Status AppendMainPage(JournalPageImage image) {
    if (!active_ || main_file_ == nullptr || cohorts_.empty()) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    if (image.bytes.size() != info_.page_size.value()) {
      return std::unexpected(Internal("coordinator supplied an invalid main-journal page image"));
    }
    if (image.page_number.value() == 0U ||
        image.page_number.value() == LockingPage(info_.page_size)) {
      return std::unexpected(Internal("coordinator supplied an invalid main-journal page number"));
    }

    if (cohorts_.back().published && !safe_append_) {
      auto aligned = AlignUp(journal_offset_, effective_sector_);
      if (!aligned.has_value()) {
        return std::unexpected(std::move(aligned.error()));
      }
      auto started = StartCohort(*aligned);
      if (!started.has_value()) {
        return started;
      }
    }

    JournalCohort& target = cohorts_.back();
    if (target.record_count == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(TooLarge("rollback-journal cohort record count is too large"));
    }

    const MutableByteView record = record_buffer_.mutable_view();
    StoreU32(record, 0, image.page_number.value());
    std::ranges::copy(image.bytes, record.begin() + static_cast<std::ptrdiff_t>(4));
    StoreU32(record, info_.page_size.value() + 4U,
             ComputeRollbackJournalChecksum(image.bytes, target.checksum_seed));

    auto written = main_file_->WriteAt(record_buffer_.view(), FileOffset{journal_offset_});
    if (!written.has_value()) {
      return written;
    }
    auto next = CheckedAdd(journal_offset_, record.size(),
                           "rollback-journal record offset overflows the file range");
    if (!next.has_value()) {
      return std::unexpected(std::move(next.error()));
    }
    journal_offset_ = *next;
    ++target.record_count;
    return {};
  }

  [[nodiscard]] Status Sync() {
    if (!active_ || main_file_ == nullptr || cohorts_.empty()) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    JournalCohort& current = cohorts_.back();

    if (!safe_append_) {
      auto next_header = AlignUp(journal_offset_, effective_sector_);
      if (!next_header.has_value()) {
        return std::unexpected(std::move(next_header.error()));
      }
      std::array<std::byte, kJournalMagic.size()> possible_magic{};
      auto read = main_file_->ReadAt(possible_magic, FileOffset{*next_header});
      if (!read.has_value()) {
        return std::unexpected(std::move(read.error()));
      }
      if (read->complete() && possible_magic == kJournalMagic) {
        constexpr std::array<std::byte, 1> kZero{std::byte{0}};
        auto invalidated = main_file_->WriteAt(kZero, FileOffset{*next_header});
        if (!invalidated.has_value()) {
          return invalidated;
        }
      }

      if (!sequential_) {
        auto synced = main_file_->Sync(SyncOptions{.mode = SyncMode::kNormal, .data_only = false});
        if (!synced.has_value()) {
          return synced;
        }
      }

      std::array<std::byte, 12> published{};
      std::ranges::copy(kJournalMagic, published.begin());
      StoreU32(published, kJournalMagic.size(), current.record_count);
      auto written = main_file_->WriteAt(published, FileOffset{current.header_offset});
      if (!written.has_value()) {
        return written;
      }
    }

    if (!sequential_) {
      auto synced = main_file_->Sync(SyncOptions{.mode = SyncMode::kNormal, .data_only = false});
      if (!synced.has_value()) {
        return synced;
      }
    }
    current.published = true;
    return {};
  }

  [[nodiscard]] Status AppendSavepointPage(JournalPageImage image) {
    if (!active_ || main_file_ == nullptr) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    if (image.bytes.size() != info_.page_size.value()) {
      return std::unexpected(Internal("coordinator supplied an invalid subjournal page image"));
    }
    if (image.page_number.value() == 0U ||
        image.page_number.value() == LockingPage(info_.page_size)) {
      return std::unexpected(Internal("coordinator supplied an invalid subjournal page number"));
    }

    if (subjournal_file_ == nullptr) {
      try {
        subjournal_buffer_ =
            ByteBuffer{ByteCount{info_.page_size.value() + kSubjournalRecordOverhead}};
      } catch (const std::bad_alloc&) {
        return std::unexpected(Error::OutOfMemory());
      }
      auto opened = vfs_->Open(std::nullopt, FileOpenOptions{
                                                 .kind = FileKind::kSubjournal,
                                                 .access = FileAccessMode::kReadWrite,
                                                 .create = true,
                                                 .exclusive_create = true,
                                                 .delete_on_close = true,
                                                 .allow_read_only_fallback = false,
                                                 .no_follow = false,
                                             });
      if (!opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
      }
      subjournal_file_ = std::move(opened->file);
    }

    const MutableByteView record = subjournal_buffer_.mutable_view();
    StoreU32(record, 0, image.page_number.value());
    std::ranges::copy(image.bytes, record.begin() + static_cast<std::ptrdiff_t>(4));
    const std::uint64_t record_size = record.size();
    if (subjournal_record_count_ > std::numeric_limits<std::uint64_t>::max() / record_size) {
      return std::unexpected(TooLarge("subjournal offset overflows the file range"));
    }
    const std::uint64_t offset = subjournal_record_count_ * record_size;
    auto written = subjournal_file_->WriteAt(subjournal_buffer_.view(), FileOffset{offset});
    if (!written.has_value()) {
      return written;
    }
    ++subjournal_record_count_;
    return {};
  }

  [[nodiscard]] Status CreateSavepoint(JournalSavepoint savepoint) {
    if (!active_) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    try {
      savepoints_.push_back(SavepointBoundary{
          .savepoint = savepoint,
          .main_offset = journal_offset_,
          .subjournal_record_count = subjournal_record_count_,
      });
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
    return {};
  }

  [[nodiscard]] Status ReleaseSavepoint(JournalSavepointId savepoint, bool rewind_subjournal) {
    const std::optional<std::size_t> index = FindSavepoint(savepoint);
    if (!index.has_value()) {
      return std::unexpected(Internal("rollback backend cannot find the released savepoint"));
    }
    if (rewind_subjournal) {
      subjournal_record_count_ = savepoints_[*index].subjournal_record_count;
    }
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(*index), savepoints_.end());
    return {};
  }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> OpenSavepointPlayback(
      JournalSavepoint savepoint) {
    if (!active_ || main_file_ == nullptr) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    const std::optional<std::size_t> index = FindSavepoint(savepoint.id);
    if (!index.has_value()) {
      return std::unexpected(Internal("rollback backend cannot find the savepoint"));
    }
    const SavepointBoundary& boundary = savepoints_[*index];
    return RollbackSavepointPlayback::Open(
        *main_file_, subjournal_file_.get(), cohorts_,
        JournalPlaybackInfo{
            .kind = JournalPlaybackKind::kSavepointRollback,
            .page_size = info_.page_size,
            .original_page_count = savepoint.original_page_count,
        },
        SavepointPlaybackRange{
            .main_boundary = boundary.main_offset,
            .subjournal_boundary = boundary.subjournal_record_count,
            .subjournal_record_count = subjournal_record_count_,
        });
  }

  [[nodiscard]] Status CompleteSavepointPlayback(JournalSavepoint savepoint) {
    const std::optional<std::size_t> index = FindSavepoint(savepoint.id);
    if (!index.has_value()) {
      return std::unexpected(Internal("rollback backend cannot complete the savepoint"));
    }
    savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
                      savepoints_.end());
    return {};
  }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> OpenTransactionPlayback() {
    if (!active_ || main_file_ == nullptr || cohorts_.empty()) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    return RollbackMainPlayback::Open(*main_file_, *vfs_, JournalPlaybackKind::kTransactionRollback,
                                      MainPlaybackOptions{
                                          .initial_sector = effective_sector_,
                                          .legacy_page_size = options_.legacy_page_size,
                                          .live_header_offset = cohorts_.back().header_offset,
                                      });
  }

  [[nodiscard]] Status PrepareHotRecovery() {
    if (terminal_failure_) {
      return std::unexpected(Internal("rollback journal is in a terminal failure state"));
    }
    if (active_ || recovering_ || main_file_ != nullptr) {
      return std::unexpected(Misuse("rollback journal is already active"));
    }
    auto opened = vfs_->Open(journal_path_, FileOpenOptions{
                                                .kind = FileKind::kMainJournal,
                                                .access = FileAccessMode::kReadWrite,
                                                .create = false,
                                                .exclusive_create = false,
                                                .delete_on_close = false,
                                                .allow_read_only_fallback = false,
                                                .no_follow = false,
                                            });
    if (!opened.has_value()) {
      return std::unexpected(std::move(opened.error()));
    }
    main_file_ = std::move(opened->file);
    auto synced = main_file_->Sync(SyncOptions{.mode = SyncMode::kNormal, .data_only = false});
    if (!synced.has_value()) {
      main_file_.reset();
      return synced;
    }
    recovering_ = true;
    return {};
  }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> OpenHotPlayback() {
    if (!recovering_ || main_file_ == nullptr) {
      return std::unexpected(Misuse("hot-journal recovery is not prepared"));
    }
    return RollbackMainPlayback::Open(*main_file_, *vfs_, JournalPlaybackKind::kHotRecovery,
                                      MainPlaybackOptions{
                                          .initial_sector = effective_sector_,
                                          .legacy_page_size = options_.legacy_page_size,
                                          .live_header_offset = std::nullopt,
                                      });
  }

  [[nodiscard]] Status Finalize() {
    if ((!active_ && !recovering_) || main_file_ == nullptr) {
      return std::unexpected(Misuse("rollback journal is not active"));
    }
    main_file_.reset();
    subjournal_file_.reset();
    active_ = false;
    recovering_ = false;

    Status deleted;
    try {
      deleted = vfs_->Delete(journal_path_, options_.delete_directory_sync);
    } catch (const std::bad_alloc&) {
      terminal_failure_ = true;
      return std::unexpected(Error::OutOfMemory());
    }
    if (!deleted.has_value()) {
      terminal_failure_ = true;
      return deleted;
    }
    ResetTransactionState();
    return {};
  }

 private:
  struct SavepointBoundary {
    JournalSavepoint savepoint;
    std::uint64_t main_offset = 0;
    std::uint64_t subjournal_record_count = 0;
  };

  [[nodiscard]] Status StartCohort(std::uint64_t header_offset) {
    if (main_file_ == nullptr) {
      return std::unexpected(Internal("rollback journal has no main file"));
    }

    std::array<std::byte, sizeof(std::uint32_t)> random_seed{};
    auto random = vfs_->RandomBytes(random_seed);
    if (!random.has_value()) {
      return random;
    }
    const auto checksum_seed = LoadBigEndian<std::uint32_t>(
        std::span<const std::byte, sizeof(std::uint32_t)>{random_seed.data(), random_seed.size()});

    std::ranges::fill(header_buffer_.mutable_view(), std::byte{0});
    const MutableByteView header = header_buffer_.mutable_view();
    if (safe_append_) {
      std::ranges::copy(kJournalMagic, header.begin());
      StoreU32(header, 8, std::numeric_limits<std::uint32_t>::max());
    }
    StoreU32(header, 12, checksum_seed);
    StoreU32(header, 16, info_.original_page_count);
    StoreU32(header, 20, static_cast<std::uint32_t>(effective_sector_.value()));
    StoreU32(header, 24, static_cast<std::uint32_t>(info_.page_size.value()));

    try {
      cohorts_.push_back(JournalCohort{
          .header_offset = header_offset,
          .first_record_offset = header_offset + effective_sector_.value(),
          .checksum_seed = checksum_seed,
          .record_count = 0,
          .published = false,
      });
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }

    auto written = main_file_->WriteAt(header_buffer_.view(), FileOffset{header_offset});
    if (!written.has_value()) {
      cohorts_.pop_back();
      return written;
    }
    auto next = CheckedAdd(header_offset, effective_sector_.value(),
                           "rollback-journal header offset overflows the file range");
    if (!next.has_value()) {
      cohorts_.pop_back();
      return std::unexpected(std::move(next.error()));
    }
    journal_offset_ = *next;
    return {};
  }

  [[nodiscard]] std::optional<std::size_t> FindSavepoint(
      JournalSavepointId savepoint) const noexcept {
    for (std::size_t index = 0; index < savepoints_.size(); ++index) {
      if (savepoints_[index].savepoint.id == savepoint) {
        return index;
      }
    }
    return std::nullopt;
  }

  void ResetTransactionState() {
    info_ = JournalTransactionInfo{
        .page_size = ByteCount{0},
        .sector_size = ByteCount{0},
        .original_page_count = 0,
    };
    header_buffer_ = ByteBuffer{};
    record_buffer_ = ByteBuffer{};
    subjournal_buffer_ = ByteBuffer{};
    cohorts_.clear();
    savepoints_.clear();
    journal_offset_ = 0;
    subjournal_record_count_ = 0;
  }

  Vfs* vfs_;
  std::string database_path_;
  std::string journal_path_;
  ByteCount effective_sector_;
  RollbackJournalOptions options_;
  bool safe_append_ = false;
  bool sequential_ = false;
  bool active_ = false;
  bool recovering_ = false;
  bool terminal_failure_ = false;
  JournalTransactionInfo info_{
      .page_size = ByteCount{0},
      .sector_size = ByteCount{0},
      .original_page_count = 0,
  };
  std::unique_ptr<File> main_file_;
  std::unique_ptr<File> subjournal_file_;
  ByteBuffer header_buffer_;
  ByteBuffer record_buffer_;
  ByteBuffer subjournal_buffer_;
  std::vector<JournalCohort> cohorts_;
  std::vector<SavepointBoundary> savepoints_;
  std::uint64_t journal_offset_ = 0;
  std::uint64_t subjournal_record_count_ = 0;
};

Result<std::unique_ptr<RollbackJournal>> RollbackJournal::Create(Vfs& vfs,
                                                                 std::string_view database_path,
                                                                 FileProperties database_properties,
                                                                 RollbackJournalOptions options) {
  try {
    if (!IsValidPageSize(options.legacy_page_size)) {
      return std::unexpected(Misuse(
          "legacy rollback-journal page size must be a power of two from 512 through 65536"));
    }
    auto effective_sector = ResolveRollbackJournalSectorSize(database_properties);
    if (!effective_sector.has_value()) {
      return std::unexpected(std::move(effective_sector.error()));
    }
    auto canonical = vfs.FullPath(database_path);
    if (!canonical.has_value()) {
      return std::unexpected(std::move(canonical.error()));
    }

    std::string journal_path = *canonical;
    journal_path += "-journal";
    auto impl = std::make_unique<Impl>(vfs, std::move(*canonical), std::move(journal_path),
                                       database_properties, *effective_sector, options);
    return std::make_unique<RollbackJournal>(ConstructionKey{}, std::move(impl));
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

RollbackJournal::RollbackJournal(ConstructionKey, std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

RollbackJournal::~RollbackJournal() = default;

std::string_view RollbackJournal::database_path() const noexcept { return impl_->database_path(); }

std::string_view RollbackJournal::journal_path() const noexcept { return impl_->journal_path(); }

Status RollbackJournal::DoBegin(JournalTransactionInfo info) { return impl_->Begin(info); }

Status RollbackJournal::DoAppendTransactionPage(JournalPageImage image) {
  return impl_->AppendMainPage(image);
}

Status RollbackJournal::DoAppendSavepointPage(JournalPageImage image) {
  return impl_->AppendSavepointPage(image);
}

Status RollbackJournal::DoCreateSavepoint(JournalSavepoint savepoint) {
  return impl_->CreateSavepoint(savepoint);
}

Status RollbackJournal::DoReleaseSavepoint(JournalSavepointId savepoint, bool rewind_subjournal) {
  return impl_->ReleaseSavepoint(savepoint, rewind_subjournal);
}

Result<std::unique_ptr<JournalPlayback>> RollbackJournal::DoOpenSavepointPlayback(
    JournalSavepoint savepoint) {
  return impl_->OpenSavepointPlayback(savepoint);
}

Status RollbackJournal::DoCompleteSavepointPlayback(JournalSavepoint savepoint) {
  return impl_->CompleteSavepointPlayback(savepoint);
}

Status RollbackJournal::DoSync() { return impl_->Sync(); }

Result<std::unique_ptr<JournalPlayback>> RollbackJournal::DoOpenTransactionPlayback() {
  return impl_->OpenTransactionPlayback();
}

Status RollbackJournal::DoPrepareHotRecovery() { return impl_->PrepareHotRecovery(); }

Result<std::unique_ptr<JournalPlayback>> RollbackJournal::DoOpenHotPlayback() {
  return impl_->OpenHotPlayback();
}

Status RollbackJournal::DoFinalizeCommit() { return impl_->Finalize(); }

Status RollbackJournal::DoFinalizeRollback() { return impl_->Finalize(); }

}  // namespace modern_sqlite
