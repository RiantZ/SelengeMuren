#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_hash.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// helpers: reassemble captured buffers into a linear payload
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Each log item is padded so its total size (mu_size) lands on an 8-byte
// boundary; mirror that rounding when computing expected wire sizes.
static uint32_t expected_item_size(size_t iz_unpadded)
{
    return static_cast<uint16_t>((iz_unpadded + 7u) & ~static_cast<size_t>(7u));
}

struct s_log_content_parsed
{
    s_p8_data_buf_hdr    mo_buf_hdr;
    s_p8_log_item_dat    mo_item_hdr;
    std::vector<uint8_t> mo_args_payload;
    std::vector<uint8_t> mo_attrs_payload;

    std::vector<std::vector<uint8_t>> mo_all_captured;
};

static s_log_content_parsed parse_captured_buffers()
{
    s_log_content_parsed lo_result = {};

    // Writers now retain buffers locally; force a synchronous drain so the
    // captured set is deterministic instead of racing the worker thread.
    p8_test_drain_writers();

    const auto &lo_bufs = p8_test_get_captured_buffers();
    if(lo_bufs.empty())
    {
        return lo_result;
    }

    lo_result.mo_all_captured.assign(lo_bufs.begin(), lo_bufs.end());

    const auto &lo_first = lo_bufs[0];
    if(lo_first.size() >= sizeof(s_p8_data_buf_hdr))
    {
        memcpy(&lo_result.mo_buf_hdr, lo_first.data(), sizeof(s_p8_data_buf_hdr));
    }

    size_t lz_item_off = sizeof(s_p8_data_buf_hdr);
    if(lo_first.size() >= lz_item_off + sizeof(s_p8_log_item_dat))
    {
        memcpy(&lo_result.mo_item_hdr, lo_first.data() + lz_item_off, sizeof(s_p8_log_item_dat));
    }

    size_t lz_payload_start = lz_item_off + sizeof(s_p8_log_item_dat);
    size_t lz_first_used    = lo_result.mo_buf_hdr.mu_size;
    if(lz_first_used > lo_first.size())
    {
        lz_first_used = lo_first.size();
    }
    if(lz_first_used > lz_payload_start)
    {
        lo_result.mo_args_payload.insert(lo_result.mo_args_payload.end(),
                                         lo_first.data() + lz_payload_start,
                                         lo_first.data() + lz_first_used);
    }

    for(size_t lz_i = 1; lz_i < lo_bufs.size(); ++lz_i)
    {
        const auto       &lo_buf = lo_bufs[lz_i];
        s_p8_data_buf_hdr lo_hdr = {};
        if(lo_buf.size() >= sizeof(s_p8_data_buf_hdr))
        {
            memcpy(&lo_hdr, lo_buf.data(), sizeof(s_p8_data_buf_hdr));
        }

        size_t lz_data_start = sizeof(s_p8_data_buf_hdr);
        size_t lz_used       = lo_hdr.mu_size;
        if(lz_used > lo_buf.size())
        {
            lz_used = lo_buf.size();
        }
        if(lz_used > lz_data_start)
        {
            lo_result.mo_args_payload.insert(lo_result.mo_args_payload.end(),
                                             lo_buf.data() + lz_data_start,
                                             lo_buf.data() + lz_used);
        }
    }

    return lo_result;
}

template <typename t_val> static t_val read_val(const std::vector<uint8_t> &io_payload, size_t &ioz_offset)
{
    t_val lo_val = {};
    if(ioz_offset + sizeof(t_val) <= io_payload.size())
    {
        memcpy(&lo_val, io_payload.data() + ioz_offset, sizeof(t_val));
    }
    ioz_offset += sizeof(t_val);
    return lo_val;
}

static std::string read_string(const std::vector<uint8_t> &io_payload, size_t &ioz_offset)
{
    uint16_t    lu_len = read_val<uint16_t>(io_payload, ioz_offset);
    std::string lo_str;
    if(lu_len > 0 && ioz_offset + lu_len <= io_payload.size())
    {
        lo_str.assign(reinterpret_cast<const char *>(io_payload.data() + ioz_offset), lu_len);
    }
    ioz_offset += lu_len;
    return lo_str;
}

static uint16_t read_string_len(const std::vector<uint8_t> &io_payload, size_t &ioz_offset)
{
    return read_val<uint16_t>(io_payload, ioz_offset);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multi-item parser: builds linear stream from captured buffers, extracts all log items
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct s_parsed_item
{
    s_p8_log_item_dat    mo_hdr;
    std::vector<uint8_t> mo_payload;
};

static std::vector<s_parsed_item> parse_all_items()
{
    std::vector<s_parsed_item> lo_items;

    // Writers now retain buffers locally; force a synchronous drain so the
    // captured set is deterministic instead of racing the worker thread.
    p8_test_drain_writers();

    const auto &lo_bufs = p8_test_get_captured_buffers();
    if(lo_bufs.empty())
    {
        return lo_items;
    }

    std::vector<uint8_t> lo_stream;
    for(const auto &lo_buf : lo_bufs)
    {
        s_p8_data_buf_hdr lo_hdr = {};
        if(lo_buf.size() >= sizeof(s_p8_data_buf_hdr))
        {
            memcpy(&lo_hdr, lo_buf.data(), sizeof(s_p8_data_buf_hdr));
        }

        size_t lz_used = lo_hdr.mu_size;
        if(lz_used > lo_buf.size())
        {
            lz_used = lo_buf.size();
        }
        size_t lz_start = sizeof(s_p8_data_buf_hdr);
        if(lz_used > lz_start)
        {
            lo_stream.insert(lo_stream.end(), lo_buf.data() + lz_start, lo_buf.data() + lz_used);
        }
    }

    size_t lz_pos = 0;
    while(lz_pos + sizeof(s_p8_log_item_dat) <= lo_stream.size())
    {
        s_parsed_item lo_item = {};
        memcpy(&lo_item.mo_hdr, lo_stream.data() + lz_pos, sizeof(s_p8_log_item_dat));

        if(lo_item.mo_hdr.mu_size < sizeof(s_p8_log_item_dat))
        {
            break;
        }

        size_t lz_payload_bytes = lo_item.mo_hdr.mu_size - sizeof(s_p8_log_item_dat);
        size_t lz_payload_start = lz_pos + sizeof(s_p8_log_item_dat);
        size_t lz_payload_end   = lz_payload_start + lz_payload_bytes;
        if(lz_payload_end > lo_stream.size())
        {
            lz_payload_end = lo_stream.size();
        }

        if(lz_payload_end > lz_payload_start)
        {
            lo_item.mo_payload.assign(lo_stream.data() + lz_payload_start, lo_stream.data() + lz_payload_end);
        }

        lo_items.push_back(std::move(lo_item));
        lz_pos += lo_item.mo_hdr.mu_size;
    }

    return lo_items;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// helper: run p8_log_sent in a separate thread so TLS destructor releases the buffer
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct s_send_ctx
{
    bool        mb_result    = false;
    uint32_t    mu_line      = 0;
    const char *mp_file      = nullptr;
    uint32_t    mu_thread_id = 0;
};

static s_send_ctx run_send_in_thread(std::function<bool()> il_fn, uint32_t iu_line, const char *ip_file)
{
    s_send_ctx lo_ctx;
    lo_ctx.mu_line = iu_line;
    lo_ctx.mp_file = ip_file;

    std::thread lo_thread(
        [&lo_ctx, &il_fn]()
        {
            lo_ctx.mu_thread_id = static_cast<uint32_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
            lo_ctx.mb_result    = il_fn();
        });
    lo_thread.join();

    return lo_ctx;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// test fixture
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class c_log_content_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{"
                                       "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"128KB\","
                                       "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"128KB\""
                                       "}";
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
// Group 1: s_p8_data_buf_hdr correctness
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, buf_hdr_packet_type)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());
    EXPECT_EQ(lo_parsed.mo_buf_hdr.mu_packet_type, P8_PACKET_LOGS);
}

TEST_F(c_log_content_test, buf_hdr_no_fragment_on_first)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());
    EXPECT_EQ(lo_parsed.mo_buf_hdr.mu_flags & P8_DATA_FLAG_FRAGMENT, 0u);
}

TEST_F(c_log_content_test, buf_hdr_size_valid)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());

    size_t lz_min = sizeof(s_p8_data_buf_hdr) + sizeof(s_p8_log_item_dat) + P8_SIZE_OF_ARG(uint32_t);
    EXPECT_GE(lo_parsed.mo_buf_hdr.mu_size, static_cast<uint16_t>(lz_min));
    EXPECT_LE(lo_parsed.mo_buf_hdr.mu_size, static_cast<uint16_t>(p8_test_get_buffer_size()));
}

TEST_F(c_log_content_test, buf_hdr_thread_id_nonzero)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());
    EXPECT_NE(lo_parsed.mo_buf_hdr.mu_thread_id, 0u);
}

TEST_F(c_log_content_test, buf_hdr_timestamps_valid)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_FALSE(lo_parsed.mo_all_captured.empty());
    EXPECT_NE(lo_parsed.mo_buf_hdr.mu_start_time, 0u);
    EXPECT_NE(lo_parsed.mo_buf_hdr.mu_stop_time, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 2: s_p8_log_item_hdr correctness
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, item_hdr_hash)
{
    const uint32_t lu_line   = __LINE__ + 4;
    const char    *lp_file   = __FILE__;

    uint32_t    lu_line_copy = lu_line;
    const char *lp_file_copy = lp_file;

    auto lo_ctx              = run_send_in_thread(
        [lu_line_copy, lp_file_copy]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               lu_line_copy,
                               lp_file_copy,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        lu_line,
        lp_file);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto     lo_parsed   = parse_captured_buffers();
    uint64_t lu_expected = P8_GET_LOG_HASH(lp_file, lu_line);
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_hash, lu_expected);
}

TEST_F(c_log_content_test, item_hdr_trace_id)
{
    const uint64_t lu_trace = 0xDEADBEEFCAFEBABEULL;

    uint64_t lu_trace_copy  = lu_trace;

    auto lo_ctx             = run_send_in_thread(
        [lu_trace_copy]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               lu_trace_copy,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_trace_id, lu_trace);
}

TEST_F(c_log_content_test, item_hdr_level)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_info0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_level, static_cast<uint8_t>(e_p8_info0));
}

TEST_F(c_log_content_test, item_hdr_thread_id)
{
    s_send_ctx lo_ctx;

    auto lo_fn = [&lo_ctx]()
    {
        lo_ctx.mu_thread_id = static_cast<uint32_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
        return p8_log_sent(e_p8_trace0,
                           nullptr,
                           0,
                           static_cast<uint32_t>(__LINE__),
                           __FILE__,
                           __FUNCTION__,
                           0,
                           nullptr,
                           "%d",
                           1);
    };

    bool        lb_result = false;
    std::thread lo_thread([&lb_result, &lo_fn]() { lb_result = lo_fn(); });
    lo_thread.join();
    ASSERT_TRUE(lb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_thread_id, lo_ctx.mu_thread_id);
}

TEST_F(c_log_content_test, item_hdr_args_size_no_args)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "hello");
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_args_size, 0u);
}

TEST_F(c_log_content_test, item_hdr_args_size_single_int)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_args_size, P8_SIZE_OF_ARG(uint64_t));
}

TEST_F(c_log_content_test, item_hdr_total_size_no_attrs)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed  = parse_captured_buffers();
    size_t lz_size    = sizeof(s_p8_log_item_dat) + lo_parsed.mo_item_hdr.mu_args_size;
    lz_size          += 8u - lz_size & 7u;

    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_size,
              expected_item_size(sizeof(s_p8_log_item_dat) + lo_parsed.mo_item_hdr.mu_args_size));
}

TEST_F(c_log_content_test, item_hdr_attrs_count_zero)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 3: argument payload — single buffer (no fragmentation)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, payload_int32)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d",
                               42);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint64_t lu_val  = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_val, 42u);
}

TEST_F(c_log_content_test, payload_int64)
{
    const int64_t li_expected = 0x0102030405060708LL;

    int64_t li_expected_copy  = li_expected;

    auto lo_ctx               = run_send_in_thread(
        [li_expected_copy]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%lld",
                               static_cast<long long>(li_expected_copy));
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint64_t lu_val  = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_val, static_cast<uint64_t>(li_expected));
}

TEST_F(c_log_content_test, payload_double)
{
    const double ld_expected = 3.14;

    auto lo_ctx              = run_send_in_thread(
        [ld_expected]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%f",
                               ld_expected);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    double ld_val    = read_val<double>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(memcmp(&ld_val, &ld_expected, sizeof(double)), 0);
}

TEST_F(c_log_content_test, payload_string_short)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               "hello");
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto        lo_parsed = parse_captured_buffers();
    size_t      lz_off    = 0;
    std::string lo_str    = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_str, "hello");
}

TEST_F(c_log_content_test, payload_string_empty)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               "");
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint16_t lu_len  = read_string_len(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_len, 0u);
}

TEST_F(c_log_content_test, payload_string_null)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               static_cast<const char *>(nullptr));
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint16_t lu_len  = read_string_len(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_len, 0u);
}

TEST_F(c_log_content_test, payload_multi_args)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d %s %lld",
                               42,
                               "test",
                               static_cast<long long>(99LL));
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint64_t lu_int1 = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_int1, 42u);

    std::string lo_str = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_str, "test");

    uint64_t lu_int2 = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_int2, 99u);
}

TEST_F(c_log_content_test, payload_pointer)
{
    const void *lp_addr = reinterpret_cast<const void *>(static_cast<uintptr_t>(0xCAFEBABE));

    auto lo_ctx         = run_send_in_thread(
        [lp_addr]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%p",
                               lp_addr);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint64_t lu_val  = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_val, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lp_addr)));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 4: fragmented argument payload
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, fragment_flag_on_continuation)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();

    std::string lo_large(lz_buf_sz * 2, 'A');

    auto lo_ctx = run_send_in_thread(
        [&lo_large]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               lo_large.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    ASSERT_GE(lo_parsed.mo_all_captured.size(), 2u);

    size_t lz_last = lo_parsed.mo_all_captured.size() - 1;

    for(size_t lz_i = 0; lz_i < lz_last; ++lz_i)
    {
        s_p8_data_buf_hdr lo_hdr = {};
        memcpy(&lo_hdr, lo_parsed.mo_all_captured[lz_i].data(), sizeof(s_p8_data_buf_hdr));
        EXPECT_NE(lo_hdr.mu_flags & P8_DATA_FLAG_FRAGMENT, 0u) << "buffer " << lz_i << " missing FRAGMENT flag";
    }

    s_p8_data_buf_hdr lo_last_hdr = {};
    memcpy(&lo_last_hdr, lo_parsed.mo_all_captured[lz_last].data(), sizeof(s_p8_data_buf_hdr));
    EXPECT_EQ(lo_last_hdr.mu_flags & P8_DATA_FLAG_FRAGMENT, 0u) << "last buffer should NOT have FRAGMENT";
}

TEST_F(c_log_content_test, fragment_string_content)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();

    std::string lo_pattern;
    lo_pattern.reserve(lz_buf_sz * 2);
    for(size_t lz_i = 0; lz_i < lz_buf_sz * 2; ++lz_i)
    {
        lo_pattern.push_back(static_cast<char>('A' + (lz_i % 26)));
    }

    auto lo_ctx = run_send_in_thread(
        [&lo_pattern]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               lo_pattern.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed      = parse_captured_buffers();
    size_t lz_off         = 0;

    std::string lo_result = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_result.size(), lo_pattern.size());
    EXPECT_EQ(lo_result, lo_pattern);
}

TEST_F(c_log_content_test, fragment_args_size_spans_buffers)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();

    std::string lo_large(lz_buf_sz * 2, 'X');

    auto lo_ctx = run_send_in_thread(
        [&lo_large]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               lo_large.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed       = parse_captured_buffers();

    uint16_t lu_expected = static_cast<uint16_t>(sizeof(uint16_t) + lo_large.size());
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_args_size, lu_expected);
}

TEST_F(c_log_content_test, fragment_multi_string)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();

    std::string lo_s1(lz_buf_sz / 2, 'X');
    std::string lo_s2(lz_buf_sz / 2, 'Y');

    auto lo_ctx = run_send_in_thread(
        [&lo_s1, &lo_s2]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s %s",
                               lo_s1.c_str(),
                               lo_s2.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed  = parse_captured_buffers();
    size_t lz_off     = 0;

    std::string lo_r1 = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_r1, lo_s1);

    std::string lo_r2 = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_r2, lo_s2);
}

TEST_F(c_log_content_test, fragment_fixed_then_string)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();

    std::string lo_large(lz_buf_sz * 2, 'C');

    auto lo_ctx = run_send_in_thread(
        [&lo_large]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%d %s",
                               777,
                               lo_large.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint64_t lu_int  = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_int, 777u);

    std::string lo_str = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_str, lo_large);
}

TEST_F(c_log_content_test, fragment_many_buffers)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();
    size_t lz_str_sz = lz_buf_sz * 7;
    ASSERT_LE(lz_str_sz, static_cast<size_t>(UINT16_MAX));

    std::string lo_huge;
    lo_huge.reserve(lz_str_sz);
    for(size_t lz_i = 0; lz_i < lz_str_sz; ++lz_i)
    {
        lo_huge.push_back(static_cast<char>('0' + (lz_i % 10)));
    }

    auto lo_ctx = run_send_in_thread(
        [&lo_huge]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               lo_huge.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed      = parse_captured_buffers();
    size_t lz_off         = 0;

    std::string lo_result = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_result.size(), lo_huge.size());
    EXPECT_EQ(lo_result, lo_huge);

    EXPECT_GE(lo_parsed.mo_all_captured.size(), 7u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 5: attribute payload
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, attr_numeric_i64)
{
    p8_attr_id li_attr = p8_attr_register("test_i64", e_p8_attr_i64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr));

    struct s_p8_attr_val lo_av = {};
    lo_av.m_id                 = li_attr;
    lo_av.mi_i64               = 12345LL;

    auto lo_ctx                = run_send_in_thread(
        [&lo_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               1,
                               &lo_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 1u);

    size_t lz_args_end = lo_parsed.mo_item_hdr.mu_args_size;
    ASSERT_GE(lo_parsed.mo_args_payload.size(), lz_args_end + sizeof(p8_attr_id) + sizeof(uint64_t));

    size_t lz_off    = lz_args_end;

    p8_attr_id li_id = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id, li_attr);

    uint64_t lu_val = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    int64_t  li_val = 0;
    memcpy(&li_val, &lu_val, sizeof(int64_t));
    EXPECT_EQ(li_val, 12345LL);
}

TEST_F(c_log_content_test, attr_numeric_f64)
{
    p8_attr_id li_attr = p8_attr_register("test_f64", e_p8_attr_f64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr));

    struct s_p8_attr_val lo_av = {};
    lo_av.m_id                 = li_attr;
    lo_av.md_f64               = 2.718;

    auto lo_ctx                = run_send_in_thread(
        [&lo_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               1,
                               &lo_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed      = parse_captured_buffers();
    size_t lz_off         = lo_parsed.mo_item_hdr.mu_args_size;

    lz_off               += sizeof(p8_attr_id);

    double   ld_expected  = 2.718;
    uint64_t lu_raw       = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    double   ld_actual    = 0;
    memcpy(&ld_actual, &lu_raw, sizeof(double));
    EXPECT_EQ(memcmp(&ld_actual, &ld_expected, sizeof(double)), 0);
}

TEST_F(c_log_content_test, attr_string)
{
    p8_attr_id li_attr = p8_attr_register("test_str", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr));

    struct s_p8_attr_val lo_av = {};
    lo_av.m_id                 = li_attr;
    lo_av.mp_str               = "my_value";

    auto lo_ctx                = run_send_in_thread(
        [&lo_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               1,
                               &lo_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = lo_parsed.mo_item_hdr.mu_args_size;
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 1u);

    p8_attr_id li_id = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id, li_attr);

    std::string lo_str = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_str, "my_value");
}

TEST_F(c_log_content_test, attr_count_multiple)
{
    p8_attr_id li_a1 = p8_attr_register("multi_i1", e_p8_attr_i64);
    p8_attr_id li_a2 = p8_attr_register("multi_u1", e_p8_attr_u64);
    p8_attr_id li_a3 = p8_attr_register("multi_s1", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_a1));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_a2));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_a3));

    struct s_p8_attr_val la_av[3] = {};
    la_av[0].m_id                 = li_a1;
    la_av[0].mi_i64               = 100;
    la_av[1].m_id                 = li_a2;
    la_av[1].mu_u64               = 200;
    la_av[2].m_id                 = li_a3;
    la_av[2].mp_str               = "attr3";

    auto lo_ctx                   = run_send_in_thread(
        [&la_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               3,
                               la_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 3u);

    size_t lz_off     = lo_parsed.mo_item_hdr.mu_args_size;

    p8_attr_id li_id1 = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id1, li_a1);
    uint64_t lu_v1 = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(static_cast<int64_t>(lu_v1), 100);

    p8_attr_id li_id2 = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id2, li_a2);
    uint64_t lu_v2 = read_val<uint64_t>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_v2, 200u);

    p8_attr_id li_id3 = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id3, li_a3);
    std::string lo_str = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_str, "attr3");
}

TEST_F(c_log_content_test, item_size_with_attrs)
{
    p8_attr_id li_attr = p8_attr_register("size_check", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr));

    struct s_p8_attr_val lo_av = {};
    lo_av.m_id                 = li_attr;
    lo_av.mu_u64               = 42;

    auto lo_ctx                = run_send_in_thread(
        [&lo_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               1,
                               &lo_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed        = parse_captured_buffers();

    size_t lz_attrs_bytes = sizeof(p8_attr_id) + sizeof(uint64_t);
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_size,
              expected_item_size(sizeof(s_p8_log_item_dat) + lo_parsed.mo_item_hdr.mu_args_size + lz_attrs_bytes));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 6: fragmented attribute
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, attr_string_fragments)
{
    size_t lz_buf_sz   = p8_test_get_buffer_size();

    p8_attr_id li_attr = p8_attr_register("big_attr_str", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr));

    std::string lo_big_val(lz_buf_sz * 2, 'V');

    struct s_p8_attr_val lo_av = {};
    lo_av.m_id                 = li_attr;
    lo_av.mp_str               = lo_big_val.c_str();

    auto lo_ctx                = run_send_in_thread(
        [&lo_av]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               1,
                               &lo_av,
                               "%d",
                               1);
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = lo_parsed.mo_item_hdr.mu_args_size;
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 1u);

    p8_attr_id li_id = read_val<p8_attr_id>(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(li_id, li_attr);

    std::string lo_result = read_string(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lo_result.size(), lo_big_val.size());
    EXPECT_EQ(lo_result, lo_big_val);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 7: edge cases
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, no_args_no_attrs)
{
    auto lo_ctx = run_send_in_thread(
        []()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "plain text");
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto lo_parsed = parse_captured_buffers();
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_args_size, 0u);
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_attrs_count, 0u);
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_size, expected_item_size(sizeof(s_p8_log_item_dat)));
}

TEST_F(c_log_content_test, string_truncated_to_uint16_max)
{
    std::string lo_huge(static_cast<size_t>(UINT16_MAX) + 1000, 'T');

    auto lo_ctx = run_send_in_thread(
        [&lo_huge]()
        {
            return p8_log_sent(e_p8_trace0,
                               nullptr,
                               0,
                               static_cast<uint32_t>(__LINE__),
                               __FILE__,
                               __FUNCTION__,
                               0,
                               nullptr,
                               "%s",
                               lo_huge.c_str());
        },
        __LINE__,
        __FILE__);
    ASSERT_TRUE(lo_ctx.mb_result);

    auto   lo_parsed = parse_captured_buffers();
    size_t lz_off    = 0;

    uint16_t lu_len  = read_string_len(lo_parsed.mo_args_payload, lz_off);
    EXPECT_EQ(lu_len, UINT16_MAX);

    size_t lz_total_args = sizeof(uint16_t) + static_cast<size_t>(UINT16_MAX);
    EXPECT_EQ(lo_parsed.mo_item_hdr.mu_args_size, static_cast<uint16_t>(lz_total_args & 0xFFFF));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Group 8: multiple p8_log_sent calls in a single thread
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_content_test, multi_send_three_items)
{
    bool lb_r1 = false, lb_r2 = false, lb_r3 = false;

    std::thread lo_thread(
        [&lb_r1, &lb_r2, &lb_r3]()
        {
            lb_r1 = p8_log_sent(e_p8_trace0,
                                nullptr,
                                1,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%d",
                                100);
            lb_r2 = p8_log_sent(e_p8_trace1,
                                nullptr,
                                2,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%s",
                                "hello world");
            lb_r3 = p8_log_sent(e_p8_trace2,
                                nullptr,
                                3,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%d %f",
                                42,
                                3.14);
        });
    lo_thread.join();

    ASSERT_TRUE(lb_r1);
    ASSERT_TRUE(lb_r2);
    ASSERT_TRUE(lb_r3);

    auto lo_items = parse_all_items();
    ASSERT_EQ(lo_items.size(), 3u);

    EXPECT_EQ(lo_items[0].mo_hdr.mu_thread_id, lo_items[1].mo_hdr.mu_thread_id);
    EXPECT_EQ(lo_items[1].mo_hdr.mu_thread_id, lo_items[2].mo_hdr.mu_thread_id);

    EXPECT_LE(lo_items[0].mo_hdr.mu_timestamp, lo_items[1].mo_hdr.mu_timestamp);
    EXPECT_LE(lo_items[1].mo_hdr.mu_timestamp, lo_items[2].mo_hdr.mu_timestamp);

    // item 0: %d 100, level=trace0, trace=1
    EXPECT_EQ(lo_items[0].mo_hdr.mu_trace_id, 1u);
    EXPECT_EQ(lo_items[0].mo_hdr.mu_level, static_cast<uint8_t>(e_p8_trace0));
    EXPECT_EQ(lo_items[0].mo_hdr.mu_args_size, P8_SIZE_OF_ARG(uint32_t));
    EXPECT_EQ(lo_items[0].mo_hdr.mu_attrs_count, 0u);
    EXPECT_EQ(lo_items[0].mo_hdr.mu_size,
              expected_item_size(sizeof(s_p8_log_item_dat) + lo_items[0].mo_hdr.mu_args_size));
    {
        size_t   lz_off = 0;
        uint64_t lu_val = read_val<uint64_t>(lo_items[0].mo_payload, lz_off);
        EXPECT_EQ(lu_val, 100u);
    }

    // item 1: %s "hello world", level=trace1, trace=2
    EXPECT_EQ(lo_items[1].mo_hdr.mu_trace_id, 2u);
    EXPECT_EQ(lo_items[1].mo_hdr.mu_level, static_cast<uint8_t>(e_p8_trace1));
    EXPECT_EQ(lo_items[1].mo_hdr.mu_attrs_count, 0u);
    EXPECT_EQ(lo_items[1].mo_hdr.mu_size,
              expected_item_size(sizeof(s_p8_log_item_dat) + lo_items[1].mo_hdr.mu_args_size));
    {
        size_t      lz_off = 0;
        std::string lo_str = read_string(lo_items[1].mo_payload, lz_off);
        EXPECT_EQ(lo_str, "hello world");
    }

    // item 2: %d %f 42 3.14, level=trace2, trace=3
    EXPECT_EQ(lo_items[2].mo_hdr.mu_trace_id, 3u);
    EXPECT_EQ(lo_items[2].mo_hdr.mu_level, static_cast<uint8_t>(e_p8_trace2));
    EXPECT_EQ(lo_items[2].mo_hdr.mu_attrs_count, 0u);
    {
        size_t   lz_off = 0;
        uint64_t lu_val = read_val<uint64_t>(lo_items[2].mo_payload, lz_off);
        EXPECT_EQ(lu_val, 42u);

        double ld_val      = read_val<double>(lo_items[2].mo_payload, lz_off);
        double ld_expected = 3.14;
        EXPECT_EQ(memcmp(&ld_val, &ld_expected, sizeof(double)), 0);
    }
}

TEST_F(c_log_content_test, item_size_8_byte_aligned)
{
    std::thread lo_thread(
        []()
        {
            // records whose unpadded sizes are deliberately not multiples of 8
            // (string args of differing length) so the padding path is exercised
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        0,
                        nullptr,
                        "%s",
                        "abc");
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        0,
                        nullptr,
                        "%s",
                        "hello world");
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        0,
                        nullptr,
                        "%d %s",
                        7,
                        "tail");
        });
    lo_thread.join();

    auto lo_items = parse_all_items();
    ASSERT_EQ(lo_items.size(), 3u);

    for(const auto &lo_item : lo_items)
    {
        EXPECT_EQ(lo_item.mo_hdr.mu_size % 8u, 0u) << "item size must be 8-byte aligned";
        EXPECT_EQ(lo_item.mo_payload.size(), lo_item.mo_hdr.mu_size - sizeof(s_p8_log_item_dat));
    }
}

TEST_F(c_log_content_test, multi_send_with_fragmentation)
{
    size_t      lz_buf_sz = p8_test_get_buffer_size();
    std::string lo_large(lz_buf_sz * 2, 'X');

    bool lb_r1 = false, lb_r2 = false, lb_r3 = false;

    std::thread lo_thread(
        [&lb_r1, &lb_r2, &lb_r3, &lo_large]()
        {
            lb_r1 = p8_log_sent(e_p8_trace0,
                                nullptr,
                                10,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%d",
                                1);
            lb_r2 = p8_log_sent(e_p8_trace1,
                                nullptr,
                                20,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%s",
                                lo_large.c_str());
            lb_r3 = p8_log_sent(e_p8_trace2,
                                nullptr,
                                30,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%d",
                                3);
        });
    lo_thread.join();

    ASSERT_TRUE(lb_r1);
    ASSERT_TRUE(lb_r2);
    ASSERT_TRUE(lb_r3);

    auto lo_items = parse_all_items();
    ASSERT_EQ(lo_items.size(), 3u);

    // item 0: small int before fragmentation
    EXPECT_EQ(lo_items[0].mo_hdr.mu_trace_id, 10u);
    {
        size_t   lz_off = 0;
        uint64_t lu_val = read_val<uint64_t>(lo_items[0].mo_payload, lz_off);
        EXPECT_EQ(lu_val, 1u);
    }

    // item 1: large string that caused fragmentation
    EXPECT_EQ(lo_items[1].mo_hdr.mu_trace_id, 20u);
    {
        size_t      lz_off = 0;
        std::string lo_str = read_string(lo_items[1].mo_payload, lz_off);
        EXPECT_EQ(lo_str.size(), lo_large.size());
        EXPECT_EQ(lo_str, lo_large);
    }

    // item 2: small int after fragmentation
    EXPECT_EQ(lo_items[2].mo_hdr.mu_trace_id, 30u);
    {
        size_t   lz_off = 0;
        uint64_t lu_val = read_val<uint64_t>(lo_items[2].mo_payload, lz_off);
        EXPECT_EQ(lu_val, 3u);
    }

    EXPECT_LE(lo_items[0].mo_hdr.mu_timestamp, lo_items[1].mo_hdr.mu_timestamp);
    EXPECT_LE(lo_items[1].mo_hdr.mu_timestamp, lo_items[2].mo_hdr.mu_timestamp);
}

TEST_F(c_log_content_test, multi_send_with_attrs)
{
    p8_attr_id li_attr_int = p8_attr_register("ms_count", e_p8_attr_i64);
    p8_attr_id li_attr_str = p8_attr_register("ms_label", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr_int));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_attr_str));

    struct s_p8_attr_val lo_av1 = {};
    lo_av1.m_id                 = li_attr_int;
    lo_av1.mi_i64               = 777;

    struct s_p8_attr_val lo_av2 = {};
    lo_av2.m_id                 = li_attr_str;
    lo_av2.mp_str               = "label_value";

    bool lb_r1 = false, lb_r2 = false;

    std::thread lo_thread(
        [&lb_r1, &lb_r2, &lo_av1, &lo_av2]()
        {
            lb_r1 = p8_log_sent(e_p8_trace0,
                                nullptr,
                                1,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                1,
                                &lo_av1,
                                "%d",
                                42);
            lb_r2 = p8_log_sent(e_p8_trace1,
                                nullptr,
                                2,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                1,
                                &lo_av2,
                                "%s",
                                "test");
        });
    lo_thread.join();

    ASSERT_TRUE(lb_r1);
    ASSERT_TRUE(lb_r2);

    auto lo_items = parse_all_items();
    ASSERT_EQ(lo_items.size(), 2u);

    // item 0: %d 42 + attr i64 = 777
    EXPECT_EQ(lo_items[0].mo_hdr.mu_trace_id, 1u);
    EXPECT_EQ(lo_items[0].mo_hdr.mu_attrs_count, 1u);
    {
        size_t   lz_off = 0;
        uint64_t lu_val = read_val<uint64_t>(lo_items[0].mo_payload, lz_off);
        EXPECT_EQ(lu_val, 42u);

        lz_off           = lo_items[0].mo_hdr.mu_args_size;
        p8_attr_id li_id = read_val<p8_attr_id>(lo_items[0].mo_payload, lz_off);
        EXPECT_EQ(li_id, li_attr_int);
        uint64_t lu_attr = read_val<uint64_t>(lo_items[0].mo_payload, lz_off);
        EXPECT_EQ(static_cast<int64_t>(lu_attr), 777);
    }

    // item 1: %s "test" + attr str = "label_value"
    EXPECT_EQ(lo_items[1].mo_hdr.mu_trace_id, 2u);
    EXPECT_EQ(lo_items[1].mo_hdr.mu_attrs_count, 1u);
    {
        size_t      lz_off = 0;
        std::string lo_str = read_string(lo_items[1].mo_payload, lz_off);
        EXPECT_EQ(lo_str, "test");

        lz_off           = lo_items[1].mo_hdr.mu_args_size;
        p8_attr_id li_id = read_val<p8_attr_id>(lo_items[1].mo_payload, lz_off);
        EXPECT_EQ(li_id, li_attr_str);
        std::string lo_attr = read_string(lo_items[1].mo_payload, lz_off);
        EXPECT_EQ(lo_attr, "label_value");
    }
}
