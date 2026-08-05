// Shared harness + statistics for the unified P7-vs-P8 comparison perf tests.
//
// percentile()/report() reproduce the exact math/layout of the P8 perf tests
// (p8_log_perf_test.cpp:27-82). emit_from_threads() reproduces the P8 latch
// harness (p8_log_perf_test.cpp:219-284) with an added untimed per-thread
// setup hook (a no-op for P8; used by the P7 bench to Register_Thread). The
// exact same code is mirrored in bench/stats.hpp + bench/harness.hpp so both
// libraries are measured and summarised identically.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace p8_perf_compare
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads an unsigned override from the environment (used by the comparison
// driver to set runs/iters without recompiling); returns iu_default if unset.
static uint32_t env_u32(const char *ip_name, uint32_t iu_default)
{
    const char *lp_val = std::getenv(ip_name);
    if(!lp_val || !*lp_val)
    {
        return iu_default;
    }
    const unsigned long lu_v = std::strtoul(lp_val, nullptr, 10);
    return (lu_v > 0) ? static_cast<uint32_t>(lu_v) : iu_default;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Reads a string override from the environment (e.g. the memory budget
// "16MB"/"32MB"); returns ip_default if unset.
static std::string env_str(const char *ip_name, const char *ip_default)
{
    const char *lp_val = std::getenv(ip_name);
    return (lp_val && *lp_val) ? std::string(lp_val) : std::string(ip_default);
}

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
    std::printf("  runs       : %zu\n", lz_n);
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
// Results collected from a single emit run.
struct s_emit_stats
{
    double              md_emit_ns = 0.0; // wall time of the whole emit phase (warmup excluded)
    std::vector<double> mo_thread_ns;     // per-thread emit-loop durations, one entry per thread
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// i_setup(thread_index)      : untimed, once per thread, before warmup.
// i_emit(thread_index, iter) : the measured operation; called in warmup and timed loops.
template <typename t_setup, typename t_emit>
s_emit_stats emit_from_threads(uint32_t iu_thread_count,
                               uint32_t iu_iters_per_thread,
                               uint32_t iu_warmup_per_thread,
                               t_setup  i_setup,
                               t_emit   i_emit)
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
                i_setup(lu_t);

                for(uint32_t lu_w = 0; lu_w < iu_warmup_per_thread; ++lu_w)
                {
                    i_emit(lu_t, lu_w);
                }

                lo_start_latch.arrive_and_wait();

                const auto lo_thread_start = std::chrono::steady_clock::now();
                for(uint32_t lu_i = 0; lu_i < iu_iters_per_thread; ++lu_i)
                {
                    i_emit(lu_t, lu_i);
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
} // namespace p8_perf_compare
