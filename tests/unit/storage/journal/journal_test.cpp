#include "modern_sqlite/storage/journal/journal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modern_sqlite {
namespace {

using PageBytes = std::array<std::byte, 512>;

struct PlaybackRecord {
  PageNumber page_number;
  std::vector<std::byte> bytes;
};

struct PlaybackScript {
  std::optional<JournalPlaybackInfo> info;
  std::vector<PlaybackRecord> records;
  std::optional<std::size_t> failing_next;
  ErrorCode next_error = ErrorCode::kIo;
};

[[nodiscard]] PlaybackRecord Record(std::uint32_t page_number, std::byte value,
                                    std::size_t size = 512) {
  return PlaybackRecord{
      .page_number = PageNumber{page_number},
      .bytes = std::vector<std::byte>(size, value),
  };
}

class ScriptedPlayback final : public JournalPlayback {
 public:
  ScriptedPlayback(PlaybackScript script, std::vector<std::string>& trace)
      : script_(std::move(script)), trace_(&trace) {}

  [[nodiscard]] std::optional<JournalPlaybackInfo> info() const noexcept override {
    return script_.info;
  }

  [[nodiscard]] Result<std::optional<JournalPageImage>> Next() override {
    if (script_.failing_next.has_value() && next_index_ == *script_.failing_next) {
      trace_->emplace_back("next-error");
      script_.failing_next.reset();
      return std::unexpected(Error::Create(script_.next_error, "injected playback failure"));
    }
    if (next_index_ == script_.records.size()) {
      trace_->emplace_back("next-end");
      return std::optional<JournalPageImage>{};
    }

    const PlaybackRecord& record = script_.records[next_index_++];
    trace_->push_back("next:" + std::to_string(record.page_number.value()));
    return std::optional<JournalPageImage>{JournalPageImage{
        .page_number = record.page_number,
        .bytes = record.bytes,
    }};
  }

 private:
  PlaybackScript script_;
  std::vector<std::string>* trace_;
  std::size_t next_index_ = 0;
};

class RecordingBackend final : public JournalBackend {
 public:
  std::vector<std::string> trace;
  std::vector<PageNumber> transaction_pages;
  std::vector<PageNumber> savepoint_pages;
  std::optional<std::string> failing_event;
  int failures_remaining = 0;
  ErrorCode failure_code = ErrorCode::kIo;
  PlaybackScript transaction_playback{
      .info =
          JournalPlaybackInfo{
              .kind = JournalPlaybackKind::kTransactionRollback,
              .page_size = ByteCount{512},
              .original_page_count = 8,
          },
      .records = {},
      .failing_next = std::nullopt,
      .next_error = ErrorCode::kIo,
  };
  PlaybackScript savepoint_playback{
      .info =
          JournalPlaybackInfo{
              .kind = JournalPlaybackKind::kSavepointRollback,
              .page_size = ByteCount{512},
              .original_page_count = 8,
          },
      .records = {},
      .failing_next = std::nullopt,
      .next_error = ErrorCode::kIo,
  };
  PlaybackScript hot_playback{
      .info = std::nullopt,
      .records = {},
      .failing_next = std::nullopt,
      .next_error = ErrorCode::kIo,
  };
  bool null_savepoint_playback = false;

 protected:
  [[nodiscard]] Status DoBegin(JournalTransactionInfo) override { return Event("begin"); }

  [[nodiscard]] Status DoAppendTransactionPage(JournalPageImage image) override {
    auto result = Event("append-main");
    if (result.has_value()) {
      transaction_pages.push_back(image.page_number);
    }
    return result;
  }

  [[nodiscard]] Status DoAppendSavepointPage(JournalPageImage image) override {
    auto result = Event("append-savepoint");
    if (result.has_value()) {
      savepoint_pages.push_back(image.page_number);
    }
    return result;
  }

  [[nodiscard]] Status DoCreateSavepoint(JournalSavepoint) override {
    return Event("create-savepoint");
  }

  [[nodiscard]] Status DoReleaseSavepoint(JournalSavepointId) override {
    return Event("release-savepoint");
  }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenSavepointPlayback(
      JournalSavepoint savepoint) override {
    auto result = Event("open-savepoint-playback");
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    if (null_savepoint_playback) {
      return std::unique_ptr<JournalPlayback>{};
    }
    PlaybackScript script = savepoint_playback;
    if (script.info.has_value()) {
      script.info->original_page_count = savepoint.original_page_count;
    }
    return std::make_unique<ScriptedPlayback>(std::move(script), trace);
  }

  [[nodiscard]] Status DoCompleteSavepointPlayback(JournalSavepoint) override {
    return Event("complete-savepoint-playback");
  }

  [[nodiscard]] Status DoSync() override { return Event("sync"); }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenTransactionPlayback() override {
    auto result = Event("open-transaction-playback");
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    return std::make_unique<ScriptedPlayback>(transaction_playback, trace);
  }

  [[nodiscard]] Status DoPrepareHotRecovery() override { return Event("prepare-hot"); }

  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenHotPlayback() override {
    auto result = Event("open-hot-playback");
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
    return std::make_unique<ScriptedPlayback>(hot_playback, trace);
  }

  [[nodiscard]] Status DoFinalizeCommit() override { return Event("finalize-commit"); }

  [[nodiscard]] Status DoFinalizeRollback() override { return Event("finalize-rollback"); }

 private:
  [[nodiscard]] Status Event(std::string_view name) {
    trace.emplace_back(name);
    if (failing_event == name && failures_remaining > 0) {
      --failures_remaining;
      return std::unexpected(Error::Create(failure_code, "injected backend failure"));
    }
    return {};
  }
};

class RecordingTarget final : public JournalRecoveryTarget {
 public:
  explicit RecordingTarget(std::vector<std::string>& trace) : trace_(&trace) {}

  std::vector<PageNumber> restored_pages;
  std::optional<std::string> failing_event;
  int failures_remaining = 0;
  ErrorCode failure_code = ErrorCode::kIo;

  [[nodiscard]] Status PreparePlayback(JournalPlaybackInfo) override {
    return Event("target-prepare");
  }

  [[nodiscard]] Status ResizeDatabase(std::uint32_t page_count) override {
    return Event("target-resize:" + std::to_string(page_count));
  }

  [[nodiscard]] Status RestorePage(JournalPageImage image) override {
    auto result = Event("target-restore:" + std::to_string(image.page_number.value()));
    if (result.has_value()) {
      restored_pages.push_back(image.page_number);
    }
    return result;
  }

  [[nodiscard]] Status SyncDatabase() override { return Event("target-sync"); }

  [[nodiscard]] Status CompletePlayback(JournalPlaybackInfo) override {
    return Event("target-complete");
  }

 private:
  [[nodiscard]] Status Event(std::string_view name) {
    trace_->emplace_back(name);
    if (failing_event == name && failures_remaining > 0) {
      --failures_remaining;
      return std::unexpected(Error::Create(failure_code, "injected target failure"));
    }
    return {};
  }

  std::vector<std::string>* trace_;
};

[[nodiscard]] std::unique_ptr<JournalTransaction> BeginTransaction(
    RecordingBackend& backend, std::uint32_t original_page_count = 8,
    ByteCount page_size = ByteCount{512}, ByteCount sector_size = ByteCount{512}) {
  auto begun = JournalTransaction::Begin(backend, JournalTransactionInfo{
                                                      .page_size = page_size,
                                                      .sector_size = sector_size,
                                                      .original_page_count = original_page_count,
                                                  });
  if (!begun.has_value()) {
    return nullptr;
  }
  return std::move(*begun);
}

[[nodiscard]] JournalPageImage Image(std::uint32_t page_number, const PageBytes& bytes) {
  return JournalPageImage{.page_number = PageNumber{page_number}, .bytes = bytes};
}

[[nodiscard]] std::size_t EventCount(const RecordingBackend& backend, std::string_view event) {
  return static_cast<std::size_t>(std::ranges::count(backend.trace, event));
}

TEST(JournalTransaction, ValidatesTransactionOptionsBeforeOpeningTheBackend) {
  RecordingBackend backend;

  const auto bad_page = JournalTransaction::Begin(backend, JournalTransactionInfo{
                                                               .page_size = ByteCount{1000},
                                                               .sector_size = ByteCount{4096},
                                                               .original_page_count = 1,
                                                           });
  ASSERT_FALSE(bad_page.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, bad_page.error().code());

  const auto bad_sector = JournalTransaction::Begin(backend, JournalTransactionInfo{
                                                                 .page_size = ByteCount{4096},
                                                                 .sector_size = ByteCount{1000},
                                                                 .original_page_count = 1,
                                                             });
  ASSERT_FALSE(bad_sector.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, bad_sector.error().code());
  EXPECT_TRUE(backend.trace.empty());
}

TEST(JournalTransaction, CapturesOnceAndEnforcesCommitOrdering) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  ASSERT_TRUE(transaction->CapturePage(Image(3, page)).has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(3, page)).has_value());
  EXPECT_EQ(std::vector<PageNumber>({PageNumber{3}}), backend.transaction_pages);

  const auto unsynced_write = transaction->AuthorizeDatabaseWrite(PageNumber{3});
  ASSERT_FALSE(unsynced_write.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, unsynced_write.error().code());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{3}).has_value());

  const auto unsynced_commit = transaction->Commit();
  ASSERT_FALSE(unsynced_commit.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, unsynced_commit.error().code());
  ASSERT_TRUE(transaction->MarkDatabaseSynced().has_value());
  ASSERT_TRUE(transaction->Commit().has_value());

  EXPECT_EQ((std::vector<std::string>{"begin", "append-main", "sync", "finalize-commit"}),
            backend.trace);
  EXPECT_EQ(JournalTransactionState::kFinished, transaction->state());
}

TEST(JournalTransaction, ValidatesPageImagesAndRejectsTheLockingPage) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};
  const std::array<std::byte, 511> short_page{};
  constexpr std::uint32_t kLockingPage = 0x40000000U / 512U + 1U;

  for (const JournalPageImage image : {
           JournalPageImage{.page_number = PageNumber{0}, .bytes = page},
           JournalPageImage{.page_number = PageNumber{kLockingPage}, .bytes = page},
           JournalPageImage{.page_number = PageNumber{1}, .bytes = short_page},
       }) {
    const auto captured = transaction->CapturePage(image);
    ASSERT_FALSE(captured.has_value());
    EXPECT_EQ(ErrorCode::kMisuse, captured.error().code());
  }
  EXPECT_EQ((std::vector<std::string>{"begin"}), backend.trace);
}

TEST(JournalTransaction, MainCapturePublishesApplicableSavepointMembership) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  const PageBytes page{};

  ASSERT_TRUE(transaction->CapturePage(Image(4, page)).has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(4, page)).has_value());

  EXPECT_EQ(std::vector<PageNumber>({PageNumber{4}}), backend.transaction_pages);
  EXPECT_TRUE(backend.savepoint_pages.empty());
}

TEST(JournalTransaction, CapturesExistingAndTransactionNewPagesForLaterSavepoints) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  ASSERT_TRUE(transaction->CapturePage(Image(2, page)).has_value());
  const auto first = transaction->CreateSavepoint(8);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(2, page)).has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(2, page)).has_value());

  ASSERT_TRUE(transaction->CapturePage(Image(9, page)).has_value());
  const auto second = transaction->CreateSavepoint(9);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(9, page)).has_value());

  EXPECT_EQ(std::vector<PageNumber>({PageNumber{2}}), backend.transaction_pages);
  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}, PageNumber{9}}), backend.savepoint_pages);
}

TEST(JournalTransaction, ReleasesTheTargetSavepointAndEveryNestedSavepoint) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto first = transaction->CreateSavepoint(8);
  const auto second = transaction->CreateSavepoint(8);
  const auto third = transaction->CreateSavepoint(8);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());

  ASSERT_TRUE(transaction->ReleaseSavepoint(*second).has_value());
  EXPECT_EQ(1U, transaction->savepoint_count());

  const auto stale = transaction->ReleaseSavepoint(*third);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, stale.error().code());
  EXPECT_EQ(1U, EventCount(backend, "release-savepoint"));
}

TEST(JournalTransaction, RejectsALiveForeignSavepointWithoutBackendIo) {
  RecordingBackend first_backend;
  RecordingBackend second_backend;
  std::unique_ptr<JournalTransaction> first = BeginTransaction(first_backend);
  std::unique_ptr<JournalTransaction> second = BeginTransaction(second_backend);
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);
  const auto first_savepoint = first->CreateSavepoint(8);
  const auto second_savepoint = second->CreateSavepoint(8);
  ASSERT_TRUE(first_savepoint.has_value());
  ASSERT_TRUE(second_savepoint.has_value());
  ASSERT_EQ(first_savepoint->value(), second_savepoint->value());

  const auto released = second->ReleaseSavepoint(*first_savepoint);

  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, released.error().code());
  EXPECT_EQ(1U, second->savepoint_count());
  EXPECT_EQ(0U, EventCount(second_backend, "release-savepoint"));
}

TEST(JournalTransaction, RejectsAStaleSavepointFromAPriorTransactionWithoutBackendIo) {
  RecordingBackend backend;
  std::optional<JournalSavepointId> stale_savepoint;
  {
    std::unique_ptr<JournalTransaction> first = BeginTransaction(backend);
    ASSERT_NE(nullptr, first);
    const auto savepoint = first->CreateSavepoint(8);
    ASSERT_TRUE(savepoint.has_value());
    stale_savepoint = *savepoint;
    RecordingTarget target(backend.trace);
    ASSERT_TRUE(first->Rollback(target).has_value());
  }

  std::unique_ptr<JournalTransaction> second = BeginTransaction(backend);
  ASSERT_NE(nullptr, second);
  const auto current_savepoint = second->CreateSavepoint(8);
  ASSERT_TRUE(current_savepoint.has_value());
  ASSERT_EQ(stale_savepoint->value(), current_savepoint->value());
  const std::size_t release_count = EventCount(backend, "release-savepoint");

  const auto released = second->ReleaseSavepoint(*stale_savepoint);

  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, released.error().code());
  EXPECT_EQ(1U, second->savepoint_count());
  EXPECT_EQ(release_count, EventCount(backend, "release-savepoint"));
}

TEST(JournalTransaction, ReleaseFailureRetainsSavepointsAndRequiresRollback) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto first = transaction->CreateSavepoint(8);
  const auto second = transaction->CreateSavepoint(8);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  backend.failing_event = "release-savepoint";
  backend.failures_remaining = 1;

  const auto released = transaction->ReleaseSavepoint(*first);

  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(ErrorCode::kIo, released.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  EXPECT_EQ(2U, transaction->savepoint_count());
  backend.failing_event.reset();
  RecordingTarget target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(target).has_value());
}

TEST(JournalTransaction, RequiresEveryOldPageInALargeSectorBeforeWriting) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction =
      BeginTransaction(backend, 16, ByteCount{512}, ByteCount{4096});
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  ASSERT_TRUE(transaction->CapturePage(Image(3, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  const auto incomplete = transaction->AuthorizeDatabaseWrite(PageNumber{3});
  ASSERT_FALSE(incomplete.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, incomplete.error().code());

  for (std::uint32_t page_number = 1; page_number <= 8; ++page_number) {
    ASSERT_TRUE(transaction->CapturePage(Image(page_number, page)).has_value());
  }
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  EXPECT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{3}).has_value());
}

TEST(JournalTransaction, ExcludesTheLockingPageFromSectorAuthorization) {
  constexpr std::uint32_t kLockingPage = 0x40000000U / 512U + 1U;
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction =
      BeginTransaction(backend, kLockingPage + 7U, ByteCount{512}, ByteCount{4096});
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  for (std::uint32_t page_number = kLockingPage + 1U; page_number <= kLockingPage + 7U;
       ++page_number) {
    ASSERT_TRUE(transaction->CapturePage(Image(page_number, page)).has_value());
  }
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  EXPECT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{kLockingPage + 1U}).has_value());
}

TEST(JournalTransaction, SupportsRepeatedSyncEpochsWithoutSyncingSubjournalOnlyAppends) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  ASSERT_TRUE(transaction->CapturePage(Image(1, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());

  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(1, page)).has_value());
  EXPECT_FALSE(transaction->main_journal_dirty());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());

  ASSERT_TRUE(transaction->CapturePage(Image(2, page)).has_value());
  EXPECT_TRUE(transaction->main_journal_dirty());
  EXPECT_FALSE(transaction->AuthorizeDatabaseWrite(PageNumber{2}).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{2}).has_value());

  EXPECT_EQ(2U, EventCount(backend, "sync"));
  EXPECT_EQ(1U, EventCount(backend, "append-savepoint"));
}

TEST(JournalTransaction, ResizeRequiresEveryApplicableRollbackImage) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend, 10);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(11);
  ASSERT_TRUE(savepoint.has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  const std::array<PageNumber, 1> new_page{PageNumber{11}};

  const auto missing = transaction->AuthorizeDatabaseResize(new_page);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(ErrorCode::kMisuse, missing.error().code());

  const PageBytes page{};
  ASSERT_TRUE(transaction->CapturePage(Image(11, page)).has_value());
  EXPECT_FALSE(transaction->main_journal_dirty());
  EXPECT_TRUE(transaction->AuthorizeDatabaseResize(new_page).has_value());

  const std::array<PageNumber, 1> old_page{PageNumber{5}};
  EXPECT_FALSE(transaction->AuthorizeDatabaseResize(old_page).has_value());
  ASSERT_TRUE(transaction->CapturePage(Image(5, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  EXPECT_TRUE(transaction->AuthorizeDatabaseResize(old_page).has_value());
}

TEST(JournalTransaction, DatabaseFailureLatchesRollbackOnlyState) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};
  ASSERT_TRUE(transaction->CapturePage(Image(1, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());

  transaction->ReportDatabaseFailure(ErrorCode::kIo);
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  EXPECT_FALSE(transaction->MarkDatabaseSynced().has_value());
  EXPECT_FALSE(transaction->Commit().has_value());

  RecordingTarget target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(target).has_value());
  EXPECT_EQ(JournalTransactionState::kFinished, transaction->state());
}

TEST(JournalTransaction, SavepointRollbackClearsPriorDatabaseSyncAndRetainsTheTarget) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  const PageBytes page{};
  ASSERT_TRUE(transaction->CapturePage(Image(1, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
  ASSERT_TRUE(transaction->MarkDatabaseSynced().has_value());

  backend.savepoint_playback.records = {Record(1, std::byte{0x11})};
  RecordingTarget target(backend.trace);
  ASSERT_TRUE(transaction->RollbackToSavepoint(*savepoint, target).has_value());

  EXPECT_EQ(1U, transaction->savepoint_count());
  EXPECT_TRUE(transaction->database_may_be_modified());
  EXPECT_FALSE(transaction->database_synced());
  EXPECT_FALSE(transaction->Commit().has_value());
  EXPECT_EQ(0U, EventCount(backend, "target-sync"));

  ASSERT_TRUE(transaction->MarkDatabaseSynced().has_value());
  EXPECT_TRUE(transaction->Commit().has_value());
}

TEST(JournalTransaction, FullRollbackFiltersPagesAndStreamsValidRecords) {
  RecordingBackend backend;
  backend.transaction_playback.records = {
      Record(2, std::byte{0x12}),
      Record(2, std::byte{0x34}),
      Record(9, std::byte{0x56}),
      Record(3, std::byte{0x78}),
  };
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(transaction->Rollback(target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}, PageNumber{2}, PageNumber{3}}),
            target.restored_pages);
  EXPECT_EQ(1U, EventCount(backend, "target-sync"));
  EXPECT_EQ(1U, EventCount(backend, "finalize-rollback"));
}

TEST(JournalTransaction, FullRollbackNextFailureBecomesPersistentAfterTargetMutation) {
  RecordingBackend backend;
  backend.transaction_playback.records = {Record(2, std::byte{0x12})};
  backend.transaction_playback.failing_next = 1;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  RecordingTarget target(backend.trace);

  const auto rolled_back = transaction->Rollback(target);

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kIo, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kError, transaction->state());
  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}}), target.restored_pages);
  EXPECT_EQ(0U, EventCount(backend, "finalize-rollback"));
}

TEST(JournalTransaction, SavepointRollbackFiltersPagesBeforeSuppressingDuplicates) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  backend.savepoint_playback.records = {
      Record(9, std::byte{0x12}),
      Record(2, std::byte{0x34}),
      Record(2, std::byte{0x56}),
      Record(3, std::byte{0x78}),
  };
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(transaction->RollbackToSavepoint(*savepoint, target).has_value());

  EXPECT_EQ((std::vector<PageNumber>{PageNumber{2}, PageNumber{3}}), target.restored_pages);
}

TEST(JournalTransaction, SavepointRollbackOmitsDatabaseSyncAndJournalFinalization) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto outer = transaction->CreateSavepoint(8);
  const auto inner = transaction->CreateSavepoint(9);
  ASSERT_TRUE(outer.has_value());
  ASSERT_TRUE(inner.has_value());
  backend.savepoint_playback.records = {Record(2, std::byte{0x22})};
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(transaction->RollbackToSavepoint(*outer, target).has_value());

  EXPECT_EQ(1U, transaction->savepoint_count());
  EXPECT_EQ(0U, EventCount(backend, "target-sync"));
  EXPECT_EQ(0U, EventCount(backend, "finalize-rollback"));
  EXPECT_EQ(1U, EventCount(backend, "complete-savepoint-playback"));
}

TEST(JournalTransaction, NoUsableFirstHeaderFinalizesWithoutTouchingTheTarget) {
  RecordingBackend backend;
  backend.transaction_playback.info.reset();
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(transaction->Rollback(target).has_value());

  EXPECT_TRUE(target.restored_pages.empty());
  EXPECT_EQ(0U, EventCount(backend, "target-prepare"));
  EXPECT_EQ(1U, EventCount(backend, "finalize-rollback"));
}

TEST(JournalRecovery, HotRecoverySynchronizesRestoresAndFinalizesInOrder) {
  RecordingBackend backend;
  backend.hot_playback.info = JournalPlaybackInfo{
      .kind = JournalPlaybackKind::kHotRecovery,
      .page_size = ByteCount{512},
      .original_page_count = 4,
  };
  backend.hot_playback.records = {Record(1, std::byte{0x44})};
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(RecoverHotJournal(backend, target).has_value());

  EXPECT_EQ((std::vector<std::string>{
                "prepare-hot",
                "open-hot-playback",
                "target-prepare",
                "target-resize:4",
                "next:1",
                "target-restore:1",
                "next-end",
                "target-sync",
                "target-complete",
                "finalize-rollback",
            }),
            backend.trace);
}

TEST(JournalRecovery, HotRecoveryWithoutAUsableHeaderOnlyFinalizes) {
  RecordingBackend backend;
  RecordingTarget target(backend.trace);

  ASSERT_TRUE(RecoverHotJournal(backend, target).has_value());

  EXPECT_EQ((std::vector<std::string>{
                "prepare-hot",
                "open-hot-playback",
                "finalize-rollback",
            }),
            backend.trace);
}

TEST(JournalRecovery, TargetFailureLeavesTheHotJournalUnfinalized) {
  RecordingBackend backend;
  backend.hot_playback.info = JournalPlaybackInfo{
      .kind = JournalPlaybackKind::kHotRecovery,
      .page_size = ByteCount{512},
      .original_page_count = 4,
  };
  backend.hot_playback.records = {Record(1, std::byte{0x44})};
  RecordingTarget target(backend.trace);
  target.failing_event = "target-restore:1";
  target.failures_remaining = 1;

  const auto recovered = RecoverHotJournal(backend, target);

  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(ErrorCode::kIo, recovered.error().code());
  EXPECT_EQ(0U, EventCount(backend, "finalize-rollback"));
}

TEST(JournalRecovery, PropagatesFinalizationFailureAfterSuccessfulPlayback) {
  RecordingBackend backend;
  backend.hot_playback.info = JournalPlaybackInfo{
      .kind = JournalPlaybackKind::kHotRecovery,
      .page_size = ByteCount{512},
      .original_page_count = 4,
  };
  backend.failing_event = "finalize-rollback";
  backend.failures_remaining = 1;
  RecordingTarget target(backend.trace);

  const auto recovered = RecoverHotJournal(backend, target);

  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(ErrorCode::kIo, recovered.error().code());
  EXPECT_EQ(1U, EventCount(backend, "target-complete"));
  EXPECT_EQ(1U, EventCount(backend, "finalize-rollback"));
}

TEST(JournalTransaction, AppendFailureAllowsOnlyFullRollback) {
  RecordingBackend backend;
  backend.failing_event = "append-main";
  backend.failures_remaining = 1;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};

  const auto captured = transaction->CapturePage(Image(1, page));
  ASSERT_FALSE(captured.has_value());
  EXPECT_EQ(ErrorCode::kIo, captured.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  EXPECT_FALSE(transaction->SyncJournal().has_value());

  backend.failing_event.reset();
  RecordingTarget target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(target).has_value());
}

TEST(JournalTransaction, SavepointPlaybackFailureStillAllowsFullRollback) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  backend.savepoint_playback.records = {Record(1, std::byte{0x55})};
  RecordingTarget failing_target(backend.trace);
  failing_target.failing_event = "target-restore:1";
  failing_target.failures_remaining = 1;

  const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, failing_target);
  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());

  RecordingTarget full_target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(full_target).has_value());
}

TEST(JournalTransaction, SavepointSemanticPlaybackFailureStillAllowsFullRollback) {
  constexpr std::uint32_t kLockingPage = 0x40000000U / 512U + 1U;
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  backend.savepoint_playback.records = {Record(kLockingPage, std::byte{0x55})};
  RecordingTarget failing_target(backend.trace);

  const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, failing_target);

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kInternal, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  RecordingTarget full_target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(full_target).has_value());
}

TEST(JournalTransaction, NullSavepointPlaybackStillAllowsFullRollback) {
  RecordingBackend backend;
  backend.null_savepoint_playback = true;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const auto savepoint = transaction->CreateSavepoint(8);
  ASSERT_TRUE(savepoint.has_value());
  RecordingTarget savepoint_target(backend.trace);

  const auto rolled_back = transaction->RollbackToSavepoint(*savepoint, savepoint_target);

  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kInternal, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  EXPECT_EQ(0U, EventCount(backend, "target-prepare"));
  RecordingTarget full_target(backend.trace);
  EXPECT_TRUE(transaction->Rollback(full_target).has_value());
}

TEST(JournalTransaction, FullRollbackOpenFailureIsRetryableBeforeTargetMutation) {
  RecordingBackend backend;
  backend.failing_event = "open-transaction-playback";
  backend.failures_remaining = 1;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target(backend.trace);

  const auto first = transaction->Rollback(target);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(JournalTransactionState::kFailed, transaction->state());
  EXPECT_EQ(0U, EventCount(backend, "target-prepare"));

  backend.failing_event.reset();
  EXPECT_TRUE(transaction->Rollback(target).has_value());
}

TEST(JournalTransaction, FullRollbackTargetFailureBecomesPersistent) {
  RecordingBackend backend;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target(backend.trace);
  target.failing_event = "target-resize:8";
  target.failures_remaining = 1;

  const auto rolled_back = transaction->Rollback(target);
  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(JournalTransactionState::kError, transaction->state());
  EXPECT_FALSE(transaction->Rollback(target).has_value());
}

TEST(JournalTransaction, CommitFinalizationFailureBecomesPersistent) {
  RecordingBackend backend;
  backend.failing_event = "finalize-commit";
  backend.failures_remaining = 1;
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  const PageBytes page{};
  ASSERT_TRUE(transaction->CapturePage(Image(1, page)).has_value());
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  ASSERT_TRUE(transaction->AuthorizeDatabaseWrite(PageNumber{1}).has_value());
  ASSERT_TRUE(transaction->MarkDatabaseSynced().has_value());

  const auto committed = transaction->Commit();
  ASSERT_FALSE(committed.has_value());
  EXPECT_EQ(JournalTransactionState::kError, transaction->state());
  RecordingTarget target(backend.trace);
  EXPECT_FALSE(transaction->Rollback(target).has_value());
}

TEST(JournalTransaction, RejectsInvalidSemanticPlaybackImages) {
  constexpr std::uint32_t kLockingPage = 0x40000000U / 512U + 1U;
  RecordingBackend backend;
  backend.transaction_playback.records = {Record(kLockingPage, std::byte{0x66})};
  std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
  ASSERT_NE(nullptr, transaction);
  ASSERT_TRUE(transaction->SyncJournal().has_value());
  RecordingTarget target(backend.trace);

  const auto rolled_back = transaction->Rollback(target);
  ASSERT_FALSE(rolled_back.has_value());
  EXPECT_EQ(ErrorCode::kInternal, rolled_back.error().code());
  EXPECT_EQ(JournalTransactionState::kError, transaction->state());
  EXPECT_TRUE(target.restored_pages.empty());
}

TEST(JournalTransaction, RejectsInvalidPlaybackMetadataBeforeTargetMutation) {
  constexpr std::array<JournalPlaybackInfo, 3> kInvalidMetadata{{
      JournalPlaybackInfo{
          .kind = JournalPlaybackKind::kHotRecovery,
          .page_size = ByteCount{512},
          .original_page_count = 8,
      },
      JournalPlaybackInfo{
          .kind = JournalPlaybackKind::kTransactionRollback,
          .page_size = ByteCount{1000},
          .original_page_count = 8,
      },
      JournalPlaybackInfo{
          .kind = JournalPlaybackKind::kTransactionRollback,
          .page_size = ByteCount{512},
          .original_page_count = 7,
      },
  }};

  for (const JournalPlaybackInfo info : kInvalidMetadata) {
    RecordingBackend backend;
    backend.transaction_playback.info = info;
    std::unique_ptr<JournalTransaction> transaction = BeginTransaction(backend);
    ASSERT_NE(nullptr, transaction);
    RecordingTarget target(backend.trace);

    const auto rolled_back = transaction->Rollback(target);

    ASSERT_FALSE(rolled_back.has_value());
    EXPECT_EQ(ErrorCode::kInternal, rolled_back.error().code());
    EXPECT_EQ(JournalTransactionState::kError, transaction->state());
    EXPECT_EQ(0U, EventCount(backend, "target-prepare"));
    EXPECT_EQ(0U, EventCount(backend, "finalize-rollback"));
  }
}

}  // namespace
}  // namespace modern_sqlite
