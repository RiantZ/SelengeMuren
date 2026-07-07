#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_config_keys.hpp"
#include "p8_log.hpp"
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
} // namespace

class c_log_fragment_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const std::string  ls_config = make_mem_config(/*buffers*/ 16);
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ls_config.c_str();

        ASSERT_TRUE(p8_initialize(&lo_config));
    }

    void TearDown() override
    {
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// small messages that fit in one buffer — no fragmentation expected
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, small_message_fits_in_buffer)
{
    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "hello %d",
                            42));
}

TEST_F(c_log_fragment_test, small_string_fits_in_buffer)
{
    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "msg: %s",
                            "short string"));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// string that exceeds one buffer — must fragment
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, large_string_fragments_across_buffers)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();
    ASSERT_GT(lz_buf_sz, 0u);

    std::string lo_large(lz_buf_sz * 2, 'A');

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "data: %s",
                            lo_large.c_str()));
}

TEST_F(c_log_fragment_test, string_exactly_fills_buffer)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();
    ASSERT_GT(lz_buf_sz, 0u);

    size_t lz_overhead = sizeof(s_p8_data_buf_hdr) + sizeof(s_p8_log_item_dat) + sizeof(uint16_t);
    size_t lz_str_len  = lz_buf_sz - lz_overhead;

    std::string lo_exact(lz_str_len, 'B');

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "%s",
                            lo_exact.c_str()));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// multiple strings that together exceed buffer — tests multi-arg fragmentation
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, multiple_large_strings)
{
    size_t lz_buf_sz = p8_test_get_buffer_size();
    ASSERT_GT(lz_buf_sz, 0u);

    std::string lo_s1(lz_buf_sz / 2, 'X');
    std::string lo_s2(lz_buf_sz / 2, 'Y');
    std::string lo_s3(lz_buf_sz / 2, 'Z');

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "%s %s %s",
                            lo_s1.c_str(),
                            lo_s2.c_str(),
                            lo_s3.c_str()));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// mixed fixed + large string — fixed arg moves whole, string fragments
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, fixed_args_with_large_string)
{
    size_t      lz_buf_sz = p8_test_get_buffer_size();
    std::string lo_large(lz_buf_sz * 3, 'C');

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "%d %lld %f %s %d",
                            42,
                            static_cast<long long>(123456789LL),
                            3.14,
                            lo_large.c_str(),
                            99));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// discard when pool is exhausted — send() returns false
// Run in a separate thread to get a fresh TLS writer connected to the current (tiny) core.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, discard_when_pool_exhausted)
{
    p8_release();

    // single-buffer pool: a record that needs a second buffer must be discarded
    const std::string  ls_config = make_mem_config(/*buffers*/ 1);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();
    ASSERT_TRUE(p8_initialize(&lo_config));
    ASSERT_EQ(p8_test_get_free_buffers_count(), 1u);

    bool        lb_result = true;
    std::thread lo_thread(
        [&lb_result]()
        {
            size_t      lz_buf_sz = p8_test_get_buffer_size();
            std::string lo_huge(lz_buf_sz * 2, 'D');

            lb_result = p8_log_sent(e_p8_trace0,
                                    nullptr,
                                    0,
                                    static_cast<uint32_t>(__LINE__),
                                    __FILE__,
                                    __FUNCTION__,
                                    0,
                                    nullptr,
                                    "%s",
                                    lo_huge.c_str());
        });
    lo_thread.join();

    EXPECT_FALSE(lb_result);
    // the discarded record is now visible through the core loss statistics,
    // flushed there when the producer thread's writer was destroyed on join
    EXPECT_EQ(p8_test_get_dropped_stats().mu_logs, 1u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// consecutive sends after fragmentation — buffer state is consistent
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, consecutive_sends_after_fragment)
{
    size_t      lz_buf_sz = p8_test_get_buffer_size();
    std::string lo_large(lz_buf_sz * 2, 'E');

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "big: %s",
                            lo_large.c_str()));

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "small: %d",
                            123));

    EXPECT_TRUE(p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "another big: %s",
                            lo_large.c_str()));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// fragmentation produces correct results for very large data spanning many buffers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_log_fragment_test, very_large_string_spans_many_buffers)
{
    // Run the logging on a fresh thread so its thread-local writer binds to the
    // pristine tiny core created in SetUp and is fully torn down on join. The
    // process-global TLS writer used by the main thread outlives p8_release and
    // stays bound to whichever core it first saw, so across --gtest_repeat runs
    // it accumulates outstanding buffers on a stale 16-buffer pool. A record
    // this large needs ~11 of those 16 buffers held at once, so any leaked
    // outstanding buffer makes acquire_buffer fail mid-record and drops the log.
    // A dedicated thread guarantees an all-free pool per test. Same isolation
    // trick as discard_when_pool_exhausted.
    bool lb_huge_sent  = false;
    bool lb_after_sent = false;

    std::thread lo_thread(
        [&lb_huge_sent, &lb_after_sent]()
        {
            size_t      lz_buf_sz = p8_test_get_buffer_size();
            std::string lo_huge(lz_buf_sz * 10, 'G');

            lb_huge_sent  = p8_log_sent(e_p8_trace0,
                                        nullptr,
                                        0,
                                        static_cast<uint32_t>(__LINE__),
                                        __FILE__,
                                        __FUNCTION__,
                                        0,
                                        nullptr,
                                        "payload: %s",
                                        lo_huge.c_str());

            lb_after_sent = p8_log_sent(e_p8_trace0,
                                        nullptr,
                                        0,
                                        static_cast<uint32_t>(__LINE__),
                                        __FILE__,
                                        __FUNCTION__,
                                        0,
                                        nullptr,
                                        "after huge: %d",
                                        42);
        });
    lo_thread.join();

    EXPECT_TRUE(lb_huge_sent);
    EXPECT_TRUE(lb_after_sent);
}
