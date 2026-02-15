# Claude Code Guidelines for tape_book

## Build & Test

This is a C++20 header-only library. Build with CMake:

```
cmake -B build -DTAPE_BOOK_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure
```

After any code change, rebuild and run tests before considering work complete.

## Project Structure

- `include/tape_book/` — Multi-file headers (canonical source)
- `single_include/tape_book.hpp` — Single-header distribution (regenerate after header changes)
- `tests/` — Test suite (basic, fuzz, multi-book pool)

## Constraints

- No exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Zero external dependencies (stdlib only)
- Relies on GCC/Clang builtins (`__builtin_ctzll`, `__builtin_clzll`)
- Tape size N must be power-of-2, multiple of 64

## Architecture

- **Tape**: Fixed-size sparse array indexed by `price - anchor`, with 64-bit bitset tracking for O(1) best-price lookup
- **SpillBuffer**: Sorted overflow for prices outside tape range; binary-search insert, worst-price eviction when full
- **Book**: Combines bid/ask tapes + shared spill buffer; handles recentering and promotion automatically
- **MultiBookPool3**: Manages multiple book instances across 3 capacity tiers (High/Medium/Low)

## Single-Header Sync

`single_include/tape_book.hpp` must stay in sync with `include/tape_book/`. When modifying headers, regenerate the single-header version to include all changes in the correct order.
