#pragma once

#include "tape_book/config.hpp"
#include "tape_book/types.hpp"
#include "tape_book/spill_buffer.hpp"
#include "tape_book/tape.hpp"
#include "tape_book/book.hpp"
#include "tape_book/multi_book_pool.hpp"

namespace tape_book {

using Book32 = Book<1024, 4096, i32, u32>;
using Book64 = Book<1024, 4096, i64, u64>;

}  // namespace tape_book
