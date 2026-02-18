#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "tape_book/tape_book.hpp"

namespace bench {

// ---------------------------------------------------------------------------
// OrderBookMap -- std::map baseline (Optiver talk "First Take")
//
// Bids: std::map with std::greater (best = begin())
// Asks: std::map with std::less    (best = begin())
// AddOrder: O(log N), best_price: O(1)
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct OrderBookMap {
    std::map<PriceT, QtyT, std::greater<PriceT>> bid_levels;
    std::map<PriceT, QtyT, std::less<PriceT>>    ask_levels;

    void reset(PriceT) {
        bid_levels.clear();
        ask_levels.clear();
    }

    void set_bid(PriceT px, QtyT qty) {
        if (qty == QtyT{0})
            bid_levels.erase(px);
        else
            bid_levels.insert_or_assign(px, qty);
    }

    void set_ask(PriceT px, QtyT qty) {
        if (qty == QtyT{0})
            ask_levels.erase(px);
        else
            ask_levels.insert_or_assign(px, qty);
    }

    [[nodiscard]] PriceT best_bid_px() const {
        return bid_levels.empty()
            ? std::numeric_limits<PriceT>::lowest()
            : bid_levels.begin()->first;
    }

    [[nodiscard]] PriceT best_ask_px() const {
        return ask_levels.empty()
            ? std::numeric_limits<PriceT>::max()
            : ask_levels.begin()->first;
    }
};

// ---------------------------------------------------------------------------
// OrderBookVector -- sorted vector with reverse ordering (Optiver talk)
//
// Bids stored ascending (best at back), asks stored descending (best at back).
// Binary search via std::lower_bound.
// Inserts near BBO touch the tail -> minimal memmove.
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct OrderBookVector {
    using Level = std::pair<PriceT, QtyT>;

    std::vector<Level> bid_levels;  // ascending  (best = back)
    std::vector<Level> ask_levels;  // descending (best = back)

    void reset(PriceT) {
        bid_levels.clear();
        ask_levels.clear();
    }

    void set_bid(PriceT px, QtyT qty) {
        // ascending order: lower_bound with less
        auto it = std::lower_bound(
            bid_levels.begin(), bid_levels.end(), px,
            [](const Level& lv, PriceT p) { return lv.first < p; });

        if (it != bid_levels.end() && it->first == px) {
            if (qty == QtyT{0})
                bid_levels.erase(it);
            else
                it->second = qty;
        } else if (qty != QtyT{0}) {
            bid_levels.insert(it, {px, qty});
        }
    }

    void set_ask(PriceT px, QtyT qty) {
        // descending order: lower_bound with greater
        auto it = std::lower_bound(
            ask_levels.begin(), ask_levels.end(), px,
            [](const Level& lv, PriceT p) { return lv.first > p; });

        if (it != ask_levels.end() && it->first == px) {
            if (qty == QtyT{0})
                ask_levels.erase(it);
            else
                it->second = qty;
        } else if (qty != QtyT{0}) {
            ask_levels.insert(it, {px, qty});
        }
    }

    [[nodiscard]] PriceT best_bid_px() const {
        return bid_levels.empty()
            ? std::numeric_limits<PriceT>::lowest()
            : bid_levels.back().first;
    }

    [[nodiscard]] PriceT best_ask_px() const {
        return ask_levels.empty()
            ? std::numeric_limits<PriceT>::max()
            : ask_levels.back().first;
    }
};

// ---------------------------------------------------------------------------
// OrderBookVectorLinear -- sorted vector with linear scan from end
//
// Same storage layout as OrderBookVector (reverse ordering, best at back).
// Uses linear scan backward instead of binary search.
// Wins when most updates cluster near the top of book.
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct OrderBookVectorLinear {
    using Level = std::pair<PriceT, QtyT>;

    std::vector<Level> bid_levels;  // ascending  (best = back)
    std::vector<Level> ask_levels;  // descending (best = back)

    void reset(PriceT) {
        bid_levels.clear();
        ask_levels.clear();
    }

    void set_bid(PriceT px, QtyT qty) {
        // Linear scan backward (ascending, best at back)
        int n = static_cast<int>(bid_levels.size());
        int i = n - 1;
        while (i >= 0 && bid_levels[static_cast<size_t>(i)].first > px) --i;

        if (i >= 0 && bid_levels[static_cast<size_t>(i)].first == px) {
            if (qty == QtyT{0})
                bid_levels.erase(bid_levels.begin() + i);
            else
                bid_levels[static_cast<size_t>(i)].second = qty;
        } else if (qty != QtyT{0}) {
            bid_levels.insert(bid_levels.begin() + (i + 1), {px, qty});
        }
    }

    void set_ask(PriceT px, QtyT qty) {
        // Linear scan backward (descending, best at back)
        int n = static_cast<int>(ask_levels.size());
        int i = n - 1;
        while (i >= 0 && ask_levels[static_cast<size_t>(i)].first < px) --i;

        if (i >= 0 && ask_levels[static_cast<size_t>(i)].first == px) {
            if (qty == QtyT{0})
                ask_levels.erase(ask_levels.begin() + i);
            else
                ask_levels[static_cast<size_t>(i)].second = qty;
        } else if (qty != QtyT{0}) {
            ask_levels.insert(ask_levels.begin() + (i + 1), {px, qty});
        }
    }

    [[nodiscard]] PriceT best_bid_px() const {
        return bid_levels.empty()
            ? std::numeric_limits<PriceT>::lowest()
            : bid_levels.back().first;
    }

    [[nodiscard]] PriceT best_ask_px() const {
        return ask_levels.empty()
            ? std::numeric_limits<PriceT>::max()
            : ask_levels.back().first;
    }
};

// ---------------------------------------------------------------------------
// TapeBookAdapter -- wraps tape_book::Book for uniform benchmark API
// ---------------------------------------------------------------------------
template <int N, typename PriceT, typename QtyT>
struct TapeBookAdapter {
    tape_book::Book<N, PriceT, QtyT> book;

    explicit TapeBookAdapter(tape_book::i32 max_cap = 4096)
        : book(max_cap) {}

    void reset(PriceT anchor) { book.reset(anchor); }

    void set_bid(PriceT px, QtyT qty) {
        book.set(true, px, qty);
    }

    void set_ask(PriceT px, QtyT qty) {
        book.set(false, px, qty);
    }

    [[nodiscard]] PriceT best_bid_px() const { return book.best_bid_px(); }
    [[nodiscard]] PriceT best_ask_px() const { return book.best_ask_px(); }
};

}  // namespace bench
