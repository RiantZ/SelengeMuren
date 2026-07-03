#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_config_keys.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <latch>
#include <string>
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
// multi-threaded file.bin sink throughput
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Each thread sends the same log statement lu_perf_iters_per_thread times
// through the real file.bin sink (not the in-memory null sink above), so
// the numbers below include actual disk I/O via cp8_sink_file. Two
// measurements, per the requested scenario:
//   * full_cycle : p8_initialize + all threads emitting + p8_release
//   * emit_only  : only the multi-threaded emission phase (init/release
//                  happen in SetUp/TearDown, outside the timed section)
// Thread count is configurable via INSTANTIATE_TEST_SUITE_P below.

namespace
{
static constexpr uint32_t lu_perf_iters_per_thread = 1'000'000;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::filesystem::path make_perf_scratch_dir(const char *ip_stem)
{
    const auto      lo_root = std::filesystem::temp_directory_path()
                              / (std::string(ip_stem) + "_" + std::to_string(static_cast<uint64_t>(std::rand())));
    std::error_code lo_ec;
    std::filesystem::remove_all(lo_root, lo_ec);
    std::filesystem::create_directories(lo_root);
    // printf("****************%s\n", lo_root.c_str());
    return lo_root;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string make_file_sink_config(const std::filesystem::path &ir_out_dir)
{
    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_SINK]                              = P8_CFG_VAL_SINK_FILE_BIN;
    lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = ir_out_dir.string();
    return lo_json.dump();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Spawns iu_thread_count threads, all released together via a latch, each
// sending the same log statement iu_iters_per_thread times. Blocks until
// every thread has finished.
void emit_from_threads(uint32_t iu_thread_count, uint32_t iu_iters_per_thread)
{
    std::latch               lo_start_latch(iu_thread_count);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(iu_thread_count);

    for(uint32_t lu_t = 0; lu_t < iu_thread_count; ++lu_t)
    {
        lo_threads.emplace_back(
            [&, lu_t]()
            {
                lo_start_latch.arrive_and_wait();
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
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }
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

// Thread counts exercised by both scenarios below - add/remove values here
// to change concurrency levels.
static const uint32_t ga_perf_thread_counts[] = { 1, 2, 4, 8 };

class c_log_perf_file_sink_full_cycle_test : public ::testing::TestWithParam<uint32_t>
{
protected:
    std::filesystem::path mo_out_dir;

    void SetUp() override
    {
        mo_out_dir = make_perf_scratch_dir("p8_perf_full_cycle");
    }

    void TearDown() override
    {
        p8_release(); // defensive: the test body already releases on the happy path
        std::error_code lo_ec;
        std::filesystem::remove_all(mo_out_dir, lo_ec);
    }
};

TEST_P(c_log_perf_file_sink_full_cycle_test, DISABLED_full_cycle)
{
    const uint32_t    lu_threads = GetParam();
    const std::string ls_config  = make_file_sink_config(mo_out_dir);

    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    const auto lo_start          = std::chrono::steady_clock::now();

    ASSERT_TRUE(p8_initialize(&lo_config));
    emit_from_threads(lu_threads, lu_perf_iters_per_thread);
    p8_release();

    const auto   lo_end = std::chrono::steady_clock::now();
    const double ld_ns
        = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(lo_end - lo_start).count());

    report_throughput("full_cycle (init + emit + release)",
                      lu_threads,
                      static_cast<uint64_t>(lu_threads) * lu_perf_iters_per_thread,
                      ld_ns);
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts,
                         c_log_perf_file_sink_full_cycle_test,
                         ::testing::ValuesIn(ga_perf_thread_counts));

class c_log_perf_file_sink_emit_only_test : public ::testing::TestWithParam<uint32_t>
{
protected:
    std::filesystem::path mo_out_dir;

    void SetUp() override
    {
        mo_out_dir                   = make_perf_scratch_dir("p8_perf_emit_only");

        const std::string  ls_config = make_file_sink_config(mo_out_dir);
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ls_config.c_str();

        ASSERT_TRUE(p8_initialize(&lo_config));
    }

    void TearDown() override
    {
        p8_release();
        std::error_code lo_ec;
        std::filesystem::remove_all(mo_out_dir, lo_ec);
    }
};

TEST_P(c_log_perf_file_sink_emit_only_test, DISABLED_emit_only)
{
    const uint32_t lu_threads = GetParam();

    const auto lo_start       = std::chrono::steady_clock::now();
    emit_from_threads(lu_threads, lu_perf_iters_per_thread);
    const auto lo_end = std::chrono::steady_clock::now();

    const double ld_ns
        = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(lo_end - lo_start).count());

    report_throughput("emit_only (sink already open)",
                      lu_threads,
                      static_cast<uint64_t>(lu_threads) * lu_perf_iters_per_thread,
                      ld_ns);
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts,
                         c_log_perf_file_sink_emit_only_test,
                         ::testing::ValuesIn(ga_perf_thread_counts));
