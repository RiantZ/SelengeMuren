#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_log.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Build a memory config sized in whole buffers so the tests stay correct
// regardless of the compile-time buffer size.
std::string make_mem_config(size_t iz_buffers)
{
    const size_t      lz_bytes = iz_buffers * p8_test_get_buffer_size();
    const std::string ls       = std::to_string(lz_bytes);
    return std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \"" + ls + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE
           + "\": \"" + ls + "\"}";
}
} // namespace

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
    // 3-buffer pool: two records fill+park two buffers, the third record
    // exhausts the single remaining buffer mid-serialization and must be
    // discarded. A single wire string is capped at UINT16_MAX, so a record can
    // span at most ~2 buffers when the buffer is large; a 3-buffer pool keeps
    // the exhaustion reachable for any compile-time buffer size.
    const std::string ls_config = make_mem_config(/*buffers*/ 3);
    init_core(ls_config.c_str());
    p8_test_enable_buffer_capture();
    ASSERT_EQ(p8_test_get_free_buffers_count(), 3u);

    bool   lb_r3      = true;
    size_t lz_capture = 0;

    std::thread lo_thread(
        [&lb_r3, &lz_capture]()
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
            lb_r3 = p8_log_sent(e_p8_trace0,
                                nullptr,
                                0,
                                static_cast<uint32_t>(__LINE__),
                                __FILE__,
                                __FUNCTION__,
                                0,
                                nullptr,
                                "%s",
                                lo_huge.c_str());

            // pull the survivors from this still-live writer
            p8_test_drain_writers();
            lz_capture = p8_test_get_captured_count();
        });
    lo_thread.join();

    EXPECT_FALSE(lb_r3);
    // the discarded record is now visible through the core loss statistics,
    // flushed there when the producer thread's writer was destroyed on join
    EXPECT_EQ(p8_test_get_dropped_stats().mu_logs, 1u);
    // the two earlier complete records (buffers A and B) were not rolled back
    EXPECT_GE(lz_capture, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// a record discarded mid-fragmentation keeps the earlier records of the buffer it started in
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, discard_keeps_earlier_records_of_its_first_buffer)
{
    // 3-buffer pool: the short record and the descriptors take two, the long record runs out on the third
    init_core(make_mem_config(3).c_str());
    p8_test_enable_buffer_capture();

    bool        lb_short = false;
    bool        lb_long  = true;
    std::thread lo_thread(
        [&lb_short, &lb_long]()
        {
            lb_short = p8_log_sent(e_p8_trace0,
                                   nullptr,
                                   0,
                                   static_cast<uint32_t>(__LINE__),
                                   __FILE__,
                                   __FUNCTION__,
                                   0,
                                   nullptr,
                                   "short");

            const std::string ls_long(3 * p8_test_get_buffer_size(), 'Z');
            lb_long = p8_log_sent(e_p8_trace0,
                                  nullptr,
                                  0,
                                  static_cast<uint32_t>(__LINE__),
                                  __FILE__,
                                  __FUNCTION__,
                                  0,
                                  nullptr,
                                  "%s",
                                  ls_long.c_str());

            p8_test_drain_writers();
        });
    lo_thread.join();
    EXPECT_TRUE(lb_short);
    EXPECT_FALSE(lb_long);
    // all buffers but the descriptors' one are back in the pool, so the rollback leaked nothing
    EXPECT_EQ(p8_test_get_free_buffers_count(), 2u);

    // the short record is still there, in a buffer that no longer looks like a fragment
    const std::vector<std::vector<uint8_t>> &lr_bufs = p8_test_get_captured_buffers();
    ASSERT_EQ(lr_bufs.size(), 1u);
    const s_p8_data_buf_hdr *lp_hdr  = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[0].data());
    const s_p8_log_item_dat *lp_item = reinterpret_cast<const s_p8_log_item_dat *>(lp_hdr + 1);
    EXPECT_EQ(lp_hdr->mu_flags & P8_DATA_FLAG_FRAGMENT, 0);
    EXPECT_EQ(static_cast<size_t>(lp_hdr->mu_size), sizeof(s_p8_data_buf_hdr) + lp_item->mu_size);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// a discarded record that started right after the tail of a fragmented one keeps that tail intact
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, discard_after_fragment_tail_keeps_the_tail)
{
    // 4-buffer pool: descriptors take one, the first record two (16 bytes spill into the second),
    // the long record starts after that tail, gets the last buffer and still does not fit
    init_core(make_mem_config(4).c_str());
    p8_test_enable_buffer_capture();

    bool        lb_first = false;
    bool        lb_long  = true;
    std::thread lo_thread(
        [&lb_first, &lb_long]()
        {
            const size_t      lz_buf = p8_test_get_buffer_size();
            const std::string ls_first(lz_buf - sizeof(s_p8_data_buf_hdr) - sizeof(s_p8_log_item_dat)
                                           - sizeof(uint16_t) + 16,
                                       'A');
            lb_first = p8_log_sent(e_p8_trace0,
                                   nullptr,
                                   0,
                                   static_cast<uint32_t>(__LINE__),
                                   __FILE__,
                                   __FUNCTION__,
                                   0,
                                   nullptr,
                                   "%s",
                                   ls_first.c_str());

            const std::string ls_long(3 * lz_buf, 'Z');
            lb_long = p8_log_sent(e_p8_trace0,
                                  nullptr,
                                  0,
                                  static_cast<uint32_t>(__LINE__),
                                  __FILE__,
                                  __FUNCTION__,
                                  0,
                                  nullptr,
                                  "%s",
                                  ls_long.c_str());

            p8_test_drain_writers();
        });
    lo_thread.join();
    EXPECT_TRUE(lb_first);
    EXPECT_FALSE(lb_long);
    EXPECT_EQ(p8_test_get_free_buffers_count(), 3u);

    // the first record is whole: its first buffer continues in the second, which holds only the tail
    const std::vector<std::vector<uint8_t>> &lr_bufs = p8_test_get_captured_buffers();
    ASSERT_EQ(lr_bufs.size(), 2u);
    const s_p8_data_buf_hdr *lp_head = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[0].data());
    const s_p8_data_buf_hdr *lp_tail = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[1].data());
    EXPECT_NE(lp_head->mu_flags & P8_DATA_FLAG_FRAGMENT, 0);
    EXPECT_EQ(lp_tail->mu_flags & P8_DATA_FLAG_FRAGMENT, 0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// with no data, an empty packet goes at start and right after data, then not until the idle interval passes
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, empty_batch_follows_data_then_silence)
{
    init_core(make_mem_config(16).c_str());
    p8_test_enable_buffer_capture();

    // the last-packet time starts at 0, so exactly one idle packet goes at start: from this drain, or from the
    // worker if its first cycle came before capture was enabled
    p8_test_drain_writers();
    p8_test_drain_writers();
    const std::vector<s_p8_batch> lo_start = p8_test_get_captured_batches();
    ASSERT_LE(lo_start.size(), 1u);
    if(!lo_start.empty())
    {
        EXPECT_EQ(lo_start[0].mu_batch, 0);
        EXPECT_EQ(lo_start[0].mu_count, 0u);
    }
    p8_test_clear_captured_buffers();

    bool        lb_sent = false;
    std::thread lo_thread(
        [&lb_sent]()
        {
            lb_sent = p8_log_sent(e_p8_trace0,
                                  nullptr,
                                  0,
                                  static_cast<uint32_t>(__LINE__),
                                  __FILE__,
                                  __FUNCTION__,
                                  0,
                                  nullptr,
                                  "tail");
        });
    lo_thread.join();
    ASSERT_TRUE(lb_sent);
    p8_test_drain_writers();

    // a worker cycle after the data, so the empty packet's W is later than the record
    std::this_thread::sleep_for(std::chrono::milliseconds(P8_CORE_THREAD_TIMEOUT_MS + 10));
    p8_test_drain_writers();
    p8_test_drain_writers();

    // the start packet took number 0 either way
    const std::vector<s_p8_batch> &lr_batches = p8_test_get_captured_batches();
    ASSERT_EQ(lr_batches.size(), 2u);
    EXPECT_EQ(lr_batches[0].mu_packet_type, P8_PACKET_BATCH);
    EXPECT_EQ(lr_batches[0].mu_batch, 1);
    EXPECT_EQ(lr_batches[0].mu_count, 1u);
    EXPECT_EQ(lr_batches[1].mu_packet_type, P8_PACKET_BATCH);
    EXPECT_EQ(lr_batches[1].mu_batch, 2);
    EXPECT_EQ(lr_batches[1].mu_count, 0u);
    EXPECT_LE(lr_batches[0].mu_timestamp, lr_batches[1].mu_timestamp);

    const std::vector<std::vector<uint8_t>> &lr_bufs = p8_test_get_captured_buffers();
    ASSERT_EQ(lr_bufs.size(), 1u);
    const s_p8_data_buf_hdr *lp_hdr = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[0].data());
    EXPECT_GT(lr_batches[1].mu_timestamp, reinterpret_cast<const s_p8_log_item_dat *>(lp_hdr + 1)->mu_timestamp);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// a cycle without data right after data sends nothing; a worker cycle later the empty packet covers the record
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, early_wake_after_data_sends_nothing)
{
    init_core(make_mem_config(16).c_str());
    p8_test_enable_buffer_capture();

    // drop the start packet, see empty_batch_follows_data_then_silence
    p8_test_drain_writers();
    p8_test_clear_captured_buffers();

    bool        lb_sent  = false;
    size_t      lz_early = 0;
    std::thread lo_thread(
        [&lb_sent, &lz_early]()
        {
            // the drain follows the record at once, so the record is later than the batch's W
            lb_sent = p8_log_sent(e_p8_trace0,
                                  nullptr,
                                  0,
                                  static_cast<uint32_t>(__LINE__),
                                  __FILE__,
                                  __FUNCTION__,
                                  0,
                                  nullptr,
                                  "tail");
            p8_test_drain_writers();

            // an early wake without data, like the one an exiting thread causes right after a batch
            p8_test_drain_writers();
            lz_early = p8_test_get_captured_batches().size();

            std::this_thread::sleep_for(std::chrono::milliseconds(P8_CORE_THREAD_TIMEOUT_MS + 10));
            p8_test_drain_writers();
            p8_test_drain_writers();
        });
    lo_thread.join();
    ASSERT_TRUE(lb_sent);

    // only the data packet before the pause
    EXPECT_EQ(lz_early, 1u);

    const std::vector<s_p8_batch>           &lr_batches = p8_test_get_captured_batches();
    const std::vector<std::vector<uint8_t>> &lr_bufs    = p8_test_get_captured_buffers();
    ASSERT_EQ(lr_batches.size(), 2u);
    ASSERT_EQ(lr_bufs.size(), 1u);
    EXPECT_EQ(lr_batches[0].mu_count, 1u);
    EXPECT_EQ(lr_batches[1].mu_count, 0u);

    // the empty packet covers the record, so the server can release it
    const s_p8_data_buf_hdr *lp_hdr = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[0].data());
    EXPECT_GT(lr_batches[1].mu_timestamp, reinterpret_cast<const s_p8_log_item_dat *>(lp_hdr + 1)->mu_timestamp);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// every writer has its own id, even two writers of one thread, and a fragment's continuation keeps it
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_pull_test, writers_have_their_own_ids)
{
    init_core(make_mem_config(16).c_str());
    p8_test_enable_buffer_capture();

    std::thread lo_thread(
        []()
        {
            // longer than a buffer, so the record continues in a second buffer
            const std::string ls_long(p8_test_get_buffer_size(), 'L');
            p8_log_sent(e_p8_trace0,
                        nullptr,
                        0,
                        static_cast<uint32_t>(__LINE__),
                        __FILE__,
                        __FUNCTION__,
                        0,
                        nullptr,
                        "%s",
                        ls_long.c_str());
            // the trace writer is the second writer of this thread
            p8_trc_end(p8_trc_begin("second", static_cast<uint32_t>(__LINE__), __FILE__, __FUNCTION__, 0, nullptr));
            p8_test_drain_writers();
        });
    lo_thread.join();

    // the drain takes the newest writer first: the trace writer, then the long record and its continuation
    const std::vector<std::vector<uint8_t>> &lr_bufs = p8_test_get_captured_buffers();
    ASSERT_EQ(lr_bufs.size(), 3u);
    const s_p8_data_buf_hdr *lp_trc  = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[0].data());
    const s_p8_data_buf_hdr *lp_head = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[1].data());
    const s_p8_data_buf_hdr *lp_tail = reinterpret_cast<const s_p8_data_buf_hdr *>(lr_bufs[2].data());
    ASSERT_NE(lp_head->mu_flags & P8_DATA_FLAG_FRAGMENT, 0);
    EXPECT_EQ(lp_tail->mu_writer_id, lp_head->mu_writer_id);
    EXPECT_NE(lp_trc->mu_writer_id, lp_head->mu_writer_id);
    // one thread for both writers, and each writer numbers its own buffers from 0
    EXPECT_EQ(lp_trc->mu_thread_id, lp_head->mu_thread_id);
    EXPECT_EQ(lp_trc->mu_buf_seq, 0u);
    EXPECT_EQ(lp_head->mu_buf_seq, 0u);
    EXPECT_EQ(lp_tail->mu_buf_seq, 1u);
}
