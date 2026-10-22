#ifndef MODERN_SQLITE_STORAGE_BTREE_DATABASE_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_DATABASE_HPP_

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/pager/pager.hpp"

namespace modern_sqlite {

class BtreeDatabaseTestPeer;

namespace btree_internal {
class CursorState;
class BtreeWriterCore;
}  // namespace btree_internal

enum class BtreeCursorTripMode : std::uint8_t {
  WriteOnly,
  All,
};

// Externally serialized. The VFS outlives the database; cursors and pins close first.
class BtreeDatabase final {
 private:
  class ConstructionKey final {
    friend class BtreeDatabase;
    ConstructionKey() noexcept = default;
  };

 public:
  [[nodiscard]] static Result<std::unique_ptr<BtreeDatabase>> Open(Vfs& vfs, std::string_view path,
                                                                   PagerOptions options = {});
  [[nodiscard]] static Result<std::unique_ptr<BtreeDatabase>> OpenWritable(
      Vfs& vfs, std::string_view path, WritablePagerOptions options = {});
  [[nodiscard]] static Result<std::unique_ptr<BtreeDatabase>> OpenEphemeral(
      Vfs& services, PagerOptions options = {});
  [[nodiscard]] static Result<std::unique_ptr<BtreeDatabase>> OpenEphemeralMemory(
      Vfs& services, PagerOptions options = {});

  explicit BtreeDatabase(ConstructionKey) noexcept {}
  BtreeDatabase(const BtreeDatabase&) = delete;
  BtreeDatabase& operator=(const BtreeDatabase&) = delete;
  BtreeDatabase(BtreeDatabase&&) = delete;
  BtreeDatabase& operator=(BtreeDatabase&&) = delete;
  ~BtreeDatabase();

  [[nodiscard]] Pager& pager() noexcept;
  [[nodiscard]] const Pager& pager() const noexcept;
  [[nodiscard]] Status RollbackToSavepoint(JournalSavepointId savepoint);
  [[nodiscard]] Status TripCursors(BtreeCursorTripMode mode);
  [[nodiscard]] Status PrepareRollbackCursors(std::optional<BtreeCursorTripMode> trip_mode);

 private:
  friend class BtreeDatabaseTestPeer;
  friend class btree_internal::CursorState;
  friend class btree_internal::BtreeWriterCore;

  template <typename OpenPager>
  [[nodiscard]] static Result<std::unique_ptr<BtreeDatabase>> OpenWith(OpenPager&& open_pager);

  void RegisterCursor(btree_internal::CursorState& cursor) noexcept;
  void UnregisterCursor(btree_internal::CursorState& cursor) noexcept;
  void RelocateCursor(const btree_internal::CursorState* previous_identity,
                      btree_internal::CursorState& replacement) noexcept;
  [[nodiscard]] Status SaveCursors(PageNumber root, btree_internal::CursorState* except);
  void TripAllCursors(int sqlite_code) noexcept;
  [[nodiscard]] bool HasCursors(PageNumber root) const noexcept;

  std::unique_ptr<Pager> pager_;
  btree_internal::CursorState* cursors_ = nullptr;
};

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_DATABASE_HPP_
