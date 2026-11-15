#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"

#include "kit/ts_helpers.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#if !defined(G_OS_WINDOWS)
    #include <sys/resource.h>
    #include <time.h>
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// the null sink against the file sink under one load, to find where the file sink loses throughput
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
static constexpr uint32_t gu_total_records = 8'000'000; // the load of tests/competitors/report.md on lau/competitors
static constexpr uint32_t gu_warmup_calls  = 1'000;
static constexpr uint32_t gu_runs          = 5;
static const char        *gp_pool_size     = "1MB"; // the pool of that report: 128 buffers of 8 KB

// what one run measured; the CPU numbers stay 0 where the platform gives none
struct s_sink_run
{
    double   md_emit_ms       = 0.0; // all writers released .. all writers done
    double   md_release_ms    = 0.0; // p8_release: the last buffers and the close of the sink
    double   md_off_cpu_pct   = 0.0; // share of a writer's time off any CPU, mean of the writers
    double   md_user_ms       = 0.0; // CPU of the whole process while the writers ran
    double   md_sys_ms        = 0.0;
    uint64_t mu_wait_arrivals = 0;   // times a writer slept in acquire_wait for a buffer
    uint64_t mu_dropped       = 0;
    uint64_t mu_file_bytes    = 0;   // size of the .p8dat file, 0 for the null sink
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CPU time of the calling thread, ns
static uint64_t get_thread_cpu_ns()
{
#if defined(G_OS_WINDOWS)
    return 0;
#else
    struct timespec lo_ts = {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &lo_ts);
    return static_cast<uint64_t>(lo_ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(lo_ts.tv_nsec);
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void get_process_usage(double &od_user_ms, double &od_sys_ms)
{
    od_user_ms = 0.0;
    od_sys_ms  = 0.0;
#if !defined(G_OS_WINDOWS)
    struct rusage lo_usage = {};
    getrusage(RUSAGE_SELF, &lo_usage);
    od_user_ms
        = static_cast<double>(lo_usage.ru_utime.tv_sec) * 1e3 + static_cast<double>(lo_usage.ru_utime.tv_usec) * 1e-3;
    od_sys_ms
        = static_cast<double>(lo_usage.ru_stime.tv_sec) * 1e3 + static_cast<double>(lo_usage.ru_stime.tv_usec) * 1e-3;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// one call site for the warm-up and the timed loop, so the timed loop reuses the descriptor
static void emit_logs(uint32_t iu_thread, uint32_t iu_count)
{
    for(uint32_t lu_i = 0; lu_i < iu_count; ++lu_i)
    {
        p8_log_sent(e_p8_trace0,
                    nullptr,
                    P8_INVALID_SPAN,
                    static_cast<uint32_t>(__LINE__),
                    __FILE__,
                    __FUNCTION__,
                    0,
                    nullptr,
                    "perf thread %u iter %u",
                    iu_thread,
                    lu_i);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// bytes of the .p8dat files under ir_dir; the sink nests them in a per-run directory
static uint64_t get_data_file_bytes(const std::filesystem::path &ir_dir)
{
    uint64_t        lu_bytes = 0;
    std::error_code lo_ec;
    for(const auto &lo_entry : std::filesystem::recursive_directory_iterator(ir_dir, lo_ec))
    {
        if(lo_entry.is_regular_file() && ".p8dat" == lo_entry.path().extension())
        {
            lu_bytes += static_cast<uint64_t>(lo_entry.file_size());
        }
    }
    return lu_bytes;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// one init .. release cycle; the writers warm up first and start together
static bool run_once(bool ib_file, uint32_t iu_threads, s_sink_run &or_run)
{
    const std::filesystem::path lo_dir = std::filesystem::temp_directory_path() / "p8_sink_perf_test";
    nlohmann::json              lo_json;
    lo_json[P8_CFG_KEY_MAX_MEMORY_SIZE]     = gp_pool_size;
    lo_json[P8_CFG_KEY_INITIAL_MEMORY_SIZE] = gp_pool_size;
    if(ib_file)
    {
        lo_json[P8_CFG_KEY_SINK]                              = P8_CFG_VAL_SINK_FILE_BIN;
        lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = lo_dir.string();
    }
    else
    {
        lo_json[P8_CFG_KEY_SINK] = P8_CFG_VAL_SINK_NETWORK_NULL;
    }
    const std::string  ls_config = lo_json.dump();
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();
    if(!p8_initialize(&lo_config))
    {
        return false;
    }

    const uint32_t           lu_per_thread = gu_total_records / iu_threads;
    std::latch               lo_ready(iu_threads);
    std::latch               lo_go(1);
    std::vector<double>      lo_off_cpu(iu_threads, 0.0);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(iu_threads);
    for(uint32_t lu_t = 0; lu_t < iu_threads; ++lu_t)
    {
        lo_threads.emplace_back(
            [&, lu_t]()
            {
                emit_logs(lu_t, gu_warmup_calls);
                lo_ready.count_down();
                lo_go.wait();

                const auto     lo_begin = std::chrono::steady_clock::now();
                const uint64_t lu_cpu   = get_thread_cpu_ns();
                emit_logs(lu_t, lu_per_thread);
                const double ld_cpu = static_cast<double>(get_thread_cpu_ns() - lu_cpu);
                const double ld_wall
                    = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - lo_begin).count();
                lo_off_cpu[lu_t] = (ld_cpu > 0.0 && ld_wall > ld_cpu) ? 100.0 * (ld_wall - ld_cpu) / ld_wall : 0.0;
            });
    }

    double ld_user = 0.0;
    double ld_sys  = 0.0;
    lo_ready.wait();
    const uint64_t lu_waits = p8_test_get_wait_arrivals();
    get_process_usage(ld_user, ld_sys);
    const auto lo_begin = std::chrono::steady_clock::now();
    lo_go.count_down();
    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }
    const auto lo_end = std::chrono::steady_clock::now();
    get_process_usage(or_run.md_user_ms, or_run.md_sys_ms);

    or_run.md_emit_ms        = std::chrono::duration<double, std::milli>(lo_end - lo_begin).count();
    or_run.md_user_ms       -= ld_user;
    or_run.md_sys_ms        -= ld_sys;
    or_run.mu_wait_arrivals  = p8_test_get_wait_arrivals() - lu_waits;
    or_run.mu_dropped        = p8_test_get_dropped_stats().mu_logs;
    or_run.md_off_cpu_pct    = 0.0;
    for(double ld_pct : lo_off_cpu)
    {
        or_run.md_off_cpu_pct += ld_pct / static_cast<double>(iu_threads);
    }

    const auto lo_rel_begin = std::chrono::steady_clock::now();
    p8_release();
    or_run.md_release_ms
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lo_rel_begin).count();

    std::error_code lo_ec;
    or_run.mu_file_bytes = ib_file ? get_data_file_bytes(lo_dir) : 0;
    std::filesystem::remove_all(lo_dir, lo_ec);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static double get_median(std::vector<double> io_values)
{
    std::sort(io_values.begin(), io_values.end());
    return io_values[io_values.size() / 2];
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void run_case(bool ib_file, uint32_t iu_threads)
{
    std::vector<double> lo_rate, lo_release, lo_off_cpu, lo_sys, lo_waits;
    const char         *lp_sink = ib_file ? "file" : "null";

    std::printf("\n");
    for(uint32_t lu_r = 0; lu_r < gu_runs; ++lu_r)
    {
        s_sink_run lo_run;
        ASSERT_TRUE(run_once(ib_file, iu_threads, lo_run));
        // a sink that fails to open falls back to the null sink, so the file must hold the records
        if(ib_file)
        {
            ASSERT_GT(lo_run.mu_file_bytes, static_cast<uint64_t>(gu_total_records) * 8u);
        }
        EXPECT_EQ(0u, lo_run.mu_dropped);

        const double ld_rate = static_cast<double>(gu_total_records) / (lo_run.md_emit_ms * 1e3);
        std::printf("  %s, %u threads, run %u: %.1f M/s, emit %.1f ms, release %.1f ms, writers off CPU %.1f%%, "
                    "user %.1f ms, sys %.1f ms, waits %llu, file %.1f MB\n",
                    lp_sink,
                    iu_threads,
                    lu_r,
                    ld_rate,
                    lo_run.md_emit_ms,
                    lo_run.md_release_ms,
                    lo_run.md_off_cpu_pct,
                    lo_run.md_user_ms,
                    lo_run.md_sys_ms,
                    static_cast<unsigned long long>(lo_run.mu_wait_arrivals),
                    static_cast<double>(lo_run.mu_file_bytes) / 1e6);

        lo_rate.push_back(ld_rate);
        lo_release.push_back(lo_run.md_release_ms);
        lo_off_cpu.push_back(lo_run.md_off_cpu_pct);
        lo_sys.push_back(lo_run.md_sys_ms);
        lo_waits.push_back(static_cast<double>(lo_run.mu_wait_arrivals));
    }

    std::printf("  %s, %u threads, median: %.1f M/s, release %.1f ms, writers off CPU %.1f%%, sys %.1f ms, "
                "waits %.0f\n\n",
                lp_sink,
                iu_threads,
                get_median(lo_rate),
                get_median(lo_release),
                get_median(lo_off_cpu),
                get_median(lo_sys),
                get_median(lo_waits));
}
} // namespace

class c_sink_perf_test : public ::testing::TestWithParam<uint32_t>
{
};

TEST_P(c_sink_perf_test, DISABLED_null)
{
    run_case(false, GetParam());
}

TEST_P(c_sink_perf_test, DISABLED_file)
{
    run_case(true, GetParam());
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts, c_sink_perf_test, ::testing::Values(1u, 4u, 8u));
