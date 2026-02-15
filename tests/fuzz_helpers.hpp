#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
#include <iostream>
#include <algorithm>
#include <memory>
#include <utility>
#include "tape_book/tape_book.hpp"

using namespace tape_book;

using px16  = std::int16_t;
using qty16 = std::uint16_t;

// ═══════════════════════════════════════════════════════════
// Reference model
// ═══════════════════════════════════════════════════════════

template <typename PriceT, typename QtyT>
struct RefSide {
  std::map<PriceT, QtyT> levels;

  void set(PriceT px, QtyT q) {
    q == QtyT{0} ? (void)levels.erase(px) : (void)(levels[px] = q);
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

template <typename PriceT, typename QtyT>
struct RefBook {
  RefSide<PriceT, QtyT> bid, ask;

  void set(bool is_bid, PriceT px, QtyT q) {
    (is_bid ? bid : ask).set(px, q);
  }

  PriceT best_bid_px() const { return bid.best_px_bid(); }
  PriceT best_ask_px() const { return ask.best_px_ask(); }
  QtyT best_bid_qty() const { return bid.best_qty_bid(); }
  QtyT best_ask_qty() const { return ask.best_qty_ask(); }

  bool crossed() const {
    auto b = best_bid_px(), a = best_ask_px();
    return b != lowest_px<PriceT>() && a != highest_px<PriceT>() && b >= a;
  }
};

// ═══════════════════════════════════════════════════════════
// Level collection helpers
// ═══════════════════════════════════════════════════════════

template <bool IsBid, typename BookT, typename PriceT, typename QtyT>
static void collect_separate(const BookT& b, std::map<PriceT, QtyT>& out) {
  out.clear();
  NullSink ns;
  auto collector = [&](PriceT px, QtyT q) -> bool {
    if (q != QtyT{0}) out[px] = q;
    return true;
  };
  if constexpr (IsBid) {
    b.core.bids.iterate_from_best(collector, ns);
    b.core.spill.template iterate_pending<true>(collector, lowest_px<PriceT>());
  } else {
    b.core.asks.iterate_from_best(collector, ns);
    b.core.spill.template iterate_pending<false>(collector, highest_px<PriceT>());
  }
}

template <bool IsBid, typename BookT, typename PriceT, typename QtyT>
static void collect_chained(
    const BookT& b,
    std::vector<std::pair<PriceT, QtyT>>& ordered,
    std::map<PriceT, QtyT>& out) {
  ordered.clear();
  out.clear();
  auto collector = [&](PriceT px, QtyT q) -> bool {
    if (q != QtyT{0}) {
      ordered.push_back({px, q});
      out[px] = q;
    }
    return true;
  };
  if constexpr (IsBid)
    b.core.bids.iterate_from_best(collector, b.core.spill);
  else
    b.core.asks.iterate_from_best(collector, b.core.spill);
}

// ═══════════════════════════════════════════════════════════
// Operations
// ═══════════════════════════════════════════════════════════

enum class OpKind : int {
  AddUpdate = 0, Cancel = 1, EraseBetter = 2,
  RecenterBid = 3, RecenterAsk = 4,
};

// ═══════════════════════════════════════════════════════════
// Validation
// ═══════════════════════════════════════════════════════════

static constexpr int DEEP_CHECK_INTERVAL = 25;

template <typename BookT, typename PriceT, typename QtyT>
static void check_light(
    uint64_t seed, int step, OpKind op, bool is_bid,
    PriceT px, QtyT q,
    const BookT& b, const RefBook<PriceT, QtyT>& ref,
    const char* tag) {
  auto fail = [&](const char* what) {
    std::cerr << "FAIL [" << tag << "]: " << what << "\n"
              << "  seed=" << seed << " step=" << step
              << " op=" << static_cast<int>(op)
              << " side=" << (is_bid ? "BID" : "ASK")
              << " px=" << px << " q=" << q << "\n"
              << "  book bid=" << b.best_bid_px() << "/" << b.best_bid_qty()
              << "  ref bid=" << ref.best_bid_px() << "/" << ref.best_bid_qty() << "\n"
              << "  book ask=" << b.best_ask_px() << "/" << b.best_ask_qty()
              << "  ref ask=" << ref.best_ask_px() << "/" << ref.best_ask_qty() << "\n";
    std::abort();
  };

  if (b.best_bid_px()  != ref.best_bid_px())  fail("best_bid_px");
  if (b.best_ask_px()  != ref.best_ask_px())  fail("best_ask_px");
  if (b.best_bid_qty() != ref.best_bid_qty()) fail("best_bid_qty");
  if (b.best_ask_qty() != ref.best_ask_qty()) fail("best_ask_qty");

  if (b.crossed() != ref.crossed()) fail("crossed");
  if (b.crossed_on_tape() && !b.crossed()) fail("crossed_on_tape but not crossed");

  if (b.crossed_on_tape()) {
    auto tb = b.core.bids.best_px();
    auto ta = b.core.asks.best_px();
    if (tb != lowest_px<PriceT>() && ta != highest_px<PriceT>() && tb < ta)
      fail("crossed_on_tape but tape bid < tape ask");
  }

  if (!b.verify_invariants()) fail("verify_invariants");
}

template <typename BookT, typename PriceT, typename QtyT>
static void check_deep(
    uint64_t seed, int step, OpKind op, bool is_bid,
    PriceT px, QtyT q,
    const BookT& b, const RefBook<PriceT, QtyT>& ref,
    const char* tag) {
  auto fail = [&](const char* what) {
    std::cerr << "DEEP FAIL [" << tag << "]: " << what << "\n"
              << "  seed=" << seed << " step=" << step
              << " op=" << static_cast<int>(op)
              << " side=" << (is_bid ? "BID" : "ASK")
              << " px=" << px << " q=" << q << "\n";
    std::abort();
  };

  std::map<PriceT, QtyT> sep_bid, sep_ask;
  collect_separate<true,  BookT, PriceT, QtyT>(b, sep_bid);
  collect_separate<false, BookT, PriceT, QtyT>(b, sep_ask);
  if (sep_bid != ref.bid.levels) fail("bid levels (separate)");
  if (sep_ask != ref.ask.levels) fail("ask levels (separate)");

  std::vector<std::pair<PriceT, QtyT>> ord_bid, ord_ask;
  std::map<PriceT, QtyT> chain_bid, chain_ask;
  collect_chained<true,  BookT, PriceT, QtyT>(b, ord_bid, chain_bid);
  collect_chained<false, BookT, PriceT, QtyT>(b, ord_ask, chain_ask);
  if (chain_bid != ref.bid.levels) fail("bid levels (chained)");
  if (chain_ask != ref.ask.levels) fail("ask levels (chained)");

  if (chain_bid.size() != ord_bid.size()) fail("duplicate price in bid iteration");
  if (chain_ask.size() != ord_ask.size()) fail("duplicate price in ask iteration");

  // NOTE: chained iteration (tape->spill) does NOT guarantee sorted output.

  auto exp_bid = sep_bid.empty() ? lowest_px<PriceT>() : sep_bid.rbegin()->first;
  auto exp_ask = sep_ask.empty() ? highest_px<PriceT>() : sep_ask.begin()->first;
  if (b.best_bid_px() != exp_bid) fail("best_bid_px vs collected");
  if (b.best_ask_px() != exp_ask) fail("best_ask_px vs collected");
}

// ═══════════════════════════════════════════════════════════
// FuzzCtx — holds book + ref, applies ops, verifies
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
struct FuzzCtx {
  using BookT = Book<N, PriceT, QtyT>;

  static constexpr PriceT anchor_lo =
      std::numeric_limits<PriceT>::lowest() + static_cast<PriceT>(N - 1);
  static constexpr PriceT anchor_hi =
      std::numeric_limits<PriceT>::max() - static_cast<PriceT>(N - 1);

  std::unique_ptr<BookT> book;
  RefBook<PriceT, QtyT> ref;
  uint64_t seed;
  const char* tag;
  int step{0};
  bool invariants_only{false};  // when true, skip ref comparison (eviction makes it invalid)

  FuzzCtx(uint64_t s, const char* t, PriceT anchor = PriceT{0}, i32 max_cap = 4096)
      : book(std::make_unique<BookT>(max_cap)), seed(s), tag(t),
        invariants_only(max_cap < N) {
    book->reset(anchor);
  }

  PriceT clamp_anchor(PriceT a) const noexcept {
    if (a < anchor_lo) return anchor_lo;
    if (a > anchor_hi) return anchor_hi;
    return a;
  }

  // Execute an operation on both book and ref.
  // For AddUpdate/Cancel/EraseBetter: px is the price, q is the quantity.
  // For RecenterBid/RecenterAsk: px is the anchor (caller must clamp).
  void apply(OpKind op, bool is_bid, PriceT px, QtyT q) {
    switch (op) {
      case OpKind::AddUpdate:
        (void)book->set(is_bid, px, q);
        ref.set(is_bid, px, q);
        break;
      case OpKind::Cancel:
        (void)book->set(is_bid, px, QtyT{0});
        ref.set(is_bid, px, QtyT{0});
        break;
      case OpKind::EraseBetter:
        if (is_bid) {
          book->template erase_better<true>(px);
          std::erase_if(ref.bid.levels, [px](auto& p) { return p.first >= px; });
        } else {
          book->template erase_better<false>(px);
          std::erase_if(ref.ask.levels, [px](auto& p) { return p.first <= px; });
        }
        break;
      case OpKind::RecenterBid:
        book->recenter_bid(px);
        break;
      case OpKind::RecenterAsk:
        book->recenter_ask(px);
        break;
    }
  }

  // Verify book vs ref, then increment step.
  // px/q are for diagnostics only. force_deep forces a deep check.
  void verify(OpKind op, bool is_bid, PriceT px, QtyT q, bool force_deep = false) {
    if (invariants_only) {
      // Small max_cap: spill eviction makes ref model diverge.
      // Only verify structural invariants (no crash + tape consistency).
      if (!book->verify_invariants()) {
        std::cerr << "FAIL [" << tag << "]: verify_invariants\n"
                  << "  seed=" << seed << " step=" << step
                  << " op=" << static_cast<int>(op)
                  << " side=" << (is_bid ? "BID" : "ASK")
                  << " px=" << px << " q=" << q << "\n";
        std::abort();
      }
    } else {
      check_light(seed, step, op, is_bid, px, q, *book, ref, tag);
      if (force_deep || step % DEEP_CHECK_INTERVAL == 0)
        check_deep(seed, step, op, is_bid, px, q, *book, ref, tag);
    }
    ++step;
  }
};

// ═══════════════════════════════════════════════════════════
// Test runner
// ═══════════════════════════════════════════════════════════

static int g_total = 0, g_passed = 0;

template <typename Fn>
static void run(const char* name, Fn&& fn) {
  ++g_total;
  std::cout << "  " << name << " ... " << std::flush;
  fn();
  ++g_passed;
  std::cout << "OK\n";
}
