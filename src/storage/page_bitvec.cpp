#include "modern_sqlite/storage/page_bitvec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <variant>

namespace modern_sqlite {
namespace {

constexpr std::size_t kNodeBytes = 512U;
constexpr std::size_t kHeaderBytes = 3U * sizeof(std::uint32_t);
constexpr std::size_t kUnionBytes = ((kNodeBytes - kHeaderBytes) / sizeof(void*)) * sizeof(void*);
constexpr std::size_t kBitmapBits = kUnionBytes * 8U;
constexpr std::size_t kHashSlots = kUnionBytes / sizeof(std::uint32_t);
constexpr std::size_t kMaximumHashEntries = kHashSlots / 2U;
constexpr std::size_t kPointerSlots = kUnionBytes / sizeof(void*);

[[nodiscard]] Error Misuse(const char* message) {
  try {
    return Error::Create(ErrorCode::kMisuse, message);
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error OutOfRange(const char* message) {
  try {
    return Error::Create(ErrorCode::kOutOfRange, message);
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  }
}

[[nodiscard]] Error Internal(const char* message) {
  try {
    return Error::Create(ErrorCode::kInternal, message);
  } catch (const std::bad_alloc&) {
    return Error::OutOfMemory();
  }
}

}  // namespace

class PageBitvecNode {
 public:
  using Bitmap = std::array<std::uint8_t, kUnionBytes>;
  using Hash = std::array<std::uint32_t, kHashSlots>;
  using Children = std::array<std::unique_ptr<PageBitvecNode>, kPointerSlots>;
  using Storage = std::variant<Bitmap, Hash, Children>;

  explicit PageBitvecNode(std::uint32_t bit_count) : size(bit_count), storage(Bitmap{}) {
    if (bit_count > kBitmapBits) {
      storage.emplace<Hash>();
    }
  }

  std::uint32_t size;
  std::uint32_t set_count = 0;
  std::uint32_t divisor = 0;
  Storage storage;
};

namespace {

struct ChildPosition {
  std::uint32_t divisor;
  std::uint32_t page;
};

[[nodiscard]] std::uint32_t ChildDivisor(std::uint32_t size) noexcept {
  const auto slots = static_cast<std::uint32_t>(kPointerSlots);
  std::uint32_t divisor = size / slots;
  if (size % slots != 0U) {
    ++divisor;
  }
  return std::max(divisor, static_cast<std::uint32_t>(kBitmapBits));
}

[[nodiscard]] bool TestNode(const PageBitvecNode& node, std::uint32_t page) noexcept {
  std::uint32_t index = page - 1U;
  const PageBitvecNode* current = &node;
  while (current->divisor != 0U) {
    const std::uint32_t child_index = index / current->divisor;
    index %= current->divisor;
    const auto* children = std::get_if<PageBitvecNode::Children>(&current->storage);
    if (children == nullptr || child_index >= children->size() ||
        (*children)[child_index] == nullptr) {
      return false;
    }
    current = (*children)[child_index].get();
  }
  if (current->size <= kBitmapBits) {
    const auto* bitmap = std::get_if<PageBitvecNode::Bitmap>(&current->storage);
    if (bitmap == nullptr) {
      return false;
    }
    const auto mask = static_cast<std::uint8_t>(1U << (index & 7U));
    return ((*bitmap)[index / 8U] & mask) != 0U;
  }
  const auto* hash = std::get_if<PageBitvecNode::Hash>(&current->storage);
  if (hash == nullptr) {
    return false;
  }
  const std::uint32_t stored = index + 1U;
  std::size_t slot = index % kHashSlots;
  while ((*hash)[slot] != 0U) {
    if ((*hash)[slot] == stored) {
      return true;
    }
    slot = (slot + 1U) % kHashSlots;
  }
  return false;
}

[[nodiscard]] Status SetNode(PageBitvecNode& node, std::uint32_t page);

[[nodiscard]] Status SetChild(PageBitvecNode::Children& children, ChildPosition position) {
  const std::uint32_t index = position.page - 1U;
  const std::size_t child_index = index / position.divisor;
  const std::uint32_t child_page = index % position.divisor + 1U;
  if (child_index >= children.size()) {
    return std::unexpected(OutOfRange("page bit is outside the recursive node"));
  }
  if (children[child_index] == nullptr) {
    try {
      children[child_index] = std::make_unique<PageBitvecNode>(position.divisor);
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::OutOfMemory());
    }
  }
  return SetNode(*children[child_index], child_page);
}

[[nodiscard]] Status Subdivide(PageBitvecNode& node, std::uint32_t new_page) {
  const auto* hash = std::get_if<PageBitvecNode::Hash>(&node.storage);
  if (hash == nullptr) {
    return std::unexpected(Internal("page bit-vector hash storage is unavailable"));
  }
  const auto old_hash = *hash;
  const std::uint32_t divisor = ChildDivisor(node.size);
  PageBitvecNode::Children children{};
  auto set = SetChild(children, ChildPosition{.divisor = divisor, .page = new_page});
  if (!set.has_value()) {
    return set;
  }
  for (const std::uint32_t page : old_hash) {
    if (page == 0U) {
      continue;
    }
    set = SetChild(children, ChildPosition{.divisor = divisor, .page = page});
    if (!set.has_value()) {
      return set;
    }
  }
  node.storage.emplace<PageBitvecNode::Children>(std::move(children));
  node.divisor = divisor;
  node.set_count = 0U;
  return {};
}

[[nodiscard]] Status SetNode(PageBitvecNode& node, std::uint32_t page) {
  if (node.divisor != 0U) {
    auto* children = std::get_if<PageBitvecNode::Children>(&node.storage);
    if (children == nullptr) {
      return std::unexpected(Internal("page bit-vector child storage is unavailable"));
    }
    return SetChild(*children, ChildPosition{.divisor = node.divisor, .page = page});
  }
  const std::uint32_t index = page - 1U;
  if (node.size <= kBitmapBits) {
    auto* bitmap = std::get_if<PageBitvecNode::Bitmap>(&node.storage);
    if (bitmap == nullptr) {
      return std::unexpected(Internal("page bit-vector bitmap storage is unavailable"));
    }
    (*bitmap)[index / 8U] |= static_cast<std::uint8_t>(1U << (index & 7U));
    return {};
  }

  auto* hash = std::get_if<PageBitvecNode::Hash>(&node.storage);
  if (hash == nullptr) {
    return std::unexpected(Internal("page bit-vector hash storage is unavailable"));
  }
  const std::uint32_t stored = index + 1U;
  std::size_t slot = index % kHashSlots;
  if ((*hash)[slot] == 0U) {
    if (node.set_count >= kHashSlots - 1U) {
      return Subdivide(node, page);
    }
  } else {
    do {
      if ((*hash)[slot] == stored) {
        return {};
      }
      slot = (slot + 1U) % kHashSlots;
    } while ((*hash)[slot] != 0U);
    if (node.set_count >= kMaximumHashEntries) {
      return Subdivide(node, page);
    }
  }
  (*hash)[slot] = stored;
  ++node.set_count;
  return {};
}

}  // namespace

Result<PageBitvec> PageBitvec::Create(std::uint32_t page_count) {
  if (page_count == 0U) {
    return std::unexpected(Misuse("page bit-vector size must be nonzero"));
  }
  try {
    return PageBitvec{page_count, std::make_unique<PageBitvecNode>(page_count)};
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::OutOfMemory());
  }
}

PageBitvec::PageBitvec(std::uint32_t page_count, std::unique_ptr<PageBitvecNode> root) noexcept
    : page_count_(page_count), root_(std::move(root)) {}

PageBitvec::PageBitvec(PageBitvec&&) noexcept = default;

PageBitvec& PageBitvec::operator=(PageBitvec&&) noexcept = default;

PageBitvec::~PageBitvec() = default;

bool PageBitvec::Test(PageNumber page_number) const noexcept {
  if (root_ == nullptr || page_number.value() == 0U || page_number.value() > page_count_) {
    return false;
  }
  return TestNode(*root_, page_number.value());
}

Status PageBitvec::Set(PageNumber page_number) {
  if (root_ == nullptr) {
    return std::unexpected(Misuse("page bit-vector is moved from"));
  }
  if (page_number.value() == 0U || page_number.value() > page_count_) {
    return std::unexpected(OutOfRange("page bit is outside the vector"));
  }
  return SetNode(*root_, page_number.value());
}

}  // namespace modern_sqlite
