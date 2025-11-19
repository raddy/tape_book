#pragma once
#include <cassert>
#include <cstring>
#include "tape_book/config.hpp"
#include "tape_book/types.hpp"

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
