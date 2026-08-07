#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <latch>
#include <thread>
#include <vector>

namespace
{
static constexpr uint32_t lu_warmup_iters = 100'000;
static constexpr uint32_t lu_batch_count  = 32;
static constexpr uint32_t lu_batch_iters  = 1'000'000;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static double percentile(std::vector<double> &or_xs, double id_p)
{
    if(or_xs.empty())
    {
        return 0.0;
    }
    std::sort(or_xs.begin(), or_xs.end());
    const double ld_rank = (static_cast<double>(or_xs.size()) - 1.0) * id_p;
    const size_t lz_lo   = static_cast<size_t>(ld_rank);
    const size_t lz_hi   = std::min(lz_lo + 1, or_xs.size() - 1);
    const double ld_frac = ld_rank - static_cast<double>(lz_lo);
    return or_xs[lz_lo] + (or_xs[lz_hi] - or_xs[lz_lo]) * ld_frac;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void report(const char *ip_label, std::vector<double> &or_samples_ns)
{
    const size_t lz_n   = or_samples_ns.size();
    double       ld_sum = 0.0;
    for(double ld_x : or_samples_ns)
    {
        ld_sum += ld_x;
    }
    const double ld_mean = ld_sum / static_cast<double>(lz_n);

    double ld_sq         = 0.0;
    for(double ld_x : or_samples_ns)
    {
        const double ld_d  = ld_x - ld_mean;
        ld_sq             += ld_d * ld_d;
    }
    const double ld_stdev       = std::sqrt(ld_sq / static_cast<double>(lz_n));

    std::vector<double> lo_copy = or_samples_ns;
    const double        ld_min  = *std::min_element(lo_copy.begin(), lo_copy.end());
    const double        ld_max  = *std::max_element(lo_copy.begin(), lo_copy.end());
    const double        ld_med  = percentile(lo_copy, 0.50);
    const double        ld_p95  = percentile(lo_copy, 0.95);

    std::printf("\n");
    std::printf("  test       : %s\n", ip_label);
    std::printf("  batches    : %zu x %u iters (warmup %u)\n", lz_n, lu_batch_iters, lu_warmup_iters);
    std::printf("  --- summary (ns/call) ---\n");
    std::printf("  n          : %zu\n", lz_n);
    std::printf("  min        : %.3f\n", ld_min);
    std::printf("  median     : %.3f\n", ld_med);
    std::printf("  mean       : %.3f\n", ld_mean);
    std::printf("  p95        : %.3f\n", ld_p95);
    std::printf("  max        : %.3f\n", ld_max);
    std::printf("  stdev      : %.3f\n", ld_stdev);
    std::printf("\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Print the running totals of telemetry elements dropped before reaching the
// sink. Captured via p8_test_get_dropped_stats(); must be read before p8_release
// since the accumulators are torn down with the instance.
static void report_drops(const s_p8_drop_stats &ir_drops)
{
    const uint64_t lu_total = ir_drops.mu_logs + ir_drops.mu_metrics + ir_drops.mu_traces;

    std::printf("  --- drops ---\n");
    std::printf("  logs       : %llu\n", static_cast<unsigned long long>(ir_drops.mu_logs));
    std::printf("  metrics    : %llu\n", static_cast<unsigned long long>(ir_drops.mu_metrics));
    std::printf("  traces     : %llu\n", static_cast<unsigned long long>(ir_drops.mu_traces));
    std::printf("  total      : %llu\n", static_cast<unsigned long long>(lu_total));
    std::printf("\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void run_batches(const std::function<void(uint32_t)> &ir_call, std::vector<double> &or_samples_ns)
{
    for(uint32_t lu_w = 0; lu_w < lu_warmup_iters; ++lu_w)
    {
        ir_call(lu_w);
    }

    or_samples_ns.clear();
    or_samples_ns.reserve(lu_batch_count);

    for(uint32_t lu_b = 0; lu_b < lu_batch_count; ++lu_b)
    {
        const auto lo_start = std::chrono::steady_clock::now();
        for(uint32_t lu_i = 0; lu_i < lu_batch_iters; ++lu_i)
        {
            ir_call(lu_i);
        }
        const auto lo_end = std::chrono::steady_clock::now();
        const auto lo_dt  = std::chrono::duration_cast<std::chrono::nanoseconds>(lo_end - lo_start).count();
        or_samples_ns.push_back(static_cast<double>(lo_dt) / static_cast<double>(lu_batch_iters));
    }
}
} // namespace

class c_mtk_perf_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{"
                                       "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"16MB\","
                                       "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"16MB\""
                                       "}";

        ASSERT_TRUE(p8_initialize(&lo_config));
    }

    void TearDown() override
    {
        p8_release();
    }
};

TEST_F(c_mtk_perf_test, DISABLED_emit_single_thread)
{
    s_p8_mtk_base lo_base   = {};
    lo_base.mp_name         = "perf.metric";
    lo_base.md_min          = 0.0;
    lo_base.md_max          = 1e9;

    const h_p8_mtk_id li_id = p8_mtk_create(&lo_base);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));

    std::vector<double> lo_samples;
    run_batches([li_id](uint32_t iu_i) { p8_mtk_emit(li_id, static_cast<double>(iu_i)); }, lo_samples);

    report("emit_single_thread", lo_samples);
    report_drops(p8_test_get_dropped_stats());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multi-threaded null sink throughput
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Each thread creates its own metric, runs an untimed warmup, then emits the
// same metric lu_perf_iters_per_thread times through the in-memory null sink.

namespace
{
static constexpr uint32_t lu_perf_iters_per_thread  = 1'000'000;
static constexpr uint32_t lu_perf_warmup_per_thread = 1'000;

struct s_emit_stats
{
    double              md_emit_ns = 0.0;
    std::vector<double> mo_thread_ns;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_emit_stats emit_from_threads(uint32_t iu_thread_count, uint32_t iu_iters_per_thread, uint32_t iu_warmup_per_thread)
{
    std::latch               lo_start_latch(iu_thread_count + 1); // + main thread
    std::vector<std::thread> lo_threads;
    std::vector<double>      lo_thread_ns(iu_thread_count, 0.0);
    lo_threads.reserve(iu_thread_count);

    for(uint32_t lu_t = 0; lu_t < iu_thread_count; ++lu_t)
    {
        lo_threads.emplace_back(
            [&, lu_t]()
            {
                // each thread registers its own metric (id valid on any thread)
                s_p8_mtk_base lo_base   = {};
                lo_base.mp_name         = "perf.thread.metric";
                const h_p8_mtk_id li_id = p8_mtk_create(&lo_base);

                // warmup phase - not counted in any measurement
                for(uint32_t lu_w = 0; lu_w < iu_warmup_per_thread; ++lu_w)
                {
                    p8_mtk_emit(li_id, static_cast<double>(lu_w));
                }

                lo_start_latch.arrive_and_wait();

                const auto lo_thread_start = std::chrono::steady_clock::now();
                for(uint32_t lu_i = 0; lu_i < iu_iters_per_thread; ++lu_i)
                {
                    p8_mtk_emit(li_id, static_cast<double>(lu_i));
                }
                const auto lo_thread_end = std::chrono::steady_clock::now();
                lo_thread_ns[lu_t]       = static_cast<double>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(lo_thread_end - lo_thread_start).count());
            });
    }

    lo_start_latch.arrive_and_wait();
    const auto lo_emit_start = std::chrono::steady_clock::now();

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }
    const auto lo_emit_end = std::chrono::steady_clock::now();

    s_emit_stats lo_stats;
    lo_stats.md_emit_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(lo_emit_end - lo_emit_start).count());
    lo_stats.mo_thread_ns = std::move(lo_thread_ns);
    return lo_stats;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void report_throughput(const char *ip_label, uint32_t iu_thread_count, uint64_t iu_total_calls, double id_elapsed_ns)
{
    const double ld_ns_per_call   = id_elapsed_ns / static_cast<double>(iu_total_calls);
    const double ld_calls_per_sec = static_cast<double>(iu_total_calls) / (id_elapsed_ns / 1e9);

    std::printf("\n");
    std::printf("  test        : %s (threads=%u)\n", ip_label, iu_thread_count);
    std::printf("  total calls : %llu\n", static_cast<unsigned long long>(iu_total_calls));
    std::printf("  elapsed     : %.3f ms\n", id_elapsed_ns / 1e6);
    std::printf("  ns/call     : %.3f\n", ld_ns_per_call);
    std::printf("  calls/sec   : %.0f\n", ld_calls_per_sec);
    std::printf("\n");
}
} // namespace

static const uint32_t ga_perf_thread_counts[] = { 1, 2, 4, 8 };

class c_mtk_perf_null_sink_full_cycle_test : public ::testing::TestWithParam<uint32_t>
{
protected:
    void TearDown() override
    {
        p8_release(); // defensive: the test body already releases on the happy path
    }
};

TEST_P(c_mtk_perf_null_sink_full_cycle_test, DISABLED_full_cycle)
{
    const uint32_t lu_threads    = GetParam();

    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{"
                                   "\"" P8_CFG_KEY_SINK "\": \"" P8_CFG_VAL_SINK_NETWORK_NULL "\","
                                   "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"32MB\","
                                   "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"32MB\""
                                   "}";

    ASSERT_TRUE(p8_initialize(&lo_config));

    const s_emit_stats lo_stats = emit_from_threads(lu_threads, lu_perf_iters_per_thread, lu_perf_warmup_per_thread);

    // Snapshot drops before release: producer writers flushed their counters into
    // the core accumulators on thread join, and p8_release tears them down.
    const s_p8_drop_stats lo_drops = p8_test_get_dropped_stats();

    p8_release();

    report_throughput("full_cycle emit (null sink)",
                      lu_threads,
                      static_cast<uint64_t>(lu_threads) * lu_perf_iters_per_thread,
                      lo_stats.md_emit_ns);
    report_drops(lo_drops);
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts,
                         c_mtk_perf_null_sink_full_cycle_test,
                         ::testing::ValuesIn(ga_perf_thread_counts));
