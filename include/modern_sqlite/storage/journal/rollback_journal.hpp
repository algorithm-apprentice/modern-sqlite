#ifndef MODERN_SQLITE_STORAGE_JOURNAL_ROLLBACK_JOURNAL_HPP_
#define MODERN_SQLITE_STORAGE_JOURNAL_ROLLBACK_JOURNAL_HPP_

#include <memory>
#include <string_view>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/journal/journal.hpp"

namespace modern_sqlite {

struct RollbackJournalOptions {
  ByteCount legacy_page_size{4096};
  DirectorySync delete_directory_sync{DirectorySync::kNo};

  constexpr bool operator==(const RollbackJournalOptions&) const noexcept = default;
};

[[nodiscard]] Result<ByteCount> ResolveRollbackJournalSectorSize(const FileProperties& properties);

class RollbackJournal final : public JournalBackend {
 private:
  struct ConstructionKey final {};
  class Impl;

 public:
  [[nodiscard]] static Result<std::unique_ptr<RollbackJournal>> Create(
      Vfs& vfs, std::string_view database_path, FileProperties database_properties,
      RollbackJournalOptions options = {});

  RollbackJournal(ConstructionKey, std::unique_ptr<Impl> impl) noexcept;
  RollbackJournal(const RollbackJournal&) = delete;
  RollbackJournal& operator=(const RollbackJournal&) = delete;
  RollbackJournal(RollbackJournal&&) = delete;
  RollbackJournal& operator=(RollbackJournal&&) = delete;
  ~RollbackJournal() override;

  [[nodiscard]] std::string_view database_path() const noexcept;
  [[nodiscard]] std::string_view journal_path() const noexcept;

 protected:
  [[nodiscard]] Status DoBegin(JournalTransactionInfo info) override;
  [[nodiscard]] Status DoAppendTransactionPage(JournalPageImage image) override;
  [[nodiscard]] Status DoAppendSavepointPage(JournalPageImage image) override;
  [[nodiscard]] Status DoCreateSavepoint(JournalSavepoint savepoint) override;
  [[nodiscard]] Status DoReleaseSavepoint(JournalSavepointId savepoint,
                                          bool rewind_subjournal) override;
  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenSavepointPlayback(
      JournalSavepoint savepoint) override;
  [[nodiscard]] Status DoCompleteSavepointPlayback(JournalSavepoint savepoint) override;
  [[nodiscard]] Status DoSync() override;
  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenTransactionPlayback() override;
  [[nodiscard]] Status DoPrepareHotRecovery() override;
  [[nodiscard]] Result<std::unique_ptr<JournalPlayback>> DoOpenHotPlayback() override;
  [[nodiscard]] Status DoFinalizeCommit() override;
  [[nodiscard]] Status DoFinalizeRollback() override;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_JOURNAL_ROLLBACK_JOURNAL_HPP_
