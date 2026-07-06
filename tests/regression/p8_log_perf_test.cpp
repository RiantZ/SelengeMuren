#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_config_keys.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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
    for(size_t lz_i = 0; lz_i < lz_n; ++lz_i)
    {
        std::printf("  batch %2zu   : %.3f ns/call\n", lz_i, or_samples_ns[lz_i]);
    }
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

class c_log_perf_test : public ::testing::Test
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

TEST_F(c_log_perf_test, DISABLED_send_hello_d_no_attrs)
{
    std::vector<double> lo_samples;
    run_batches(
        [](uint32_t iu_i)
        {
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        0,
                        nullptr,
                        "hello %d",
                        static_cast<int>(iu_i));
        },
        lo_samples);

    report("send_hello_d_no_attrs", lo_samples);
}

TEST_F(c_log_perf_test, DISABLED_send_hello_d_3_attrs)
{
    p8_attr_id li_str = p8_attr_register("perf_label", e_p8_attr_str);
    p8_attr_id li_i64 = p8_attr_register("perf_count", e_p8_attr_i64);
    p8_attr_id li_f64 = p8_attr_register("perf_ratio", e_p8_attr_f64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_str));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_i64));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_f64));

    struct s_p8_attr_val la_attrs[3] = {};
    la_attrs[0].m_id                 = li_str;
    la_attrs[0].mp_str               = "request_handler";
    la_attrs[1].m_id                 = li_i64;
    la_attrs[1].mi_i64               = 42;
    la_attrs[2].m_id                 = li_f64;
    la_attrs[2].md_f64               = 3.14;

    std::vector<double> lo_samples;
    run_batches(
        [&la_attrs](uint32_t iu_i)
        {
            la_attrs[1].mi_i64 = static_cast<int64_t>(iu_i);
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        3,
                        la_attrs,
                        "hello %d",
                        static_cast<int>(iu_i));
        },
        lo_samples);

    report("send_hello_d_3_attrs", lo_samples);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multi-threaded null sink throughput
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Each thread first runs a warmup phase of lu_perf_warmup_per_thread log
// statements (excluded from all measurements), then sends the same log
// statement lu_perf_iters_per_thread times through the in-memory null sink
// under a fixed 32MB memory budget. Two measurements are reported:
//   * full_cycle : p8_initialize + emit phase + p8_release (warmup excluded)
//   * per-thread : how long each individual thread spent in its emit loop
// Thread count is configurable via INSTANTIATE_TEST_SUITE_P below.

namespace
{
static constexpr uint32_t lu_perf_iters_per_thread  = 1'000'000;
static constexpr uint32_t lu_perf_warmup_per_thread = 1'000;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Results collected from a single emit run.
struct s_emit_stats
{
    double              md_emit_ns = 0.0; // wall time of the whole emit phase (warmup excluded)
    std::vector<double> mo_thread_ns;     // per-thread emit-loop durations, one entry per thread
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Spawns iu_thread_count threads. Each thread runs an untimed warmup of
// iu_warmup_per_thread messages, then all threads are released together via
// a latch (which the main thread joins so the timed window starts only once
// every warmup is done). Each thread times its own emit loop; the main
// thread times the whole emit phase. Blocks until every thread has finished.
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
                // warmup phase - not counted in any measurement
                for(uint32_t lu_w = 0; lu_w < iu_warmup_per_thread; ++lu_w)
                {
                    p8_log_sent(e_p8_trace0,
                                nullptr,
                                0,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "perf warmup %u iter %u",
                                lu_t,
                                lu_w);
                }

                lo_start_latch.arrive_and_wait();

                const auto lo_thread_start = std::chrono::steady_clock::now();
                for(uint32_t lu_i = 0; lu_i < iu_iters_per_thread; ++lu_i)
                {
                    p8_log_sent(e_p8_trace0,
                                nullptr,
                                0,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "perf thread %u iter %u",
                                lu_t,
                                lu_i);
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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void report_per_thread(const std::vector<double> &ir_thread_ns, uint32_t iu_iters_per_thread)
{
    std::printf("  --- per-thread emit times ---\n");
    for(size_t lz_t = 0; lz_t < ir_thread_ns.size(); ++lz_t)
    {
        const double ld_ns_per_call = ir_thread_ns[lz_t] / static_cast<double>(iu_iters_per_thread);
        std::printf("  thread %2zu   : %.3f ms (%.3f ns/call)\n", lz_t, ir_thread_ns[lz_t] / 1e6, ld_ns_per_call);
    }
    std::printf("\n");
}
} // namespace

// Thread counts exercised below - add/remove values here to change
// concurrency levels.
static const uint32_t ga_perf_thread_counts[] = { 1, 2, 4, 8 };

class c_log_perf_file_sink_full_cycle_test : public ::testing::TestWithParam<uint32_t>
{
protected:
    void TearDown() override
    {
        p8_release(); // defensive: the test body already releases on the happy path
    }
};

TEST_P(c_log_perf_file_sink_full_cycle_test, DISABLED_full_cycle)
{
    const uint32_t lu_threads    = GetParam();

    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{"
                                   "\"" P8_CFG_KEY_SINK "\": \"" P8_CFG_VAL_SINK_NETWORK_NULL "\","
                                   "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"32MB\","
                                   "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"32MB\""
                                   "}";

    const auto lo_init_start     = std::chrono::steady_clock::now();
    ASSERT_TRUE(p8_initialize(&lo_config));
    const auto lo_init_end      = std::chrono::steady_clock::now();

    const s_emit_stats lo_stats = emit_from_threads(lu_threads, lu_perf_iters_per_thread, lu_perf_warmup_per_thread);

    const auto lo_rel_start     = std::chrono::steady_clock::now();
    p8_release();
    const auto lo_rel_end   = std::chrono::steady_clock::now();

    const double ld_init_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(lo_init_end - lo_init_start).count());
    const double ld_rel_ns
        = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(lo_rel_end - lo_rel_start).count());
    const double ld_full_ns = ld_init_ns + lo_stats.md_emit_ns + ld_rel_ns;

    report_throughput("full_cycle (init + emit + release)",
                      lu_threads,
                      static_cast<uint64_t>(lu_threads) * lu_perf_iters_per_thread,
                      ld_full_ns);
    report_per_thread(lo_stats.mo_thread_ns, lu_perf_iters_per_thread);
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts,
                         c_log_perf_file_sink_full_cycle_test,
                         ::testing::ValuesIn(ga_perf_thread_counts));
