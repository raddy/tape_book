#pragma once
#include "tape_book/config.hpp"
#include "tape_book/types.hpp"
#include "tape_book/spill_buffer.hpp"
#include "tape_book/spill_pool.hpp"
#include "tape_book/tape.hpp"

namespace tape_book {

// TODO: Two-layer tape optimization for BBO-hot workloads.
// Idea: add a tiny sorted array (4-8 entries per side) in front of the tape
// that always holds the best K levels. best_px()/best_qty() become a direct
// array read (no bitset scan). Canceling the best just falls through to the
// next entry in the hot buffer — only refill from the tape when the buffer
// runs low. Promotion/demotion between hot buffer and tape on insert/erase.
// Main benefit: guaranteed L1-resident BBO data (~64 bytes vs 2KB+ tape).
// Main cost: extra routing branch on every set(), recenter must drain/refill.

template <i32 N, typename PriceT, typename QtyT>
struct TapeBook {
  using price_type = PriceT;
  using qty_type   = QtyT;

  static constexpr i32 N32 = static_cast<i32>(N);
  static constexpr i64 N64 = static_cast<i64>(N);

  [[nodiscard]] static constexpr price_type compute_anchor(price_type px, i64 offset) noexcept {
    constexpr auto min_px = std::numeric_limits<price_type>::lowest();
    constexpr auto max_px = std::numeric_limits<price_type>::max();
    constexpr auto min_anchor = static_cast<price_type>(min_px + (N64 - 1));
    constexpr auto max_anchor = static_cast<price_type>(max_px - (N64 - 1));
    if (px < min_px + offset) return min_anchor;
    // px - offset is computed in i64 due to offset being i64
    i64 result = static_cast<i64>(px) - offset;
    return (result > max_anchor) ? max_anchor : static_cast<price_type>(result);
  }

  Tape<N, true,  price_type, qty_type> bids;
  Tape<N, false, price_type, qty_type> asks;
  DynSpillBuffer<price_type, qty_type> spill;

  explicit TapeBook(i32 max_cap = 4096,
                    SpillPool<price_type, qty_type>* pool = nullptr) noexcept
      : spill(max_cap, pool) {}

  TB_ALWAYS_INLINE void reset(price_type anchor) noexcept {
    bids.reset(anchor);
    asks.reset(anchor);
    spill.clear();
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void reset_at_mid(price_type mid_px) noexcept {
    const price_type anchor = compute_anchor(mid_px, N64 / 2);
    if constexpr (IsBid) bids.reset(anchor);
    else                 asks.reset(anchor);
  }

  [[nodiscard]] TB_ALWAYS_INLINE price_type best_bid_px() const noexcept {
    const auto tape_best = bids.best_px();
    const auto spill_best = spill.template best_px<true>();
    return (tape_best > spill_best) ? tape_best : spill_best;
  }

  [[nodiscard]] TB_ALWAYS_INLINE price_type best_ask_px() const noexcept {
    const auto tape_best = asks.best_px();
    const auto spill_best = spill.template best_px<false>();
    return (tape_best < spill_best) ? tape_best : spill_best;
  }

  [[nodiscard]] TB_ALWAYS_INLINE qty_type best_bid_qty() const noexcept {
    const auto tape_best  = bids.best_px();
    const auto spill_best = spill.template best_px<true>();
    return (tape_best >= spill_best)
           ? bids.best_qty()
           : spill.template best_qty<true>();
  }

  [[nodiscard]] TB_ALWAYS_INLINE qty_type best_ask_qty() const noexcept {
    const auto tape_best  = asks.best_px();
    const auto spill_best = spill.template best_px<false>();
    return (tape_best <= spill_best)
           ? asks.best_qty()
           : spill.template best_qty<false>();
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void erase_better(price_type px) noexcept {
    if constexpr (IsBid) bids.erase_better(px, spill);
    else                 asks.erase_better(px, spill);
  }

  template <bool IsBid, typename TapeT>
  [[nodiscard]] TB_ALWAYS_INLINE UpdateResult
  set_impl(TapeT& tape, price_type px, qty_type q) noexcept {
    auto rc = tape.set_qty(px, q, spill);
    if (TB_LIKELY(rc != UpdateResult::Promote)) return rc;

    price_type A = compute_anchor(px, N64 / 2);
    const price_type minA = compute_anchor(px, N64 - 1);
    if (A < minA) A = minA;
    if (A > px)   A = px;
    TB_ASSUME(A >= TapeT::min_valid_anchor() && A <= TapeT::max_valid_anchor());

    tape.recenter_to_anchor(A, spill);

    const price_type lo = tape.anchor();
    const price_type hi =
        static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);

    NullSink nospill;
    spill.template drain<IsBid>(lo, hi, [&](price_type p, qty_type qq) {
      (void)tape.set_qty(p, qq, nospill);
    });

    return tape.set_qty(px, q, nospill);
  }

  template <bool IsBid>
  [[nodiscard]] TB_ALWAYS_INLINE UpdateResult set(price_type px, qty_type q) noexcept {
    if constexpr (IsBid)
      return set_impl<true>(bids, px, q);
    else
      return set_impl<false>(asks, px, q);
  }

  [[nodiscard]] TB_ALWAYS_INLINE UpdateResult set(bool is_bid, price_type px, qty_type q) noexcept {
    return is_bid ? set<true>(px, q) : set<false>(px, q);
  }

  TB_ALWAYS_INLINE void recenter_bid(price_type new_anchor) noexcept {
    bids.recenter_to_anchor(new_anchor, spill);
    const price_type lo = bids.anchor();
    const price_type hi = static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);
    NullSink nospill;
    spill.template drain<true>(lo, hi, [&](price_type px, qty_type q) {
      (void)bids.set_qty(px, q, nospill);
    });
  }

  TB_ALWAYS_INLINE void recenter_ask(price_type new_anchor) noexcept {
    asks.recenter_to_anchor(new_anchor, spill);
    const price_type lo = asks.anchor();
    const price_type hi = static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);
    NullSink nospill;
    spill.template drain<false>(lo, hi, [&](price_type px, qty_type q) {
      (void)asks.set_qty(px, q, nospill);
    });
  }

  [[nodiscard]] TB_ALWAYS_INLINE bool crossed_on_tape() const noexcept {
    const price_type b = bids.best_px();
    const price_type a = asks.best_px();
    return (b != lowest_px<price_type>() &&
            a != highest_px<price_type>() &&
            b >= a);
  }
};

template <i32 N,
          typename PriceT,
          typename QtyT>
struct Book {
  using price_type = PriceT;
  using qty_type   = QtyT;
  using Core       = TapeBook<N, PriceT, QtyT>;

  Core core;

  explicit Book(i32 max_cap = 4096,
                SpillPool<price_type, qty_type>* pool = nullptr) noexcept
      : core(max_cap, pool) {}

  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;

  Book(Book&&) noexcept = default;
  Book& operator=(Book&&) noexcept = default;

  TB_ALWAYS_INLINE void reset(price_type anchor_px) noexcept { core.reset(anchor_px); }

  template <bool IsBid>
  TB_ALWAYS_INLINE void reset_at_mid(price_type mid_px) noexcept {
    core.template reset_at_mid<IsBid>(mid_px);
  }

  TB_ALWAYS_INLINE UpdateResult set(bool is_bid, price_type px, qty_type q) noexcept {
    return core.set(is_bid, px, q);
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE UpdateResult set(price_type px, qty_type q) noexcept {
    return core.template set<IsBid>(px, q);
  }

  [[nodiscard]] TB_ALWAYS_INLINE price_type best_bid_px() const noexcept { return core.best_bid_px(); }
  [[nodiscard]] TB_ALWAYS_INLINE price_type best_ask_px() const noexcept { return core.best_ask_px(); }
  [[nodiscard]] TB_ALWAYS_INLINE qty_type   best_bid_qty() const noexcept { return core.best_bid_qty(); }
  [[nodiscard]] TB_ALWAYS_INLINE qty_type   best_ask_qty() const noexcept { return core.best_ask_qty(); }

  [[nodiscard]] TB_ALWAYS_INLINE bool crossed_on_tape() const noexcept {
    return core.crossed_on_tape();
  }

  [[nodiscard]] TB_ALWAYS_INLINE bool crossed() const noexcept {
    const price_type b = best_bid_px();
    const price_type a = best_ask_px();
    return (b != lowest_px<price_type>() &&
            a != highest_px<price_type>() &&
            b >= a);
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void erase_better(price_type px) noexcept {
    core.template erase_better<IsBid>(px);
  }

  TB_ALWAYS_INLINE void recenter_bid(price_type new_anchor) noexcept {
    core.recenter_bid(new_anchor);
  }

  TB_ALWAYS_INLINE void recenter_ask(price_type new_anchor) noexcept {
    core.recenter_ask(new_anchor);
  }

  [[nodiscard]] TB_ALWAYS_INLINE bool verify_invariants() const noexcept {
    return core.bids.verify_invariants() && core.asks.verify_invariants();
  }
};

}  // namespace tape_book
