#ifndef MODERN_SQLITE_VM_INVOCATION_EPOCH_HPP_
#define MODERN_SQLITE_VM_INVOCATION_EPOCH_HPP_

#include <algorithm>
#include <cstdint>
#include <span>

namespace modern_sqlite::vm_internal {

inline void AdvanceInvocationEpoch(std::uint32_t& epoch,
                                   std::span<std::uint32_t> site_epochs) noexcept {
  if (epoch >= 0x7fff'fffeU) {
    std::ranges::fill(site_epochs, 0U);
    epoch = 0;
  }
  ++epoch;
}

}  // namespace modern_sqlite::vm_internal

#endif  // MODERN_SQLITE_VM_INVOCATION_EPOCH_HPP_
