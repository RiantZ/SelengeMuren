// Unified P7-vs-P8 comparison: P8 telemetry (mtk) path.
//
// Same methodology as p8_log_perf_compare_test.cpp, for metrics:
//   * null sink, 16MB budget, thread counts {1,2,4,8}, 100 runs;
//   * ONE shared metric that every thread emits to (mirrors P7's single shared
//     counter; P8 writers stay thread-local/lock-free regardless);
//   * P8-style summary stats via report(); per-run dropped-metric count printed.
//
// DISABLED_ by P8 convention; run with --gtest_also_run_disabled_tests.
#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_perf_compare_common.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
static constexpr uint32_t lu_warmup_per_thread = 1'000;

static const uint32_t ga_perf_thread_counts[]  = { 1, 2, 4, 8 };
} // namespace

class c_mtk_perf_compare_test : public ::testing::TestWithParam<uint32_t>
{
protected:
    void TearDown() override
    {
        p8_release(); // defensive: the happy path already releases every run
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
TEST_P(c_mtk_perf_compare_test, DISABLED_null_16mb)
{
    const uint32_t      lu_threads          = GetParam();
    const uint32_t      lu_iters_per_thread = p8_perf_compare::env_u32("P8_PERF_ITERS", 1'000'000);
    const uint32_t      lu_runs             = p8_perf_compare::env_u32("P8_PERF_RUNS", 100);
    const std::string   ls_mem              = p8_perf_compare::env_str("P8_PERF_MEM", "16MB");
    std::vector<double> lo_samples;
    lo_samples.reserve(lu_runs);
    uint64_t lu_drops         = 0;

    const std::string ls_json = std::string("{\"") + P8_CFG_KEY_SINK + "\":\"" + P8_CFG_VAL_SINK_NETWORK_NULL + "\",\""
                                + P8_CFG_KEY_MAX_MEMORY_SIZE + "\":\"" + ls_mem + "\",\""
                                + P8_CFG_KEY_INITIAL_MEMORY_SIZE + "\":\"" + ls_mem + "\"}";

    for(uint32_t lu_run = 0; lu_run < lu_runs; ++lu_run)
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ls_json.c_str();
        ASSERT_TRUE(p8_initialize(&lo_config));

        s_p8_mtk_base lo_base   = {};
        lo_base.mp_name         = "perf.metric";
        lo_base.md_min          = 0.0;
        lo_base.md_max          = 1e9;

        const h_p8_mtk_id lh_id = p8_mtk_create(&lo_base);
        ASSERT_TRUE(P8_IS_METRIC_VALID(lh_id));

        const p8_perf_compare::s_emit_stats lo_stats = p8_perf_compare::emit_from_threads(
            lu_threads,
            lu_iters_per_thread,
            lu_warmup_per_thread,
            [](uint32_t) {}, // shared metric created on the main thread; no per-thread setup
            [lh_id](uint32_t /*iu_t*/, uint32_t iu_i) { p8_mtk_emit(lh_id, static_cast<double>(iu_i)); });

        const s_p8_drop_stats lo_d = p8_test_get_dropped_stats();
        p8_release();

        lo_samples.push_back(lo_stats.md_emit_ns
                             / static_cast<double>(static_cast<uint64_t>(lu_threads) * lu_iters_per_thread));
        lu_drops += lo_d.mu_metrics;
    }

    char la_label[80];
    std::snprintf(la_label, sizeof(la_label), "p8 mtk null %s threads=%u", ls_mem.c_str(), lu_threads);
    p8_perf_compare::report(la_label, lo_samples);
    std::printf("  drops      : %llu\n", static_cast<unsigned long long>(lu_drops));
    std::printf("\n");
}

INSTANTIATE_TEST_SUITE_P(ThreadCounts, c_mtk_perf_compare_test, ::testing::ValuesIn(ga_perf_thread_counts));
