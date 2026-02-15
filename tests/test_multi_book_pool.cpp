#include <cassert>
#include <iostream>
#include <type_traits>
#include <vector>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

// ── Compile-time check — Book is not copyable ──
static_assert(!std::is_copy_constructible_v<Book32>);
static_assert(!std::is_copy_assignable_v<Book32>);
static_assert(std::is_move_constructible_v<Book32>);
static_assert(std::is_move_assignable_v<Book32>);

static void test_reallocation_survival() {
  // Create pool WITHOUT reserving — forces vector reallocation
  using SmallBook = Book<64, i32, u32>;
  std::vector<SmallBook> books;
  // Do NOT reserve — we want reallocation to happen

  books.emplace_back(512);
  books[0].reset(1000);
  books[0].set<true>(1005, 10);
  books[0].set<false>(1010, 20);
  assert(books[0].best_bid_px() == 1005);
  assert(books[0].best_ask_px() == 1010);

  // Force many reallocations
  for (int i = 0; i < 100; ++i) {
    books.emplace_back(512);
    books.back().reset(2000 + i);
    books.back().set<true>(2005 + i, static_cast<u32>(i + 1));
  }

  // Verify the FIRST book still works after all reallocations
  assert(books[0].best_bid_px() == 1005);
  assert(books[0].best_bid_qty() == 10);
  assert(books[0].best_ask_px() == 1010);
  assert(books[0].best_ask_qty() == 20);
  assert(books[0].verify_invariants());

  // Verify a book in the middle survived too
  // books[50] = loop i=49: reset(2049), set<true>(2054, 50)
  assert(books[50].best_bid_px() == 2054);
  assert(books[50].best_bid_qty() == 50);
  assert(books[50].verify_invariants());

  std::cout << "  reallocation_survival ... OK\n";
}

static void test_move_correctness() {
  using SmallBook = Book<64, i32, u32>;

  SmallBook b1(512);
  b1.reset(1000);
  b1.set<true>(1005, 10);
  b1.set<true>(1010, 20);
  b1.set<false>(1020, 30);

  // Force a spill: set a bid far away from the tape window
  b1.set<true>(5000, 42);
  assert(b1.best_bid_px() == 5000);
  assert(b1.best_bid_qty() == 42);

  // Move-construct
  SmallBook b2(std::move(b1));
  assert(b2.best_bid_px() == 5000);
  assert(b2.best_bid_qty() == 42);
  assert(b2.best_ask_px() == 1020);
  assert(b2.best_ask_qty() == 30);
  assert(b2.verify_invariants());

  // Source after move: must be destructible, spill pointers nulled
  // (best_px returns sentinel since spill was moved out)

  // Verify spill buffer works on moved-to book (set another spilled level)
  b2.set<false>(100, 99);
  assert(b2.best_ask_px() == 100);
  assert(b2.best_ask_qty() == 99);

  // Move-assign
  SmallBook b3(512);
  b3.reset(500);
  b3.set<true>(510, 7);
  b3 = std::move(b2);
  assert(b3.best_bid_px() == 5000);
  assert(b3.best_bid_qty() == 42);
  assert(b3.best_ask_px() == 100);
  assert(b3.best_ask_qty() == 99);
  assert(b3.verify_invariants());

  std::cout << "  move_correctness ... OK\n";
}

int main() {
  test_reallocation_survival();
  test_move_correctness();

  MultiBookPool3<i32, u32, 64, 128, 256> pool;

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
