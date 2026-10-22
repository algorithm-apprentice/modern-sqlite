#ifndef MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
#define MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/platform/vfs.hpp"
#include "modern_sqlite/storage/page_number.hpp"

namespace modern_sqlite {

class BtreeDatabase;
class BtreeDatabaseTestPeer;
class BtreeCellView;
class BtreePageGeometry;

namespace btree_internal {

inline constexpr std::size_t kSavedIndexKeyPadding = 17U;

struct CursorTripCode {
  int sqlite_code;
};

using CursorPositionData = std::variant<std::int64_t, ByteBuffer, CursorTripCode>;

enum class BtreeCursorPhase : std::uint8_t {
  Valid,
  Invalid,
  SkipNext,
  RequireSeek,
  Fault,
};

enum class CursorAccess : std::uint8_t {
  Read,
  Write,
};

enum class PayloadReadExtent : std::uint8_t {
  RequestedRange,
  CompletePayload,
};

[[nodiscard]] Result<std::optional<PageNumber>> OverflowPageCandidate(PageNumber current,
                                                                      BtreePageGeometry geometry,
                                                                      std::uint32_t page_count);
// The cell must be a validated table-leaf cell borrowing this page.
[[nodiscard]] FileOffset PayloadFileOffset(PageNumber page_number, BtreePageGeometry geometry,
                                           ByteView page, const BtreeCellView& cell) noexcept;

class CursorState {
 public:
  CursorState(const CursorState&) = delete;
  CursorState& operator=(const CursorState&) = delete;
  CursorState& operator=(CursorState&&) = delete;

  [[nodiscard]] PageNumber root_page() const noexcept { return root_page_; }
  [[nodiscard]] BtreeCursorPhase state() const noexcept { return state_; }
  [[nodiscard]] CursorAccess access() const noexcept { return access_; }
  [[nodiscard]] bool multiple() const noexcept { return multiple_; }
  [[nodiscard]] bool HasMoved() const noexcept { return state_ != BtreeCursorPhase::Valid; }
  [[nodiscard]] bool HasPosition() const noexcept {
    return state_ == BtreeCursorPhase::Valid || state_ == BtreeCursorPhase::SkipNext;
  }
  [[nodiscard]] Result<bool> RestorePosition();
  [[nodiscard]] Status CheckTripped() const noexcept;

 protected:
  CursorState(BtreeDatabase& database, PageNumber root_page, CursorAccess access) noexcept;
  CursorState(CursorState&& other) noexcept;
  virtual ~CursorState();

  void Detach() noexcept;
  void DiscardSavedPosition() noexcept;
  void InvalidateOverflowPages() noexcept { overflow_pages_valid_ = false; }
  [[nodiscard]] Status ReadPayloadRange(
      const BtreeCellView& cell, BtreePageGeometry geometry, ByteOffset offset,
      MutableByteView destination, PayloadReadExtent extent = PayloadReadExtent::RequestedRange);
  [[nodiscard]] Status SavePeers();
  [[nodiscard]] Result<std::optional<bool>> PrepareStep(bool forward);
  [[nodiscard]] virtual Status CheckPositionActive() = 0;
  [[nodiscard]] virtual Result<CursorPositionData> CapturePositionKey() = 0;
  virtual void ReleasePathPages() noexcept = 0;
  virtual void InvalidatePositionMetadata() noexcept = 0;
  [[nodiscard]] virtual Result<int> MoveToSavedKey(const CursorPositionData& key) = 0;

 private:
  friend class modern_sqlite::BtreeDatabase;
  friend class modern_sqlite::BtreeDatabaseTestPeer;

  BtreeDatabase* database_;
  CursorState* next_ = nullptr;
  CursorAccess access_;
  bool multiple_ = false;
  CursorPositionData position_data_{std::int64_t{0}};
  int skip_direction_ = 0;
  std::vector<PageNumber> overflow_pages_;
  bool overflow_pages_valid_ = false;

  [[nodiscard]] Status SavePosition();
  [[nodiscard]] Result<std::optional<PageNumber>> ReadOverflowLink(PageNumber current,
                                                                   BtreePageGeometry geometry);
  void Trip(int sqlite_code) noexcept;

 protected:
  PageNumber root_page_;
  BtreeCursorPhase state_ = BtreeCursorPhase::Invalid;
};

}  // namespace btree_internal
}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_STORAGE_BTREE_CURSOR_STATE_HPP_
