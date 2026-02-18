#pragma once
#include <cstdint>
#include <cstdlib>
#include <random>

namespace bench {

// ---------------------------------------------------------------------------
// Op -- single operation for the benchmark
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct Op {
    bool   is_bid;
    PriceT px;
    QtyT   qty;
};

// ---------------------------------------------------------------------------
// WorkloadClustered -- most updates near BBO (realistic)
//
// Distribution: 70% within tight_range ticks, 20% within 4x, 10% within 16x.
// 15% of ops are cancels (qty=0).
// Bids: center - |offset|, Asks: center + |offset|.
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct WorkloadClustered {
    std::mt19937_64 rng;
    int64_t center;
    int64_t tight_range;

    WorkloadClustered(uint64_t seed, int64_t center_, int64_t tight_range_)
        : rng(seed), center(center_), tight_range(tight_range_) {}

    Op<PriceT, QtyT> operator()() {
        bool is_bid = (rng() & 1) != 0;

        int64_t offset;
        uint64_t roll = rng() % 100;
        if (roll < 70)
            offset = uniform(0, tight_range);
        else if (roll < 90)
            offset = uniform(tight_range, tight_range * 4);
        else
            offset = uniform(tight_range * 4, tight_range * 16);

        PriceT px;
        if (is_bid)
            px = static_cast<PriceT>(center - offset);
        else
            px = static_cast<PriceT>(center + offset);

        QtyT qty;
        if (rng() % 100 < 15)
            qty = QtyT{0};
        else
            qty = static_cast<QtyT>(1 + rng() % 500);

        return {is_bid, px, qty};
    }

private:
    int64_t uniform(int64_t lo, int64_t hi) {
        if (lo >= hi) return lo;
        return lo + static_cast<int64_t>(rng() % static_cast<uint64_t>(hi - lo + 1));
    }
};

// ---------------------------------------------------------------------------
// WorkloadUniform -- prices spread uniformly across wide range
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct WorkloadUniform {
    std::mt19937_64 rng;
    int64_t center;
    int64_t range;

    WorkloadUniform(uint64_t seed, int64_t center_, int64_t range_)
        : rng(seed), center(center_), range(range_) {}

    Op<PriceT, QtyT> operator()() {
        bool is_bid = (rng() & 1) != 0;

        int64_t offset = static_cast<int64_t>(rng() % static_cast<uint64_t>(range + 1));

        PriceT px;
        if (is_bid)
            px = static_cast<PriceT>(center - offset);
        else
            px = static_cast<PriceT>(center + offset);

        QtyT qty;
        if (rng() % 100 < 15)
            qty = QtyT{0};
        else
            qty = static_cast<QtyT>(1 + rng() % 500);

        return {is_bid, px, qty};
    }
};

// ---------------------------------------------------------------------------
// WorkloadHeavySpill -- most prices outside tape window
//
// 80% far from center (between tape_half and tape_half*4),
// 20% within tape range.
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct WorkloadHeavySpill {
    std::mt19937_64 rng;
    int64_t center;
    int64_t tape_half;  // N/2

    WorkloadHeavySpill(uint64_t seed, int64_t center_, int64_t tape_half_)
        : rng(seed), center(center_), tape_half(tape_half_) {}

    Op<PriceT, QtyT> operator()() {
        bool is_bid = (rng() & 1) != 0;

        int64_t offset;
        if (rng() % 100 < 80) {
            // Far from center (will spill in tape_book)
            offset = tape_half +
                static_cast<int64_t>(rng() % static_cast<uint64_t>(tape_half * 3 + 1));
        } else {
            // Near center
            offset = static_cast<int64_t>(rng() % static_cast<uint64_t>(tape_half));
        }

        PriceT px;
        if (is_bid)
            px = static_cast<PriceT>(center - offset);
        else
            px = static_cast<PriceT>(center + offset);

        QtyT qty;
        if (rng() % 100 < 10)
            qty = QtyT{0};
        else
            qty = static_cast<QtyT>(1 + rng() % 500);

        return {is_bid, px, qty};
    }
};

// ---------------------------------------------------------------------------
// WorkloadPriceWalk -- monotonically drifting prices (trending market)
//
// Bid cursor moves up, ask cursor moves up, forcing recenters.
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct WorkloadPriceWalk {
    std::mt19937_64 rng;
    int64_t bid_cursor;
    int64_t ask_cursor;
    int64_t max_step;

    WorkloadPriceWalk(uint64_t seed, int64_t start_bid, int64_t start_ask, int64_t max_step_)
        : rng(seed), bid_cursor(start_bid), ask_cursor(start_ask), max_step(max_step_) {}

    Op<PriceT, QtyT> operator()() {
        bool is_bid = (rng() & 1) != 0;

        PriceT px;
        if (is_bid) {
            bid_cursor += static_cast<int64_t>(rng() % static_cast<uint64_t>(max_step + 1));
            px = static_cast<PriceT>(bid_cursor);
        } else {
            ask_cursor += static_cast<int64_t>(rng() % static_cast<uint64_t>(max_step + 1));
            px = static_cast<PriceT>(ask_cursor);
        }

        QtyT qty = static_cast<QtyT>(1 + rng() % 500);
        return {is_bid, px, qty};
    }
};

// ---------------------------------------------------------------------------
// WorkloadCancelHeavy -- 70% cancels (qty=0), realistic cancel-dominated flow
//
// Prices cluster near BBO so cancels frequently hit existing levels.
// Models markets with high cancel-to-trade ratios (typical 10:1 to 30:1).
// ---------------------------------------------------------------------------
template <typename PriceT, typename QtyT>
struct WorkloadCancelHeavy {
    std::mt19937_64 rng;
    int64_t center;
    int64_t range;

    WorkloadCancelHeavy(uint64_t seed, int64_t center_, int64_t range_)
        : rng(seed), center(center_), range(range_) {}

    Op<PriceT, QtyT> operator()() {
        bool is_bid = (rng() & 1) != 0;

        int64_t offset = static_cast<int64_t>(rng() % static_cast<uint64_t>(range + 1));

        PriceT px;
        if (is_bid)
            px = static_cast<PriceT>(center - offset);
        else
            px = static_cast<PriceT>(center + offset);

        QtyT qty;
        if (rng() % 100 < 70)
            qty = QtyT{0};  // 70% cancel rate
        else
            qty = static_cast<QtyT>(1 + rng() % 500);

        return {is_bid, px, qty};
    }
};

}  // namespace bench
