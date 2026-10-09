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

}  // namespace
}  // namespace modern_sqlite
