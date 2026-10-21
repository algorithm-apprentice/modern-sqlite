#include "modern_sqlite/storage/btree/database.hpp"

#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace modern_sqlite {

template <typename OpenPager>
Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenWith(OpenPager&& open_pager) {
  try {
    auto database = std::make_unique<BtreeDatabase>(ConstructionKey{});
    auto pager = std::forward<OpenPager>(open_pager)();
    if (!pager.has_value()) {
      return std::unexpected(std::move(pager.error()));
    }
    database->pager_ = std::move(*pager);
    return database;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::Open(Vfs& vfs, std::string_view path,
                                                           PagerOptions options) {
  return OpenWith([&] { return Pager::Open(vfs, path, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenWritable(Vfs& vfs, std::string_view path,
                                                                   WritablePagerOptions options) {
  return OpenWith([&] { return Pager::OpenWritable(vfs, path, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenEphemeral(Vfs& services,
                                                                    PagerOptions options) {
  return OpenWith([&] { return Pager::OpenEphemeral(services, options); });
}

Result<std::unique_ptr<BtreeDatabase>> BtreeDatabase::OpenEphemeralMemory(Vfs& services,
                                                                          PagerOptions options) {
  return OpenWith([&] { return Pager::OpenEphemeralMemory(services, options); });
}

BtreeDatabase::~BtreeDatabase() = default;

Pager& BtreeDatabase::pager() noexcept {
  if (pager_ == nullptr) {
    std::terminate();
  }
  return *pager_;
}

const Pager& BtreeDatabase::pager() const noexcept {
  if (pager_ == nullptr) {
    std::terminate();
  }
  return *pager_;
}

}  // namespace modern_sqlite
