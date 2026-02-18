#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// DoNotOptimize -- prevent compiler from eliminating dead results
// ---------------------------------------------------------------------------
template <typename T>
inline void DoNotOptimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    volatile auto sink = value;
    (void)sink;
#endif
}

inline void ClobberMemory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" ::: "memory");
#endif
}

// ---------------------------------------------------------------------------
// Timer -- steady_clock based nanosecond timer
// ---------------------------------------------------------------------------
struct Timer {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    clock::time_point start_;

    inline void start() noexcept { start_ = clock::now(); }

    [[nodiscard]] inline int64_t elapsed_ns() const noexcept {
        return std::chrono::duration_cast<ns>(clock::now() - start_).count();
    }
};

// ---------------------------------------------------------------------------
// LatencyStats -- percentile summary of latency measurements
// ---------------------------------------------------------------------------
struct LatencyStats {
    int64_t min   = 0;
    int64_t max   = 0;
    int64_t mean  = 0;
    int64_t p50   = 0;
    int64_t p90   = 0;
    int64_t p99   = 0;
    int64_t p999  = 0;
    int64_t count = 0;
};

// ---------------------------------------------------------------------------
// LatencyCollector -- collects per-operation latencies, computes percentiles
// ---------------------------------------------------------------------------
struct LatencyCollector {
    std::vector<int64_t> samples;

    void reserve(size_t n) { samples.reserve(n); }

    inline void record(int64_t ns) { samples.push_back(ns); }

    [[nodiscard]] LatencyStats compute() {
        if (samples.empty()) return {};

        std::sort(samples.begin(), samples.end());

        auto pct = [&](double p) -> int64_t {
            auto idx = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
            return samples[idx];
        };

        int64_t sum = 0;
        for (auto s : samples) sum += s;

        return LatencyStats{
            .min   = samples.front(),
            .max   = samples.back(),
            .mean  = sum / static_cast<int64_t>(samples.size()),
            .p50   = pct(0.50),
            .p90   = pct(0.90),
            .p99   = pct(0.99),
            .p999  = pct(0.999),
            .count = static_cast<int64_t>(samples.size()),
        };
    }

    void clear() { samples.clear(); }
};

// ---------------------------------------------------------------------------
// measure_clock_resolution -- reports minimum observable delta in ns
// ---------------------------------------------------------------------------
inline int64_t measure_clock_resolution() {
    constexpr int ITERS = 1000;
    int64_t min_delta = INT64_MAX;
    auto prev = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev).count();
        if (delta > 0 && delta < min_delta) min_delta = delta;
        prev = now;
    }
    return min_delta;
}

// ---------------------------------------------------------------------------
// run_benchmark -- generic benchmark runner
//
// Template parameters:
//   BookT    -- must have reset(), set_bid(), set_ask(), best_bid_px(), best_ask_px()
//   GenOp    -- callable that returns { bool is_bid, PriceT px, QtyT qty }
// ---------------------------------------------------------------------------
template <typename BookT, typename GenOp>
LatencyStats run_benchmark(
    BookT& book,
    GenOp& gen,
    int warmup_ops,
    int measured_ops)
{
    // Warmup phase (not timed)
    for (int i = 0; i < warmup_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);
    }

    // Measured phase
    LatencyCollector collector;
    collector.reserve(static_cast<size_t>(measured_ops));
    Timer timer;

    for (int i = 0; i < measured_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        ClobberMemory();
        timer.start();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);
        ClobberMemory();
        collector.record(timer.elapsed_ns());
    }

    return collector.compute();
}

// ---------------------------------------------------------------------------
// run_query_benchmark -- benchmark best_bid_px / best_ask_px queries
// ---------------------------------------------------------------------------
template <typename BookT, typename PriceT>
LatencyStats run_query_benchmark(BookT& book, int ops) {
    LatencyCollector collector;
    collector.reserve(static_cast<size_t>(ops));
    Timer timer;

    for (int i = 0; i < ops; ++i) {
        ClobberMemory();
        timer.start();
        PriceT px = (i & 1) ? book.best_bid_px() : book.best_ask_px();
        ClobberMemory();
        DoNotOptimize(px);
        collector.record(timer.elapsed_ns());
    }

    return collector.compute();
}

// ---------------------------------------------------------------------------
// ThroughputStats -- batch-timed throughput (no per-op clock overhead)
// ---------------------------------------------------------------------------
struct ThroughputStats {
    int64_t total_ns = 0;
    int64_t ops      = 0;
    double  mops     = 0.0;
};

// ---------------------------------------------------------------------------
// run_throughput_benchmark -- times N ops with a single start/stop
//
// Bypasses the clock resolution floor by not reading the clock per-op.
// Gives clean Mops/s numbers on macOS where clock resolution is ~41ns.
// ---------------------------------------------------------------------------
template <typename BookT, typename GenOp>
ThroughputStats run_throughput_benchmark(
    BookT& book,
    GenOp& gen,
    int warmup_ops,
    int measured_ops)
{
    // Warmup phase (not timed)
    for (int i = 0; i < warmup_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);
    }

    // Measured phase -- single start/stop
    ClobberMemory();
    Timer timer;
    timer.start();

    for (int i = 0; i < measured_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);
    }

    ClobberMemory();
    int64_t elapsed = timer.elapsed_ns();

    ThroughputStats stats;
    stats.total_ns = elapsed;
    stats.ops = measured_ops;
    stats.mops = (elapsed > 0)
        ? static_cast<double>(measured_ops) / (static_cast<double>(elapsed) / 1000.0)
        : 0.0;
    return stats;
}

// ---------------------------------------------------------------------------
// run_mixed_throughput -- interleaves set() and best_price queries
//
// Ratio: 1 query per `query_every` sets (default 3 = 25% reads, 75% writes).
// Batch-timed for clean Mops/s.
// ---------------------------------------------------------------------------
template <typename BookT, typename GenOp>
ThroughputStats run_mixed_throughput(
    BookT& book,
    GenOp& gen,
    int warmup_ops,
    int measured_ops,
    int query_every = 3)
{
    // Warmup phase (not timed)
    for (int i = 0; i < warmup_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);
    }

    // Measured phase -- single start/stop
    ClobberMemory();
    Timer timer;
    timer.start();

    for (int i = 0; i < measured_ops; ++i) {
        auto [is_bid, px, qty] = gen();
        if (is_bid) book.set_bid(px, qty);
        else        book.set_ask(px, qty);

        if (i % query_every == 0) {
            auto bid = book.best_bid_px();
            auto ask = book.best_ask_px();
            DoNotOptimize(bid);
            DoNotOptimize(ask);
        }
    }

    ClobberMemory();
    int64_t elapsed = timer.elapsed_ns();

    ThroughputStats stats;
    stats.total_ns = elapsed;
    stats.ops = measured_ops;
    stats.mops = (elapsed > 0)
        ? static_cast<double>(measured_ops) / (static_cast<double>(elapsed) / 1000.0)
        : 0.0;
    return stats;
}

// ---------------------------------------------------------------------------
// Reporting helpers
// ---------------------------------------------------------------------------
struct BenchResult {
    char name[48];
    LatencyStats set_stats;
    LatencyStats query_stats;
    ThroughputStats throughput_stats;
};

inline void print_header(const char* workload_name, int ops) {
    std::fprintf(stdout,
        "\n=== Workload: %s (%d ops) ===\n\n", workload_name, ops);
}

inline void print_table_header() {
    std::fprintf(stdout,
        "  %-26s | %5s | %5s | %5s | %5s | %5s | %7s | %5s | %7s\n",
        "Implementation", "min", "p50", "p90", "p99", "p99.9", "max", "mean", "Mops/s");
    std::fprintf(stdout,
        "  %-26s-+-%5s-+-%5s-+-%5s-+-%5s-+-%5s-+-%7s-+-%5s-+-%7s\n",
        "--------------------------", "-----", "-----", "-----",
        "-----", "-----", "-------", "-----", "-------");
}

inline void print_row(const BenchResult& r) {
    const auto& s = r.set_stats;
    double mops = (s.mean > 0) ? 1000.0 / static_cast<double>(s.mean) : 0.0;
    std::fprintf(stdout,
        "  %-26s | %5lld | %5lld | %5lld | %5lld | %5lld | %7lld | %5lld | %7.1f\n",
        r.name,
        (long long)s.min, (long long)s.p50, (long long)s.p90,
        (long long)s.p99, (long long)s.p999, (long long)s.max,
        (long long)s.mean, mops);
}

inline void print_query_header() {
    std::fprintf(stdout, "\n  -- best_price query latency --\n");
    print_table_header();
}

inline void print_query_row(const BenchResult& r) {
    const auto& s = r.query_stats;
    double mops = (s.mean > 0) ? 1000.0 / static_cast<double>(s.mean) : 0.0;
    std::fprintf(stdout,
        "  %-26s | %5lld | %5lld | %5lld | %5lld | %5lld | %7lld | %5lld | %7.1f\n",
        r.name,
        (long long)s.min, (long long)s.p50, (long long)s.p90,
        (long long)s.p99, (long long)s.p999, (long long)s.max,
        (long long)s.mean, mops);
}

inline void print_throughput_header() {
    std::fprintf(stdout, "\n  -- throughput (batch-timed, no per-op clock overhead) --\n");
    std::fprintf(stdout,
        "  %-26s | %12s | %10s | %7s\n",
        "Implementation", "total_ns", "ops", "Mops/s");
    std::fprintf(stdout,
        "  %-26s-+-%12s-+-%10s-+-%7s\n",
        "--------------------------", "------------",
        "----------", "-------");
}

inline void print_throughput_row(const BenchResult& r) {
    const auto& s = r.throughput_stats;
    std::fprintf(stdout,
        "  %-26s | %12lld | %10lld | %7.1f\n",
        r.name,
        (long long)s.total_ns, (long long)s.ops, s.mops);
}

inline void print_system_info() {
    std::fprintf(stdout, "tape_book benchmark\n");
    std::fprintf(stdout, "-------------------\n");
#if defined(__clang__)
    std::fprintf(stdout, "Compiler: Clang %d.%d.%d\n",
        __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    std::fprintf(stdout, "Compiler: GCC %d.%d.%d\n",
        __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    std::fprintf(stdout, "Compiler: unknown\n");
#endif
    std::fprintf(stdout, "Clock resolution: %lld ns\n",
        (long long)measure_clock_resolution());
    std::fprintf(stdout, "-------------------\n");
}

}  // namespace bench
