# tape_book

Header-only C++20 order book using a tape-based sparse array for O(1) price-level operations near the spread, with a sorted spill buffer for outlier prices.

## Features

- **Tape**: Fixed-size array indexed by `price - anchor` with 64-bit bitset tracking for fast best-price lookup
- **Spill buffer**: Dynamic sorted overflow for out-of-range prices with automatic promotion on recenter
- **Shared spill pool**: Arena-based allocator for spill buffers — multiple books share pre-allocated memory, only actively-spilling books consume pool space
- **Multi-book pool**: Manage many books across 3 capacity tiers (High/Medium/Low) with shared spill pool
- **Compact**: ~8.5 KB per book (N=1024, i32/u32). Spill metadata is inline, adjacent to tape data for cache-friendly BBO queries
- Zero dependencies, no exceptions, no RTTI

## Usage

```cpp
#include <tape_book/tape_book.hpp>

tape_book::Book32 book;               // 1024-wide tape, i32/u32
book.reset(1000);                     // set anchor price
book.set(true,  100, 50u);           // bid 50 @ 100
book.set(false, 101, 30u);           // ask 30 @ 101

// template-based side selection (constexpr)
book.set<true>(102, 25u);            // bid 25 @ 102
book.set<false>(103, 10u);           // ask 10 @ 103

auto best_bid = book.best_bid_px();   // best bid price
auto best_ask = book.best_ask_px();   // best ask price
```

### Custom spill capacity

The `max_cap` parameter controls the maximum spill buffer size per side (default: 4096). Must be a power of 2.

```cpp
using namespace tape_book;
Book<64, i32, u32> book(512);         // 64-wide tape, max 512 spill levels per side
book.reset(1000);
```

### Shared SpillPool

When managing many books, use `SpillPool` to share a single pre-allocated arena across all books. Only books that are actively spilling consume pool memory.

```cpp
using namespace tape_book;

SpillPool<i32, u32> pool(131072);     // 131072 levels = 1 MB arena

Book<64, i32, u32> b1(1024, &pool);   // spill buffers use pool
Book<64, i32, u32> b2(1024, &pool);
b1.reset(1000);
b2.reset(2000);

// When pool is nullptr (default), books use malloc/free instead
Book<64, i32, u32> standalone(1024);  // uses malloc/free
```

### MultiBookPool3

Manage many books across 3 capacity tiers with a shared spill pool:

```cpp
using namespace tape_book;

MultiBookPool3<i32, u32, 64, 128, 256> mbp(4096, 131072);
//                                          ^       ^
//                                     max_cap   pool_cap (0 = no pool)

mbp.reserve_high(100);
mbp.reserve_medium(200);
mbp.reserve_low(500);

auto handle = mbp.alloc(BookTier::High, 1000);  // allocate + reset at anchor 1000

mbp.with_book(handle, [](auto& book) {
    book.template set<true>(1005, 50u);
    book.template set<false>(1010, 30u);
});
```

Or use the single-header version:

```cpp
#include "tape_book.hpp"  // from single_include/
```

## Spill buffer behavior

Prices outside the tape's current range spill into a sorted dynamic buffer. The buffer grows lazily: `0 -> 16 -> 32 -> 64 -> ... -> max_cap`. When the buffer reaches `max_cap`, the worst level is evicted (lowest bid or highest ask) to make room.

- `clear()` resets the count to 0 but retains the allocation (fast)
- `release()` frees the underlying memory and resets to `cap=0` (use for explicit reclamation)

When the tape recenters, spill levels within the new tape range are drained back into the tape automatically.

## Build & Test

```
cmake -B build -DTAPE_BOOK_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Integration

Header-only — add `include/` to your include path, or copy `single_include/tape_book.hpp`. CMake:

```cmake
add_subdirectory(tape_book)
target_link_libraries(your_target PRIVATE tape_book::tape_book)
```

Requires C++20 and GCC or Clang (uses `__builtin_ctzll`/`__builtin_clzll`).

## Thread safety

`SpillPool` and `MultiBookPool3` are **not thread-safe**. Use one pool per thread, or protect shared pools with external synchronization.

## License

TBD
