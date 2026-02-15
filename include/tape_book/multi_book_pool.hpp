#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include <cassert>

#include "tape_book/book.hpp"

namespace tape_book {

enum class BookTier : std::uint8_t {
  High   = 0,
  Medium = 1,
  Low    = 2
};

template <
    typename PriceT,
    typename QtyT,
    i32 N_HIGH,
    i32 N_MEDIUM,
    i32 N_LOW,
    class HighAlloc   = std::allocator<Book<N_HIGH,   PriceT, QtyT>>,
    class MediumAlloc = std::allocator<Book<N_MEDIUM, PriceT, QtyT>>,
    class LowAlloc    = std::allocator<Book<N_LOW,    PriceT, QtyT>>>
struct MultiBookPool3 {
  using price_type = PriceT;
  using qty_type   = QtyT;

  using HighBook   = Book<N_HIGH,   price_type, qty_type>;
  using MediumBook = Book<N_MEDIUM, price_type, qty_type>;
  using LowBook    = Book<N_LOW,    price_type, qty_type>;

  struct Handle {
    BookTier       tier;
    std::uint32_t  idx;
  };

  using pool_type = SpillPool<price_type, qty_type>;

  // pool_ declared BEFORE book vectors — destruction is reverse order,
  // so books are destroyed before the pool they reference.
  std::unique_ptr<pool_type> pool_;

  std::vector<HighBook,   HighAlloc>   high_;
  std::vector<MediumBook, MediumAlloc> medium_;
  std::vector<LowBook,    LowAlloc>    low_;

  i32 default_max_cap_{4096};

  MultiBookPool3() = default;
  explicit MultiBookPool3(i32 max_cap, i32 pool_cap = 0) noexcept
      : pool_(pool_cap > 0 ? new pool_type(pool_cap) : nullptr),
        default_max_cap_(max_cap) {}

  TB_ALWAYS_INLINE void reserve_high(std::size_t n)   { high_.reserve(n); }
  TB_ALWAYS_INLINE void reserve_medium(std::size_t n) { medium_.reserve(n); }
  TB_ALWAYS_INLINE void reserve_low(std::size_t n)    { low_.reserve(n); }

  [[nodiscard]] TB_ALWAYS_INLINE Handle alloc(BookTier tier,
                                               price_type anchor_px = price_type{0},
                                               i32 max_cap = 0) {
    if (max_cap == 0) max_cap = default_max_cap_;
    auto* p = pool_.get();
    switch (tier) {
      case BookTier::High: {
        const std::uint32_t idx = static_cast<std::uint32_t>(high_.size());
        high_.emplace_back(max_cap, p);
        high_.back().reset(anchor_px);
        return Handle{BookTier::High, idx};
      }
      case BookTier::Medium: {
        const std::uint32_t idx = static_cast<std::uint32_t>(medium_.size());
        medium_.emplace_back(max_cap, p);
        medium_.back().reset(anchor_px);
        return Handle{BookTier::Medium, idx};
      }
      case BookTier::Low: {
        const std::uint32_t idx = static_cast<std::uint32_t>(low_.size());
        low_.emplace_back(max_cap, p);
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
