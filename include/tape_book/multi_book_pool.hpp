#pragma once
#include <vector>
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
