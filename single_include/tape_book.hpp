#pragma once

#if defined(_MSC_VER)
#  define TB_ALWAYS_INLINE __forceinline
#else
#  define TB_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define TB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define TB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define TB_LIKELY(x)   (x)
#  define TB_UNLIKELY(x) (x)
#endif

#include <cstdint>
#include <limits>
#include <type_traits>

namespace tape_book {

using i8  = std::int8_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

template <typename PriceT_, typename QtyT_>
struct LevelT {
  PriceT_ px;
  QtyT_   qty;
};

enum class UpdateResult : i8 {
  Insert  = +1,
  Update  =  0,
  Erase   = -1,
  Spill   = -2,
  Promote = +2
};

template <typename PriceT>
[[nodiscard]] constexpr PriceT lowest_px() noexcept {
  return std::numeric_limits<PriceT>::lowest();
}
template <typename PriceT>
[[nodiscard]] constexpr PriceT highest_px() noexcept {
  return std::numeric_limits<PriceT>::max();
}

}  // namespace tape_book

#include <cstring>

namespace tape_book {

template <i32 CAP, typename PriceT, typename QtyT>
struct SpillBuffer {
  static_assert(CAP > 0);
  static_assert(std::is_integral_v<PriceT> && std::is_signed_v<PriceT>);
  static_assert(std::is_integral_v<QtyT>   && std::is_unsigned_v<QtyT>);

  using level_type = LevelT<PriceT, QtyT>;
  using price_type = PriceT;
  using qty_type   = QtyT;

  struct SideFlat {
    level_type a[CAP];
    i32        n{0};

    [[nodiscard]] constexpr TB_ALWAYS_INLINE i32 lb(price_type px) const noexcept {
      i32 lo = 0, hi = n;
      while (lo < hi) {
        i32 mid = (lo + hi) >> 1;
        if (a[mid].px < px) lo = mid + 1;
        else hi = mid;
      }
      return lo;
    }

    template <bool IsBid>
    TB_ALWAYS_INLINE void add_point(price_type px, qty_type q) noexcept {
      const i32 i = lb(px);
      if (i < n && a[i].px == px) {
        if (q == qty_type{0}) {
          if (i + 1 < n)
            std::memmove(&a[i], &a[i + 1], (size_t)(n - i - 1) * sizeof(level_type));
          --n;
        } else {
          a[i].qty = q;
        }
        return;
      }

      if (q == qty_type{0}) return;

      if (n == CAP) {
        if constexpr (IsBid) {
          if (px <= a[0].px) return;
          if (n > 1)
            std::memmove(&a[0], &a[1], (size_t)(n - 1) * sizeof(level_type));
          --n;
        } else {
          if (px >= a[n - 1].px) return;
          --n;
        }
      }

      const i32 j = lb(px);
      if (j < n)
        std::memmove(&a[j + 1], &a[j], (size_t)(n - j) * sizeof(level_type));
      a[j] = level_type{px, q};
      ++n;
    }

    template <class Sink>
    TB_ALWAYS_INLINE void drain_range(price_type lo_px, price_type hi_px, Sink&& sink) noexcept {
      if (n == 0) return;
      const i32 L = lb(lo_px);
      i32 R = L;
      while (R < n && a[R].px <= hi_px) {
        if (a[R].qty) sink(a[R].px, a[R].qty);
        ++R;
      }
      if (L < R) {
        const i32 keepR = n - R;
        if (keepR > 0)
          std::memmove(&a[L], &a[R], (size_t)keepR * sizeof(level_type));
        n = L + keepR;
      }
    }

    template <bool IsBid>
    TB_ALWAYS_INLINE void erase_better(price_type th) noexcept {
      if (n == 0) return;
      i32 w = 0;
      if constexpr (IsBid) {
        for (i32 i = 0; i < n; ++i) {
          if (!(a[i].px >= th)) {
            if (w != i) a[w] = a[i];
            ++w;
          }
        }
      } else {
        for (i32 i = 0; i < n; ++i) {
          if (!(a[i].px <= th)) {
            if (w != i) a[w] = a[i];
            ++w;
          }
        }
      }
      n = w;
    }

    template <bool IsBid, class Fn>
    TB_ALWAYS_INLINE void iterate(Fn&& fn, price_type worst_px) const noexcept {
      if constexpr (IsBid) {
        for (i32 i = n - 1; i >= 0; --i) {
          const auto& lv = a[i];
          if (lv.px < worst_px) break;
          if (!fn(lv.px, lv.qty)) return;
        }
      } else {
        for (i32 i = 0; i < n; ++i) {
          const auto& lv = a[i];
          if (lv.px > worst_px) break;
          if (!fn(lv.px, lv.qty)) return;
        }
      }
    }

    template <bool IsBid>
    [[nodiscard]] constexpr TB_ALWAYS_INLINE price_type best_px() const noexcept {
      if (n == 0) return IsBid ? lowest_px<price_type>() : highest_px<price_type>();
      return IsBid ? a[n - 1].px : a[0].px;
    }

    template <bool IsBid>
    [[nodiscard]] constexpr TB_ALWAYS_INLINE qty_type best_qty() const noexcept {
      if (n == 0) return qty_type{0};
      return IsBid ? a[n - 1].qty : a[0].qty;
    }

    constexpr TB_ALWAYS_INLINE void clear() noexcept { n = 0; }
  };

  SideFlat bid, ask;

  constexpr TB_ALWAYS_INLINE void clear() noexcept {
    bid.clear();
    ask.clear();
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void push(price_type px, qty_type q) noexcept {
    if constexpr (IsBid) bid.template add_point<true>(px, q);
    else                 ask.template add_point<false>(px, q);
  }

  template <bool IsBid, class Fn>
  TB_ALWAYS_INLINE void drain(price_type lo_px, price_type hi_px, Fn&& fn) noexcept {
    if constexpr (IsBid) bid.drain_range(lo_px, hi_px, std::forward<Fn>(fn));
    else                 ask.drain_range(lo_px, hi_px, std::forward<Fn>(fn));
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void erase_better(price_type threshold_px) noexcept {
    if constexpr (IsBid) bid.template erase_better<true>(threshold_px);
    else                 ask.template erase_better<false>(threshold_px);
  }

  template <bool IsBid, class Fn>
  TB_ALWAYS_INLINE void iterate_pending(Fn&& fn,
                                        price_type worst_px = IsBid
                                            ? lowest_px<price_type>()
                                            : highest_px<price_type>()) const noexcept {
    if constexpr (IsBid) bid.template iterate<true>(std::forward<Fn>(fn), worst_px);
    else                 ask.template iterate<false>(std::forward<Fn>(fn), worst_px);
  }

  template <bool IsBid>
  [[nodiscard]] TB_ALWAYS_INLINE price_type best_px() const noexcept {
    if constexpr (IsBid) return bid.template best_px<true>();
    else                 return ask.template best_px<false>();
  }

  template <bool IsBid>
  [[nodiscard]] TB_ALWAYS_INLINE qty_type best_qty() const noexcept {
    if constexpr (IsBid) return bid.template best_qty<true>();
    else                 return ask.template best_qty<false>();
  }
};

struct NullSink {
  template <bool IsBid>
  TB_ALWAYS_INLINE void push(auto, auto) noexcept {}
  TB_ALWAYS_INLINE void clear() noexcept {}
};

}  // namespace tape_book

#include <cassert>
#include <cstring>

namespace tape_book {

template <i32 N, bool IsBid, typename PriceT, typename QtyT>
class Tape {
  static_assert(N > 0 && (N & (N - 1)) == 0);
  static_assert((N % 64) == 0);
  static constexpr i32 WORDS = N / 64;

  static_assert(std::is_integral_v<PriceT> && std::is_signed_v<PriceT>);
  static_assert(std::is_integral_v<QtyT>   && std::is_unsigned_v<QtyT>);

  alignas(64) QtyT qty[N]{};
  alignas(64) u64  bits[WORDS]{};
  PriceT anchor_px{0};
  i32    best_idx{IsBid ? -1 : N};

  static constexpr i64 N64 = static_cast<i64>(N);
  static constexpr i32 N32 = static_cast<i32>(N);

  [[nodiscard]] static constexpr PriceT min_valid_anchor() noexcept {
    return std::numeric_limits<PriceT>::lowest() + (N - 1);
  }

  [[nodiscard]] static constexpr PriceT max_valid_anchor() noexcept {
    return std::numeric_limits<PriceT>::max() - (N - 1);
  }

  [[nodiscard]] static constexpr u64 safe_abs(i64 d) noexcept {
    return (d == std::numeric_limits<i64>::min())
      ? static_cast<u64>(std::numeric_limits<i64>::max()) + 1u
      : static_cast<u64>(d >= 0 ? d : -d);
  }

 public:
  static constexpr i32 size() noexcept { return N32; }

  TB_ALWAYS_INLINE void reset(PriceT anchor) noexcept {
#ifndef NDEBUG
    assert(anchor >= min_valid_anchor() && anchor <= max_valid_anchor());
#endif
    std::memset(qty,  0, sizeof(qty));
    std::memset(bits, 0, sizeof(bits));
    anchor_px = anchor;
    best_idx  = IsBid ? -1 : N;
  }

  [[nodiscard]] TB_ALWAYS_INLINE PriceT anchor() const noexcept { return anchor_px; }

  [[nodiscard]] TB_ALWAYS_INLINE i32 idx_from_price(PriceT px) const noexcept {
    const i64 d = static_cast<i64>(px) - static_cast<i64>(anchor_px);
    return (d >= 0 && d < N64) ? static_cast<i32>(d) : -1;
  }

  [[nodiscard]] TB_ALWAYS_INLINE PriceT price_from_idx(i32 i) const noexcept {
    return static_cast<PriceT>(static_cast<i64>(anchor_px) + static_cast<i64>(i));
  }

  [[nodiscard]] TB_ALWAYS_INLINE PriceT best_px() const noexcept {
    if constexpr (IsBid) {
      return (best_idx < 0) ? lowest_px<PriceT>() : price_from_idx(best_idx);
    } else {
      return (best_idx >= N) ? highest_px<PriceT>() : price_from_idx(best_idx);
    }
  }

  [[nodiscard]] TB_ALWAYS_INLINE QtyT best_qty() const noexcept {
    if constexpr (IsBid) {
      return (best_idx < 0) ? QtyT{0} : qty[best_idx];
    } else {
      return (best_idx >= N) ? QtyT{0} : qty[best_idx];
    }
  }

  [[nodiscard]] TB_ALWAYS_INLINE bool is_empty() const noexcept {
    if constexpr (IsBid) return best_idx < 0;
    else                 return best_idx >= N32;
  }

  [[nodiscard]] bool verify_invariants() const noexcept {
    for (i32 w = 0; w < WORDS; ++w) {
      u64 expect = 0;
      const i32 base = w << 6;
      for (i32 b = 0; b < 64; ++b)
        expect |= (u64)(qty[(size_t)(base + b)] != QtyT{0}) << b;
      if (bits[(size_t)w] != expect) return false;
    }
    const i32 scan = IsBid ? scan_prev_set(N32 - 1) : scan_next_set(0);
    return scan == best_idx;
  }

  [[nodiscard]] TB_ALWAYS_INLINE i32 headroom_dn(i32 H = 0) const noexcept {
    if constexpr (IsBid) {
      if (best_idx < 0) return N32;
      i32 m = (N32 - 1 - best_idx) - H;
      return m > 0 ? m : 0;
    } else {
      return 0;
    }
  }

  [[nodiscard]] TB_ALWAYS_INLINE i32 headroom_up(i32 H = 0) const noexcept {
    if constexpr (IsBid) {
      return 0;
    } else {
      if (best_idx >= N32) return N32;
      i32 m = best_idx - H;
      return m > 0 ? m : 0;
    }
  }

  template <class Sink>
  [[nodiscard]] TB_ALWAYS_INLINE UpdateResult set_qty(PriceT px, QtyT q, Sink&& sink) noexcept {
    i32 i = idx_from_price(px);
    if (TB_UNLIKELY(i < 0 || i >= N32)) {
      if (q == QtyT{0}) {
        sink.template push<IsBid>(px, QtyT{0});
        return UpdateResult::Spill;
      }

      const bool empty = is_empty();
      if (empty) return UpdateResult::Promote;

      const PriceT cur_best = best_px();
      const bool strictly_better = (IsBid ? (px > cur_best) : (px < cur_best));
      if (strictly_better) return UpdateResult::Promote;

      sink.template push<IsBid>(px, q);
      return UpdateResult::Spill;
    }

    QtyT& cell = qty[i];
    if (q == QtyT{0} && cell == QtyT{0}) return UpdateResult::Update;

    i32 w = i >> 6;
    u64 m = 1ULL << (i & 63);

    if (q == QtyT{0}) {
      cell = QtyT{0};
      bits[(size_t)w] &= ~m;
      if (i == best_idx) {
        best_idx = IsBid ? scan_prev_set(i - 1) : scan_next_set(i + 1);
      }
      return UpdateResult::Erase;
    } else {
      auto rc = (cell == QtyT{0}) ? UpdateResult::Insert : UpdateResult::Update;
      cell = q;
      bits[(size_t)w] |= m;
      if constexpr (IsBid) {
        if (i > best_idx) best_idx = i;
      } else {
        if (i < best_idx) best_idx = i;
      }
      return rc;
    }
  }

  template <class Sink>
  TB_ALWAYS_INLINE void recenter_to_anchor(PriceT new_anchor, Sink&& sink) noexcept {
#ifndef NDEBUG
    assert(new_anchor >= min_valid_anchor() && new_anchor <= max_valid_anchor());
#endif
    const i64 d = static_cast<i64>(new_anchor) - static_cast<i64>(anchor_px);
    if (d == 0) return;

    auto spill_one = [&](i32 i) noexcept {
      const QtyT q = qty[(size_t)i];
      if (!q) return;
      const PriceT px = price_from_idx(i);
      sink.template push<IsBid>(px, q);
      qty[(size_t)i] = QtyT{0};
    };

    if (d >= N64 || d <= -N64) {
      for_each_set(0, N32 - 1, spill_one);
      best_idx = IsBid ? -1 : N32;
    } else {
      if (d > 0) {
        const i32 D = (i32)d;
        for_each_set(0, D - 1, spill_one);
      } else {
        const i32 D = (i32)(-d);
        const i32 L = N32 - D;
        for_each_set(L, N32 - 1, spill_one);
      }

      const u64 abs_d = safe_abs(d);
      assert(abs_d < static_cast<u64>(N64));
      const u32 k = static_cast<u32>(abs_d);
      if (k) {
        if (d > 0) rotate_qty_left(k);
        else       rotate_qty_right(k);
      }
    }

    anchor_px = new_anchor;

    for (i32 w = 0; w < WORDS; ++w) {
      const i32 base = w << 6;
      u64 mask = 0;
      for (i32 b = 0; b < 64; ++b)
        mask |= (u64)(qty[(size_t)(base + b)] != QtyT{0}) << b;
      bits[(size_t)w] = mask;
    }

    best_idx = IsBid ? scan_prev_set(N32 - 1) : scan_next_set(0);
  }

  template <class Sink>
  TB_ALWAYS_INLINE void erase_better(PriceT px, Sink&& sink) noexcept {
    const i64 offset = static_cast<i64>(px) - static_cast<i64>(anchor_px);
    if constexpr (IsBid) {
      if (offset < 0) {
        erase_range(0, N32 - 1);
        sink.template erase_better<true>(px);
        return;
      }
      if (offset >= N64) {
        sink.template erase_better<true>(px);
        return;
      }
      const i32 idx = static_cast<i32>(offset);
      erase_range(idx, N32 - 1);
      sink.template erase_better<true>(px);
    } else {
      if (offset < 0) {
        sink.template erase_better<false>(px);
        return;
      }
      if (offset >= N64) {
        erase_range(0, N32 - 1);
        sink.template erase_better<false>(px);
        return;
      }
      const i32 idx = static_cast<i32>(offset);
      erase_range(0, idx);
      sink.template erase_better<false>(px);
    }
  }

  template <typename Fn, typename Sink>
  TB_ALWAYS_INLINE void iterate_from_best(Fn&& fn, Sink&& sink) const noexcept {
    if constexpr (IsBid) {
      for (i32 idx = best_idx; idx >= 0; idx = scan_prev_set(idx - 1)) {
        if (!fn(price_from_idx(idx), qty[idx])) return;
      }
    } else {
      for (i32 idx = best_idx; idx < N32; idx = scan_next_set(idx + 1)) {
        if (!fn(price_from_idx(idx), qty[idx])) return;
      }
    }
    sink.template iterate_pending<IsBid>(std::forward<Fn>(fn));
  }

 private:
  template <typename Fn>
  TB_ALWAYS_INLINE void for_each_set(i32 L, i32 R, Fn&& fn) const noexcept {
    if (L > R) return;
    i32 wl = L >> 6, wr = R >> 6;

    u64 left_mask  = ~0ULL << (L & 63);
    u64 right_mask = (wr == wl)
                         ? ((R & 63) == 63 ? ~0ULL : ((1ULL << (u32)((R & 63) + 1)) - 1ULL))
                         : ~0ULL;

    for (i32 w = wl; w <= wr; ++w) {
      u64 word = bits[(size_t)w];
      if (w == wl) word &= left_mask;
      if (w == wr) word &= right_mask;
      while (word) {
        i32 off = __builtin_ctzll(word);
        fn((w << 6) + off);
        word &= word - 1;
      }
    }
  }

  void erase_range(i32 start_idx, i32 end_idx) noexcept {
    if (start_idx > end_idx) return;
    for_each_set(start_idx, end_idx, [this](i32 i) { qty[i] = QtyT{0}; });

    i32 start_word = start_idx >> 6;
    i32 end_word   = end_idx   >> 6;
    for (i32 w = start_word; w <= end_word; ++w) {
      const i32 base = w << 6;
      u64 mask = 0;
      for (i32 b = 0; b < 64; ++b)
        mask |= (u64)(qty[(size_t)(base + b)] != QtyT{0}) << b;
      bits[(size_t)w] = mask;
    }

    if constexpr (IsBid) {
      if (best_idx >= start_idx) best_idx = scan_prev_set(start_idx - 1);
    } else {
      if (best_idx <= end_idx)   best_idx = scan_next_set(end_idx + 1);
    }
  }

  [[nodiscard]] TB_ALWAYS_INLINE i32 scan_prev_set(i32 idx) const noexcept {
    if (idx < 0) return -1;
    i32 wi = idx >> 6, bi = idx & 63;
    u64 m = bits[(size_t)wi] &
            ((bi == 63) ? ~0ULL : ((1ULL << (bi + 1)) - 1ULL));
    for (i32 w = wi; w >= 0; --w) {
      u64 x = (w == wi) ? m : bits[(size_t)w];
      if (x) {
        i32 off = 63 - __builtin_clzll(x);
        return (w << 6) + off;
      }
    }
    return -1;
  }

  [[nodiscard]] TB_ALWAYS_INLINE i32 scan_next_set(i32 idx) const noexcept {
    if (idx >= N32) return N32;
    i32 wi = idx >> 6, bi = idx & 63;
    u64 m = bits[(size_t)wi] & (~0ULL << (u32)bi);
    for (i32 w = wi; w < WORDS; ++w) {
      u64 x = (w == wi) ? m : bits[(size_t)w];
      if (x) {
        i32 off = __builtin_ctzll(x);
        return (w << 6) + off;
      }
    }
    return N32;
  }

  TB_ALWAYS_INLINE void rotate_qty_right(u32 k) noexcept {
    if (!k) return;
    if (k <= N / 2) {
      alignas(64) QtyT tmp[(N / 2) ? (N / 2) : 1];
      std::memcpy(tmp, qty + (N - k), k * sizeof(QtyT));
      std::memmove(qty + k, qty, (N - k) * sizeof(QtyT));
      std::memcpy(qty, tmp, k * sizeof(QtyT));
    } else {
      u32 left = N - k;
      alignas(64) QtyT tmp[(N / 2) ? (N / 2) : 1];
      std::memcpy(tmp, qty, left * sizeof(QtyT));
      std::memmove(qty, qty + left, (N - left) * sizeof(QtyT));
      std::memcpy(qty + (N - left), tmp, left * sizeof(QtyT));
    }
  }

  TB_ALWAYS_INLINE void rotate_qty_left(u32 k) noexcept {
    k = k & (N - 1);
    if (k == 0) return;
    rotate_qty_right(N - k);
  }
};

}  // namespace tape_book

namespace tape_book {

template <i32 N, class SpillBuf>
struct TapeBook {
  using price_type = typename SpillBuf::price_type;
  using qty_type   = typename SpillBuf::qty_type;

  static constexpr i32 N32 = static_cast<i32>(N);
  static constexpr i64 N64 = static_cast<i64>(N);

  [[nodiscard]] static constexpr price_type compute_anchor(price_type px, i64 offset) noexcept {
    constexpr auto min_px = std::numeric_limits<price_type>::lowest();
    constexpr auto min_anchor = min_px + (N64 - 1);
    return (px < min_px + offset) ? min_anchor : px - offset;
  }

  Tape<N, true,  price_type, qty_type> bids;
  Tape<N, false, price_type, qty_type> asks;
  SpillBuf& spill_buffer;

  explicit TapeBook(SpillBuf& sb) : spill_buffer(sb) {}

  TB_ALWAYS_INLINE void reset(price_type anchor) noexcept {
    bids.reset(anchor);
    asks.reset(anchor);
    spill_buffer.clear();
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void reset_at_mid(price_type mid_px) noexcept {
    const price_type anchor = compute_anchor(mid_px, N64 / 2);
    if constexpr (IsBid) bids.reset(anchor);
    else                 asks.reset(anchor);
  }

  [[nodiscard]] TB_ALWAYS_INLINE price_type best_bid_px() const noexcept {
    const auto tape_best = bids.best_px();
    const auto spill_best = spill_buffer.template best_px<true>();
    return (tape_best > spill_best) ? tape_best : spill_best;
  }

  [[nodiscard]] TB_ALWAYS_INLINE price_type best_ask_px() const noexcept {
    const auto tape_best = asks.best_px();
    const auto spill_best = spill_buffer.template best_px<false>();
    return (tape_best < spill_best) ? tape_best : spill_best;
  }

  template <bool IsBid>
  [[nodiscard]] TB_ALWAYS_INLINE price_type best_px() const noexcept {
    if constexpr (IsBid) return bids.best_px();
    else                 return asks.best_px();
  }

  [[nodiscard]] TB_ALWAYS_INLINE qty_type best_bid_qty() const noexcept {
    const auto tape_best  = bids.best_px();
    const auto spill_best = spill_buffer.template best_px<true>();
    return (tape_best > spill_best)
           ? bids.best_qty()
           : spill_buffer.template best_qty<true>();
  }

  [[nodiscard]] TB_ALWAYS_INLINE qty_type best_ask_qty() const noexcept {
    const auto tape_best  = asks.best_px();
    const auto spill_best = spill_buffer.template best_px<false>();
    return (tape_best < spill_best)
           ? asks.best_qty()
           : spill_buffer.template best_qty<false>();
  }

  [[nodiscard]] TB_ALWAYS_INLINE i32 bid_headroom_dn(i32 H = 0) const noexcept { return bids.headroom_dn(H); }
  [[nodiscard]] TB_ALWAYS_INLINE i32 ask_headroom_up(i32 H = 0) const noexcept { return asks.headroom_up(H); }

  template <bool IsBid>
  TB_ALWAYS_INLINE void erase_better(price_type px) noexcept {
    if constexpr (IsBid) bids.erase_better(px, spill_buffer);
    else                 asks.erase_better(px, spill_buffer);
  }

  template <bool IsBid, typename TapeT>
  [[nodiscard]] TB_ALWAYS_INLINE UpdateResult
  set_impl(TapeT& tape, price_type px, qty_type q) noexcept {
    auto rc = tape.set_qty(px, q, spill_buffer);
    if (TB_LIKELY(rc != UpdateResult::Promote)) return rc;

    price_type A = compute_anchor(px, N64 / 2);
    const price_type minA = compute_anchor(px, N64 - 1);
    if (A < minA) A = minA;
    if (A > px)   A = px;

    tape.recenter_to_anchor(A, spill_buffer);

    const price_type lo = tape.anchor();
    const price_type hi =
        static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);

    NullSink nospill;
    spill_buffer.template drain<IsBid>(lo, hi, [&](price_type p, qty_type qq) {
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
    bids.recenter_to_anchor(new_anchor, spill_buffer);
    const price_type lo = bids.anchor();
    const price_type hi = static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);
    NullSink nospill;
    spill_buffer.template drain<true>(lo, hi, [&](price_type px, qty_type q) {
      (void)bids.set_qty(px, q, nospill);
    });
  }

  TB_ALWAYS_INLINE void recenter_ask(price_type new_anchor) noexcept {
    asks.recenter_to_anchor(new_anchor, spill_buffer);
    const price_type lo = asks.anchor();
    const price_type hi = static_cast<price_type>(static_cast<i64>(lo) + N32 - 1);
    NullSink nospill;
    spill_buffer.template drain<false>(lo, hi, [&](price_type px, qty_type q) {
      (void)asks.set_qty(px, q, nospill);
    });
  }

  [[nodiscard]] TB_ALWAYS_INLINE bool anchors_equal() const noexcept {
    return bids.anchor() == asks.anchor();
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
          i32 CAP,
          typename PriceT,
          typename QtyT>
struct Book {
  using price_type = PriceT;
  using qty_type   = QtyT;
  using Spill      = SpillBuffer<CAP, PriceT, QtyT>;
  using Core       = TapeBook<N, Spill>;

  Spill spill;
  Core  core;

  Book() : spill(), core(spill) {}

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

#include <vector>
#include <cstdint>
#include <cassert>

namespace tape_book {

enum class BookTier : std::uint8_t {
  High   = 0,
  Medium = 1,
  Low    = 2
};

template <
    typename PriceT,
    typename QtyT,
    i32 N_HIGH,   i32 CAP_HIGH,
    i32 N_MEDIUM, i32 CAP_MEDIUM,
    i32 N_LOW,    i32 CAP_LOW,
    class HighAlloc   = std::allocator<Book<N_HIGH,   CAP_HIGH,   PriceT, QtyT>>,
    class MediumAlloc = std::allocator<Book<N_MEDIUM, CAP_MEDIUM, PriceT, QtyT>>,
    class LowAlloc    = std::allocator<Book<N_LOW,    CAP_LOW,    PriceT, QtyT>>>
struct MultiBookPool3 {
  using price_type = PriceT;
  using qty_type   = QtyT;

  using HighBook   = Book<N_HIGH,   CAP_HIGH,   price_type, qty_type>;
  using MediumBook = Book<N_MEDIUM, CAP_MEDIUM, price_type, qty_type>;
  using LowBook    = Book<N_LOW,    CAP_LOW,    price_type, qty_type>;

  struct Handle {
    BookTier       tier;
    std::uint32_t  idx;
  };

  std::vector<HighBook,   HighAlloc>   high_;
  std::vector<MediumBook, MediumAlloc> medium_;
  std::vector<LowBook,    LowAlloc>    low_;

  constexpr MultiBookPool3() = default;

  TB_ALWAYS_INLINE void reserve_high(std::size_t n)   { high_.reserve(n); }
  TB_ALWAYS_INLINE void reserve_medium(std::size_t n) { medium_.reserve(n); }
  TB_ALWAYS_INLINE void reserve_low(std::size_t n)    { low_.reserve(n); }

  [[nodiscard]] TB_ALWAYS_INLINE Handle alloc(BookTier tier,
                                               price_type anchor_px = price_type{0}) {
    switch (tier) {
      case BookTier::High: {
        const std::uint32_t idx = static_cast<std::uint32_t>(high_.size());
        high_.emplace_back();
        high_.back().reset(anchor_px);
        return Handle{BookTier::High, idx};
      }
      case BookTier::Medium: {
        const std::uint32_t idx = static_cast<std::uint32_t>(medium_.size());
        medium_.emplace_back();
        medium_.back().reset(anchor_px);
        return Handle{BookTier::Medium, idx};
      }
      case BookTier::Low: {
        const std::uint32_t idx = static_cast<std::uint32_t>(low_.size());
        low_.emplace_back();
        low_.back().reset(anchor_px);
        return Handle{BookTier::Low, idx};
      }
    }
    assert(false && "invalid BookTier");
    __builtin_unreachable();
  }

  TB_ALWAYS_INLINE HighBook&   high(std::uint32_t i)   noexcept { return high_[i];   }
  TB_ALWAYS_INLINE MediumBook& medium(std::uint32_t i) noexcept { return medium_[i]; }
  TB_ALWAYS_INLINE LowBook&    low(std::uint32_t i)    noexcept { return low_[i];    }

  TB_ALWAYS_INLINE const HighBook&   high(std::uint32_t i)   const noexcept { return high_[i];   }
  TB_ALWAYS_INLINE const MediumBook& medium(std::uint32_t i) const noexcept { return medium_[i]; }
  TB_ALWAYS_INLINE const LowBook&    low(std::uint32_t i)    const noexcept { return low_[i];    }

  template <class Fn>
  TB_ALWAYS_INLINE decltype(auto)
  with_book(const Handle& h, Fn&& fn) noexcept {
    switch (h.tier) {
      case BookTier::High:
        return fn(high_[h.idx]);
      case BookTier::Medium:
        return fn(medium_[h.idx]);
      case BookTier::Low:
        return fn(low_[h.idx]);
    }
    assert(false && "invalid BookTier");
    __builtin_unreachable();
  }

  template <class Fn>
  TB_ALWAYS_INLINE decltype(auto)
  with_book(const Handle& h, Fn&& fn) const noexcept {
    switch (h.tier) {
      case BookTier::High:
        return fn(high_[h.idx]);
      case BookTier::Medium:
        return fn(medium_[h.idx]);
      case BookTier::Low:
        return fn(low_[h.idx]);
    }
    assert(false && "invalid BookTier");
    __builtin_unreachable();
  }
};

}  // namespace tape_book

namespace tape_book {

using Book32 = Book<1024, 4096, i32, u32>;
using Book64 = Book<1024, 4096, i64, u64>;

}  // namespace tape_book
