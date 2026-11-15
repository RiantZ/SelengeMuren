#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_log.hpp"
#include "p8_mtk.hpp"
#include "p8_trc.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// writers without the transfer to the core: the test rewinds the writer's buffer, so it never fills or leaves the
// writer; see doc/perf_baseline_design.md
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// the library API functions are not inlined into the caller, so their bench copies are not either
#if defined(_MSC_VER)
    #define P8_BENCH_NOINLINE __declspec(noinline)
#else
    #define P8_BENCH_NOINLINE __attribute__((noinline))
#endif

namespace
{
static constexpr uint32_t gu_rewind_calls = 64; // 64 spans with 3 attributes take 7168 bytes of an 8192-byte buffer
static constexpr uint32_t gu_warmup_iters = 100'000;
static constexpr uint32_t gu_batch_count  = 32;
static constexpr uint32_t gu_batch_iters  = 1'000'000;
static_assert(0 == gu_batch_iters % gu_rewind_calls, "a timed batch is whole rewind chunks");

// what one run on the bench thread measured
struct s_bench_result
{
    std::vector<double> mo_samples_ns;     // ns per call, one entry per batch
    uint32_t            mu_bufs_taken = 0; // buffers the writer took while timed, 0 in a clean run
    uint64_t            mu_dropped    = 0; // records the writer dropped, 0 in a clean run
};

// a writer the test rewinds; it reaches the protected state of cp8_tls_writer
template <typename t_writer> class c_bench_writer : public t_writer
{
public:
    // the next record overwrites the buffer from its start
    void rewind_buffer()
    {
        std::lock_guard<kit::c_spin_lock> lo_guard(*this->mp_lock);
        if(this->mp_buffer)
        {
            this->mz_buf_used = sizeof(s_p8_data_buf_hdr);
        }
    }

    uint32_t get_buf_seq() const
    {
        return this->mu_buf_seq;
    }

    uint64_t get_dropped() const
    {
        return this->mu_dropped_logs.load(std::memory_order_relaxed)
               + this->mu_dropped_metrics.load(std::memory_order_relaxed)
               + this->mu_dropped_traces.load(std::memory_order_relaxed);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// one writer per thread, reached through thread_local as get_tls_log() and its siblings do in the engine
template <typename t_writer> static c_bench_writer<t_writer> &get_bench_writer()
{
    thread_local c_bench_writer<t_writer> lo_writer;
    return lo_writer;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// p8_log_sent on the bench writer
static P8_BENCH_NOINLINE bool bench_log_sent(enum e_p8_level             ie_level,
                                             p_p8_module                 ip_module,
                                             uint64_t                    iu_trace_id,
                                             uint32_t                    iu_line,
                                             const char                 *ip_file,
                                             const char                 *ip_function,
                                             size_t                      iz_attrs,
                                             const struct s_p8_attr_val *ip_attrs,
                                             const char                 *ip_format,
                                             ...)
{
    va_list lo_args;
    va_start(lo_args, ip_format);
    bool lb_result = get_bench_writer<cp8_log>().send(ie_level,
                                                      ip_module,
                                                      iu_trace_id,
                                                      iu_line,
                                                      ip_file,
                                                      ip_function,
                                                      iz_attrs,
                                                      ip_attrs,
                                                      ip_format,
                                                      lo_args);
    va_end(lo_args);
    return lb_result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// p8_trc_begin on the bench writer
static P8_BENCH_NOINLINE uint64_t bench_trc_begin(const char                 *ip_name,
                                                  uint32_t                    iu_line,
                                                  const char                 *ip_file,
                                                  const char                 *ip_function,
                                                  size_t                      iz_attrs,
                                                  const struct s_p8_attr_val *ip_attrs)
{
    return get_bench_writer<cp8_trc>().begin(ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// p8_trc_end on the bench writer; the library inlines cp8_trc::end into p8_trc_end, the test cannot
static P8_BENCH_NOINLINE void bench_trc_end(uint64_t iu_span_id)
{
    if(P8_INVALID_SPAN != iu_span_id) [[likely]]
    {
        get_bench_writer<cp8_trc>().end(iu_span_id);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// p8_mtk_emit on the bench writer
static P8_BENCH_NOINLINE bool bench_mtk_emit(h_p8_mtk_id ih_id, double id_value)
{
    return get_bench_writer<cp8_mtk>().emit(ih_id, id_value);
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
    std::printf("  batches    : %zu x %u iters (warmup %u)\n", lz_n, gu_batch_iters, gu_warmup_iters);
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
// a clean run took no buffer while timed and dropped nothing
static void report_clean(const s_bench_result &ir_result)
{
    std::printf("  --- writer ---\n");
    std::printf("  buffers    : %u taken while timed\n", ir_result.mu_bufs_taken);
    std::printf("  drops      : %llu\n", static_cast<unsigned long long>(ir_result.mu_dropped));
    std::printf("\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// iu_calls calls with the buffer rewound after every gu_rewind_calls of them, so the writer needs no other buffer
template <typename t_writer> static void run_rewound(const std::function<void(uint32_t)> &ir_call, uint32_t iu_calls)
{
    for(uint32_t lu_first = 0; lu_first < iu_calls; lu_first += gu_rewind_calls)
    {
        const uint32_t lu_end = std::min(lu_first + gu_rewind_calls, iu_calls);
        for(uint32_t lu_i = lu_first; lu_i < lu_end; ++lu_i)
        {
            ir_call(lu_i);
        }
        get_bench_writer<t_writer>().rewind_buffer();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// on a new thread, as in the other perf tests; the first warm-up call makes the writer and takes its only buffer
template <typename t_writer>
static void run_batches(const std::function<void()>         &ir_setup,
                        const std::function<void(uint32_t)> &ir_call,
                        s_bench_result                      &or_result)
{
    std::thread lo_thread(
        [&]()
        {
            ir_setup();
            run_rewound<t_writer>(ir_call, gu_warmup_iters);

            or_result.mo_samples_ns.clear();
            or_result.mo_samples_ns.reserve(gu_batch_count);
            const uint32_t lu_buf_seq = get_bench_writer<t_writer>().get_buf_seq();

            for(uint32_t lu_b = 0; lu_b < gu_batch_count; ++lu_b)
            {
                const auto lo_start = std::chrono::steady_clock::now();
                run_rewound<t_writer>(ir_call, gu_batch_iters);
                const auto lo_end = std::chrono::steady_clock::now();
                const auto lo_dt  = std::chrono::duration_cast<std::chrono::nanoseconds>(lo_end - lo_start).count();
                or_result.mo_samples_ns.push_back(static_cast<double>(lo_dt) / static_cast<double>(gu_batch_iters));
            }

            or_result.mu_bufs_taken = get_bench_writer<t_writer>().get_buf_seq() - lu_buf_seq;
            or_result.mu_dropped    = get_bench_writer<t_writer>().get_dropped();
        });
    lo_thread.join();
}
} // namespace

// capture mode keeps the worker away from the writers, so nothing collects the bench writer's buffer
class c_perf_baseline_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{"
                                       "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"4MB\","
                                       "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"4MB\""
                                       "}";

        ASSERT_TRUE(p8_initialize(&lo_config));
        p8_test_enable_buffer_capture();
    }

    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_release();
    }
};

using c_log_perf_baseline_test = c_perf_baseline_test;

TEST_F(c_log_perf_baseline_test, DISABLED_send_hello_d_no_attrs)
{
    s_bench_result lo_result;
    run_batches<cp8_log>([]() {},
                         [](uint32_t iu_i)
                         {
                             bench_log_sent(e_p8_trace0,
                                            nullptr,
                                            P8_INVALID_SPAN,
                                            static_cast<uint32_t>(__LINE__),
                                            __FILE__,
                                            __FUNCTION__,
                                            0,
                                            nullptr,
                                            "hello %d",
                                            static_cast<int>(iu_i));
                         },
                         lo_result);

    report("send_hello_d_no_attrs", lo_result.mo_samples_ns);
    report_clean(lo_result);
    EXPECT_EQ(0u, lo_result.mu_bufs_taken);
    EXPECT_EQ(0u, lo_result.mu_dropped);
}

TEST_F(c_log_perf_baseline_test, DISABLED_send_hello_d_3_attrs)
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

    s_bench_result lo_result;
    run_batches<cp8_log>([]() {},
                         [&la_attrs](uint32_t iu_i)
                         {
                             la_attrs[1].mi_i64 = static_cast<int64_t>(iu_i);
                             bench_log_sent(e_p8_trace0,
                                            nullptr,
                                            P8_INVALID_SPAN,
                                            static_cast<uint32_t>(__LINE__),
                                            __FILE__,
                                            __FUNCTION__,
                                            3,
                                            la_attrs,
                                            "hello %d",
                                            static_cast<int>(iu_i));
                         },
                         lo_result);

    report("send_hello_d_3_attrs", lo_result.mo_samples_ns);
    report_clean(lo_result);
    EXPECT_EQ(0u, lo_result.mu_bufs_taken);
    EXPECT_EQ(0u, lo_result.mu_dropped);
}

using c_trc_perf_baseline_test = c_perf_baseline_test;

// one call is one span: begin + end
TEST_F(c_trc_perf_baseline_test, DISABLED_begin_end_no_attrs)
{
    s_bench_result lo_result;
    run_batches<cp8_trc>(
        []() {},
        [](uint32_t)
        {
            bench_trc_end(
                bench_trc_begin("perf_span", static_cast<uint32_t>(__LINE__), __FILE__, __FUNCTION__, 0, nullptr));
        },
        lo_result);

    report("begin_end_no_attrs", lo_result.mo_samples_ns);
    report_clean(lo_result);
    EXPECT_EQ(0u, lo_result.mu_bufs_taken);
    EXPECT_EQ(0u, lo_result.mu_dropped);
}

TEST_F(c_trc_perf_baseline_test, DISABLED_begin_end_3_attrs)
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

    s_bench_result lo_result;
    run_batches<cp8_trc>(
        []() {},
        [&la_attrs](uint32_t iu_i)
        {
            la_attrs[1].mi_i64 = static_cast<int64_t>(iu_i);
            bench_trc_end(
                bench_trc_begin("perf_span", static_cast<uint32_t>(__LINE__), __FILE__, __FUNCTION__, 3, la_attrs));
        },
        lo_result);

    report("begin_end_3_attrs", lo_result.mo_samples_ns);
    report_clean(lo_result);
    EXPECT_EQ(0u, lo_result.mu_bufs_taken);
    EXPECT_EQ(0u, lo_result.mu_dropped);
}

using c_mtk_perf_baseline_test = c_perf_baseline_test;

TEST_F(c_mtk_perf_baseline_test, DISABLED_emit_single_thread)
{
    s_p8_mtk_base lo_base = {};
    lo_base.mp_name       = "perf.metric";
    lo_base.md_min        = 0.0;
    lo_base.md_max        = 1e9;

    h_p8_mtk_id    li_id  = P8_MTK_ERROR_NOT_INITIALIZED;
    s_bench_result lo_result;
    // made on the bench thread, so the main thread gets no writer bound to this test's core
    run_batches<cp8_mtk>([&]() { li_id = get_bench_writer<cp8_mtk>().create(&lo_base); },
                         [&li_id](uint32_t iu_i) { bench_mtk_emit(li_id, static_cast<double>(iu_i)); },
                         lo_result);

    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));
    report("emit_single_thread", lo_result.mo_samples_ns);
    report_clean(lo_result);
    EXPECT_EQ(0u, lo_result.mu_bufs_taken);
    EXPECT_EQ(0u, lo_result.mu_dropped);
}
