#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "src/runtime/sql_value_access.hpp"

namespace {

std::atomic<bool> fail_allocations = false;
std::atomic<std::size_t> allocation_count = 0;
std::atomic<std::size_t> live_allocations = 0;

[[nodiscard]] void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (fail_allocations.load(std::memory_order_relaxed)) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (fail_allocations.load(std::memory_order_relaxed)) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? alignment : size) == 0) {
    live_allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
  }
  throw std::bad_alloc{};
}

void Deallocate(void* memory) noexcept {
  if (memory != nullptr) {
    live_allocations.fetch_sub(1, std::memory_order_relaxed);
    std::free(memory);
  }
}

[[nodiscard]] bool CheckInlineFactoriesAndMoves() {
  using namespace modern_sqlite;
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  bool correct = false;
  {
    fail_allocations.store(true, std::memory_order_relaxed);
    SqlValue source = SqlValue::Text("inline");
    SqlValue clone = source.Clone();
    SqlValue moved{std::move(source)};
    SqlValue destination = SqlValue::Integer(1);
    destination = std::move(moved);
    correct = source.type() == SqlValueType::kNull && moved.type() == SqlValueType::kNull &&
              destination.text_value()->bytes() == "inline" &&
              clone.text_value()->bytes() == "inline" &&
              destination.text_value()->data() != clone.text_value()->data();
    fail_allocations.store(false, std::memory_order_relaxed);
  }
  return correct && live_before == live_allocations.load(std::memory_order_relaxed);
}

[[nodiscard]] bool CheckRetainedTransitions() {
  using namespace modern_sqlite;
  const std::string text(512, 't');
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  bool correct = false;
  {
    SqlValue value;
    allocation_count.store(0, std::memory_order_relaxed);
    const auto copied = SqlValueAccess::CopyText(value, Utf8View{text});
    if (!copied.has_value() || allocation_count.load(std::memory_order_relaxed) != 1U) {
      return false;
    }
    const auto* address = AsBytes(value.text_value()->bytes()).data();
    const std::size_t capacity = value.owned_capacity_bytes();
    allocation_count.store(0, std::memory_order_relaxed);
    fail_allocations.store(true, std::memory_order_relaxed);
    SqlValueAccess::SetInteger(value, 3);
    SqlValueAccess::SetReal(value, 1.25);
    SqlValueAccess::SetNull(value);
    const auto blob = SqlValueAccess::CopyBlob(value, AsBytes("blob"));
    correct = blob.has_value() && value.blob_value()->data() == address &&
              AsStringView(*value.blob_value()) == "blob";
    const auto empty = SqlValueAccess::CopyText(value, Utf8View{});
    correct = correct && empty.has_value() && value.text_value()->empty() &&
              value.owned_capacity_bytes() == capacity &&
              allocation_count.load(std::memory_order_relaxed) == 0U;
    SqlValueAccess::Release(value);
    correct = correct && value.type() == SqlValueType::kNull && value.owned_capacity_bytes() == 0U;
    fail_allocations.store(false, std::memory_order_relaxed);
  }
  return correct && live_before == live_allocations.load(std::memory_order_relaxed);
}

[[nodiscard]] bool CheckStrongCopyFailures() {
  using namespace modern_sqlite;
  const std::string text(512, 't');
  for (const bool blob : {false, true}) {
    const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
    bool correct = false;
    {
      SqlValue value =
          blob ? SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(text))) : SqlValue::Text(text);
      const ByteView source = blob ? *value.blob_value() : AsBytes(value.text_value()->bytes());
      const auto* address = source.data();
      const std::size_t capacity = value.owned_capacity_bytes();
      const std::size_t owned_live = live_allocations.load(std::memory_order_relaxed);
      fail_allocations.store(true, std::memory_order_relaxed);
      const auto rejected = blob ? SqlValueAccess::CopyBlob(value, source)
                                 : SqlValueAccess::CopyText(value, Utf8View{AsStringView(source)});
      fail_allocations.store(false, std::memory_order_relaxed);
      const ByteView preserved = blob ? *value.blob_value() : AsBytes(value.text_value()->bytes());
      correct = !rejected.has_value() && rejected.error().code() == ErrorCode::kOutOfMemory &&
                preserved.data() == address && AsStringView(preserved) == text &&
                value.owned_capacity_bytes() == capacity &&
                live_allocations.load(std::memory_order_relaxed) == owned_live;
      const auto retried = SqlValueAccess::CopyBlob(value, preserved);
      correct = correct && retried.has_value() && AsStringView(*value.blob_value()) == text;
    }
    if (!correct || live_before != live_allocations.load(std::memory_order_relaxed)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CheckAliasedGrowFailure() {
  using namespace modern_sqlite;
  ByteBuffer storage = ByteBuffer::CopyOf(AsBytes("payload"));
  const auto* address = storage.view().data();
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  fail_allocations.store(true, std::memory_order_relaxed);
  const auto rejected = SqlValueAccess::RetainBytes(storage, storage.view(), ByteCount{2});
  fail_allocations.store(false, std::memory_order_relaxed);
  if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
      address != storage.view().data() || AsStringView(storage.view()) != "payload" ||
      live_before != live_allocations.load(std::memory_order_relaxed)) {
    return false;
  }
  const auto retried = SqlValueAccess::RetainBytes(storage, storage.view(), ByteCount{2});
  return retried.has_value() && AsStringView(*retried) == "payload" &&
         storage.size() == ByteCount{9} &&
         live_before == live_allocations.load(std::memory_order_relaxed);
}

[[nodiscard]] bool CheckRetainedGrowFailure() {
  using namespace modern_sqlite;
  const std::string text(512, 't');
  SqlValue value;
  if (!SqlValueAccess::CopyText(value, Utf8View{"short"}).has_value()) {
    return false;
  }
  const auto* address = value.text_value()->data();
  const std::size_t capacity = value.owned_capacity_bytes();
  const std::size_t live_before = live_allocations.load(std::memory_order_relaxed);
  fail_allocations.store(true, std::memory_order_relaxed);
  const auto rejected = SqlValueAccess::CopyText(value, Utf8View{text});
  fail_allocations.store(false, std::memory_order_relaxed);
  return !rejected.has_value() && rejected.error().code() == ErrorCode::kOutOfMemory &&
         value.text_value()->bytes() == "short" && value.text_value()->data() == address &&
         value.owned_capacity_bytes() == capacity &&
         live_before == live_allocations.load(std::memory_order_relaxed);
}

[[nodiscard]] bool CheckStaticBorrowCopies() {
  using namespace modern_sqlite;
  const std::string text(512, 't');
  for (const bool blob : {false, true}) {
    const SqlValue owner =
        blob ? SqlValue::Blob(ByteBuffer::CopyOf(AsBytes(text))) : SqlValue::Text(text);
    SqlValue destination;
    if (!SqlValueAccess::CopyText(destination, Utf8View{text}).has_value()) {
      return false;
    }
    const std::size_t capacity = destination.owned_capacity_bytes();
    SqlValue copy;
    allocation_count.store(0, std::memory_order_relaxed);
    fail_allocations.store(true, std::memory_order_relaxed);
    SqlValueAccess::BorrowStable(destination, owner);
    const auto copied = SqlValueAccess::Copy(copy, destination);
    bool correct = copied.has_value() && SqlValueAccess::IsStaticBorrow(destination) &&
                   SqlValueAccess::IsStaticBorrow(copy) &&
                   destination.owned_capacity_bytes() == capacity &&
                   allocation_count.load(std::memory_order_relaxed) == 0U;
    bool clone_failed = false;
    try {
      const SqlValue clone = copy.Clone();
    } catch (const std::bad_alloc&) {
      clone_failed = true;
    }
    fail_allocations.store(false, std::memory_order_relaxed);
    SqlValue clone = copy.Clone();
    const ByteView original = blob ? *owner.blob_value() : AsBytes(owner.text_value()->bytes());
    const ByteView cloned = blob ? *clone.blob_value() : AsBytes(clone.text_value()->bytes());
    correct =
        correct && clone_failed && original.data() != cloned.data() && AsStringView(cloned) == text;
    SqlValueAccess::Release(copy);
    SqlValueAccess::Release(destination);
    if (!correct) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CheckOwnedCopies() {
  using namespace modern_sqlite;
  const std::string text(512, 't');
  const SqlValue source = SqlValue::Text(text);
  SqlValue destination;
  if (!SqlValueAccess::CopyText(destination, Utf8View{"old"}).has_value()) {
    return false;
  }
  const std::size_t capacity = destination.owned_capacity_bytes();
  fail_allocations.store(true, std::memory_order_relaxed);
  const auto rejected = SqlValueAccess::Copy(destination, source);
  fail_allocations.store(false, std::memory_order_relaxed);
  if (rejected.has_value() || rejected.error().code() != ErrorCode::kOutOfMemory ||
      destination.text_value()->bytes() != "old" ||
      destination.owned_capacity_bytes() != capacity || source.text_value()->bytes() != text) {
    return false;
  }
  if (!SqlValueAccess::Copy(destination, source).has_value()) {
    return false;
  }
  allocation_count.store(0, std::memory_order_relaxed);
  fail_allocations.store(true, std::memory_order_relaxed);
  const auto reused = SqlValueAccess::Copy(destination, source);
  const auto self = SqlValueAccess::Copy(destination, destination);
  fail_allocations.store(false, std::memory_order_relaxed);
  return reused.has_value() && self.has_value() &&
         allocation_count.load(std::memory_order_relaxed) == 0U &&
         destination.text_value()->data() != source.text_value()->data() &&
         destination.text_value()->bytes() == text;
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { Deallocate(memory); }
void operator delete[](void* memory) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { Deallocate(memory); }

int main() try {
  struct Case {
    const char* name;
    bool (*run)();
  };
  for (const Case test : {Case{"inline factories and moves", CheckInlineFactoriesAndMoves},
                          Case{"retained transitions", CheckRetainedTransitions},
                          Case{"strong copy failures", CheckStrongCopyFailures},
                          Case{"aliased grow failure", CheckAliasedGrowFailure},
                          Case{"retained grow failure", CheckRetainedGrowFailure},
                          Case{"static borrow copies", CheckStaticBorrowCopies},
                          Case{"owned copies", CheckOwnedCopies}}) {
    if (!test.run()) {
      std::fprintf(stderr, "SQL value allocation control failed: %s\n", test.name);
      return 1;
    }
  }
  return 0;
} catch (const std::exception& error) {
  fail_allocations.store(false, std::memory_order_relaxed);
  std::fprintf(stderr, "SQL value allocation control escaped: %s\n", error.what());
  return 1;
}
