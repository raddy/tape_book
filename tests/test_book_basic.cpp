#include <cassert>
#include <ranges>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

static constexpr auto N = 256;
static constexpr auto CAP = 512;
using BookT = Book<N, CAP, i32, u32>;

static void test_basic_operations() {
  BookT b;
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

  assert(b.set<true>(1005, 0) == UpdateResult::Update);

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
  BookT b;
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
  BookT b;
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
  BookT b;
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
  BookT b;
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
  BookT b;
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
  BookT b;
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

int main() {
  test_basic_operations();
  test_spill_buffer();
  test_crossed_states();
  test_erase_better();
  test_anchor_and_recentering();
  test_edge_cases();
  test_sequences();
  return 0;
}
