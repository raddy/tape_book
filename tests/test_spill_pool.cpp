#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

// ── SpillPool standalone tests ──

static void test_pool_alloc_dealloc() {
  SpillPool<i32, u32> pool(1024);
  assert(pool.used_levels() == 0);
  assert(pool.total_levels() == 1024);

  // Allocate a block of 16 (size class 0)
  auto* p1 = pool.allocate(16);
  assert(p1 != nullptr);
  assert(pool.used_levels() == 16);

  // Allocate another block of 32 (size class 1)
  auto* p2 = pool.allocate(32);
  assert(p2 != nullptr);
  assert(pool.used_levels() == 48);  // 16 + 32

  // Deallocate first block
  pool.deallocate(p1, 16);

  // Re-allocate same size — should reuse freed block
  auto* p3 = pool.allocate(16);
  assert(p3 == p1);  // free-list reuse

  // Deallocate nullptr is no-op
  pool.deallocate(nullptr, 16);

  pool.deallocate(p2, 32);
  pool.deallocate(p3, 16);

  std::cout << "  pool_alloc_dealloc ... OK\n";
}

static void test_pool_size_classes() {
  using Pool = SpillPool<i32, u32>;

  // size_class maps capacity to class index
  assert(Pool::size_class(1) == 0);    // 1 → class 0 (16)
  assert(Pool::size_class(16) == 0);   // 16 → class 0 (16)
  assert(Pool::size_class(17) == 1);   // 17 → class 1 (32)
  assert(Pool::size_class(32) == 1);   // 32 → class 1 (32)
  assert(Pool::size_class(33) == 2);   // 33 → class 2 (64)
  assert(Pool::size_class(64) == 2);   // 64 → class 2 (64)
  assert(Pool::size_class(65) == 3);   // 65 → class 3 (128)

  // class_size maps class index to block size
  assert(Pool::class_size(0) == 16);
  assert(Pool::class_size(1) == 32);
  assert(Pool::class_size(2) == 64);
  assert(Pool::class_size(11) == 32768);

  std::cout << "  pool_size_classes ... OK\n";
}

static void test_pool_reallocate() {
  SpillPool<i32, u32> pool(4096);

  // Reallocate from nullptr (just allocates)
  auto* p1 = pool.reallocate(nullptr, 0, 16, 0);
  assert(p1 != nullptr);

  // Write some data
  p1[0] = {100, 10};
  p1[1] = {200, 20};

  // Reallocate to larger size, preserving 2 entries
  auto* p2 = pool.reallocate(p1, 16, 32, 2);
  assert(p2 != nullptr);
  assert(p2[0].px == 100 && p2[0].qty == 10);
  assert(p2[1].px == 200 && p2[1].qty == 20);

  pool.deallocate(p2, 32);

  std::cout << "  pool_reallocate ... OK\n";
}

static void test_pool_exhaustion() {
  // Small arena: only 32 levels
  SpillPool<i32, u32> pool(32);
  assert(pool.alloc_fail_count == 0);

  // Allocate 16 (succeeds, watermark=16)
  auto* p1 = pool.allocate(16);
  assert(p1 != nullptr);

  // Allocate 16 more (succeeds, watermark=32)
  auto* p2 = pool.allocate(16);
  assert(p2 != nullptr);

  // Allocate 16 more — should fail (arena full)
  auto* p3 = pool.allocate(16);
  assert(p3 == nullptr);
  assert(pool.alloc_fail_count == 1);

  // Deallocate one and retry — should succeed via free list
  pool.deallocate(p1, 16);
  auto* p4 = pool.allocate(16);
  assert(p4 == p1);  // reused
  assert(pool.alloc_fail_count == 1);  // no new failure

  pool.deallocate(p2, 16);
  pool.deallocate(p4, 16);

  std::cout << "  pool_exhaustion ... OK\n";
}

static void test_pool_free_list_reuse() {
  SpillPool<i32, u32> pool(256);

  // Allocate and free several blocks of same size class
  std::vector<LevelT<i32, u32>*> ptrs;
  for (int i = 0; i < 8; ++i) {
    ptrs.push_back(pool.allocate(16));
    assert(ptrs.back() != nullptr);
  }

  // Free all in order
  for (auto* p : ptrs) pool.deallocate(p, 16);

  // Re-allocate — all should come from free list (LIFO order)
  for (int i = 7; i >= 0; --i) {
    auto* p = pool.allocate(16);
    assert(p == ptrs[static_cast<size_t>(i)]);
  }

  // Cleanup
  for (auto* p : ptrs) pool.deallocate(p, 16);

  std::cout << "  pool_free_list_reuse ... OK\n";
}

// ── Pool-backed Book tests ──

static void test_pool_backed_book() {
  SpillPool<i32, u32> pool(65536);

  // Create a book that uses the pool
  Book<64, i32, u32> b(1024, &pool);
  b.reset(1000);

  // Normal operations
  b.set<true>(1005, 10u);
  b.set<false>(1010, 20u);
  assert(b.best_bid_px() == 1005);
  assert(b.best_ask_px() == 1010);

  // Force spill by setting a bid far from tape
  b.set<true>(5000, 42u);
  assert(b.best_bid_px() == 5000);
  assert(b.best_bid_qty() == 42u);

  // Pool should have some allocations now
  assert(pool.used_levels() > 0);

  assert(b.verify_invariants());
  std::cout << "  pool_backed_book ... OK\n";
}

static void test_pool_backed_move() {
  SpillPool<i32, u32> pool(65536);

  Book<64, i32, u32> b1(512, &pool);
  b1.reset(1000);
  b1.set<true>(5000, 42u);
  b1.set<false>(100, 99u);

  // Move-construct
  Book<64, i32, u32> b2(std::move(b1));
  assert(b2.best_bid_px() == 5000);
  assert(b2.best_bid_qty() == 42u);
  assert(b2.best_ask_px() == 100);
  assert(b2.best_ask_qty() == 99u);
  assert(b2.verify_invariants());

  // Move-assign
  Book<64, i32, u32> b3(512, &pool);
  b3.reset(500);
  b3 = std::move(b2);
  assert(b3.best_bid_px() == 5000);
  assert(b3.verify_invariants());

  std::cout << "  pool_backed_move ... OK\n";
}

static void test_pool_multi_book_stress() {
  SpillPool<i32, u32> pool(262144);  // 256K levels = 2 MB

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int64_t> px_dist(-500, 500);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);

  // Create many pool-backed books
  constexpr int NUM_BOOKS = 50;
  std::vector<Book<64, i32, u32>> books;
  books.reserve(NUM_BOOKS);
  for (int i = 0; i < NUM_BOOKS; ++i) {
    books.emplace_back(1024, &pool);
    books.back().reset(static_cast<i32>(i * 100));
  }

  // Heavy spill traffic across all books
  for (int step = 0; step < 10000; ++step) {
    int book_idx = static_cast<int>(rng() % NUM_BOOKS);
    auto& b = books[static_cast<size_t>(book_idx)];
    i32 px = static_cast<i32>(book_idx * 100 + px_dist(rng));
    u32 q = static_cast<u32>(qty_dist(rng));
    bool is_bid = (rng() & 1) != 0;
    b.set(is_bid, px, q);
  }

  // Verify all books
  for (auto& b : books)
    assert(b.verify_invariants());

  assert(pool.alloc_fail_count == 0);
  std::cout << "  pool_multi_book_stress ... OK\n";
}

static void test_pool_backed_multibook_pool3() {
  // MultiBookPool3 with pool
  MultiBookPool3<i32, u32, 64, 128, 256> mbp(4096, 131072);

  mbp.reserve_high(10);
  mbp.reserve_medium(10);
  mbp.reserve_low(10);

  auto h1 = mbp.alloc(BookTier::High, 1000);
  auto h2 = mbp.alloc(BookTier::Medium, 1000);
  auto h3 = mbp.alloc(BookTier::Low, 1000);

  mbp.with_book(h1, [](auto& b) {
    b.template set<true>(1005, 10u);
    // Force spill
    b.template set<true>(5000, 42u);
    assert(b.best_bid_px() == 5000);
  });

  mbp.with_book(h2, [](auto& b) {
    b.template set<false>(1010, 20u);
    assert(b.best_ask_px() == 1010);
  });

  mbp.with_book(h3, [](auto& b) {
    b.template set<true>(1000, 5u);
    b.template set<false>(1020, 15u);
    assert(b.best_bid_px() == 1000);
    assert(b.best_ask_px() == 1020);
  });

  assert(mbp.pool_ != nullptr);
  assert(mbp.pool_->alloc_fail_count == 0);

  std::cout << "  pool_backed_multibook_pool3 ... OK\n";
}

// ── Pool-backed fuzz: run existing fuzz patterns with pool ──

static void test_pool_fuzz() {
  SpillPool<i32, u32> pool(262144);

  using BookT = Book<64, i32, u32>;
  BookT book(2048, &pool);
  book.reset(0);

  std::mt19937_64 rng(123);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  std::uniform_int_distribution<int64_t> px_dist(-512, 512);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> roll_dist(0, 99);

  for (int step = 0; step < 50000; ++step) {
    bool is_bid = side_dist(rng) != 0;
    i32 px = static_cast<i32>(px_dist(rng));
    u32 q = static_cast<u32>(qty_dist(rng));
    int roll = static_cast<int>(roll_dist(rng));

    if (roll < 60) {
      book.set(is_bid, px, q);
    } else if (roll < 80) {
      book.set(is_bid, px, 0u);
    } else if (roll < 90) {
      if (is_bid) book.erase_better<true>(px);
      else        book.erase_better<false>(px);
    } else {
      if (is_bid) book.recenter_bid(px);
      else        book.recenter_ask(px);
    }

    if (step % 100 == 0)
      assert(book.verify_invariants());
  }
  assert(book.verify_invariants());
  assert(pool.alloc_fail_count == 0);

  std::cout << "  pool_fuzz ... OK\n";
}

int main() {
  std::cout << "=== SpillPool standalone ===\n";
  test_pool_alloc_dealloc();
  test_pool_size_classes();
  test_pool_reallocate();
  test_pool_exhaustion();
  test_pool_free_list_reuse();

  std::cout << "\n=== Pool-backed Book ===\n";
  test_pool_backed_book();
  test_pool_backed_move();
  test_pool_multi_book_stress();
  test_pool_backed_multibook_pool3();

  std::cout << "\n=== Pool-backed fuzz ===\n";
  test_pool_fuzz();

  std::cout << "\nAll SpillPool tests PASS\n";
  return 0;
}
