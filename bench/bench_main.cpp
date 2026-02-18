#include "bench_harness.hpp"
#include "reference_books.hpp"
#include "workloads.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
using PriceT = tape_book::i32;
using QtyT   = tape_book::u32;

static constexpr int    TAPE_N       = 256;
static constexpr int    WARMUP_OPS   = 50'000;
static constexpr int    MEASURED_OPS = 500'000;
static constexpr int    QUERY_OPS    = 200'000;
static constexpr PriceT ANCHOR       = 100'000;
static constexpr int    SPILL_CAP    = 4096;
static constexpr uint64_t SEED       = 42;

// ---------------------------------------------------------------------------
// run_suite -- benchmark all implementations on one workload
// ---------------------------------------------------------------------------
template <typename MakeWorkload>
void run_suite(const char* name, MakeWorkload make_wl) {
    bench::print_header(name, MEASURED_OPS);

    // We need separate workload instances per implementation to ensure
    // identical operation sequences (same seed -> same ops).
    bench::BenchResult results[4];

    // 1. TapeBook
    {
        auto wl = make_wl();
        bench::TapeBookAdapter<TAPE_N, PriceT, QtyT> book(SPILL_CAP);
        book.reset(ANCHOR);
        auto stats = bench::run_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
        auto qstats = bench::run_query_benchmark<decltype(book), PriceT>(book, QUERY_OPS);
        auto& r = results[0];
        std::snprintf(r.name, sizeof(r.name), "TapeBook<%d>", TAPE_N);
        r.set_stats = stats;
        r.query_stats = qstats;
    }
    // TapeBook throughput (separate workload instance for clean state)
    {
        auto wl = make_wl();
        bench::TapeBookAdapter<TAPE_N, PriceT, QtyT> book(SPILL_CAP);
        book.reset(ANCHOR);
        results[0].throughput_stats =
            bench::run_throughput_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
    }

    // 2. OrderBookMap
    {
        auto wl = make_wl();
        bench::OrderBookMap<PriceT, QtyT> book;
        book.reset(ANCHOR);
        auto stats = bench::run_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
        auto qstats = bench::run_query_benchmark<decltype(book), PriceT>(book, QUERY_OPS);
        auto& r = results[1];
        std::snprintf(r.name, sizeof(r.name), "OrderBookMap");
        r.set_stats = stats;
        r.query_stats = qstats;
    }
    {
        auto wl = make_wl();
        bench::OrderBookMap<PriceT, QtyT> book;
        book.reset(ANCHOR);
        results[1].throughput_stats =
            bench::run_throughput_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
    }

    // 3. OrderBookVector (binary search)
    {
        auto wl = make_wl();
        bench::OrderBookVector<PriceT, QtyT> book;
        book.reset(ANCHOR);
        auto stats = bench::run_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
        auto qstats = bench::run_query_benchmark<decltype(book), PriceT>(book, QUERY_OPS);
        auto& r = results[2];
        std::snprintf(r.name, sizeof(r.name), "OrderBookVector");
        r.set_stats = stats;
        r.query_stats = qstats;
    }
    {
        auto wl = make_wl();
        bench::OrderBookVector<PriceT, QtyT> book;
        book.reset(ANCHOR);
        results[2].throughput_stats =
            bench::run_throughput_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
    }

    // 4. OrderBookVectorLinear
    {
        auto wl = make_wl();
        bench::OrderBookVectorLinear<PriceT, QtyT> book;
        book.reset(ANCHOR);
        auto stats = bench::run_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
        auto qstats = bench::run_query_benchmark<decltype(book), PriceT>(book, QUERY_OPS);
        auto& r = results[3];
        std::snprintf(r.name, sizeof(r.name), "OrderBookVectorLinear");
        r.set_stats = stats;
        r.query_stats = qstats;
    }
    {
        auto wl = make_wl();
        bench::OrderBookVectorLinear<PriceT, QtyT> book;
        book.reset(ANCHOR);
        results[3].throughput_stats =
            bench::run_throughput_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
    }

    // Print set() latency table
    std::fprintf(stdout, "  -- set() latency (ns) --\n");
    bench::print_table_header();
    for (auto& r : results) bench::print_row(r);

    // Print throughput table
    bench::print_throughput_header();
    for (auto& r : results) bench::print_throughput_row(r);

    // Print query latency table
    bench::print_query_header();
    for (auto& r : results) bench::print_query_row(r);

    std::fprintf(stdout, "\n");
}

// ---------------------------------------------------------------------------
// cross_validate -- run all 4 implementations with identical ops, assert
//                   best_bid_px() and best_ask_px() agree after every op.
// ---------------------------------------------------------------------------
static constexpr int VALIDATE_OPS = 100'000;

template <typename MakeWorkload>
bool cross_validate(const char* name, MakeWorkload make_wl) {
    // Create 4 identical workload generators (same seed -> same sequence)
    auto wl1 = make_wl();
    auto wl2 = make_wl();
    auto wl3 = make_wl();
    auto wl4 = make_wl();

    bench::TapeBookAdapter<TAPE_N, PriceT, QtyT> tb(SPILL_CAP);
    bench::OrderBookMap<PriceT, QtyT>             obm;
    bench::OrderBookVector<PriceT, QtyT>          obv;
    bench::OrderBookVectorLinear<PriceT, QtyT>    obvl;

    tb.reset(ANCHOR);
    obm.reset(ANCHOR);
    obv.reset(ANCHOR);
    obvl.reset(ANCHOR);

    for (int i = 0; i < VALIDATE_OPS; ++i) {
        auto [is_bid1, px1, qty1] = wl1();
        auto [is_bid2, px2, qty2] = wl2();
        auto [is_bid3, px3, qty3] = wl3();
        auto [is_bid4, px4, qty4] = wl4();

        // All generators should produce identical ops
        (void)is_bid2; (void)px2; (void)qty2;
        (void)is_bid3; (void)px3; (void)qty3;
        (void)is_bid4; (void)px4; (void)qty4;

        if (is_bid1) {
            tb.set_bid(px1, qty1);
            obm.set_bid(px1, qty1);
            obv.set_bid(px1, qty1);
            obvl.set_bid(px1, qty1);
        } else {
            tb.set_ask(px1, qty1);
            obm.set_ask(px1, qty1);
            obv.set_ask(px1, qty1);
            obvl.set_ask(px1, qty1);
        }

        // Compare best prices across all implementations
        PriceT tb_bid   = tb.best_bid_px();
        PriceT obm_bid  = obm.best_bid_px();
        PriceT obv_bid  = obv.best_bid_px();
        PriceT obvl_bid = obvl.best_bid_px();

        PriceT tb_ask   = tb.best_ask_px();
        PriceT obm_ask  = obm.best_ask_px();
        PriceT obv_ask  = obv.best_ask_px();
        PriceT obvl_ask = obvl.best_ask_px();

        // Map/Vector/VectorLinear should always agree with each other.
        // TapeBook uses different sentinel values, so normalize for comparison.
        // TapeBook: lowest_px for no-bid, highest_px for no-ask
        // Map/Vec:  numeric_limits::lowest() for no-bid, max() for no-ask
        // These are the same values, so direct comparison works.

        bool bid_ok = (tb_bid == obm_bid) && (obm_bid == obv_bid) && (obv_bid == obvl_bid);
        bool ask_ok = (tb_ask == obm_ask) && (obm_ask == obv_ask) && (obv_ask == obvl_ask);

        if (!bid_ok || !ask_ok) {
            std::fprintf(stderr,
                "CROSS-VALIDATION FAILED: %s, op %d\n"
                "  is_bid=%d px=%d qty=%u\n"
                "  best_bid: TB=%d Map=%d Vec=%d VecLin=%d\n"
                "  best_ask: TB=%d Map=%d Vec=%d VecLin=%d\n",
                name, i,
                (int)is_bid1, (int)px1, (unsigned)qty1,
                (int)tb_bid, (int)obm_bid, (int)obv_bid, (int)obvl_bid,
                (int)tb_ask, (int)obm_ask, (int)obv_ask, (int)obvl_ask);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// run_tape_sweep -- benchmark TapeBook at N=64,128,256,512,1024
//                   on a single workload (throughput only)
// ---------------------------------------------------------------------------
template <typename MakeWorkload>
void run_tape_sweep(const char* name, MakeWorkload make_wl) {
    std::fprintf(stdout,
        "\n=== Tape Size Sweep: %s (%d ops) ===\n\n", name, MEASURED_OPS);
    std::fprintf(stdout,
        "  %-16s | %7s | %7s | %7s\n",
        "TapeBook<N>", "Mops/s", "mixed", "sizeof");
    std::fprintf(stdout,
        "  %-16s-+-%7s-+-%7s-+-%7s\n",
        "----------------", "-------", "-------", "-------");

    auto run_one = [&](auto tag, const char* label) {
        constexpr int N = decltype(tag)::value;
        // Throughput
        bench::ThroughputStats thr;
        {
            auto wl = make_wl();
            bench::TapeBookAdapter<N, PriceT, QtyT> book(SPILL_CAP);
            book.reset(ANCHOR);
            thr = bench::run_throughput_benchmark(book, wl, WARMUP_OPS, MEASURED_OPS);
        }
        // Mixed throughput
        bench::ThroughputStats mix;
        {
            auto wl = make_wl();
            bench::TapeBookAdapter<N, PriceT, QtyT> book(SPILL_CAP);
            book.reset(ANCHOR);
            mix = bench::run_mixed_throughput(book, wl, WARMUP_OPS, MEASURED_OPS);
        }
        std::fprintf(stdout,
            "  %-16s | %7.1f | %7.1f | %7zu\n",
            label, thr.mops, mix.mops,
            sizeof(bench::TapeBookAdapter<N, PriceT, QtyT>));
    };

    run_one(std::integral_constant<int,   64>{}, "TapeBook<64>");
    run_one(std::integral_constant<int,  128>{}, "TapeBook<128>");
    run_one(std::integral_constant<int,  256>{}, "TapeBook<256>");
    run_one(std::integral_constant<int,  512>{}, "TapeBook<512>");
    run_one(std::integral_constant<int, 1024>{}, "TapeBook<1024>");

    std::fprintf(stdout, "\n");
}

// ---------------------------------------------------------------------------
// run_mixed_suite -- mixed read/write throughput across all implementations
// ---------------------------------------------------------------------------
template <typename MakeWorkload>
void run_mixed_suite(const char* name, MakeWorkload make_wl) {
    std::fprintf(stdout,
        "\n=== Mixed R/W: %s (%d ops, 1 query per 3 sets) ===\n\n",
        name, MEASURED_OPS);
    std::fprintf(stdout,
        "  %-26s | %7s | %7s\n",
        "Implementation", "Mops/s", "vs pure");
    std::fprintf(stdout,
        "  %-26s-+-%7s-+-%7s\n",
        "--------------------------", "-------", "-------");

    struct Row { char name[48]; double mixed_mops; double pure_mops; };
    Row rows[4];

    auto run_one = [&](int idx, auto& book_ref, const char* label) {
        std::snprintf(rows[idx].name, sizeof(rows[idx].name), "%s", label);
        // Pure throughput
        {
            auto wl = make_wl();
            book_ref.reset(ANCHOR);
            auto thr = bench::run_throughput_benchmark(book_ref, wl, WARMUP_OPS, MEASURED_OPS);
            rows[idx].pure_mops = thr.mops;
        }
        // Mixed throughput
        {
            auto wl = make_wl();
            book_ref.reset(ANCHOR);
            auto mix = bench::run_mixed_throughput(book_ref, wl, WARMUP_OPS, MEASURED_OPS);
            rows[idx].mixed_mops = mix.mops;
        }
    };

    bench::TapeBookAdapter<TAPE_N, PriceT, QtyT> tb(SPILL_CAP);
    bench::OrderBookMap<PriceT, QtyT> obm;
    bench::OrderBookVector<PriceT, QtyT> obv;
    bench::OrderBookVectorLinear<PriceT, QtyT> obvl;

    char tb_name[48];
    std::snprintf(tb_name, sizeof(tb_name), "TapeBook<%d>", TAPE_N);

    run_one(0, tb,   tb_name);
    run_one(1, obm,  "OrderBookMap");
    run_one(2, obv,  "OrderBookVector");
    run_one(3, obvl, "OrderBookVectorLinear");

    for (auto& r : rows) {
        double ratio = (r.pure_mops > 0) ? r.mixed_mops / r.pure_mops : 0.0;
        std::fprintf(stdout,
            "  %-26s | %7.1f | %6.0f%%\n",
            r.name, r.mixed_mops, ratio * 100.0);
    }
    std::fprintf(stdout, "\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    bench::print_system_info();

    // -----------------------------------------------------------------------
    // Cross-validation: verify all implementations agree
    // -----------------------------------------------------------------------
    std::fprintf(stdout, "\n=== Cross-Validation (%d ops per workload) ===\n\n", VALIDATE_OPS);
    int pass = 0, fail = 0;

    auto check = [&](const char* name, auto make_wl) {
        bool ok = cross_validate(name, make_wl);
        std::fprintf(stdout, "  %-40s %s\n", name, ok ? "PASS" : "FAIL");
        if (ok) ++pass; else ++fail;
    };

    check("Clustered BBO", []() {
        return bench::WorkloadClustered<PriceT, QtyT>(SEED, ANCHOR, 10);
    });
    check("Uniform Random", []() {
        return bench::WorkloadUniform<PriceT, QtyT>(SEED, ANCHOR, 500);
    });
    check("Heavy Spill", []() {
        return bench::WorkloadHeavySpill<PriceT, QtyT>(SEED, ANCHOR, TAPE_N / 2);
    });
    check("Cancel Heavy", []() {
        return bench::WorkloadCancelHeavy<PriceT, QtyT>(SEED, ANCHOR, 50);
    });

    std::fprintf(stdout, "\n  Result: %d/%d passed\n", pass, pass + fail);

    if (fail > 0) {
        std::fprintf(stderr, "\nAborting benchmarks due to validation failure.\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Performance benchmarks
    // -----------------------------------------------------------------------

    // 1. Clustered near BBO (realistic)
    run_suite("Clustered Near BBO (tight_range=10)", []() {
        return bench::WorkloadClustered<PriceT, QtyT>(SEED, ANCHOR, 10);
    });

    // 2. Uniform random (wide spread)
    run_suite("Uniform Random (range=500)", []() {
        return bench::WorkloadUniform<PriceT, QtyT>(SEED, ANCHOR, 500);
    });

    // 3. Heavy spill (tape_book worst case)
    run_suite("Heavy Spill (80%% outside tape)", []() {
        return bench::WorkloadHeavySpill<PriceT, QtyT>(SEED, ANCHOR, TAPE_N / 2);
    });

    // 4. Price walk (trending, forces recenters)
    run_suite("Price Walk (step=2)", []() {
        return bench::WorkloadPriceWalk<PriceT, QtyT>(SEED, ANCHOR - 5, ANCHOR + 5, 2);
    });

    // 5. Cancel heavy (70% cancels)
    run_suite("Cancel Heavy (70%% cancels, range=50)", []() {
        return bench::WorkloadCancelHeavy<PriceT, QtyT>(SEED, ANCHOR, 50);
    });

    // -----------------------------------------------------------------------
    // Tape size sweep
    // -----------------------------------------------------------------------
    run_tape_sweep("Clustered BBO (tight_range=10)", []() {
        return bench::WorkloadClustered<PriceT, QtyT>(SEED, ANCHOR, 10);
    });
    run_tape_sweep("Uniform Random (range=500)", []() {
        return bench::WorkloadUniform<PriceT, QtyT>(SEED, ANCHOR, 500);
    });

    // -----------------------------------------------------------------------
    // Mixed read/write throughput
    // -----------------------------------------------------------------------
    run_mixed_suite("Clustered BBO (tight_range=10)", []() {
        return bench::WorkloadClustered<PriceT, QtyT>(SEED, ANCHOR, 10);
    });
    run_mixed_suite("Uniform Random (range=500)", []() {
        return bench::WorkloadUniform<PriceT, QtyT>(SEED, ANCHOR, 500);
    });

    return 0;
}
