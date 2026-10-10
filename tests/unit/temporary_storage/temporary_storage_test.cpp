#include "modern_sqlite/temporary_storage/temporary_storage.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/pager/pager.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "tests/unit/pager/write_pager_test_support.hpp"

namespace modern_sqlite {
namespace {

template <typename T>
[[nodiscard]] T TakeValue(Result<T> result) {
  if (!result.has_value()) {
    throw std::runtime_error(result.error().ToString());
  }
  return std::move(*result);
}

template <typename Enum>
[[nodiscard]] constexpr Enum InvalidEnumValue(std::uint8_t value) noexcept {
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  return static_cast<Enum>(value);
}

class TestSpillTarget final : public TemporaryStorageSpillTarget {
 public:
  void Attach(TemporaryMemoryAccount& account) noexcept { account_ = &account; }
  void SetReclaimable(std::size_t bytes) noexcept { reclaimable_ = ByteCount{bytes}; }
  void FailNext(ErrorCode code) noexcept { failure_ = code; }
  void SucceedWithoutReleaseNext() noexcept { no_progress_ = true; }
  void ReserveDuringNextSpill(TemporaryMemoryAccount& account, ByteCount amount) noexcept {
    nested_account_ = &account;
    nested_amount_ = amount;
  }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
  [[nodiscard]] std::optional<ErrorCode> nested_error() const noexcept { return nested_error_; }

  [[nodiscard]] ByteCount reclaimable_bytes() const noexcept override { return reclaimable_; }

  [[nodiscard]] Status SpillForPressure() override {
    ++calls_;
    if (failure_.has_value()) {
      const ErrorCode code = *failure_;
      failure_.reset();
      return std::unexpected(Error::Create(code, "injected pressure spill failure"));
    }
    if (no_progress_) {
      no_progress_ = false;
      return {};
    }
    if (nested_account_ != nullptr) {
      Status nested = nested_account_->Reserve(nested_amount_);
      if (!nested.has_value()) {
        nested_error_ = nested.error().code();
      }
      nested_account_ = nullptr;
    }
    if (account_ == nullptr) {
      return std::unexpected(Error::Create(ErrorCode::kInternal, "spill target is unattached"));
    }
    const ByteCount released = reclaimable_;
    reclaimable_ = ByteCount{0};
    return account_->Release(released);
  }

 private:
  TemporaryMemoryAccount* account_ = nullptr;
  ByteCount reclaimable_{0};
  std::optional<ErrorCode> failure_{};
  std::size_t calls_ = 0;
  bool no_progress_ = false;
  TemporaryMemoryAccount* nested_account_ = nullptr;
  ByteCount nested_amount_{};
  std::optional<ErrorCode> nested_error_{};
};

TEST(TemporaryStorageFactory, DerivesSQLiteAlignedThresholdFromCurrentPagerFacts) {
  test::WritePagerFixedVfs vfs;
  std::unique_ptr<Pager> pager =
      TakeValue(Pager::Open(vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{4096},
                                .cache_capacity_pages = 1,
                            }));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, TemporaryStorageOptions{}));

  EXPECT_EQ(ByteCount{std::size_t{250} * 4096U}, factory.sorter_memory_threshold());
  ASSERT_TRUE(pager->BeginRead().has_value());
  EXPECT_EQ(ByteCount{250U * test::kWritePagerPageSize}, factory.sorter_memory_threshold());
  EXPECT_TRUE(pager->EndRead().has_value());

  test::WritePagerFixedVfs capped_vfs;
  std::unique_ptr<Pager> capped_pager =
      TakeValue(Pager::Open(capped_vfs, test::kWritePagerInputPath,
                            PagerOptions{
                                .empty_database_page_size = ByteCount{4096},
                                .cache_capacity_pages = 2'000'000,
                            }));
  const TemporaryStorageFactory capped =
      TakeValue(TemporaryStorageFactory::Create(capped_vfs, *capped_pager, {}));
  ASSERT_TRUE(capped_pager->BeginRead().has_value());
  EXPECT_EQ(ByteCount{1U << 29U}, capped.sorter_memory_threshold());
  EXPECT_TRUE(capped_pager->EndRead().has_value());

  const TemporaryStorageOptions overridden_options{
      .mode = TemporaryStoreMode::kMemory,
      .sorter_memory_threshold = ByteCount{1234},
  };
  const TemporaryStorageFactory overridden =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, overridden_options));
  EXPECT_EQ(overridden_options, overridden.options());
  EXPECT_EQ(ByteCount{1234}, overridden.sorter_memory_threshold());
  EXPECT_FALSE(overridden.file_spill_enabled());
}

TEST(TemporaryStorageFactory, OpensExclusiveDeleteOnCloseTemporaryJournalFiles) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, {}));

  Result<std::unique_ptr<File>> opened = factory.CreateTemporaryFile();
  ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
  ASSERT_NE(nullptr, *opened);
  EXPECT_TRUE(vfs.pathless_file_present());
  EXPECT_EQ(1U, vfs.pathless_open_count());
  ASSERT_TRUE(vfs.last_pathless_open_options().has_value());
  EXPECT_EQ((FileOpenOptions{
                .kind = FileKind::kTemporaryJournal,
                .access = FileAccessMode::kReadWrite,
                .create = true,
                .exclusive_create = true,
                .delete_on_close = true,
            }),
            *vfs.last_pathless_open_options());

  const std::array bytes{
      std::byte{0x01},
      std::byte{0x02},
      std::byte{0x03},
  };
  EXPECT_TRUE((*opened)->WriteAt(bytes, FileOffset{}).has_value());
  const auto size = (*opened)->Size();
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(FileSize{bytes.size()}, *size);

  opened->reset();
  EXPECT_FALSE(vfs.pathless_file_present());
}

TEST(TemporaryStorageFactory, RejectsInvalidConfigurationAndMemoryModeFileSpill) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  test::WritePagerFixedVfs unrelated_vfs;

  const auto invalid_mode =
      TemporaryStorageFactory::Create(vfs, *pager,
                                      TemporaryStorageOptions{
                                          .mode = InvalidEnumValue<TemporaryStoreMode>(2),
                                      });
  const auto zero_threshold =
      TemporaryStorageFactory::Create(vfs, *pager,
                                      TemporaryStorageOptions{
                                          .sorter_memory_threshold = ByteCount{0},
                                      });
  const auto oversized_threshold =
      TemporaryStorageFactory::Create(vfs, *pager,
                                      TemporaryStorageOptions{
                                          .sorter_memory_threshold = ByteCount{(1U << 29U) + 1U},
                                      });
  ASSERT_FALSE(invalid_mode.has_value());
  ASSERT_FALSE(zero_threshold.has_value());
  ASSERT_FALSE(oversized_threshold.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, invalid_mode.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, zero_threshold.error().code());
  EXPECT_EQ(ErrorCode::kMisuse, oversized_threshold.error().code());
  const auto mismatched = TemporaryStorageFactory::Create(unrelated_vfs, *pager, {});
  ASSERT_FALSE(mismatched.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, mismatched.error().code());

  const TemporaryStorageFactory memory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                }));
  const auto file = memory.CreateTemporaryFile();
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, file.error().code());
  EXPECT_EQ(0U, vfs.pathless_open_count());
}

TEST(TemporaryStorageFactory, PropagatesPathlessOpenFailuresAndRecovers) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, {}));
  vfs.FailNextPathlessOpen(ErrorCode::kIo);

  const auto failed = factory.CreateTemporaryFile();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_EQ(1U, vfs.pathless_open_count());

  const auto recovered = factory.CreateTemporaryFile();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(2U, vfs.pathless_open_count());
}

TEST(StatementTemporaryStorage, TracksIndependentOwnerReservationsAndPeaks) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{64},
                                                }));
  StatementTemporaryStorage first = TakeValue(factory.CreateStatementStorage());
  const StatementTemporaryStorage second = TakeValue(factory.CreateStatementStorage());
  EXPECT_TRUE(first.valid());
  EXPECT_FALSE(first.file_spill_enabled());
  EXPECT_EQ(ByteCount{64}, first.memory_budget());
  EXPECT_EQ(ByteCount{0}, first.retained_bytes());
  EXPECT_EQ(ByteCount{0}, first.peak_retained_bytes());

  {
    TemporaryMemoryAccount first_owner = TakeValue(first.RegisterMemoryOwner());
    TemporaryMemoryAccount second_owner = TakeValue(first.RegisterMemoryOwner());
    ASSERT_TRUE(first_owner.Reserve(ByteCount{40}).has_value());
    ASSERT_TRUE(second_owner.Reserve(ByteCount{24}).has_value());
    EXPECT_EQ(ByteCount{40}, first_owner.retained_bytes());
    EXPECT_EQ(ByteCount{24}, second_owner.retained_bytes());
    EXPECT_EQ(ByteCount{64}, first.retained_bytes());
    EXPECT_EQ(ByteCount{64}, first.peak_retained_bytes());
    EXPECT_GT(first.overhead_bytes(), ByteCount{0});

    const auto exceeded = second_owner.Reserve(ByteCount{1});
    ASSERT_FALSE(exceeded.has_value());
    EXPECT_EQ(ErrorCode::kTooLarge, exceeded.error().code());
    EXPECT_EQ(ByteCount{64}, first.retained_bytes());

    ASSERT_TRUE(first_owner.Release(ByteCount{16}).has_value());
    ASSERT_TRUE(second_owner.Reserve(ByteCount{16}).has_value());
    EXPECT_EQ(ByteCount{40}, second_owner.retained_bytes());
    EXPECT_EQ(ByteCount{64}, first.retained_bytes());
    EXPECT_EQ(ByteCount{0}, second.retained_bytes());
  }

  EXPECT_EQ(ByteCount{0}, first.retained_bytes());
  EXPECT_EQ(ByteCount{64}, first.peak_retained_bytes());
}

TEST(StatementTemporaryStorage, SpillsLargestOwnerAndUsesOrdinalForTies) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{100},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  TestSpillTarget first_target;
  TestSpillTarget second_target;
  TemporaryMemoryAccount first = TakeValue(storage.RegisterMemoryOwner(first_target));
  TemporaryMemoryAccount second = TakeValue(storage.RegisterMemoryOwner(second_target));
  TemporaryMemoryAccount requester = TakeValue(storage.RegisterMemoryOwner());
  first_target.Attach(first);
  second_target.Attach(second);

  ASSERT_TRUE(first.Reserve(ByteCount{40}).has_value());
  ASSERT_TRUE(second.Reserve(ByteCount{40}).has_value());
  first_target.SetReclaimable(20);
  second_target.SetReclaimable(30);
  ASSERT_TRUE(requester.Reserve(ByteCount{30}).has_value());
  EXPECT_EQ(0U, first_target.calls());
  EXPECT_EQ(1U, second_target.calls());
  EXPECT_EQ(ByteCount{40}, first.retained_bytes());
  EXPECT_EQ(ByteCount{10}, second.retained_bytes());
  EXPECT_EQ(ByteCount{30}, requester.retained_bytes());

  ASSERT_TRUE(requester.Release(ByteCount{30}).has_value());
  first_target.SetReclaimable(20);
  second_target.SetReclaimable(10);
  ASSERT_TRUE(second.Reserve(ByteCount{40}).has_value());
  first_target.SetReclaimable(20);
  second_target.SetReclaimable(20);
  ASSERT_TRUE(requester.Reserve(ByteCount{21}).has_value());
  EXPECT_EQ(1U, first_target.calls());
  EXPECT_EQ(1U, second_target.calls());

  ASSERT_TRUE(requester.Release(ByteCount{21}).has_value());
  first_target.SetReclaimable(20);
  second_target.SetReclaimable(30);
  ASSERT_TRUE(requester.Reserve(ByteCount{71}).has_value());
  EXPECT_EQ(2U, first_target.calls());
  EXPECT_EQ(2U, second_target.calls());
  EXPECT_EQ(ByteCount{91}, storage.retained_bytes());
}

TEST(StatementTemporaryStorage, MemoryModeRejectsPressureAndSpillFailurePoisonsStorage) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory memory_factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage memory = TakeValue(memory_factory.CreateStatementStorage());
  TestSpillTarget ignored_target;
  TemporaryMemoryAccount ignored = TakeValue(memory.RegisterMemoryOwner(ignored_target));
  ignored_target.Attach(ignored);
  ignored_target.SetReclaimable(8);
  ASSERT_TRUE(ignored.Reserve(ByteCount{8}).has_value());
  const auto memory_exceeded = ignored.Reserve(ByteCount{1});
  ASSERT_FALSE(memory_exceeded.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, memory_exceeded.error().code());
  EXPECT_EQ(0U, ignored_target.calls());
  EXPECT_FALSE(memory.failed());

  const TemporaryStorageFactory file_factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage file = TakeValue(file_factory.CreateStatementStorage());
  TestSpillTarget failing_target;
  TemporaryMemoryAccount failing = TakeValue(file.RegisterMemoryOwner(failing_target));
  TemporaryMemoryAccount requester = TakeValue(file.RegisterMemoryOwner());
  failing_target.Attach(failing);
  failing_target.SetReclaimable(8);
  failing_target.FailNext(ErrorCode::kIo);
  ASSERT_TRUE(failing.Reserve(ByteCount{8}).has_value());
  const auto failed = requester.Reserve(ByteCount{1});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kIo, failed.error().code());
  EXPECT_TRUE(file.failed());
  const auto after_failure = requester.Reserve(ByteCount{1});
  ASSERT_FALSE(after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, after_failure.error().code());
  const auto registration_after_failure = file.RegisterMemoryOwner();
  ASSERT_FALSE(registration_after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, registration_after_failure.error().code());
  const auto file_after_failure = file.AcquireFileLease();
  ASSERT_FALSE(file_after_failure.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, file_after_failure.error().code());
  EXPECT_TRUE(failing.Release(ByteCount{8}).has_value());
  EXPECT_EQ(ByteCount{0}, file.retained_bytes());
}

TEST(StatementTemporaryStorage, RejectsInvalidAccountingAndMalformedSpillTargets) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));

  StatementTemporaryStorage accounting = TakeValue(factory.CreateStatementStorage());
  TemporaryMemoryAccount owner = TakeValue(accounting.RegisterMemoryOwner());
  ASSERT_TRUE(owner.Reserve(ByteCount{1}).has_value());
  const auto underflow = owner.Release(ByteCount{2});
  ASSERT_FALSE(underflow.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, underflow.error().code());
  const auto overflow = owner.Reserve(ByteCount{std::numeric_limits<std::size_t>::max()});
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, overflow.error().code());
  const auto no_spill_target = owner.Reserve(ByteCount{8});
  ASSERT_FALSE(no_spill_target.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, no_spill_target.error().code());
  EXPECT_EQ(ByteCount{1}, owner.retained_bytes());
  EXPECT_EQ(ByteCount{1}, accounting.retained_bytes());
  EXPECT_FALSE(accounting.failed());

  StatementTemporaryStorage malformed = TakeValue(factory.CreateStatementStorage());
  TestSpillTarget malformed_target;
  TemporaryMemoryAccount spillable = TakeValue(malformed.RegisterMemoryOwner(malformed_target));
  TemporaryMemoryAccount requester = TakeValue(malformed.RegisterMemoryOwner());
  malformed_target.Attach(spillable);
  ASSERT_TRUE(spillable.Reserve(ByteCount{8}).has_value());
  malformed_target.SetReclaimable(9);
  const auto failed = requester.Reserve(ByteCount{1});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kInternal, failed.error().code());
  EXPECT_EQ(0U, malformed_target.calls());
  EXPECT_TRUE(malformed.failed());
  EXPECT_TRUE(spillable.Release(ByteCount{8}).has_value());
}

TEST(StatementTemporaryStorage, PoisonsStorageWhenPressureSpillMakesNoProgress) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  TestSpillTarget target;
  TemporaryMemoryAccount spillable = TakeValue(storage.RegisterMemoryOwner(target));
  TemporaryMemoryAccount requester = TakeValue(storage.RegisterMemoryOwner());
  target.Attach(spillable);
  ASSERT_TRUE(spillable.Reserve(ByteCount{8}).has_value());
  target.SetReclaimable(8);
  target.SucceedWithoutReleaseNext();

  const auto failed = requester.Reserve(ByteCount{1});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kInternal, failed.error().code());
  EXPECT_EQ(1U, target.calls());
  EXPECT_TRUE(storage.failed());
  EXPECT_TRUE(spillable.Release(ByteCount{8}).has_value());
}

TEST(StatementTemporaryStorage, ExcludesActiveTargetDuringReentrantPressure) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  TestSpillTarget first_target;
  TestSpillTarget second_target;
  TemporaryMemoryAccount first = TakeValue(storage.RegisterMemoryOwner(first_target));
  TemporaryMemoryAccount second = TakeValue(storage.RegisterMemoryOwner(second_target));
  TemporaryMemoryAccount requester = TakeValue(storage.RegisterMemoryOwner());
  first_target.Attach(first);
  second_target.Attach(second);
  ASSERT_TRUE(first.Reserve(ByteCount{4}).has_value());
  ASSERT_TRUE(second.Reserve(ByteCount{4}).has_value());
  first_target.SetReclaimable(4);
  first_target.ReserveDuringNextSpill(requester, ByteCount{1});
  second_target.SetReclaimable(3);
  second_target.FailNext(ErrorCode::kIo);

  const auto failed = requester.Reserve(ByteCount{1});
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(ErrorCode::kInternal, failed.error().code());
  EXPECT_EQ(1U, first_target.calls());
  EXPECT_EQ(1U, second_target.calls());
  ASSERT_TRUE(first_target.nested_error().has_value());
  EXPECT_EQ(ErrorCode::kIo, *first_target.nested_error());
  EXPECT_TRUE(storage.failed());
  EXPECT_EQ(ByteCount{4}, storage.retained_bytes());
}

TEST(StatementTemporaryStorage, PreservesRegistrationsAndLeasesAcrossMoves) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, {}));
  StatementTemporaryStorage original = TakeValue(factory.CreateStatementStorage());
  const ByteCount empty_overhead = original.overhead_bytes();
  TemporaryMemoryAccount account = TakeValue(original.RegisterMemoryOwner());
  ASSERT_TRUE(account.Reserve(ByteCount{3}).has_value());
  EXPECT_GT(original.overhead_bytes(), empty_overhead);

  StatementTemporaryStorage moved{std::move(original)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(original.valid());
  EXPECT_TRUE(moved.valid());
  EXPECT_EQ(ByteCount{3}, moved.retained_bytes());
  const auto moved_from_registration = original.RegisterMemoryOwner();
  ASSERT_FALSE(moved_from_registration.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, moved_from_registration.error().code());

  TemporaryMemoryAccount moved_account{std::move(account)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(account.valid());
  EXPECT_TRUE(moved_account.valid());
  EXPECT_EQ(ByteCount{3}, moved_account.retained_bytes());
  TemporaryMemoryAccount replacement = TakeValue(moved.RegisterMemoryOwner());
  ASSERT_TRUE(replacement.Reserve(ByteCount{2}).has_value());
  replacement = std::move(moved_account);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(moved_account.valid());
  EXPECT_EQ(ByteCount{3}, replacement.retained_bytes());
  EXPECT_EQ(ByteCount{3}, moved.retained_bytes());

  TemporaryFileLease first = TakeValue(moved.AcquireFileLease());
  TemporaryFileLease second = TakeValue(moved.AcquireFileLease());
  TemporaryFileLease moved_lease{std::move(first)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(first.valid());
  EXPECT_TRUE(moved_lease.valid());
  second = std::move(moved_lease);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_FALSE(moved_lease.valid());
  EXPECT_TRUE(second.valid());
  EXPECT_EQ(1U, moved.live_file_count());
}

TEST(StatementTemporaryStorage, BoundsAndReleasesTemporaryFileLeases) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager, {}));
  StatementTemporaryStorage storage = TakeValue(factory.CreateStatementStorage());
  EXPECT_TRUE(storage.file_spill_enabled());
  EXPECT_EQ(0U, storage.live_file_count());
  EXPECT_EQ(0U, storage.peak_live_file_count());

  const TemporaryFileLease first = TakeValue(storage.AcquireFileLease());
  EXPECT_TRUE(first.valid());
  std::optional<TemporaryFileLease> second;
  second.emplace(TakeValue(storage.AcquireFileLease()));
  EXPECT_EQ(2U, storage.live_file_count());
  EXPECT_EQ(2U, storage.peak_live_file_count());
  const auto exceeded = storage.AcquireFileLease();
  ASSERT_FALSE(exceeded.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, exceeded.error().code());

  second.reset();
  EXPECT_EQ(1U, storage.live_file_count());
  second.emplace(TakeValue(storage.AcquireFileLease()));
  EXPECT_EQ(2U, storage.live_file_count());

  const TemporaryStorageFactory memory_factory = TakeValue(TemporaryStorageFactory::Create(
      vfs, *pager, TemporaryStorageOptions{.mode = TemporaryStoreMode::kMemory}));
  StatementTemporaryStorage memory = TakeValue(memory_factory.CreateStatementStorage());
  const auto memory_file = memory.AcquireFileLease();
  ASSERT_FALSE(memory_file.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, memory_file.error().code());
}

TEST(StatementTemporaryStorage, TracksBoundedFileWorkingSetsBeyondTheSpillBudget) {
  test::WritePagerFixedVfs vfs;
  const std::unique_ptr<Pager> pager = TakeValue(Pager::Open(vfs, test::kWritePagerInputPath));
  const TemporaryStorageFactory file_factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kFile,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage file = TakeValue(file_factory.CreateStatementStorage());
  TemporaryMemoryAccount file_owner = TakeValue(file.RegisterMemoryOwner());
  ASSERT_TRUE(file_owner.Reserve(ByteCount{8}).has_value());
  ASSERT_TRUE(file_owner.ReserveWorkingSet(ByteCount{5}).has_value());
  EXPECT_EQ(ByteCount{13}, file_owner.retained_bytes());
  EXPECT_EQ(ByteCount{13}, file.retained_bytes());
  EXPECT_EQ(ByteCount{13}, file.peak_retained_bytes());
  EXPECT_TRUE(file_owner.Release(ByteCount{13}).has_value());

  const TemporaryStorageFactory memory_factory =
      TakeValue(TemporaryStorageFactory::Create(vfs, *pager,
                                                TemporaryStorageOptions{
                                                    .mode = TemporaryStoreMode::kMemory,
                                                    .sorter_memory_threshold = ByteCount{8},
                                                }));
  StatementTemporaryStorage memory = TakeValue(memory_factory.CreateStatementStorage());
  TemporaryMemoryAccount memory_owner = TakeValue(memory.RegisterMemoryOwner());
  const auto rejected = memory_owner.ReserveWorkingSet(ByteCount{1});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(ErrorCode::kTooLarge, rejected.error().code());
  EXPECT_EQ(ByteCount{0}, memory.retained_bytes());
}

}  // namespace
}  // namespace modern_sqlite
