#include <random>
#include "fuzz_helpers.hpp"

// ═══════════════════════════════════════════════════════════
// Generic fuzz runner
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_run(
    uint64_t seed, int steps,
    int64_t center, int64_t near_range, int64_t far_range,
    int recenter_pct, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, static_cast<PriceT>(center), max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  std::uniform_int_distribution<int64_t> op_roll(0, 99);
  std::uniform_int_distribution<int64_t> near_dist(-near_range, near_range);
  std::uniform_int_distribution<int64_t> far_dist(-far_range, far_range);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> extreme_roll(0, 99);

  for (int step = 0; step < steps; ++step) {
    const bool is_bid = side_dist(rng) != 0;
    const bool use_far = (rng() & 7u) == 0u;
    PriceT px = static_cast<PriceT>(center + (use_far ? far_dist(rng) : near_dist(rng)));
    QtyT q = static_cast<QtyT>(qty_dist(rng));
    if (extreme_roll(rng) == 0) q = std::numeric_limits<QtyT>::max();

    int roll = static_cast<int>(op_roll(rng));
    OpKind op;
    if (roll < recenter_pct) {
      op = (rng() & 1) ? OpKind::RecenterBid : OpKind::RecenterAsk;
      PriceT anchor = ctx.clamp_anchor(static_cast<PriceT>(center + near_dist(rng)));
      ctx.apply(op, is_bid, anchor, q);
    } else {
      int rem = roll - recenter_pct;
      int range = 100 - recenter_pct;
      if (rem < range / 3) op = OpKind::AddUpdate;
      else if (rem < 2 * range / 3) op = OpKind::Cancel;
      else op = OpKind::EraseBetter;
      ctx.apply(op, is_bid, px, q);
    }

    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Deep book: 80% adds to build many levels, few erases
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_deep(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, PriceT{0}, max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  std::uniform_int_distribution<int64_t> px_dist(static_cast<int64_t>(-N),
                                                  static_cast<int64_t>(N));
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> roll_dist(0, 99);

  for (int step = 0; step < steps; ++step) {
    bool is_bid = side_dist(rng) != 0;
    PriceT px = static_cast<PriceT>(px_dist(rng));
    QtyT q = static_cast<QtyT>(qty_dist(rng));
    int roll = static_cast<int>(roll_dist(rng));

    OpKind op;
    if (roll < 80) {
      op = OpKind::AddUpdate;
    } else if (roll < 90) {
      op = OpKind::Cancel;
    } else if (roll < 95) {
      op = OpKind::EraseBetter;
    } else {
      op = (rng() & 1) ? OpKind::RecenterBid : OpKind::RecenterAsk;
      PriceT anchor = ctx.clamp_anchor(static_cast<PriceT>(px_dist(rng)));
      ctx.apply(op, is_bid, anchor, q);
      ctx.verify(op, is_bid, px, q, step == steps - 1);
      continue;
    }
    ctx.apply(op, is_bid, px, q);
    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Boundary: prices near INT_MAX/INT_MIN, no promotes
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_boundary(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  constexpr PriceT hi_anchor = std::numeric_limits<PriceT>::max() - static_cast<PriceT>(N - 1);
  constexpr PriceT lo_anchor = std::numeric_limits<PriceT>::lowest() + static_cast<PriceT>(N - 1);

  auto run_phase = [&](PriceT anchor, const char* phase_tag) {
    FuzzCtx<N, PriceT, QtyT> ctx(seed, phase_tag, anchor, max_cap);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> idx_dist(0, N - 1);
    std::uniform_int_distribution<int64_t> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> op_dist(0, 2);
    std::uniform_int_distribution<int64_t> qty_dist(1, 10000);

    for (int step = 0; step < steps; ++step) {
      PriceT px = static_cast<PriceT>(static_cast<int64_t>(anchor) + idx_dist(rng));
      bool is_bid = side_dist(rng) != 0;
      QtyT q = static_cast<QtyT>(qty_dist(rng));
      int op_roll = static_cast<int>(op_dist(rng));

      OpKind op;
      if (op_roll == 0) op = OpKind::AddUpdate;
      else if (op_roll == 1) op = OpKind::Cancel;
      else op = OpKind::EraseBetter;

      ctx.apply(op, is_bid, px, q);
      ctx.verify(op, is_bid, px, q, step == steps - 1);
    }
  };

  std::string hi_tag = std::string(tag) + "_hi";
  std::string lo_tag = std::string(tag) + "_lo";
  run_phase(hi_anchor, hi_tag.c_str());
  run_phase(lo_anchor, lo_tag.c_str());
}

// ═══════════════════════════════════════════════════════════
// Spill saturation: wide price range, heavy spill traffic
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_spill_saturate(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, PriceT{0}, max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  const int64_t wide = static_cast<int64_t>(N) * 8;
  std::uniform_int_distribution<int64_t> px_dist(-wide, wide);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> roll_dist(0, 99);

  for (int step = 0; step < steps; ++step) {
    bool is_bid = side_dist(rng) != 0;
    PriceT px = static_cast<PriceT>(px_dist(rng));
    QtyT q = static_cast<QtyT>(qty_dist(rng));
    int roll = static_cast<int>(roll_dist(rng));

    OpKind op;
    if (roll < 60) {
      op = OpKind::AddUpdate;
    } else if (roll < 75) {
      op = OpKind::Cancel;
    } else if (roll < 85) {
      op = OpKind::EraseBetter;
    } else {
      op = (rng() & 1) ? OpKind::RecenterBid : OpKind::RecenterAsk;
      PriceT anchor = ctx.clamp_anchor(static_cast<PriceT>(px_dist(rng)));
      ctx.apply(op, is_bid, anchor, q);
      ctx.verify(op, is_bid, px, q, step == steps - 1);
      continue;
    }
    ctx.apply(op, is_bid, px, q);
    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Promote storm: monotonically improving prices
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_promote_storm(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, PriceT{0}, max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> roll_dist(0, 99);
  std::uniform_int_distribution<int64_t> step_dist(1, static_cast<int64_t>(N));
  std::uniform_int_distribution<int64_t> fill_dist(0, static_cast<int64_t>(N) - 1);

  int64_t bid_frontier = 0, ask_frontier = 0;
  constexpr int64_t max_safe = static_cast<int64_t>(std::numeric_limits<PriceT>::max()) - N * 2;
  constexpr int64_t min_safe = static_cast<int64_t>(std::numeric_limits<PriceT>::lowest()) + N * 2;

  for (int step = 0; step < steps; ++step) {
    int roll = static_cast<int>(roll_dist(rng));
    QtyT q = static_cast<QtyT>(qty_dist(rng));
    OpKind op = OpKind::AddUpdate;
    bool is_bid = true;
    PriceT px{};

    if (roll < 35) {
      is_bid = true;
      bid_frontier += step_dist(rng);
      if (bid_frontier > max_safe) bid_frontier = 0;
      px = static_cast<PriceT>(bid_frontier);
    } else if (roll < 70) {
      is_bid = false;
      ask_frontier -= step_dist(rng);
      if (ask_frontier < min_safe) ask_frontier = 0;
      px = static_cast<PriceT>(ask_frontier);
    } else if (roll < 85) {
      is_bid = (rng() & 1) != 0;
      int64_t base = is_bid ? bid_frontier : ask_frontier;
      px = static_cast<PriceT>(is_bid ? (base - fill_dist(rng)) : (base + fill_dist(rng)));
    } else if (roll < 93) {
      op = OpKind::Cancel;
      is_bid = (rng() & 1) != 0;
      int64_t base = is_bid ? bid_frontier : ask_frontier;
      px = static_cast<PriceT>(is_bid ? (base - fill_dist(rng)) : (base + fill_dist(rng)));
    } else {
      op = OpKind::EraseBetter;
      is_bid = (rng() & 1) != 0;
      px = static_cast<PriceT>(is_bid ? bid_frontier : ask_frontier);
    }

    ctx.apply(op, is_bid, px, q);
    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Wipe/rebuild cycles: fill → erase_better → refill
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_wipe_rebuild(uint64_t seed, int cycles, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, PriceT{0}, max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> px_dist(-static_cast<int64_t>(N),
                                                   static_cast<int64_t>(N));
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);

  const int fill_count = N * 2;

  for (int cycle = 0; cycle < cycles; ++cycle) {
    // Phase 1: Fill with many levels
    for (int i = 0; i < fill_count; ++i) {
      bool is_bid = side_dist(rng) != 0;
      PriceT px = static_cast<PriceT>(px_dist(rng));
      QtyT q = static_cast<QtyT>(qty_dist(rng));
      ctx.apply(OpKind::AddUpdate, is_bid, px, q);
      ctx.verify(OpKind::AddUpdate, is_bid, px, q);
    }

    // Phase 2: Wipe via erase_better on both sides
    PriceT wipe_bid = static_cast<PriceT>(std::numeric_limits<PriceT>::lowest() + N);
    PriceT wipe_ask = static_cast<PriceT>(std::numeric_limits<PriceT>::max() - N);
    ctx.apply(OpKind::EraseBetter, true, wipe_bid, QtyT{0});
    ctx.apply(OpKind::EraseBetter, false, wipe_ask, QtyT{0});
    ctx.verify(OpKind::EraseBetter, true, wipe_bid, QtyT{0}, true);

    if (!ctx.book->core.bids.is_empty() || !ctx.book->core.asks.is_empty()) {
      std::cerr << "FAIL [" << tag << "]: book not empty after full wipe, cycle=" << cycle << "\n";
      std::abort();
    }
  }
}

// ═══════════════════════════════════════════════════════════
// i16/u16 coverage
// ═══════════════════════════════════════════════════════════

template <i32 N>
static void fuzz_i16(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, px16, qty16> ctx(seed, tag, px16{0}, max_cap);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  std::uniform_int_distribution<int64_t> op_roll(0, 99);
  std::uniform_int_distribution<int64_t> px_dist(-20000, 20000);
  std::uniform_int_distribution<int64_t> qty_dist(1, 65535);
  std::uniform_int_distribution<int64_t> extreme_roll(0, 49);

  for (int step = 0; step < steps; ++step) {
    bool is_bid = side_dist(rng) != 0;
    int64_t raw_px = px_dist(rng);
    if (raw_px < std::numeric_limits<px16>::lowest()) raw_px = std::numeric_limits<px16>::lowest();
    if (raw_px > std::numeric_limits<px16>::max()) raw_px = std::numeric_limits<px16>::max();
    px16 px = static_cast<px16>(raw_px);
    qty16 q = static_cast<qty16>(qty_dist(rng));
    if (extreme_roll(rng) == 0) q = std::numeric_limits<qty16>::max();

    int roll = static_cast<int>(op_roll(rng));
    OpKind op;
    if (roll < 20) {
      op = (rng() & 1) ? OpKind::RecenterBid : OpKind::RecenterAsk;
      int64_t raw_a = px_dist(rng);
      if (raw_a < ctx.anchor_lo) raw_a = ctx.anchor_lo;
      if (raw_a > ctx.anchor_hi) raw_a = ctx.anchor_hi;
      ctx.apply(op, is_bid, static_cast<px16>(raw_a), q);
    } else {
      if (roll < 55) op = OpKind::AddUpdate;
      else if (roll < 85) op = OpKind::Cancel;
      else op = OpKind::EraseBetter;
      ctx.apply(op, is_bid, px, q);
    }

    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Split anchor: non-overlapping bid/ask tape windows
// ═══════════════════════════════════════════════════════════

template <i32 N, typename PriceT, typename QtyT>
static void fuzz_split_anchor(uint64_t seed, int steps, const char* tag, i32 max_cap = 4096) {
  FuzzCtx<N, PriceT, QtyT> ctx(seed, tag, PriceT{0}, max_cap);

  // Immediately split tape windows
  PriceT bid_anchor = ctx.clamp_anchor(static_cast<PriceT>(-N * 2));
  PriceT ask_anchor = ctx.clamp_anchor(static_cast<PriceT>(N * 2));
  ctx.book->recenter_bid(bid_anchor);
  ctx.book->recenter_ask(ask_anchor);

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> side_dist(0, 1);
  std::uniform_int_distribution<int64_t> roll_dist(0, 99);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10000);
  const int64_t span = static_cast<int64_t>(N) * 4;
  std::uniform_int_distribution<int64_t> px_dist(-span, span);

  for (int step = 0; step < steps; ++step) {
    bool is_bid = side_dist(rng) != 0;
    PriceT px = static_cast<PriceT>(px_dist(rng));
    QtyT q = static_cast<QtyT>(qty_dist(rng));
    int roll = static_cast<int>(roll_dist(rng));

    OpKind op;
    if (roll < 50) {
      op = OpKind::AddUpdate;
    } else if (roll < 75) {
      op = OpKind::Cancel;
    } else if (roll < 90) {
      op = OpKind::EraseBetter;
    } else {
      op = (rng() & 1) ? OpKind::RecenterBid : OpKind::RecenterAsk;
      PriceT anchor = ctx.clamp_anchor(static_cast<PriceT>(px_dist(rng)));
      ctx.apply(op, is_bid, anchor, q);
      ctx.verify(op, is_bid, px, q, step == steps - 1);
      continue;
    }
    ctx.apply(op, is_bid, px, q);
    ctx.verify(op, is_bid, px, q, step == steps - 1);
  }
}

// ═══════════════════════════════════════════════════════════
// Seed runner — eliminates per-suite for-loop boilerplate
// ═══════════════════════════════════════════════════════════

template <typename Fn>
static void seeds(const char* label, int n, Fn&& fn) {
  for (uint64_t s = 1; s <= static_cast<uint64_t>(n); ++s)
    run((std::string(label) + " s=" + std::to_string(s)).c_str(),
        [&fn, s] { fn(s); });
}

// ═══════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════

int main() {
  // ── 1. Multi-N: exercises multi-word bitset scanning ──
  std::cout << "=== Multi-N core fuzz (i32/u32) ===\n";
  seeds("N=64",  12, [](uint64_t s) { fuzz_run<64,  i32, u32>(s, 100'000, 0, 32,  256,  0, "N64",  1024); });
  seeds("N=128",  8, [](uint64_t s) { fuzz_run<128, i32, u32>(s, 100'000, 0, 64,  512,  0, "N128", 2048); });
  seeds("N=256",  8, [](uint64_t s) { fuzz_run<256, i32, u32>(s, 100'000, 0, 128, 1024, 0, "N256", 2048); });
  seeds("N=1024", 4, [](uint64_t s) { fuzz_run<1024,i32, u32>(s, 50'000,  0, 512, 4096, 0, "N1024",4096); });

  // ── 2. i64/u64 type coverage ──
  std::cout << "\n=== i64/u64 type coverage ===\n";
  seeds("i64 N=64",  8, [](uint64_t s) { fuzz_run<64,  i64, u64>(s, 100'000, 0, 32,  256,  0, "i64_N64",  1024); });
  seeds("i64 N=128", 4, [](uint64_t s) { fuzz_run<128, i64, u64>(s, 50'000,  0, 64,  512,  0, "i64_N128", 2048); });
  seeds("i64 N=256", 4, [](uint64_t s) { fuzz_run<256, i64, u64>(s, 50'000,  0, 128, 1024, 0, "i64_N256", 2048); });

  // ── 3. i16/u16 coverage ──
  std::cout << "\n=== i16/u16 type coverage ===\n";
  seeds("i16 N=64",  8, [](uint64_t s) { fuzz_i16<64> (s, 100'000, "i16_N64",  512);  });
  seeds("i16 N=128", 4, [](uint64_t s) { fuzz_i16<128>(s, 50'000,  "i16_N128", 1024); });
  seeds("i16 N=256", 4, [](uint64_t s) { fuzz_i16<256>(s, 50'000,  "i16_N256", 2048); });

  // ── 4. Recenter stress: 30% recenters ──
  std::cout << "\n=== Recenter stress (30%) ===\n";
  seeds("rc30 N=64",  8, [](uint64_t s) { fuzz_run<64,  i32, u32>(s, 100'000, 0, 32,  256,  30, "rc30_N64",  2048); });
  seeds("rc30 N=256", 4, [](uint64_t s) { fuzz_run<256, i32, u32>(s, 50'000,  0, 128, 1024, 30, "rc30_N256", 4096); });

  // ── 5. Extreme recenter stress: 50% recenters ──
  std::cout << "\n=== Extreme recenter stress (50%) ===\n";
  seeds("rc50 N=64",  8, [](uint64_t s) { fuzz_run<64,  i32, u32>(s, 100'000, 0, 32, 256, 50, "rc50_N64",  2048); });
  seeds("rc50 N=128", 4, [](uint64_t s) { fuzz_run<128, i32, u32>(s, 50'000,  0, 64, 512, 50, "rc50_N128", 2048); });

  // ── 6. Deep book: 80% adds ──
  std::cout << "\n=== Deep book ===\n";
  seeds("deep N=64",  8, [](uint64_t s) { fuzz_deep<64,  i32, u32>(s, 100'000, "deep_N64",  2048); });
  seeds("deep N=256", 4, [](uint64_t s) { fuzz_deep<256, i32, u32>(s, 50'000,  "deep_N256", 4096); });

  // ── 7. Spill saturation ──
  std::cout << "\n=== Spill saturation ===\n";
  seeds("spill N=64",  8, [](uint64_t s) { fuzz_spill_saturate<64,  i32, u32>(s, 100'000, "spill_N64",  2048); });
  seeds("spill N=128", 4, [](uint64_t s) { fuzz_spill_saturate<128, i32, u32>(s, 50'000,  "spill_N128", 4096); });

  // ── 8. Promote storm ──
  std::cout << "\n=== Promote storm ===\n";
  seeds("promote N=64",  8, [](uint64_t s) { fuzz_promote_storm<64,  i32, u32>(s, 20'000, "promo_N64",  32768); });
  seeds("promote N=256", 4, [](uint64_t s) { fuzz_promote_storm<256, i32, u32>(s, 20'000, "promo_N256", 32768); });

  // ── 9. Wipe/rebuild cycles ──
  std::cout << "\n=== Wipe/rebuild cycles ===\n";
  seeds("wipe N=64",  4, [](uint64_t s) { fuzz_wipe_rebuild<64,  i32, u32>(s, 200, "wipe_N64",  1024); });
  seeds("wipe N=256", 4, [](uint64_t s) { fuzz_wipe_rebuild<256, i32, u32>(s, 100, "wipe_N256", 2048); });

  // ── 10. Split anchor ──
  std::cout << "\n=== Split anchor (asymmetric tapes) ===\n";
  seeds("split N=64",  8, [](uint64_t s) { fuzz_split_anchor<64,  i32, u32>(s, 100'000, "split_N64",  2048); });
  seeds("split N=256", 4, [](uint64_t s) { fuzz_split_anchor<256, i32, u32>(s, 50'000,  "split_N256", 4096); });

  // ── 11. Non-zero center ──
  std::cout << "\n=== Non-zero center ===\n";
  seeds("center=10000 N=64", 8, [](uint64_t s) { fuzz_run<64, i32, u32>(s, 100'000, 10'000, 32, 256, 0, "c10k_N64", 1024); });

  // ── 12. Negative center ──
  std::cout << "\n=== Negative center ===\n";
  seeds("center=-5000 N=64", 8, [](uint64_t s) { fuzz_run<64, i32, u32>(s, 100'000, -5'000, 32, 256, 0, "cn5k_N64", 1024); });

  // ── 13. Boundary prices ──
  std::cout << "\n=== Boundary prices (no promotes) ===\n";
  seeds("boundary i32 N=64",  8, [](uint64_t s) { fuzz_boundary<64,  i32, u32>  (s, 50'000, "bnd_i32_N64",  1024); });
  seeds("boundary i64 N=64",  4, [](uint64_t s) { fuzz_boundary<64,  i64, u64>  (s, 50'000, "bnd_i64_N64",  1024); });
  seeds("boundary i32 N=256", 4, [](uint64_t s) { fuzz_boundary<256, i32, u32>  (s, 50'000, "bnd_i32_N256", 2048); });
  seeds("boundary i16 N=64",  4, [](uint64_t s) { fuzz_boundary<64, px16, qty16>(s, 50'000, "bnd_i16_N64",  512);  });

  // ── 14. Small max_cap: stress dynamic growth + frequent eviction ──
  std::cout << "\n=== Small max_cap (dynamic growth stress) ===\n";
  seeds("smallcap=16 N=64",          8, [](uint64_t s) { fuzz_spill_saturate<64, i32, u32>(s, 100'000, "sc16_N64",       16); });
  seeds("smallcap=32 N=64",          4, [](uint64_t s) { fuzz_spill_saturate<64, i32, u32>(s, 100'000, "sc32_N64",       32); });
  seeds("smallcap=16 deep N=64",     4, [](uint64_t s) { fuzz_deep<64, i32, u32>          (s, 100'000, "sc16_deep_N64",  16); });
  seeds("smallcap=32 promote N=64",  4, [](uint64_t s) { fuzz_promote_storm<64, i32, u32> (s, 20'000,  "sc32_promo_N64", 32); });
  seeds("smallcap=16 wipe N=64",     4, [](uint64_t s) { fuzz_wipe_rebuild<64, i32, u32>  (s, 200,     "sc16_wipe_N64",  16); });

  // ── Summary ──
  std::cout << "\n" << g_passed << "/" << g_total << " fuzz suites passed\n";
  return (g_passed == g_total) ? 0 : 1;
}
