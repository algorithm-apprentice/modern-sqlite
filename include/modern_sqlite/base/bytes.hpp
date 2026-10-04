#ifndef MODERN_SQLITE_BASE_BYTES_HPP_
#define MODERN_SQLITE_BASE_BYTES_HPP_

#include <compare>
#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace modern_sqlite {

using ByteView = std::span<const std::byte>;
using MutableByteView = std::span<std::byte>;

class ByteOffset {
 public:
  constexpr ByteOffset() noexcept = default;
  constexpr explicit ByteOffset(std::size_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const ByteOffset&) const noexcept = default;

 private:
  std::size_t value_ = 0;
};

class ByteCount {
 public:
  constexpr ByteCount() noexcept = default;
  constexpr explicit ByteCount(std::size_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::size_t value() const noexcept { return value_; }

  constexpr auto operator<=>(const ByteCount&) const noexcept = default;

 private:
  std::size_t value_ = 0;
};

enum class ByteError {
  kOutOfRange,
  kDestinationTooSmall,
};

[[nodiscard]] inline ByteView AsBytes(std::string_view value) noexcept {
  if (value.empty()) {
    return {};
  }
  return std::as_bytes(std::span<const char>{value.data(), value.size()});
}

[[nodiscard]] inline MutableByteView AsWritableBytes(std::span<char> value) noexcept {
  if (value.empty()) {
    return {};
  }
  return std::as_writable_bytes(value);
}

[[nodiscard]] inline std::string_view AsStringView(ByteView value) noexcept {
  if (value.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] inline std::expected<ByteView, ByteError> Slice(ByteView value, ByteOffset offset,
                                                              ByteCount count) noexcept {
  if (offset.value() > value.size() || count.value() > value.size() - offset.value()) {
    return std::unexpected(ByteError::kOutOfRange);
  }
  return value.subspan(offset.value(), count.value());
}

[[nodiscard]] inline std::expected<MutableByteView, ByteError> Slice(MutableByteView value,
                                                                     ByteOffset offset,
                                                                     ByteCount count) noexcept {
  if (offset.value() > value.size() || count.value() > value.size() - offset.value()) {
    return std::unexpected(ByteError::kOutOfRange);
  }
  return value.subspan(offset.value(), count.value());
}

class ByteBuffer {
 public:
  ByteBuffer() = default;
  explicit ByteBuffer(ByteCount size);

  ByteBuffer(const ByteBuffer&) = delete;
  ByteBuffer& operator=(const ByteBuffer&) = delete;
  ByteBuffer(ByteBuffer&&) noexcept = default;
  ByteBuffer& operator=(ByteBuffer&&) noexcept = default;
  ~ByteBuffer() = default;

  [[nodiscard]] static ByteBuffer CopyOf(ByteView value);
  [[nodiscard]] ByteBuffer Clone() const;

  [[nodiscard]] ByteCount size() const noexcept { return ByteCount{storage_.size()}; }
  [[nodiscard]] ByteCount capacity() const noexcept { return ByteCount{storage_.capacity()}; }
  [[nodiscard]] bool empty() const noexcept { return storage_.empty(); }
  [[nodiscard]] ByteView view() const noexcept { return ByteView{storage_}; }
  [[nodiscard]] MutableByteView mutable_view() noexcept { return MutableByteView{storage_}; }

 private:
  explicit ByteBuffer(std::vector<std::byte> storage) noexcept : storage_(std::move(storage)) {}

  std::vector<std::byte> storage_;
};

[[nodiscard]] inline std::expected<void, ByteError> CopyBytes(MutableByteView destination,
                                                              ByteView source) noexcept {
  if (destination.size() < source.size()) {
    return std::unexpected(ByteError::kDestinationTooSmall);
  }
  if (!source.empty()) {
    std::memmove(destination.data(), source.data(), source.size());
  }
  return {};
}

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_BASE_BYTES_HPP_
