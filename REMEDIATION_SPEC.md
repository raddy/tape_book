# tape_book Remediation Spec

## Overview

Fix all outstanding bugs identified in the critical re-review. 8 issues across 4 severity tiers. The core tape+bitset+spill algorithm is proven correct by 180 fuzz suites (~14M ops). The issues are structural (move semantics, API shape, clamps, docs).

## Intended Outcome

- Book is safe under copy/move — no dangling references, MultiBookPool3 works after vector reallocation
- All public best-price methods return correct results (tape+spill, not tape-only); tape-only query renamed to `tape_best_px`
- compute_anchor clamps both underflow AND overflow — no UB in release
- set_qty no-op delete returns a semantically correct result
- NullSink compiles when passed to any Tape method
- README examples actually compile against the real API (no MSVC claim)
- Single-include stays in sync via a script
- All 180 fuzz suites + 3 test binaries pass after every change

## Assumptions

1. **Book keeps copy deleted, gets a custom move** that rebinds `TapeBook::spill_buffer` (changed from `SpillBuf&` to `SpillBuf*`). This preserves contiguous `vector<Book>` layout in MultiBookPool3 — critical for cache locality on the hot path.
2. **`TapeBook::best_px<IsBid>()` is renamed to `tape_best_px<IsBid>()`** — kept as a public method for tape-only queries, but the name now makes scope explicit. Callers who want tape+spill use `best_bid_px()`/`best_ask_px()`.
3. **No-op delete should return `Erase`** (not a new `NoOp` variant) — "you asked to delete, the result is: it's erased." This avoids adding enum variants and changing all switch sites. The existing test at `test_book_basic.cpp:27` asserts `Update` — this line changes.
4. **No MSVC support.** We don't target Windows. GCC builtins (`__builtin_ctzll`, `__builtin_clzll`, `__builtin_unreachable`) stay as-is. README will explicitly state GCC/Clang only.
5. **Spill overflow is a documented design choice**, not a bug to fix with a counter. We'll add a doc comment.
6. **Single-include sync script** will be a simple shell script that concatenates headers in order, run manually. No CI for now (no CI exists in the repo).

## Open Questions (resolved)

- Q: `unique_ptr` vs pointer-rebind for MultiBookPool3? **Decision: Pointer-rebind (Option B).** Change `SpillBuf&` to `SpillBuf*` in TapeBook, write custom move on Book that rebinds the pointer. Preserves contiguous vector layout. Add `// TODO: future refactor — inline spill buffer into TapeBook to eliminate pointer entirely`.
- Q: Should `UpdateResult` get a `NoOp` variant? **Decision: No.** Return `Erase` for no-op delete. Less churn.

---

## Sprint 1: P0 + P1 — Memory safety and API correctness

**Goal:** After this sprint, Book is move-safe and all public best-price methods are correctly named. Existing tests + fuzz pass.

### Ticket 1.1: Fix Book move safety via pointer-rebind

**Files:** `include/tape_book/book.hpp`, `single_include/tape_book.hpp`, `tests/test_multi_book_pool.cpp`

**Changes:**
- `TapeBook`: Change `SpillBuf& spill_buffer` to `SpillBuf* spill_buffer`. Update constructor:
  ```cpp
  explicit TapeBook(SpillBuf& sb) : spill_buffer(&sb) {}
  ```
  Update all `spill_buffer.` references to `spill_buffer->` (~15 sites in TapeBook methods: `reset`, `best_bid_px`, `best_ask_px`, `best_bid_qty`, `best_ask_qty`, `erase_better`, `set_impl`, `recenter_bid`, `recenter_ask`).
- `Book`: Delete copy, write custom move that rebinds the pointer:
  ```cpp
  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;

  Book(Book&& o) noexcept : spill(std::move(o.spill)), core(std::move(o.core)) {
    core.spill_buffer = &spill;
  }
  Book& operator=(Book&& o) noexcept {
    if (this != &o) {
      spill = std::move(o.spill);
      core = std::move(o.core);
      core.spill_buffer = &spill;
    }
    return *this;
  }
  ```
- Add TODO comment in `TapeBook`:
  ```cpp
  // TODO: future refactor — inline spill buffer into TapeBook to eliminate pointer indirection entirely
  ```
- Mirror all changes in `single_include/tape_book.hpp`.

**Tests:**
- **Reallocation survival test** (new in `test_multi_book_pool.cpp`): Create pool WITHOUT reserving, alloc a book, set bid/ask levels, alloc many more books to force vector reallocation, then verify the first book still has correct `best_bid_px()`/`best_ask_px()`. This is the test that would have caught the original P0 bug.
- **Move correctness test** (new in `test_book_basic.cpp` or `test_multi_book_pool.cpp`): Move-construct a populated Book, verify the moved-to book has correct state and spill buffer works.
- **Compile-time check**: `static_assert(!std::is_copy_constructible_v<Book32>)` — Book is not copyable.
- Existing fuzz suite (180/180) passes unchanged.

**Acceptance:** `cmake --build build && ctest --test-dir build --output-on-failure` passes. Reallocation survival test passes. Book is not copyable but is movable.

**Verification command:** `cmake --build build 2>&1 | grep -c error` → 0; `ctest --test-dir build --output-on-failure` → all pass.

---

### Ticket 1.2: Rename TapeBook::best_px<IsBid>() to tape_best_px<IsBid>()

**Files:** `include/tape_book/book.hpp`, `single_include/tape_book.hpp`

**Changes:**
- Rename `TapeBook::best_px<IsBid>()` to `tape_best_px<IsBid>()`.
- Verify callers: `crossed_on_tape()` at book.hpp:147-148 calls `bids.best_px()` / `asks.best_px()` (Tape-level, not TapeBook-level) — **zero** callers of `TapeBook::best_px<IsBid>()`. The rename is safe.
- The method stays public — it's a legitimate tape-only query, now with an unambiguous name.

**Tests:**
- Verify compile succeeds (no callers broken).
- Existing fuzz + basic tests pass.

**Acceptance:** Method renamed. `grep -r 'best_px' include/tape_book/book.hpp` shows only `tape_best_px`, `best_bid_px`, `best_ask_px`. All tests pass.

---

## Sprint 2: P2 fixes — Correctness and docs

**Goal:** compute_anchor is safe at all price boundaries, no-op deletes return correct result, README compiles and is accurate.

### Ticket 2.1: Add upper-bound clamp to compute_anchor

**Files:** `include/tape_book/book.hpp`, `single_include/tape_book.hpp`, `tests/test_book_basic.cpp`

**Changes:**
- Add `max_anchor` clamp symmetric with existing `min_anchor` clamp:
  ```cpp
  [[nodiscard]] static constexpr price_type compute_anchor(price_type px, i64 offset) noexcept {
    constexpr auto min_px = std::numeric_limits<price_type>::lowest();
    constexpr auto max_px = std::numeric_limits<price_type>::max();
    constexpr auto min_anchor = min_px + (N64 - 1);
    constexpr auto max_anchor = max_px - (N64 - 1);
    if (px < min_px + offset) return min_anchor;
    price_type result = px - offset;
    return (result > max_anchor) ? max_anchor : result;
  }
  ```

**Tests:**
- **Direct unit test for compute_anchor** (new in `test_book_basic.cpp`):
  - Call `TapeBook<64, Spill>::compute_anchor(INT32_MAX, 32)` and verify result ≤ `max_valid_anchor` (i.e., `INT32_MAX - 63`).
  - Call `compute_anchor(INT32_MIN, 32)` and verify result ≥ `min_valid_anchor`.
  - Call `compute_anchor(INT32_MAX, 0)` — edge case where `px - offset` overflows anchor range.
- **Integration boundary test** (new in `test_book_basic.cpp`):
  - `Book<64, 512, i32, u32>` reset near `INT32_MAX - 63`, set prices at `INT32_MAX`, `INT32_MAX - 1`, verify no assert/crash, correct best_px.
  - Same for near `INT32_MIN + 63`.
- Existing fuzz boundary tests (seeds 1-8, i32/i64/i16, N=64/256) continue to pass.

**Acceptance:** Setting a price at `INT32_MAX` on a book anchored near `INT32_MAX - N` succeeds without UB. All tests pass.

---

### Ticket 2.2: Fix set_qty no-op delete return value

**Files:** `include/tape_book/tape.hpp`, `single_include/tape_book.hpp`, `tests/test_book_basic.cpp`

**Changes:**
- tape.hpp:138: Change `return UpdateResult::Update;` to `return UpdateResult::Erase;`
- test_book_basic.cpp:27: Change `assert(b.set<true>(1005, 0) == UpdateResult::Update);` to `assert(b.set<true>(1005, 0) == UpdateResult::Erase);`

**Tests:**
- The existing test at line 27 validates this case. Update the expected value.
- Verify fuzz tests don't depend on `set()` return values for no-op deletes (they use `std::ignore`).

**Acceptance:** All tests pass. Deleting a non-existent level returns `Erase`.

---

### Ticket 2.3: Fix README.md API examples and platform claims

**Files:** `README.md`

**Changes:**
- Replace the usage example with code that compiles against the actual API:
  ```cpp
  #include <tape_book/tape_book.hpp>

  tape_book::Book32 book;
  book.reset(0);
  book.set(true, 100, 50u);   // bid 50 @ 100
  book.set(false, 101, 30u);  // ask 30 @ 101
  auto best_bid = book.best_bid_px();   // 100
  auto best_ask = book.best_ask_px();   // 101
  ```
- Remove any MSVC support claims. State "GCC/Clang, C++20" as supported compilers.
- Remove nonexistent `tape_book::Side::Bid` enum references.

**Tests:** None (documentation only). Visually verify examples match actual API.

**Acceptance:** The code block in README, if pasted into a .cpp file with the right includes, compiles cleanly. No mention of MSVC.

---

## Sprint 3: P3 fixes — Completeness and hygiene

**Goal:** NullSink compiles for all Tape paths, spill overflow is documented, single-include has a sync script.

### Ticket 3.1: Complete NullSink interface

**Files:** `include/tape_book/spill_buffer.hpp`, `single_include/tape_book.hpp`

**Changes:**
- Add missing no-op methods:
  ```cpp
  struct NullSink {
    template <bool IsBid>
    TB_ALWAYS_INLINE void push(auto, auto) noexcept {}
    template <bool IsBid>
    TB_ALWAYS_INLINE void erase_better(auto) noexcept {}
    template <bool IsBid, class Fn>
    TB_ALWAYS_INLINE void iterate_pending(Fn&&) const noexcept {}
    TB_ALWAYS_INLINE void clear() noexcept {}
  };
  ```

**Tests:**
- Add a compile-time smoke test (in `test_book_basic.cpp` or a new small test): instantiate `Tape::erase_better` and `Tape::iterate_from_best` with `NullSink` as the sink, ensuring compilation succeeds. Doesn't need to do anything meaningful at runtime — just prove the template instantiates.

**Acceptance:** NullSink compiles when used with every Tape method. All tests pass.

---

### Ticket 3.2: Document spill overflow behavior

**Files:** `include/tape_book/spill_buffer.hpp`, `single_include/tape_book.hpp`

**Changes:**
- Add doc comment above `SideFlat::add_point`:
  ```cpp
  // When the buffer is full (n == CAP), the worst level (lowest for bids,
  // highest for asks) is silently evicted to make room. Callers must size
  // CAP large enough for their workload to avoid data loss.
  ```
- Add similar note in the `SpillBuffer` class-level comment.

**Tests:** None (documentation only).

**Acceptance:** `grep -c "silently evicted" include/tape_book/spill_buffer.hpp` ≥ 1.

---

### Ticket 3.3: Add single-include sync script

**Files:** new file `scripts/sync_single_include.sh`

**Changes:**
- Shell script that concatenates headers in dependency order into `single_include/tape_book.hpp`, stripping duplicate `#pragma once` and internal `#include "tape_book/..."` lines, appending the `Book32`/`Book64` aliases.
- Document the command in README.

**Tests:**
- Run the script, then `diff` the output against the current `single_include/tape_book.hpp`. They should match (after all prior tickets are applied to both).
- Build tests using the single-include path and verify they pass.

**Acceptance:** `scripts/sync_single_include.sh` exists and is executable. Running it produces a `single_include/tape_book.hpp` that compiles and passes tests.

---

## Validation Matrix

| Ticket | Files changed | Test command | Expected |
|--------|--------------|--------------|----------|
| 1.1 | book.hpp, single_include, test_multi_book_pool.cpp | `cmake --build build && ctest --test-dir build --output-on-failure` | All pass. Reallocation survival test passes. Move rebinds spill pointer. |
| 1.2 | book.hpp, single_include | Same | All pass. `best_px` renamed to `tape_best_px`. |
| 2.1 | book.hpp, single_include, test_book_basic.cpp | Same | All pass. No crash/UB on `INT32_MAX` prices. Direct compute_anchor test validates clamp. |
| 2.2 | tape.hpp, single_include, test_book_basic.cpp | Same | All pass. No-op delete returns `Erase`. |
| 2.3 | README.md | Visual inspection | Examples match actual API. No MSVC claim. |
| 3.1 | spill_buffer.hpp, single_include, test_book_basic.cpp | Same | All pass. NullSink compiles with all Tape methods. |
| 3.2 | spill_buffer.hpp, single_include | `grep "silently evicted"` | ≥ 1 match. |
| 3.3 | new script | `bash scripts/sync_single_include.sh && cmake --build build && ctest ...` | All pass. |

---

## Critic Feedback and Revisions

### Round 1 (self-review)

**Issue 1: Ticket 1.1 deleting move on Book breaks `std::vector<Book>` in MultiBookPool3.**
`std::vector` requires move-constructible elements. Deleting move on Book makes `std::vector<Book>` unusable.

**Revision:** Changed approach from delete-move+unique_ptr to pointer-rebind+custom-move. Preserves contiguous vector layout for hot-path cache locality. **[INCORPORATED]**

**Issue 2: `__builtin_unreachable()` in multi_book_pool.hpp has no MSVC fallback.**
**Revision:** Dropped. We don't support MSVC. GCC builtins stay as-is. **[RESOLVED — no MSVC support]**

**Issue 3: Sync script ordering.** During Sprints 1-2, manually mirror changes. Ticket 3.3 validates. Correct as-is.

**Issue 4: Ticket 1.2 — fuzz_helpers.hpp doesn't use `TapeBook::best_px<IsBid>()`.** Confirmed safe to rename.

**Issue 5: Only `test_book_basic.cpp:27` depends on no-op delete returning `Update`.** Confirmed safe.

### Round 2 (subagent critic)

**CRITICAL: Ticket 1.1 body contradicted revision section.**
**Revision:** Ticket 1.1 fully rewritten with pointer-rebind approach. **[INCORPORATED]**

**HIGH: No reallocation survival test.**
**Revision:** Added explicit reallocation survival test to Ticket 1.1. **[INCORPORATED]**

**HIGH: compute_anchor overflow test was indirect.**
**Revision:** Added direct `compute_anchor()` unit test to Ticket 2.1. **[INCORPORATED]**

**MEDIUM: test_multi_book_pool.cpp not listed in Ticket 1.1 affected files.**
**Revision:** Added. **[INCORPORATED]**

### Round 3 (user review)

**Pool latency concern:** `unique_ptr` adds pointer chase + destroys contiguous layout. Pool access IS on the hot path.
**Revision:** Switched from unique_ptr to pointer-rebind approach (Option B). Added TODO for future inline-spill refactor (Option C). **[INCORPORATED]**

**No MSVC:** Dropped MSVC ticket entirely. README will state GCC/Clang only.
**Revision:** Removed Ticket 2.3 (MSVC intrinsics). Renumbered. README ticket updated to drop MSVC claim. **[INCORPORATED]**

**Rename not remove:** `TapeBook::best_px<IsBid>()` renamed to `tape_best_px<IsBid>()`, kept as public method.
**Revision:** Ticket 1.2 updated. **[INCORPORATED]**

---

## Final Sprint/Ticket Summary

| Sprint | Ticket | Title | Priority | Est. complexity |
|--------|--------|-------|----------|-----------------|
| 1 | 1.1 | Fix Book move safety via pointer-rebind | P0 | Medium |
| 1 | 1.2 | Rename best_px to tape_best_px | P1 | Trivial |
| 2 | 2.1 | Add upper-bound clamp to compute_anchor | P2 | Small |
| 2 | 2.2 | Fix set_qty no-op delete return value | P2 | Trivial |
| 2 | 2.3 | Fix README.md API examples, drop MSVC claim | P2 | Trivial |
| 3 | 3.1 | Complete NullSink interface | P3 | Trivial |
| 3 | 3.2 | Document spill overflow behavior | P3 | Trivial |
| 3 | 3.3 | Add single-include sync script | P3 | Small |
