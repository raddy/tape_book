#include <cassert>
#include <iostream>
#include "tape_book/multi_book_pool.hpp"

using namespace tape_book;

int main() {
  MultiBookPool3<i32, u32, 64, 512, 128, 1024, 256, 2048> pool;

  pool.reserve_high(10);
  pool.reserve_medium(10);
  pool.reserve_low(10);

  auto h1 = pool.alloc(BookTier::High, 1000);
  auto h2 = pool.alloc(BookTier::Medium, 1000);
  auto h3 = pool.alloc(BookTier::Low, 1000);

  assert(h1.tier == BookTier::High && h1.idx == 0);
  assert(h2.tier == BookTier::Medium && h2.idx == 0);
  assert(h3.tier == BookTier::Low && h3.idx == 0);

  pool.with_book(h1, [](auto& b) {
    b.template set<true>(1005, 10);
    assert(b.best_bid_px() == 1005);
    assert(b.best_bid_qty() == 10);
  });

  pool.with_book(h2, [](auto& b) {
    b.template set<false>(1010, 20);
    assert(b.best_ask_px() == 1010);
    assert(b.best_ask_qty() == 20);
  });

  pool.with_book(h3, [](auto& b) {
    b.template set<true>(1000, 5);
    b.template set<false>(1020, 15);
    assert(b.best_bid_px() == 1000);
    assert(b.best_ask_px() == 1020);
  });

  assert(pool.high(0).core.bids.size() == 64);
  assert(pool.medium(0).core.bids.size() == 128);
  assert(pool.low(0).core.bids.size() == 256);

  pool.high(0).set<true>(1010, 15);
  assert(pool.high(0).best_bid_px() == 1010);

  auto h4 = pool.alloc(BookTier::High, 2000);
  assert(h4.idx == 1);
  pool.with_book(h4, [](auto& b) {
    assert(b.core.bids.anchor() == 2000);
  });

  const auto& const_pool = pool;
  const_pool.with_book(h1, [](const auto& b) {
    assert(b.best_bid_px() == 1010);
  });

  std::cout << "MultiBookPool3 tests PASS\n";
  return 0;
}
