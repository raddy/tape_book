#pragma once
#include <cstring>
#include "tape_book/config.hpp"
#include "tape_book/types.hpp"

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
