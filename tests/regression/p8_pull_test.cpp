#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_log.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tests for the pull-based drain architecture: writers retain filled buffers
// locally and the core pulls them via cp8_core::drain_writers (exercised here
// through the synchronous p8_test_drain_writers helper).
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class c_pull_test : public ::testing::Test
{
protected:
    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_test_clear_captured_buffers();
        p8_release();
    }

    static void init_core(const char *ip_config)
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ip_config;
        ASSERT_TRUE(p8_initialize(&lo_config));
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// outstanding-buffer counter: acquire increments, release decrements
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, outstanding_counter_tracks_acquire_release)
{
    init_core("{"
              "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"64KB\","
              "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"64KB\""
              "}");

    EXPECT_EQ(p8_test_get_outstanding_buffers(), 0u);

    std::vector<uint8_t *> lo_bufs;
    for(size_t lz_i = 0; lz_i < 6; ++lz_i)
    {
        uint8_t *lp_buf = p8_test_acquire_buffer();
        ASSERT_NE(lp_buf, nullptr);
        lo_bufs.push_back(lp_buf);
    }

    // 6 of 8 pool buffers outstanding == 75%, crosses P8_CORE_DRAIN_PERCENT
    EXPECT_EQ(p8_test_get_outstanding_buffers(), 6u);

    for(uint8_t *lp_buf : lo_bufs)
    {
        p8_test_release_buffer(lp_buf);
    }

    EXPECT_EQ(p8_test_get_outstanding_buffers(), 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// drain pulls a record from a single live writer (not the shutdown path)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, drain_pulls_from_live_writer)
{
    init_core("{"
              "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"128KB\","
              "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"128KB\""
              "}");
    p8_test_enable_buffer_capture();

    bool   lb_sent    = false;
    size_t lz_capture = 0;

    // Do everything on one thread so the writer is still alive (registered)
    // when we drain — this exercises cp8_tls_writer::pull() on a live writer.
    std::thread lo_thread(
        [&lb_sent, &lz_capture]()
        {
            lb_sent = p8_log_sent(e_p8_trace0,
                                  nullptr,
                                  0,
                                  static_cast<uint32_t>(__LINE__),
                                  __FILE__,
                                  __FUNCTION__,
                                  0,
                                  nullptr,
                                  "%d",
                                  42);
            p8_test_drain_writers();
            lz_capture = p8_test_get_captured_count();
        });
    lo_thread.join();

    EXPECT_TRUE(lb_sent);
    EXPECT_GE(lz_capture, 1u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// drain pulls from several writers that are all alive simultaneously
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, drain_pulls_from_multiple_live_writers)
{
    static constexpr uint32_t lu_thread_count = 4;

    init_core("{"
              "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"1MB\","
              "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"1MB\""
              "}");

    // Capture on before any send so the worker leaves data buffers to us.
    p8_test_enable_buffer_capture();

    std::latch               lo_sent_latch(lu_thread_count);
    std::latch               lo_release_latch(1);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lu_thread_count);

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&, lu_i]()
            {
                p8_log_sent(e_p8_trace0,
                            nullptr,
                            0,
                            static_cast<uint32_t>(__LINE__),
                            __FILE__,
                            __FUNCTION__,
                            0,
                            nullptr,
                            "thread %u",
                            lu_i);
                lo_sent_latch.count_down();
                lo_release_latch.wait();
            });
    }

    // all writers registered and holding their buffers
    lo_sent_latch.wait();
    ASSERT_EQ(p8_test_get_writer_count(), lu_thread_count);

    p8_test_drain_writers();
    size_t lz_capture = p8_test_get_captured_count();

    lo_release_latch.count_down();
    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    // each live writer contributed at least its one record buffer
    EXPECT_GE(lz_capture, static_cast<size_t>(lu_thread_count));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Option A: a record discarded on pool exhaustion rolls back only its own
// buffers; complete records parked earlier survive and are still drainable.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, discard_preserves_earlier_records)
{
    // 4-buffer pool: two records fill+park two buffers, the third record
    // exhausts the remaining pool mid-serialization and must be discarded.
    init_core("{"
              "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"32KB\","
              "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"32KB\""
              "}");
    p8_test_enable_buffer_capture();
    ASSERT_EQ(p8_test_get_free_buffers_count(), 4u);

    bool     lb_r3      = true;
    uint64_t lu_dropped = 0;
    size_t   lz_capture = 0;

    std::thread lo_thread(
        [&lb_r3, &lu_dropped, &lz_capture]()
        {
            const size_t lz_buf_sz   = p8_test_get_buffer_size();
            // size the string so the record all but fills its buffer, leaving
            // < P8_LOG_MIN_BUFFER_SPACE so the buffer is parked (not kept)
            const size_t lz_overhead = sizeof(s_p8_data_buf_hdr) + sizeof(s_p8_log_item_dat) + sizeof(uint16_t);
            const size_t lz_fill     = lz_buf_sz - lz_overhead - 8;

            std::string lo_fill(lz_fill, 'A');

            // R1 -> fills+parks buffer A
            ASSERT_TRUE(p8_log_sent(e_p8_trace0,
                                    nullptr,
                                    0,
                                    static_cast<uint32_t>(__LINE__),
                                    __FILE__,
                                    __FUNCTION__,
                                    0,
                                    nullptr,
                                    "%s",
                                    lo_fill.c_str()));
            // R2 -> fills+parks buffer B
            ASSERT_TRUE(p8_log_sent(e_p8_trace0,
                                    nullptr,
                                    0,
                                    static_cast<uint32_t>(__LINE__),
                                    __FILE__,
                                    __FUNCTION__,
                                    0,
                                    nullptr,
                                    "%s",
                                    lo_fill.c_str()));

            // R3 -> huge, exhausts the remaining pool and is discarded
            std::string lo_huge(lz_buf_sz * 3, 'Z');
            lb_r3      = p8_log_sent(e_p8_trace0,
                                     nullptr,
                                     0,
                                     static_cast<uint32_t>(__LINE__),
                                     __FILE__,
                                     __FUNCTION__,
                                     0,
                                     nullptr,
                                     "%s",
                                     lo_huge.c_str());

            lu_dropped = p8_test_get_tls_dropped_records();

            // pull the survivors from this still-live writer
            p8_test_drain_writers();
            lz_capture = p8_test_get_captured_count();
        });
    lo_thread.join();

    EXPECT_FALSE(lb_r3);
    EXPECT_EQ(lu_dropped, 1u);
    // the two earlier complete records (buffers A and B) were not rolled back
    EXPECT_GE(lz_capture, 2u);
}
