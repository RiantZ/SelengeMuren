#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Build a memory config sized in whole buffers so the fixture stays correct
// regardless of the compile-time buffer size.
std::string make_mem_config(size_t iz_buffers)
{
    const size_t      lz_bytes = iz_buffers * p8_test_get_buffer_size();
    const std::string ls       = std::to_string(lz_bytes);
    return std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \"" + ls + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE
           + "\": \"" + ls + "\"}";
}

// create a metric and emit the given values, all on a dedicated thread so each
// test gets a fresh TLS writer whose buffers are flushed on thread exit (mirrors
// run_send_in_thread in the log suite).
struct s_mtk_emit_ctx
{
    h_p8_mtk_id mi_id = -1;
    bool        mb_ok = false;
};

s_mtk_emit_ctx run_create_emit_in_thread(const s_p8_mtk_base *ip_base, const std::vector<double> &ir_values)
{
    s_mtk_emit_ctx lo_ctx;
    std::thread    lo_thread(
        [&]()
        {
            lo_ctx.mi_id = p8_mtk_create(ip_base);
            if(!P8_IS_METRIC_VALID(lo_ctx.mi_id))
            {
                return;
            }
            lo_ctx.mb_ok = true;
            for(double ld_val : ir_values)
            {
                lo_ctx.mb_ok = p8_mtk_emit(lo_ctx.mi_id, ld_val) && lo_ctx.mb_ok;
            }
        });
    lo_thread.join();
    return lo_ctx;
}

struct s_mtk_parsed
{
    s_p8_data_buf_hdr                 mo_buf_hdr = {};
    std::vector<s_p8_mtk_item_dat>    mo_items;
    std::vector<std::vector<uint8_t>> mo_all_captured;
};

// Force a synchronous drain, then walk every captured P8_PACKET_METRICS buffer,
// extracting each fixed-size s_p8_mtk_item_dat in wire order.
s_mtk_parsed parse_captured_metrics()
{
    s_mtk_parsed lo_result;

    p8_test_drain_writers();

    const auto &lo_bufs = p8_test_get_captured_buffers();
    if(lo_bufs.empty())
    {
        return lo_result;
    }
    lo_result.mo_all_captured.assign(lo_bufs.begin(), lo_bufs.end());

    if(lo_bufs[0].size() >= sizeof(s_p8_data_buf_hdr))
    {
        memcpy(&lo_result.mo_buf_hdr, lo_bufs[0].data(), sizeof(s_p8_data_buf_hdr));
    }

    for(const auto &lr_buf : lo_bufs)
    {
        if(lr_buf.size() < sizeof(s_p8_data_buf_hdr))
        {
            continue;
        }
        s_p8_data_buf_hdr lo_hdr = {};
        memcpy(&lo_hdr, lr_buf.data(), sizeof(lo_hdr));
        if(lo_hdr.mu_packet_type != P8_PACKET_METRICS)
        {
            continue;
        }

        size_t lz_used = lo_hdr.mu_size;
        if(lz_used > lr_buf.size())
        {
            lz_used = lr_buf.size();
        }

        size_t lz_off = sizeof(s_p8_data_buf_hdr);
        while(lz_off + sizeof(s_p8_mtk_item_dat) <= lz_used)
        {
            s_p8_mtk_item_dat lo_item = {};
            memcpy(&lo_item, lr_buf.data() + lz_off, sizeof(lo_item));
            if(lo_item.mu_size < sizeof(s_p8_mtk_item_dat))
            {
                break;
            }
            lo_result.mo_items.push_back(lo_item);
            lz_off += lo_item.mu_size;
        }
    }

    return lo_result;
}
} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// test fixture
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class c_mtk_content_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const std::string  ls_config = make_mem_config(/*buffers*/ 16);
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ls_config.c_str();
        ASSERT_TRUE(p8_initialize(&lo_config));
        p8_test_enable_buffer_capture();
    }

    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_test_clear_captured_buffers();
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// buffer header
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_content_test, buf_hdr_packet_type)
{
    s_p8_mtk_base lo_base = {};
    lo_base.mp_name       = "cpu.load";

    auto lo_ctx           = run_create_emit_in_thread(&lo_base, { 42.0 });
    ASSERT_TRUE(P8_IS_METRIC_VALID(lo_ctx.mi_id));
    ASSERT_TRUE(lo_ctx.mb_ok);

    auto lo_parsed = parse_captured_metrics();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());
    EXPECT_EQ(lo_parsed.mo_buf_hdr.mu_packet_type, P8_PACKET_METRICS);
    EXPECT_EQ(lo_parsed.mo_buf_hdr.mu_flags, 0u);
    EXPECT_NE(lo_parsed.mo_buf_hdr.mu_start_time, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// sample content
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_content_test, single_sample_fields)
{
    s_p8_mtk_base lo_base = {};
    lo_base.mp_name       = "temp";

    auto lo_ctx           = run_create_emit_in_thread(&lo_base, { 3.14 });
    ASSERT_TRUE(lo_ctx.mb_ok);

    auto lo_parsed = parse_captured_metrics();
    ASSERT_EQ(lo_parsed.mo_items.size(), 1u);
    EXPECT_EQ(lo_parsed.mo_items[0].mi_id, lo_ctx.mi_id);
    EXPECT_DOUBLE_EQ(lo_parsed.mo_items[0].md_value, 3.14);
    EXPECT_EQ(lo_parsed.mo_items[0].mu_size, static_cast<uint16_t>(sizeof(s_p8_mtk_item_dat)));
    EXPECT_EQ(lo_parsed.mo_items[0].mu_attrs_count, 0u);
    EXPECT_NE(lo_parsed.mo_items[0].mu_timestamp, 0u);
}

TEST_F(c_mtk_content_test, multiple_samples_preserve_order_and_values)
{
    s_p8_mtk_base lo_base         = {};
    lo_base.mp_name               = "counter";

    std::vector<double> lo_values = { 1.0, 2.0, 3.0, 4.0, 5.0 };
    auto                lo_ctx    = run_create_emit_in_thread(&lo_base, lo_values);
    ASSERT_TRUE(lo_ctx.mb_ok);

    auto lo_parsed = parse_captured_metrics();
    ASSERT_EQ(lo_parsed.mo_items.size(), lo_values.size());
    for(size_t lz_i = 0; lz_i < lo_values.size(); ++lz_i)
    {
        EXPECT_EQ(lo_parsed.mo_items[lz_i].mi_id, lo_ctx.mi_id);
        EXPECT_DOUBLE_EQ(lo_parsed.mo_items[lz_i].md_value, lo_values[lz_i]);
    }
}

TEST_F(c_mtk_content_test, timestamps_are_monotonic)
{
    s_p8_mtk_base lo_base = {};
    lo_base.mp_name       = "ts";

    auto lo_ctx           = run_create_emit_in_thread(&lo_base, { 1.0, 2.0, 3.0 });
    ASSERT_TRUE(lo_ctx.mb_ok);

    auto lo_parsed = parse_captured_metrics();
    ASSERT_EQ(lo_parsed.mo_items.size(), 3u);
    for(size_t lz_i = 1; lz_i < lo_parsed.mo_items.size(); ++lz_i)
    {
        EXPECT_GE(lo_parsed.mo_items[lz_i].mu_timestamp, lo_parsed.mo_items[lz_i - 1].mu_timestamp);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// invalid id
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_content_test, emit_invalid_id_writes_nothing)
{
    bool        lb_ok = true;
    std::thread lo_thread([&]() { lb_ok = p8_mtk_emit(9999, 1.0); });
    lo_thread.join();
    EXPECT_FALSE(lb_ok);

    auto lo_parsed = parse_captured_metrics();
    EXPECT_TRUE(lo_parsed.mo_items.empty());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multi-buffer
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_content_test, samples_span_multiple_buffers)
{
    s_p8_mtk_base lo_base   = {};
    lo_base.mp_name         = "many";

    const size_t lz_per_buf = (p8_test_get_buffer_size() - sizeof(s_p8_data_buf_hdr)) / sizeof(s_p8_mtk_item_dat);
    const size_t lz_count   = lz_per_buf * 2 + 5;

    std::vector<double> lo_values(lz_count);
    for(size_t lz_i = 0; lz_i < lz_count; ++lz_i)
    {
        lo_values[lz_i] = static_cast<double>(lz_i);
    }

    auto lo_ctx = run_create_emit_in_thread(&lo_base, lo_values);
    ASSERT_TRUE(lo_ctx.mb_ok);

    auto lo_parsed = parse_captured_metrics();
    ASSERT_EQ(lo_parsed.mo_items.size(), lz_count);
    EXPECT_GT(lo_parsed.mo_all_captured.size(), 1u);
    for(size_t lz_i = 0; lz_i < lz_count; ++lz_i)
    {
        EXPECT_DOUBLE_EQ(lo_parsed.mo_items[lz_i].md_value, static_cast<double>(lz_i));
    }
}
