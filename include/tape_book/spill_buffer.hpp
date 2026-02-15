#pragma once
#include <cstdlib>
#include <cstring>
#include "tape_book/config.hpp"
#include "tape_book/types.hpp"

namespace tape_book {

// Forward declaration — full definition in spill_pool.hpp.
template <typename PriceT, typename QtyT>
struct SpillPool;

struct NullSink {
  template <bool IsBid>
  TB_ALWAYS_INLINE void push(auto, auto) noexcept {}
  template <bool IsBid>
  TB_ALWAYS_INLINE void erase_better(auto) noexcept {}
  template <bool IsBid, class Fn>
  TB_ALWAYS_INLINE void iterate_pending(Fn&&) const noexcept {}
  TB_ALWAYS_INLINE void clear() noexcept {}
};

// Dynamically-allocated spill side. Grows from 0 up to max_cap using malloc/free.
// When full (n == cap == max_cap), worst level is evicted (lowest bid, highest ask).
template <typename PriceT, typename QtyT>
struct SideDynamic {
  using level_type = LevelT<PriceT, QtyT>;
  using price_type = PriceT;
  using qty_type   = QtyT;
  using pool_type  = SpillPool<PriceT, QtyT>;

  level_type* a{nullptr};
  i32 n{0};
  i32 cap{0};
  i32 max_cap;

  explicit SideDynamic(i32 mc) noexcept : max_cap(mc) {}

  [[nodiscard]] constexpr TB_ALWAYS_INLINE i32 lb(price_type px) const noexcept {
    i32 lo = 0, hi = n;
    while (lo < hi) {
      i32 mid = (lo + hi) >> 1;
      if (a[mid].px < px) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

  TB_ALWAYS_INLINE void ensure_cap(pool_type* pool) noexcept {
    i32 new_cap = cap ? cap * 2 : 16;
    if (new_cap > max_cap) new_cap = max_cap;
    if (new_cap <= cap) return;
    level_type* p;
    if (pool) {
      p = pool->reallocate(a, cap, new_cap, n);
    } else {
      p = static_cast<level_type*>(std::malloc(static_cast<size_t>(new_cap) * sizeof(level_type)));
      if (!p) return;
      if (a) {
        std::memcpy(p, a, static_cast<size_t>(n) * sizeof(level_type));
        std::free(a);
      }
    }
    if (!p) return;
    a = p;
    cap = new_cap;
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void add_point(price_type px, qty_type q, pool_type* pool) noexcept {
    if (n == cap && cap < max_cap) ensure_cap(pool);

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

    if (n == cap) {
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

  TB_ALWAYS_INLINE void release(pool_type* pool) noexcept {
    if (pool) pool->deallocate(a, cap);
    else      std::free(a);
    a = nullptr;
    n = 0;
    cap = 0;
  }

};

// DynSpillBuffer wraps two SideDynamic instances (bid + ask).
// Satisfies the Sink concept used by Tape.
// When pool_ is non-null, alloc/dealloc routes through SpillPool.
// When pool_ is null (default), uses malloc/free.
template <typename PriceT, typename QtyT>
struct DynSpillBuffer {
  using price_type = PriceT;
  using qty_type   = QtyT;
  using pool_type  = SpillPool<PriceT, QtyT>;

  SideDynamic<PriceT, QtyT> bid;
  SideDynamic<PriceT, QtyT> ask;
  pool_type* pool_{nullptr};

  explicit DynSpillBuffer(i32 max_cap = 4096, pool_type* pool = nullptr) noexcept
      : bid(max_cap), ask(max_cap), pool_(pool) {
    assert(max_cap >= 1);
    assert((max_cap & (max_cap - 1)) == 0 && "max_cap must be a power of 2");
  }

  ~DynSpillBuffer() {
    bid.release(pool_);
    ask.release(pool_);
  }

  DynSpillBuffer(const DynSpillBuffer&) = delete;
  DynSpillBuffer& operator=(const DynSpillBuffer&) = delete;

  DynSpillBuffer(DynSpillBuffer&& o) noexcept
      : bid(o.bid.max_cap), ask(o.ask.max_cap), pool_(o.pool_) {
    bid.a = o.bid.a; bid.n = o.bid.n; bid.cap = o.bid.cap;
    ask.a = o.ask.a; ask.n = o.ask.n; ask.cap = o.ask.cap;
    o.bid.a = nullptr; o.bid.n = 0; o.bid.cap = 0;
    o.ask.a = nullptr; o.ask.n = 0; o.ask.cap = 0;
  }

  DynSpillBuffer& operator=(DynSpillBuffer&& o) noexcept {
    if (this != &o) {
      bid.release(pool_);
      ask.release(pool_);
      pool_ = o.pool_;
      bid.max_cap = o.bid.max_cap;
      bid.a = o.bid.a; bid.n = o.bid.n; bid.cap = o.bid.cap;
      ask.max_cap = o.ask.max_cap;
      ask.a = o.ask.a; ask.n = o.ask.n; ask.cap = o.ask.cap;
      o.bid.a = nullptr; o.bid.n = 0; o.bid.cap = 0;
      o.ask.a = nullptr; o.ask.n = 0; o.ask.cap = 0;
    }
    return *this;
  }

  constexpr TB_ALWAYS_INLINE void clear() noexcept {
    bid.clear();
    ask.clear();
  }

  TB_ALWAYS_INLINE void release() noexcept {
    bid.release(pool_);
    ask.release(pool_);
  }

  template <bool IsBid>
  TB_ALWAYS_INLINE void push(price_type px, qty_type q) noexcept {
    if constexpr (IsBid) bid.template add_point<true>(px, q, pool_);
    else                 ask.template add_point<false>(px, q, pool_);
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

}  // namespace tape_book
