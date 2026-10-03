#include "modern_sqlite/base/bytes.hpp"

namespace modern_sqlite {

ByteBuffer::ByteBuffer(ByteCount size) : storage_(size.value(), std::byte{0}) {}

ByteBuffer ByteBuffer::CopyOf(ByteView value) {
  return ByteBuffer{std::vector<std::byte>(value.begin(), value.end())};
}

ByteBuffer ByteBuffer::Clone() const { return CopyOf(view()); }

}  // namespace modern_sqlite
