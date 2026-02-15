#include <cassert>
#include <climits>
#include <ranges>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

// Book<1024> should be ~8.5 KB (tape data + inline spill metadata), not ~72 KB
static_assert(sizeof(Book<1024, i32, u32>) < 9000);

static constexpr auto N = 256;
using BookT = Book<N, i32, u32>;

static void test_basic_operations() {
  BookT b(512);
  b.reset(1000);

  assert(b.set<true>(1005, 10) == UpdateResult::Insert);
  assert(b.set<false>(1010, 20) == UpdateResult::Insert);
  assert(b.best_bid_px() == 1005 && b.best_ask_px() == 1010);
  assert(b.best_bid_qty() == 10 && b.best_ask_qty() == 20);
  assert(!b.crossed_on_tape() && !b.crossed());

  assert(b.set<true>(1005, 15) == UpdateResult::Update);
  assert(b.best_bid_qty() == 15);

  assert(b.set<true>(1005, 0) == UpdateResult::Erase);
  assert(b.best_bid_px() == lowest_px<i32>() && b.best_bid_qty() == 0);

  assert(b.set<true>(1005, 0) == UpdateResult::Erase);

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<true>(1005, 15);
  b.set<true>(1010, 20);
  assert(b.best_bid_px() == 1010 && b.best_bid_qty() == 20);

  b.set<true>(1010, 0);
  assert(b.best_bid_px() == 1005 && b.best_bid_qty() == 15);
  b.set<true>(1005, 0);
  assert(b.best_bid_px() == 1000 && b.best_bid_qty() == 10);
  b.set<true>(1000, 0);
  assert(b.best_bid_px() == lowest_px<i32>() && b.best_bid_qty() == 0);

  b.reset(1000);
  assert(b.best_bid_px() == lowest_px<i32>() && b.best_ask_px() == highest_px<i32>());
  assert(b.best_bid_qty() == 0 && b.best_ask_qty() == 0);
  assert(!b.crossed_on_tape() && !b.crossed());

  b.set<true>(1000, 10);
  b.set<true>(1000 + N - 1, 20);
  assert(b.best_bid_px() == 1000 + N - 1 && b.best_bid_qty() == 20);

  assert(b.verify_invariants());
}

static void test_spill_buffer() {
  BookT b(512);
  b.reset(1000);

  b.set<true>(1100, 10);
  assert(b.set<true>(500, 5) == UpdateResult::Spill);
  assert(b.best_bid_px() == 1100);

  b.reset(1000);
  b.set<true>(1100, 10);
  b.set<true>(2000, 20);
  assert(b.best_bid_px() == 2000 && b.best_bid_qty() == 20);

  b.reset(1000);
  b.set<true>(2000, 15);
  assert(b.best_bid_px() == 2000);

  b.reset(1000);
  b.set<true>(1100, 10);
  assert(b.set<true>(2000, 0) == UpdateResult::Spill);
  assert(b.best_bid_px() == 1100);

  assert(b.verify_invariants());
}

static void test_crossed_states() {
  BookT b(512);
  b.reset(1000);

  b.set<true>(1000, 10);
  b.set<false>(1010, 20);
  assert(!b.crossed_on_tape() && !b.crossed());

  b.reset(1000);
  b.set<true>(1010, 10);
  b.set<false>(1005, 20);
  assert(b.crossed_on_tape() && b.crossed());

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<false>(1010, 20);
  assert(!b.crossed());
  b.set<true>(1010, 15);
  assert(b.crossed());

  assert(b.verify_invariants());
}

static void test_erase_better() {
  BookT b(512);
  b.reset(1000);

  b.set<true>(1000, 10);
  b.set<true>(1005, 15);
  b.set<true>(1010, 20);
  b.erase_better<true>(1005);
  assert(b.best_bid_px() == 1000 && b.best_bid_qty() == 10);

  b.reset(1000);
  b.set<false>(1010, 10);
  b.set<false>(1015, 15);
  b.set<false>(1020, 20);
  b.erase_better<false>(1015);
  assert(b.best_ask_px() == 1020 && b.best_ask_qty() == 20);

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<true>(1005, 15);
  b.set<true>(1010, 20);
  b.erase_better<true>(999);
  assert(b.best_bid_px() == lowest_px<i32>() && b.best_bid_qty() == 0);

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<true>(1005, 15);
  b.erase_better<true>(1020);
  assert(b.best_bid_px() == 1005 && b.best_bid_qty() == 15);

  b.reset(1000);
  b.erase_better<true>(1000);
  assert(b.best_bid_px() == lowest_px<i32>());

  assert(b.verify_invariants());
}

static void test_anchor_and_recentering() {
  BookT b(512);
  b.reset(1000);

  b.set<true>(1100, 10);
  auto old_anchor = b.core.bids.anchor();
  b.set<true>(2000, 20);
  assert(b.core.bids.anchor() != old_anchor);
  assert(b.best_bid_px() == 2000 && b.best_bid_qty() == 20);

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<true>(1050, 15);
  b.set<true>(2000, 20);
  assert(b.best_bid_px() == 2000);

  b.reset(1000);
  b.set<true>(1050, 10);
  b.recenter_bid(1025);
  assert(b.core.bids.anchor() == 1025 && b.best_bid_px() == 1050);

  b.reset(1000);
  b.set<false>(1050, 10);
  b.recenter_ask(1025);
  assert(b.core.asks.anchor() == 1025 && b.best_ask_px() == 1050);

  assert(b.verify_invariants());
}

static void test_edge_cases() {
  BookT b(512);
  b.reset(1000);

  b.set<true>(1000, UINT32_MAX);
  assert(b.best_bid_qty() == UINT32_MAX);
  b.set<true>(1000, UINT32_MAX - 1);
  assert(b.best_bid_qty() == UINT32_MAX - 1);

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<false>(1255, 20);
  assert(b.best_bid_px() == 1000 && b.best_ask_px() == 1255);

  b.reset(1000);
  for (auto i : std::views::iota(0, 10)) {
    b.set<true>(1100 - i * 5, static_cast<u32>(10 + i));
    b.set<false>(1110 + i * 5, static_cast<u32>(20 + i));
  }
  assert(b.best_bid_px() == 1100 && b.best_ask_px() == 1110 && !b.crossed());

  b.reset(1000);
  b.set<true>(1000, 10);
  b.set<true>(1005, 15);
  b.set<true>(1010, 20);
  b.set<true>(1015, 25);
  b.set<true>(1005, 0);
  b.set<true>(1010, 0);
  assert(b.best_bid_px() == 1015);
  b.set<true>(1015, 0);
  assert(b.best_bid_px() == 1000);

  assert(b.verify_invariants());
}

static void test_sequences() {
  BookT b(512);
  b.reset(1000);

  for (auto i : std::views::iota(0, 20))
    b.set<true>(1100 + i, static_cast<u32>(100 + i));
  assert(b.best_bid_px() == 1119);

  b.set<true>(1110, 200);
  assert(b.best_bid_px() == 1119);
  b.erase_better<true>(1110);
  assert(b.best_bid_px() == 1109);

  b.reset(1000);
  b.set<true>(1100, 10);
  b.set<false>(1110, 20);
  b.set<true>(1105, 15);
  b.set<false>(1115, 25);
  b.set<true>(1110, 30);
  assert(b.crossed());
  b.set<true>(1110, 0);
  assert(!b.crossed());

  b.reset(500);
  assert(b.best_bid_px() == lowest_px<i32>() && b.best_ask_px() == highest_px<i32>());
  assert(b.core.bids.anchor() == 500 && b.core.asks.anchor() == 500);

  assert(b.verify_invariants());
}

static void test_compute_anchor_clamp() {
  // N=256 for BookT, so N64=256, N64-1=255
  using Core = BookT::Core;

  // Upper boundary: compute_anchor(INT32_MAX, 32) must not exceed max_anchor = INT32_MAX - 255
  {
    constexpr auto max_anchor = std::numeric_limits<i32>::max() - (256 - 1);
    auto anchor = Core::compute_anchor(INT32_MAX, 32);
    assert(anchor <= max_anchor);
    // INT32_MAX - 32 = 2147483615, max_anchor = 2147483392
    // So it should be clamped to max_anchor
    assert(anchor == max_anchor);
  }

  // Lower boundary: compute_anchor(INT32_MIN, 32) must not go below min_anchor = INT32_MIN + 255
  {
    constexpr auto min_anchor = std::numeric_limits<i32>::lowest() + (256 - 1);
    auto anchor = Core::compute_anchor(INT32_MIN, 32);
    assert(anchor >= min_anchor);
    // INT32_MIN < INT32_MIN + 32 triggers the lower clamp
    assert(anchor == min_anchor);
  }

  // Edge case: compute_anchor(INT32_MAX, 0)
  {
    constexpr auto max_anchor = std::numeric_limits<i32>::max() - (256 - 1);
    auto anchor = Core::compute_anchor(INT32_MAX, 0);
    // INT32_MAX - 0 = INT32_MAX > max_anchor, so clamped
    assert(anchor == max_anchor);
  }

  // Normal case: result within valid range
  {
    auto anchor = Core::compute_anchor(1000, 128);
    assert(anchor == 1000 - 128);
  }

  // compute_anchor(INT32_MIN + 256, 0) — should return INT32_MIN + 256 (>= min_anchor and within max)
  {
    constexpr auto min_anchor = std::numeric_limits<i32>::lowest() + 255;
    auto anchor = Core::compute_anchor(std::numeric_limits<i32>::lowest() + 256, 0);
    // result = INT32_MIN + 256 - 0 = INT32_MIN + 256
    // min_anchor = INT32_MIN + 255, max_anchor = INT32_MAX - 255
    // INT32_MIN + 256 > min_anchor, INT32_MIN + 256 < max_anchor
    assert(anchor == std::numeric_limits<i32>::lowest() + 256);
    (void)min_anchor;
  }
}

static void test_boundary_integration() {
  // Integration test: Book near INT32_MAX
  {
    using SmallBook = Book<64, i32, u32>;
    // max valid anchor for Tape<64> = INT32_MAX - 63
    constexpr i32 max_anchor = std::numeric_limits<i32>::max() - 63;
    SmallBook b(512);
    b.reset(max_anchor);
    // Set at INT32_MAX (index = INT32_MAX - max_anchor = 63, which is valid)
    auto r1 = b.set<true>(std::numeric_limits<i32>::max(), 10u);
    assert(r1 == UpdateResult::Insert);
    assert(b.best_bid_px() == std::numeric_limits<i32>::max());
    // Set at INT32_MAX - 1
    auto r2 = b.set<true>(std::numeric_limits<i32>::max() - 1, 5u);
    assert(r2 == UpdateResult::Insert);
    assert(b.best_bid_px() == std::numeric_limits<i32>::max());
    assert(b.best_bid_qty() == 10u);
    assert(b.verify_invariants());
  }

  // Integration test: Book near INT32_MIN
  {
    using SmallBook = Book<64, i32, u32>;
    // min valid anchor for Tape<64> = INT32_MIN + 63
    constexpr i32 min_anchor = std::numeric_limits<i32>::lowest() + 63;
    SmallBook b(512);
    b.reset(min_anchor);
    // Set at min_anchor (index 0, valid)
    auto r1 = b.set<false>(min_anchor, 10u);
    assert(r1 == UpdateResult::Insert);
    assert(b.best_ask_px() == min_anchor);
    // Set at min_anchor + 1
    auto r2 = b.set<false>(min_anchor + 1, 5u);
    assert(r2 == UpdateResult::Insert);
    // Best ask should be min_anchor (lowest price ask is best)
    assert(b.best_ask_px() == min_anchor);
    assert(b.best_ask_qty() == 10u);
    assert(b.verify_invariants());
  }
}

static void test_nullsink_interface() {
  // Compile-time smoke test: NullSink must satisfy the Sink interface
  // for Tape::erase_better and Tape::iterate_from_best.
  using TapeT = Tape<256, true, i32, u32>;
  TapeT tape;
  tape.reset(1000);
  (void)tape.set_qty(1050, 10u, NullSink{});

  NullSink sink;
  tape.erase_better(1040, sink);

  (void)tape.set_qty(1020, 5u, NullSink{});
  tape.iterate_from_best([](i32, u32) { return true; }, sink);

  // Also test ask-side instantiation
  using TapeAsk = Tape<256, false, i32, u32>;
  TapeAsk ask_tape;
  ask_tape.reset(1000);
  (void)ask_tape.set_qty(1050, 10u, NullSink{});
  ask_tape.erase_better(1060, sink);
  ask_tape.iterate_from_best([](i32, u32) { return true; }, sink);
}

int main() {
  test_basic_operations();
  test_spill_buffer();
  test_crossed_states();
  test_erase_better();
  test_anchor_and_recentering();
  test_edge_cases();
  test_sequences();
  test_compute_anchor_clamp();
  test_boundary_integration();
  test_nullsink_interface();
  return 0;
}
