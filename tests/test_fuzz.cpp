#include <cassert>
#include <cstdint>
#include <map>
#include <random>
#include <iostream>
#include <algorithm>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

using PriceT = i32;
using QtyT = u32;
static constexpr auto N = 64;
static constexpr auto CAP = 4096;
using BookT = Book<N, CAP, PriceT, QtyT>;

static constexpr int FUZZ_STEPS = 100'000;
static constexpr std::uint64_t NUM_SEEDS = 16;

enum class OpKind { AddUpdate, Cancel, EraseBetter };

struct RefSide {
  std::map<PriceT, QtyT> levels;

  void set(PriceT px, QtyT q) {
    q == 0 ? levels.erase(px) : levels[px] = q;
  }

  PriceT best_px_bid() const {
    return levels.empty() ? lowest_px<PriceT>() : levels.rbegin()->first;
  }

  PriceT best_px_ask() const {
    return levels.empty() ? highest_px<PriceT>() : levels.begin()->first;
  }

  QtyT best_qty_bid() const {
    return levels.empty() ? QtyT{0} : levels.rbegin()->second;
  }

  QtyT best_qty_ask() const {
    return levels.empty() ? QtyT{0} : levels.begin()->second;
  }
};

struct RefBook {
  RefSide bid, ask;

  void set(bool is_bid, PriceT px, QtyT q) {
    (is_bid ? bid : ask).set(px, q);
  }

  PriceT best_bid_px() const { return bid.best_px_bid(); }
  PriceT best_ask_px() const { return ask.best_px_ask(); }
  QtyT best_bid_qty() const { return bid.best_qty_bid(); }
  QtyT best_ask_qty() const { return ask.best_qty_ask(); }

  bool crossed() const {
    auto [b, a] = std::pair{best_bid_px(), best_ask_px()};
    return b != lowest_px<PriceT>() && a != highest_px<PriceT>() && b >= a;
  }
};

struct DummySink {
  template <bool IsBid, typename Fn>
  void iterate_pending(Fn&&) const noexcept {}
};

template <bool IsBid>
static void collect_side(const BookT& b, std::map<PriceT, QtyT>& out) {
  out.clear();
  DummySink ds;
  auto collector = [&](PriceT px, QtyT q) {
    if (q != 0) out[px] = q;
    return true;
  };

  if constexpr (IsBid) {
    b.core.bids.iterate_from_best(collector, ds);
    b.spill.template iterate_pending<true>(collector, lowest_px<PriceT>());
  } else {
    b.core.asks.iterate_from_best(collector, ds);
    b.spill.template iterate_pending<false>(collector, highest_px<PriceT>());
  }
}

template <typename M>
static void validate_levels(const M& got, const M& ref, const char* side, auto&& fail) {
  if (got.size() != ref.size()) fail(side, " size mismatch");
  auto [itg, itr] = std::pair{got.begin(), ref.begin()};
  for (; itg != got.end(); ++itg, ++itr) {
    if (itr == ref.end() || itg->first != itr->first || itg->second != itr->second)
      fail(side, " levels mismatch");
  }
}

static void check_consistency(std::uint64_t seed, int step, OpKind op,
                               bool is_bid, PriceT px, QtyT q,
                               const BookT& b, const RefBook& ref) {
  auto fail = [&](const char* what, const char* extra = "") {
    std::cerr << "FUZZ FAIL: " << what << extra << "\n"
              << "  seed=" << seed << " step=" << step
              << " op=" << static_cast<int>(op) << " side=" << (is_bid ? "BID" : "ASK")
              << " px=" << px << " q=" << q << "\n"
              << "  book.best_bid_px=" << b.best_bid_px()
              << " ref.best_bid_px=" << ref.best_bid_px()
              << "  book.best_ask_px=" << b.best_ask_px()
              << " ref.best_ask_px=" << ref.best_ask_px() << "\n";
    std::abort();
  };

  if (b.best_bid_px() != ref.best_bid_px()) fail("best_bid_px mismatch");
  if (b.best_ask_px() != ref.best_ask_px()) fail("best_ask_px mismatch");
  if (b.best_bid_qty() != ref.best_bid_qty()) fail("best_bid_qty mismatch");
  if (b.best_ask_qty() != ref.best_ask_qty()) fail("best_ask_qty mismatch");

  if (b.crossed() != ref.crossed()) fail("crossed flag mismatch");
  if (b.crossed_on_tape() && !b.crossed())
    fail("crossed_on_tape true but global crossed() false");

  if (!b.verify_invariants()) fail("verify_invariants failed");

  std::map<PriceT, QtyT> got_bid, got_ask;
  collect_side<true>(b, got_bid);
  collect_side<false>(b, got_ask);

  validate_levels(got_bid, ref.bid.levels, "bid", fail);
  validate_levels(got_ask, ref.ask.levels, "ask", fail);

  auto [got_best_bid_px, got_best_bid_qty] = got_bid.empty()
      ? std::pair{lowest_px<PriceT>(), QtyT{0}}
      : std::pair{got_bid.rbegin()->first, got_bid.rbegin()->second};
  auto [got_best_ask_px, got_best_ask_qty] = got_ask.empty()
      ? std::pair{highest_px<PriceT>(), QtyT{0}}
      : std::pair{got_ask.begin()->first, got_ask.begin()->second};

  if (b.best_bid_px() != got_best_bid_px) fail("best_bid_px vs got_bid mismatch");
  if (b.best_ask_px() != got_best_ask_px) fail("best_ask_px vs got_ask mismatch");
  if (b.best_bid_qty() != got_best_bid_qty) fail("best_bid_qty vs got_bid mismatch");
  if (b.best_ask_qty() != got_best_ask_qty) fail("best_ask_qty vs got_ask mismatch");
}

static void fuzz_once(std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  BookT book;
  RefBook ref;
  book.reset(0);

  using Dist = std::uniform_int_distribution<int>;
  Dist side_dist(0, 1), op_dist(0, 2), px_near(-32, 32), px_far(-256, 256), qty_dist(1, 100);

  for (int step = 0; step < FUZZ_STEPS; ++step) {
    const bool is_bid = side_dist(rng) != 0;
    const OpKind op = static_cast<OpKind>(op_dist(rng));
    const bool use_far = (rng() & 7u) == 0u;
    PriceT px = static_cast<PriceT>(use_far ? px_far(rng) : px_near(rng));
    QtyT q = static_cast<QtyT>(qty_dist(rng));

    switch (op) {
      case OpKind::AddUpdate:
        std::ignore = book.set(is_bid, px, q);
        ref.set(is_bid, px, q);
        break;
      case OpKind::Cancel:
        std::ignore = book.set(is_bid, px, QtyT{0});
        ref.set(is_bid, px, QtyT{0});
        break;
      case OpKind::EraseBetter:
        if (is_bid) {
          book.template erase_better<true>(px);
          std::erase_if(ref.bid.levels, [px](auto& p) { return p.first >= px; });
        } else {
          book.template erase_better<false>(px);
          std::erase_if(ref.ask.levels, [px](auto& p) { return p.first <= px; });
        }
        break;
    }

    check_consistency(seed, step, op, is_bid, px, q, book, ref);
  }

  check_consistency(seed, FUZZ_STEPS, OpKind::AddUpdate, true, 0, 0, book, ref);
}

int main() {
  for (std::uint64_t seed = 1; seed <= NUM_SEEDS; ++seed)
    fuzz_once(seed);
  std::cout << "tape_book fuzz OK\n";
  return 0;
}
