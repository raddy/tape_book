# Shared Spill Pool Refactor — Spec

## Intended Outcome

- Spill level data moves from inline `LevelT a[CAP]` to dynamically-allocated contiguous blocks
- Spill metadata (`bid.n`, `ask.n`, pointers) is inlined into TapeBook, adjacent to tape data — same/nearby cache lines for BBO queries
- Per-book footprint drops from ~72 KB to ~8.5 KB (N=1024, i32/u32)
- BBO queries (`best_bid_px`, `best_ask_px`) no longer incur 2 cold cache-line loads for spill n-checks
- `CAP` moves from compile-time template parameter to runtime `max_cap` (configurable per-book)
- `SpillPool` enables memory sharing across instruments: only actively-spilling books consume pool memory
- Sorted-array memmove semantics preserved — contiguous allocation, same algorithmic behavior
- Tape unchanged (`Sink` concept compatible), NullSink unchanged
- Book public API unchanged (set, best_bid_px, best_ask_px, best_bid_qty, best_ask_qty, reset, recenter, crossed, erase_better, verify_invariants)
- All 180 fuzz suites pass against reference model

## Assumptions

- **A1**: `malloc`/`free` acceptable for Sprint 1 cold-path allocation. SpillPool replaces this in Sprint 2.
- **A2**: Default `max_cap = 4096` matches current Book32 behavior. Users can tune lower per-book.
- **A3**: SideDynamic starts at `cap=0` (`a=nullptr`). No allocation until first spill. Most books never allocate.
- **A4**: Growth strategy: double capacity. Sequence: `0 → 16 → 32 → 64 → ... → max_cap`. Eviction at `max_cap` (identical to current SideFlat at CAP).
- **A5**: SpillPool uses power-of-2 size-class free lists with intrusive nodes in freed blocks. O(1) alloc/dealloc.
- **A6**: SpillPool is per-MultiBookPool3 (or per-thread). No synchronization needed.
- **A7**: SpillPool arena is fixed-size (pre-allocated). Assert on exhaustion. Trading systems pre-size.
- **A8**: When `pool == nullptr`, DynSpillBuffer falls back to malloc/free. This supports standalone Book usage without a pool.

## Architecture Changes

### Current layout (Book32, ~72 KB)
```
Book {
  Spill spill {             // ~64 KB
    SideFlat bid {
      LevelT a[4096];       // 32 KB — COLD, rarely touched
      i32 n;                // ← BBO checks this, buried 32 KB deep
    };
    SideFlat ask {
      LevelT a[4096];       // 32 KB — COLD
      i32 n;                // ← BBO checks this, buried 65 KB deep
    };
  };
  TapeBook core {           // ~8.5 KB
    Tape bids;              // 4.2 KB — HOT
    Tape asks;              // 4.2 KB — HOT
    SpillBuf* spill_buffer; // 8 bytes — pointer to spill above
  };
};
```

### New layout (Book32, ~8.5 KB)
```
Book {
  TapeBook core {
    Tape bids;              // 4.2 KB — HOT
    Tape asks;              // 4.2 KB — HOT
    DynSpillBuffer spill {  // ~56 bytes — HOT metadata inline
      SideDynamic bid {
        LevelT* a;          // → heap/pool (only if n > 0)
        i32 n;              // ← BBO checks this, now adjacent to tape data
        i32 cap;
        i32 max_cap;
      };
      SideDynamic ask {
        LevelT* a;
        i32 n;              // ← adjacent to bid.n
        i32 cap;
        i32 max_cap;
      };
      SpillPool* pool;      // null = malloc, non-null = pool alloc
    };
  };
};
```

### Sink concept (unchanged)
```cpp
concept Sink = requires(S s, PriceT px, QtyT q, Fn fn) {
    s.template push<IsBid>(px, q);
    s.template erase_better<IsBid>(px);
    s.template iterate_pending<IsBid>(fn);
    s.clear();
};
```

DynSpillBuffer satisfies this. NullSink satisfies this. Tape is unchanged.

---

## Sprint 1: Dynamic Spill Buffer

Replace inline fixed-capacity arrays with dynamically-allocated contiguous blocks. Inline spill metadata into TapeBook. Eliminates the ~64 KB cache gap.

### Ticket 1.1: Implement SideDynamic and DynSpillBuffer

**File:** `include/tape_book/spill_buffer.hpp`

**Add** (alongside existing types):
- `SideDynamic<PriceT, QtyT>`: `{ level_type* a{nullptr}; i32 n{0}; i32 cap{0}; i32 max_cap; }`
- Port all SideFlat methods to SideDynamic:
  - `lb`, `add_point<IsBid>`, `drain_range`, `erase_better<IsBid>`, `iterate<IsBid>`, `best_px<IsBid>`, `best_qty<IsBid>`, `clear`
  - Key difference: `add_point` calls `ensure_cap()` before the insertion path when `n == cap && cap < max_cap`
- `ensure_cap(i32 needed, SpillPool* pool) noexcept`:
  - New cap = max(current*2 or 16, needed), clamped to max_cap
  - If pool: `pool->allocate(new_cap)`, else: `std::malloc(...)`
  - memcpy old data, free old allocation
  - If allocation fails: don't grow (caller will evict at max_cap)
- `destroy(SpillPool* pool) noexcept`: free via pool or std::free
- `DynSpillBuffer<PriceT, QtyT>`: wraps `SideDynamic bid, ask` + `SpillPool* pool_{nullptr}`
  - Implements full Sink concept: `push<IsBid>`, `erase_better<IsBid>`, `iterate_pending<IsBid>`, `clear`
  - Also: `drain<IsBid>`, `best_px<IsBid>`, `best_qty<IsBid>` (called by TapeBook directly)
  - Constructor: `DynSpillBuffer(i32 max_cap = 4096, SpillPool* pool = nullptr)`
  - Destructor: calls `bid.destroy(pool_); ask.destroy(pool_);`
  - Move: transfer pointers (heap/pool memory is stable), null out source
  - Copy: deleted
- Include `<cstdlib>` for malloc/free
- Keep old SideFlat/SpillBuffer (removed in Ticket 1.4)

**Acceptance:**
- New types compile
- Existing tests still pass (old types untouched)
- New standalone unit test: create DynSpillBuffer, push levels, verify sorted order, drain, verify eviction at max_cap

### Ticket 1.2: Restructure TapeBook and Book

**Files:** `include/tape_book/book.hpp`, `include/tape_book/tape_book.hpp`

**TapeBook changes:**
- `TapeBook<N, SpillBuf>` → `TapeBook<N, PriceT, QtyT>`
- Remove `SpillBuf* spill_buffer` pointer
- Add `DynSpillBuffer<PriceT, QtyT> spill` as inline member
- Constructor: `explicit TapeBook(i32 max_cap = 4096, SpillPool* pool = nullptr) : spill(max_cap, pool) {}`
- All `spill_buffer->` → `spill.` (14+ sites)
- `using price_type = PriceT; using qty_type = QtyT;` (was derived from SpillBuf)
- compute_anchor, set_impl, recenter_bid/ask, best_*: same logic, just member access instead of pointer deref

**Book changes:**
- `Book<N, CAP, PriceT, QtyT>` → `Book<N, PriceT, QtyT>` (drop CAP)
- Remove `Spill spill` member — spill lives inside `Core core` now
- `using Core = TapeBook<N, PriceT, QtyT>`
- Constructor: `explicit Book(i32 max_cap = 4096, SpillPool* pool = nullptr) : core(max_cap, pool) {}`
- Delete copy (same as now)
- **Default move is correct** — DynSpillBuffer's move transfers heap pointers. No dangling-reference fix needed.
- Remove custom move constructor and move-assign operator
- All `core.` method calls unchanged
- Drop `spill` member references

**tape_book.hpp:**
- `Book32 = Book<1024, i32, u32>` (was `Book<1024, 4096, i32, u32>`)
- `Book64 = Book<1024, i64, u64>` (was `Book<1024, 4096, i64, u64>`)
- Include `spill_pool.hpp` (forward-declare SpillPool for now)

**Acceptance:**
- `sizeof(Book<1024, i32, u32>)` approximately 8.5 KB
- Default move works (no custom move needed)
- Book public API unchanged

### Ticket 1.3: Update MultiBookPool3

**File:** `include/tape_book/multi_book_pool.hpp`

**Changes:**
- Drop `CAP_HIGH, CAP_MEDIUM, CAP_LOW` template parameters
- Template params: `<PriceT, QtyT, N_HIGH, N_MEDIUM, N_LOW, ...Allocs>`
- Book types: `Book<N_HIGH, PriceT, QtyT>` etc. (no CAP)
- `alloc()` takes optional `i32 max_cap = 4096` forwarded to Book constructor
- Or: each tier stores a default max_cap, set via setter or template param

**Acceptance:**
- Pool compiles with new Book types
- `with_book` still works

### Ticket 1.4: Update tests, remove old types, sync

**Files:** All test files, spill_buffer.hpp, scripts/sync_single_include.sh

**spill_buffer.hpp:**
- Remove `SideFlat` and `SpillBuffer<CAP, PriceT, QtyT>`
- Only `SideDynamic`, `DynSpillBuffer`, and `NullSink` remain

**test_book_basic.cpp:**
- `Book<N, CAP, i32, u32>` → `Book<N, i32, u32>` (construct with max_cap if needed)
- `b.core.spill_buffer` references → removed (TapeBook no longer has this)
- Access pattern: `b.core.bids`, `b.core.asks`, `b.core.spill` (spill is now a TapeBook member)

**fuzz_helpers.hpp:**
- `FuzzCtx<N, CAP, PriceT, QtyT>` → `FuzzCtx<N, PriceT, QtyT>` with runtime `max_cap`
- `Book<N, CAP, ...>` → `Book<N, ...>` constructed with max_cap
- `b.spill` → `b.core.spill` (spill moved from Book to TapeBook)
- `collect_separate`: `b.spill.template iterate_pending<true>(...)` → `b.core.spill.template iterate_pending<true>(...)`
- `collect_chained`: `b.core.bids.iterate_from_best(collector, b.spill)` → `b.core.bids.iterate_from_best(collector, b.core.spill)`

**test_fuzz.cpp:**
- Drop CAP from all `fuzz_run<N, CAP, ...>`, `fuzz_deep<N, CAP, ...>`, etc.
- Pass max_cap to FuzzCtx constructor instead
- All 13 categories × multiple seeds must pass

**test_multi_book_pool.cpp:**
- Drop CAP from pool template params
- Update static_asserts for Book type
- Move tests: default move should just work (simpler than before)

**Sync:**
- Run `scripts/sync_single_include.sh`
- Verify idempotent (diff = 0)

**Acceptance:**
- `cmake --build build && ctest --test-dir build --output-on-failure`
- All 3 test binaries pass
- All 180 fuzz suites pass
- Single-include synced

---

## Sprint 2: Shared SpillPool

Introduce pool allocator so multiple books share a pre-allocated arena. Eliminates per-book malloc overhead and enables memory budgeting.

### Ticket 2.1: Implement SpillPool

**File:** `include/tape_book/spill_pool.hpp` (new)

```cpp
template <typename PriceT, typename QtyT>
struct SpillPool {
    using level_type = LevelT<PriceT, QtyT>;

    // Pre-allocated arena
    level_type* arena;
    i32 arena_cap;
    i32 watermark;  // bump allocator high-water mark

    // Power-of-2 size-class free lists (classes 0..11 → sizes 16..32768)
    static constexpr i32 NUM_CLASSES = 12;
    static constexpr i32 MIN_BLOCK = 16;
    i32 free_heads[NUM_CLASSES];  // -1 = empty

    // Intrusive free-list node (stored in first bytes of freed block)
    // Safe because freed blocks are >= 16 * sizeof(LevelT) = 128 bytes
    struct FreeNode { i32 next_offset; };  // 4 bytes

    explicit SpillPool(i32 total_cap);
    ~SpillPool();

    SpillPool(const SpillPool&) = delete;
    SpillPool& operator=(const SpillPool&) = delete;
    SpillPool(SpillPool&&) = delete;
    SpillPool& operator=(SpillPool&&) = delete;

    level_type* allocate(i32 cap) noexcept;     // O(1): check free list, fallback bump
    void deallocate(level_type* ptr, i32 cap) noexcept;  // O(1): push to free list
    level_type* reallocate(level_type* old, i32 old_cap, i32 new_cap, i32 used) noexcept;

    static i32 size_class(i32 cap) noexcept;    // cap → class index
    static i32 class_size(i32 cls) noexcept;    // class index → actual cap

    // Diagnostics
    i32 used_bytes() const noexcept;
    i32 total_bytes() const noexcept;
};
```

**Design:**
- `allocate(cap)`: round up to size class, check free list head, pop if available, else bump-allocate from watermark. Return nullptr if exhausted (assert in debug).
- `deallocate(ptr, cap)`: compute size class, write FreeNode at ptr, push to free list.
- `reallocate(old, old_cap, new_cap, used)`: allocate new, memcpy used entries, deallocate old.
- Size classes: `MIN_BLOCK << cls` for cls in [0, NUM_CLASSES). So 16, 32, 64, ..., 32768.
- No coalescing — size classes mean blocks are always same-size within a class.

**Acceptance:**
- Compiles standalone
- Unit test: alloc/dealloc cycle, realloc, exhaustion behavior, free-list reuse

### Ticket 2.2: Integrate SpillPool into DynSpillBuffer + Book

**Files:** `include/tape_book/spill_buffer.hpp`, `include/tape_book/book.hpp`

**Changes:**
- DynSpillBuffer already has `SpillPool* pool_` — now we wire it through
- SideDynamic::ensure_cap already routes through pool when non-null
- Book constructor: `explicit Book(i32 max_cap = 4096, SpillPool<PriceT, QtyT>* pool = nullptr)`
- TapeBook constructor: same, forwarded

**Acceptance:**
- All existing tests pass (pool=nullptr path unchanged)
- New test: create SpillPool, create Books with pool, verify spill operations route through pool

### Ticket 2.3: Update MultiBookPool3 to own SpillPool

**File:** `include/tape_book/multi_book_pool.hpp`

**Changes:**
- Add template parameter for pool arena size (or runtime configuration)
- MultiBookPool3 owns `SpillPool<PriceT, QtyT> pool_`
- `alloc()` passes `&pool_` to Book constructor
- Pool sizing: configurable, default = reasonable (e.g., 64K levels = 512 KB for i32/u32)

**Acceptance:**
- Multi-book pool tests pass with shared pool
- Allocation stress test: many books, verify pool memory reuse

### Ticket 2.4: Tests + sync

**Changes:**
- Add `tests/test_spill_pool.cpp` with:
  - SpillPool standalone tests (alloc, dealloc, realloc, free-list reuse, exhaustion)
  - Multi-book shared-pool stress test (N books sharing one pool, heavy spill traffic)
- Update `tests/CMakeLists.txt`
- Update `scripts/sync_single_include.sh` HEADERS array to include `spill_pool.hpp`
- Run sync script

**Acceptance:**
- All tests pass
- Sync idempotent

---

## Sprint 3: Cleanup

### Ticket 3.1: Update README

- Drop CAP from API examples
- Add SpillPool usage example for MultiBookPool3
- Document `max_cap` parameter
- Update memory footprint info
- Document growth strategy and eviction behavior

### Ticket 3.2: Final validation

- Build + test all
- Verify: `sizeof(Book<1024, i32, u32>)` ≈ 8.5 KB
- Verify: SpillPool reuse works (alloc, use, free, realloc reuses freed blocks)
- Verify: 180 fuzz suites pass
- Verify: single-include synced

---

## Subagent Critic Prompt

> You are reviewing a refactoring spec for a C++20 header-only order book library. The refactor replaces inline fixed-capacity spill buffers with dynamic pool-backed allocation to improve cache locality.
>
> Review the spec for:
> 1. **Missing tickets**: Are there changes needed that aren't captured? Think about every file that references SpillBuffer, SideFlat, the CAP template param, or `b.spill`.
> 2. **Hidden dependencies**: Do any tickets depend on work in later tickets? Is the ordering correct?
> 3. **Test gaps**: Are there scenarios that the existing fuzz tests won't cover after the refactor? Think about: dynamic growth, pool exhaustion, pool reuse after dealloc, move semantics with pool-backed books, multi-book sharing a pool.
> 4. **API breakage**: Does the public API change in any way callers would notice? Think about template parameter changes, constructor changes, type alias changes.
> 5. **Oversized tickets**: Are any tickets too large to implement atomically? Should they be split?
> 6. **Memory safety**: After move, does the source's destructor double-free? After pool destruction, do books with dangling pool pointers cause UB?
> 7. **Thread safety**: Is the "pool per thread" assumption sufficient? What if MultiBookPool3 is accessed from multiple threads?
> 8. **Performance regression**: Could the indirection through `pool->allocate()` hurt the cold path? Does the DynSpillBuffer's larger metadata (vs SideFlat's simple struct) regress hot-path access patterns?
> 9. **Edge cases**: max_cap=0? max_cap=1? Pool with total_cap < MIN_BLOCK? Book constructed with pool, then pool destroyed before book?
>
> For each finding, state severity (CRITICAL/HIGH/MEDIUM/LOW), the affected ticket, and suggested fix.

---

## Critic Review Summary

21 findings reviewed. Key changes incorporated:

### CRITICAL fixes
- **F1 — SpillPool forward-decl cycle:** Sprint 1 now has NO reference to SpillPool. `SideDynamic` and `DynSpillBuffer` use only `malloc`/`free` in Sprint 1. The `SpillPool*` parameter is introduced in Sprint 2 (Ticket 2.2) when the type exists.
- **F2 — Move semantics double-free:** `DynSpillBuffer` gets an explicit move constructor/assignment that nulls the source's `a` pointers and sets `n=0, cap=0`. Move-assignment calls `destroy()` on the target's old allocations before overwriting. Book's default move is then safe because DynSpillBuffer handles it.

### HIGH fixes
- **F3 — ensure_cap call site:** Spec now explicitly states: call `ensure_cap()` at the TOP of `add_point`, before `lb()`, when `n == cap && cap < max_cap`.
- **F4 — max_cap=0 guard:** `DynSpillBuffer` constructor asserts `max_cap >= 1`. Tests added for edge cases `max_cap=1`, `max_cap=16`.
- **F5 — iterate_pending signature:** DynSpillBuffer::iterate_pending includes the `worst_px` default parameter matching current SpillBuffer signature.
- **F12 — Pool outlives books:** Ticket 2.3 specifies `SpillPool pool_` declared BEFORE all `vector<Book>` members. Comment explains lifetime requirement.

### MEDIUM fixes
- **F7 — Ticket 1.2 split:** Split into 1.2a (TapeBook restructure) and 1.2b (Book restructure + tape_book.hpp aliases).
- **F10 — Power-of-2 cap:** `max_cap` must be a power of 2 ≥ 1. Asserted in DynSpillBuffer constructor. Documented.
- **F11 — Pool exhaustion vs eviction:** Pool exhaustion increments a `SpillPool::alloc_fail_count` diagnostic counter and falls back to "don't grow" (same as max_cap eviction). Users can check the counter.
- **F13 — Fuzz coverage for growth:** Added small `max_cap` (16, 32) fuzz suites to Ticket 1.4. Pool-backed fuzz suite added to Ticket 2.4.
- **F14 — sync script ordering:** `spill_pool.hpp` inserted after `spill_buffer.hpp` in HEADERS array.
- **F15 — Thread safety docs:** README documents single-thread requirement for SpillPool and MultiBookPool3.
- **F17 — clear() semantics:** `clear()` = `n = 0` only (matches current SideFlat, fast). New `release()` method frees allocation and resets `a=nullptr, cap=0`. `Book::reset()` calls `clear()`. Users call `release()` explicitly for memory reclamation.

### LOW fixes (documentation/minor)
- **F16:** Added `static_assert(sizeof(Book<1024, i32, u32>) < 9000)` to Ticket 3.2.
- **F18:** Documented that final growth step clamps to max_cap.
- **F19:** `SideDynamic::ensure_cap` calls `allocate` (not `reallocate`) when `a == nullptr`.
- **F20:** No config.hpp changes needed — confirmed.

---

## Revised Plan (post-critic)

### Sprint 1: Dynamic Spill Buffer (malloc/free only)

**Ticket 1.1: Implement SideDynamic and DynSpillBuffer**

File: `include/tape_book/spill_buffer.hpp`

Add alongside existing types (old types stay until Ticket 1.5):

```cpp
template <typename PriceT, typename QtyT>
struct SideDynamic {
    using level_type = LevelT<PriceT, QtyT>;
    using price_type = PriceT;
    using qty_type   = QtyT;

    level_type* a{nullptr};
    i32 n{0};
    i32 cap{0};
    i32 max_cap;

    // Port from SideFlat: lb, add_point<IsBid>, drain_range, erase_better<IsBid>,
    // iterate<IsBid>, best_px<IsBid>, best_qty<IsBid>, clear
    //
    // Key differences from SideFlat:
    // - add_point: at TOP, before lb(), if (n == cap && cap < max_cap) ensure_cap()
    // - ensure_cap(): new_cap = max(cap ? cap*2 : 16, needed), clamped to max_cap
    //   malloc new array, memcpy, free old. If a==nullptr, just malloc (no memcpy/free).
    // - clear(): n = 0 only (retains allocation, matches SideFlat semantics)
    // - release(): free(a); a=nullptr; n=0; cap=0; (new — explicit deallocation)
    // - destroy(): same as release() (called by DynSpillBuffer destructor)
};
```

`DynSpillBuffer<PriceT, QtyT>`:
- Members: `SideDynamic<PriceT, QtyT> bid, ask;`
- Constructor: `explicit DynSpillBuffer(i32 max_cap = 4096)` — asserts `max_cap >= 1` and `(max_cap & (max_cap-1)) == 0` (power of 2)
- Destructor: calls `bid.destroy(); ask.destroy();`
- **Explicit move constructor:** transfers `bid` and `ask` members, nulls source (`a=nullptr, n=0, cap=0`)
- **Explicit move-assignment:** calls `bid.destroy(); ask.destroy();` on target first, then transfers, nulls source
- Copy: deleted
- Sink concept: `push<IsBid>`, `erase_better<IsBid>`, `iterate_pending<IsBid>(Fn&&, price_type worst_px = ...)` (with default worst_px matching SpillBuffer signature), `clear()`
- Also: `drain<IsBid>`, `best_px<IsBid>`, `best_qty<IsBid>`, `release()` (calls both sides' release)

Include `<cstdlib>` for malloc/free.

Acceptance:
- New types compile
- Existing tests still pass (old types untouched)
- New standalone unit test: create DynSpillBuffer, push levels, verify sorted order, drain, verify eviction at max_cap, verify release() frees memory, verify move nulls source

---

**Ticket 1.2a: Restructure TapeBook**

File: `include/tape_book/book.hpp`

- `TapeBook<N, SpillBuf>` → `TapeBook<N, PriceT, QtyT>`
- Remove `SpillBuf* spill_buffer` pointer
- Add `DynSpillBuffer<PriceT, QtyT> spill` as inline member
- Constructor: `explicit TapeBook(i32 max_cap = 4096) : spill(max_cap) {}`
- `using price_type = PriceT; using qty_type = QtyT;` (direct, not derived from SpillBuf)
- All `spill_buffer->` → `spill.` (14+ sites)
- `set_impl`: remove `SpillBuf` template parameter, use `spill` member directly
- compute_anchor, best_*, recenter_*: same logic, member access instead of pointer deref

Acceptance:
- TapeBook compiles with new template params
- Can be tested via Book (after Ticket 1.2b)

---

**Ticket 1.2b: Restructure Book and update tape_book.hpp aliases**

Files: `include/tape_book/book.hpp`, `include/tape_book/tape_book.hpp`

Book changes:
- `Book<N, CAP, PriceT, QtyT>` → `Book<N, PriceT, QtyT>` (drop CAP)
- Remove `Spill spill` member — spill lives inside `Core core` now
- `using Core = TapeBook<N, PriceT, QtyT>`
- Constructor: `explicit Book(i32 max_cap = 4096) : core(max_cap) {}`
- Delete copy (same as now)
- **Default move is now correct** — DynSpillBuffer's explicit move handles pointer transfer + source nulling. No custom move needed on Book.
- Remove custom move constructor and move-assign operator
- All forwarding methods unchanged

tape_book.hpp:
- `Book32 = Book<1024, i32, u32>` (was `Book<1024, 4096, i32, u32>`)
- `Book64 = Book<1024, i64, u64>` (was `Book<1024, 4096, i64, u64>`)

Acceptance:
- `sizeof(Book<1024, i32, u32>)` approximately 8.5 KB
- Default move works (no custom move needed)
- Book public API unchanged

---

**Ticket 1.3: Update MultiBookPool3**

File: `include/tape_book/multi_book_pool.hpp`

- Drop `CAP_HIGH, CAP_MEDIUM, CAP_LOW` template parameters
- Template params: `<PriceT, QtyT, N_HIGH, N_MEDIUM, N_LOW>`
- Book types: `Book<N_HIGH, PriceT, QtyT>` etc. (no CAP)
- `alloc()` takes optional `i32 max_cap = 4096` forwarded to Book constructor

Acceptance:
- Pool compiles with new Book types
- `with_book` still works

---

**Ticket 1.4: Update tests, add dynamic growth fuzz suites**

Files: All test files

Grep-based checklist — search and convert:
- `Book<.*,.*,.*,.*>` (4-param) → 3-param form with runtime max_cap
- `FuzzCtx<N, CAP, ...>` → `FuzzCtx<N, ...>` with runtime max_cap
- `b.spill` → `b.core.spill`
- `MultiBookPool3<..., CAP_HIGH, ..., CAP_MEDIUM, ..., CAP_LOW>` → drop CAP params

test_book_basic.cpp:
- `Book<256, 512, i32, u32>` → `Book<256, i32, u32>` with max_cap=512
- `Book<64, 512, i32, u32>` (SmallBook) → `Book<64, i32, u32>` with max_cap=512

fuzz_helpers.hpp:
- `FuzzCtx<N, CAP, PriceT, QtyT>` → `FuzzCtx<N, PriceT, QtyT>` with runtime max_cap in constructor
- `collect_separate`: `b.core.spill.template iterate_pending<true/false>(...)`
- `collect_chained`: `b.core.bids.iterate_from_best(collector, b.core.spill)`

test_fuzz.cpp:
- Drop CAP from all `fuzz_run`, `fuzz_deep`, etc.
- Pass max_cap to FuzzCtx constructor
- **Add small max_cap fuzz suites:** max_cap=16 and max_cap=32 to stress dynamic growth and frequent eviction/reallocation

test_multi_book_pool.cpp:
- Drop CAP from pool template params and Book instantiations
- Add test: source book after move is in valid empty/destructible state (best_bid_px returns lowest_px, destructor does not crash)

Acceptance:
- All 3 test binaries pass
- All existing 180 fuzz suites pass
- New small-cap fuzz suites pass

---

**Ticket 1.5: Remove old types, sync single-include**

Files: `include/tape_book/spill_buffer.hpp`, `scripts/sync_single_include.sh`

- Remove `SideFlat` and `SpillBuffer<CAP, PriceT, QtyT>` from spill_buffer.hpp
- Only `SideDynamic`, `DynSpillBuffer`, and `NullSink` remain
- Run `scripts/sync_single_include.sh`
- Verify single-include is idempotent (diff = 0)

Acceptance:
- Clean build, all tests pass
- `grep -r "SideFlat\|SpillBuffer" include/` returns nothing (except NullSink)
- Single-include synced

---

### Sprint 2: Shared SpillPool

**Ticket 2.1: Implement SpillPool**

File: `include/tape_book/spill_pool.hpp` (new)

```cpp
template <typename PriceT, typename QtyT>
struct SpillPool {
    using level_type = LevelT<PriceT, QtyT>;

    level_type* arena;
    i32 arena_cap;
    i32 watermark{0};

    static constexpr i32 NUM_CLASSES = 12;
    static constexpr i32 MIN_BLOCK = 16;
    i32 free_heads[NUM_CLASSES];  // -1 = empty

    struct FreeNode { i32 next_offset; };

    i32 alloc_fail_count{0};  // diagnostic counter for pool exhaustion

    explicit SpillPool(i32 total_cap);
    ~SpillPool();
    // Non-copyable, non-movable
    SpillPool(const SpillPool&) = delete;
    SpillPool& operator=(const SpillPool&) = delete;
    SpillPool(SpillPool&&) = delete;
    SpillPool& operator=(SpillPool&&) = delete;

    level_type* allocate(i32 cap) noexcept;     // O(1)
    void deallocate(level_type* ptr, i32 cap) noexcept;  // O(1), nullptr is no-op
    level_type* reallocate(level_type* old, i32 old_cap, i32 new_cap, i32 used) noexcept;
    // Guard: if old==nullptr, just allocate (no memcpy/free)

    static i32 size_class(i32 cap) noexcept;
    static i32 class_size(i32 cls) noexcept;
    i32 used_bytes() const noexcept;
    i32 total_bytes() const noexcept;
};
```

Design:
- `allocate(cap)`: round up to size class, check free list, pop if available, else bump from watermark. If exhausted: increment `alloc_fail_count`, return nullptr.
- `deallocate(ptr, cap)`: if ptr==nullptr, no-op. Else compute size class, push to free list.
- Size classes: `MIN_BLOCK << cls` for cls [0, NUM_CLASSES). So 16, 32, 64, ..., 32768.

Acceptance:
- Compiles standalone
- Unit test: alloc/dealloc cycle, realloc, exhaustion increments counter, free-list reuse

---

**Ticket 2.2: Integrate SpillPool into DynSpillBuffer + Book**

Files: `include/tape_book/spill_buffer.hpp`, `include/tape_book/book.hpp`

Changes to SideDynamic:
- `ensure_cap()` gains optional `SpillPool*` parameter: if non-null, use `pool->allocate/reallocate`; if null, use malloc/free (existing path)
- `destroy(SpillPool* pool)`: if pool, `pool->deallocate(a, cap)`; else `std::free(a)`
- `release(SpillPool* pool)`: same routing

Changes to DynSpillBuffer:
- Add `SpillPool<PriceT, QtyT>* pool_{nullptr}` member (declared as `void*` internally to avoid template dependency, or include `spill_pool.hpp`)
- Constructor: `explicit DynSpillBuffer(i32 max_cap = 4096, SpillPool<PriceT, QtyT>* pool = nullptr)`
- Destructor/move: route through pool_ for alloc/dealloc

Changes to TapeBook:
- Constructor: `explicit TapeBook(i32 max_cap = 4096, SpillPool<PriceT, QtyT>* pool = nullptr) : spill(max_cap, pool) {}`

Changes to Book:
- Constructor: `explicit Book(i32 max_cap = 4096, SpillPool<PriceT, QtyT>* pool = nullptr) : core(max_cap, pool) {}`

Acceptance:
- All existing tests pass (pool=nullptr path unchanged)
- New test: create SpillPool, create Books with pool, verify spill operations route through pool
- Test: pool exhaustion increments alloc_fail_count, book degrades gracefully

---

**Ticket 2.3: Update MultiBookPool3 to own SpillPool**

File: `include/tape_book/multi_book_pool.hpp`

Changes:
- Add `SpillPool<PriceT, QtyT> pool_` member — **declared BEFORE all `vector<Book>` members** (ensures pool outlives books during destruction)
- Comment: `// pool_ must be declared before book vectors — destruction is reverse order`
- `alloc()` passes `&pool_` to Book constructor
- Pool sizing: constructor takes `i32 pool_cap` parameter, default = 65536 levels (512 KB for i32/u32)

Acceptance:
- Multi-book pool tests pass with shared pool
- Pool destruction order verified (run under ASan if available)

---

**Ticket 2.4: Tests + sync**

Files: tests/, scripts/

- Add `tests/test_spill_pool.cpp`:
  - SpillPool standalone: alloc, dealloc, realloc, free-list reuse, exhaustion counter
  - Multi-book shared-pool stress: N books sharing one pool, heavy spill traffic
  - Pool-backed fuzz suite: run existing fuzz patterns with pool!=nullptr
- Update `tests/CMakeLists.txt`
- Update `scripts/sync_single_include.sh` HEADERS array: insert `spill_pool.hpp` after `spill_buffer.hpp` (before `tape.hpp`)
- Run sync script

Acceptance:
- All tests pass
- Sync idempotent

---

### Sprint 3: Cleanup

**Ticket 3.1: Update README**

- Drop CAP from API examples
- Add SpillPool usage example for MultiBookPool3
- Document `max_cap` parameter (must be power of 2 ≥ 1)
- Document growth strategy (0 → 16 → 32 → ... → max_cap, final step clamps)
- Document `release()` for explicit memory reclamation
- Document: SpillPool and MultiBookPool3 are NOT thread-safe (single-thread use only)
- Update memory footprint info (~8.5 KB per Book32 vs previous ~72 KB)

**Ticket 3.2: Final validation**

- Build + test all
- `static_assert(sizeof(Book<1024, i32, u32>) < 9000)` — add to test or tape_book.hpp
- Verify SpillPool reuse: alloc, use, free, realloc reuses freed blocks
- Verify all 180+ fuzz suites pass (including new small-cap and pool-backed suites)
- Verify single-include synced
- Run under ASan/UBSan if available

---

## Ticket Dependency Graph

```
1.1 (SideDynamic + DynSpillBuffer)
 ├── 1.2a (TapeBook restructure)
 │    └── 1.2b (Book restructure + aliases)
 │         ├── 1.3 (MultiBookPool3)
 │         └── 1.4 (Update all tests)
 │              └── 1.5 (Remove old types + sync)
 │                   └── 2.1 (SpillPool)
 │                        └── 2.2 (Integrate pool into DynSpillBuffer)
 │                             └── 2.3 (MultiBookPool3 owns pool)
 │                                  └── 2.4 (Pool tests + sync)
 │                                       ├── 3.1 (README)
 │                                       └── 3.2 (Final validation)
```
