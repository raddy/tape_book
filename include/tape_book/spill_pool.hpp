#pragma once
#include <cstdlib>
#include <cstring>
#include <cassert>
#include "tape_book/config.hpp"
#include "tape_book/types.hpp"

namespace tape_book {

// SpillPool: arena-based allocator for spill buffer blocks.
// Uses power-of-2 size-class free lists with intrusive nodes.
// Single-threaded — no synchronization.
template <typename PriceT, typename QtyT>
struct SpillPool {
  using level_type = LevelT<PriceT, QtyT>;

  static constexpr i32 NUM_CLASSES = 12;   // classes 0..11
  static constexpr i32 MIN_BLOCK   = 16;   // smallest block = 16 levels

  // Intrusive free-list node, stored in the first bytes of a freed block.
  // Safe because each block is >= MIN_BLOCK * sizeof(level_type) bytes.
  struct FreeNode { i32 next_offset; };  // offset into arena, -1 = end

  level_type* arena;
  i32 arena_cap;                          // total capacity in levels
  i32 watermark{0};                       // bump-allocator high-water mark
  i32 free_heads[NUM_CLASSES];            // per-class free list heads (-1 = empty)
  i32 alloc_fail_count{0};               // diagnostic: incremented on pool exhaustion

  explicit SpillPool(i32 total_cap) noexcept : arena_cap(total_cap) {
    assert(total_cap >= MIN_BLOCK);
    arena = static_cast<level_type*>(
        std::malloc(static_cast<size_t>(total_cap) * sizeof(level_type)));
    assert(arena && "SpillPool arena allocation failed");
    for (i32 i = 0; i < NUM_CLASSES; ++i) free_heads[i] = -1;
  }

  ~SpillPool() { std::free(arena); }

  SpillPool(const SpillPool&) = delete;
  SpillPool& operator=(const SpillPool&) = delete;
  SpillPool(SpillPool&&) = delete;
  SpillPool& operator=(SpillPool&&) = delete;

  // Map a requested capacity to a size-class index [0, NUM_CLASSES).
  [[nodiscard]] static TB_ALWAYS_INLINE i32 size_class(i32 cap) noexcept {
    if (cap <= MIN_BLOCK) return 0;
    // ceil(log2(cap)) - log2(MIN_BLOCK)
    int bits = 32 - __builtin_clz(static_cast<unsigned>(cap - 1));
    int cls = bits - 4;  // MIN_BLOCK = 16 = 2^4
    if (cls < 0) cls = 0;
    if (cls >= NUM_CLASSES) cls = NUM_CLASSES - 1;
    return cls;
  }

  // Actual block size (in levels) for a given class index.
  [[nodiscard]] static constexpr TB_ALWAYS_INLINE i32 class_size(i32 cls) noexcept {
    return MIN_BLOCK << cls;
  }

  // Allocate a block of at least `cap` levels. O(1).
  // Returns nullptr on pool exhaustion (increments alloc_fail_count).
  [[nodiscard]] TB_ALWAYS_INLINE level_type* allocate(i32 cap) noexcept {
    const i32 cls = size_class(cap);
    const i32 actual = class_size(cls);

    // Try free list first
    if (free_heads[cls] != -1) {
      const i32 off = free_heads[cls];
      auto* node = reinterpret_cast<FreeNode*>(arena + off);
      free_heads[cls] = node->next_offset;
      return arena + off;
    }

    // Bump allocate
    if (watermark + actual <= arena_cap) {
      level_type* ptr = arena + watermark;
      watermark += actual;
      return ptr;
    }

    // Exhausted
    ++alloc_fail_count;
    return nullptr;
  }

  // Return a block to the pool. nullptr is a no-op.
  TB_ALWAYS_INLINE void deallocate(level_type* ptr, i32 cap) noexcept {
    if (!ptr) return;
    const i32 cls = size_class(cap);
    const i32 off = static_cast<i32>(ptr - arena);
    auto* node = reinterpret_cast<FreeNode*>(ptr);
    node->next_offset = free_heads[cls];
    free_heads[cls] = off;
  }

  // Grow a block: allocate new, memcpy used entries, deallocate old.
  // If old==nullptr, just allocates (no memcpy/free).
  [[nodiscard]] TB_ALWAYS_INLINE level_type* reallocate(
      level_type* old, i32 old_cap, i32 new_cap, i32 used) noexcept {
    auto* p = allocate(new_cap);
    if (!p) return nullptr;
    if (old) {
      std::memcpy(p, old, static_cast<size_t>(used) * sizeof(level_type));
      deallocate(old, old_cap);
    }
    return p;
  }

  [[nodiscard]] i32 used_levels() const noexcept { return watermark; }
  [[nodiscard]] i32 total_levels() const noexcept { return arena_cap; }
};

}  // namespace tape_book
