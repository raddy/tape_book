#pragma once
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
