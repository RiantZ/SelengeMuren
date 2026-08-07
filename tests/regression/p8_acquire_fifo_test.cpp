#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_config_keys.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Correctness of the blocking, strict-FIFO acquire path (acquire_buffer(true) /
// p8_test_acquire_buffer_wait). All scenarios keep the P8_TESTING buffer-capture
// mode OFF: with capture on the worker skips recycling, so a blocked acquirer that
// relied on the worker would hang. These tests drive recycling directly instead
// (p8_test_release_buffer / p8_test_release_buffers), so the worker is never on the
// critical path.

namespace
{
// Build a memory config whose sizes are expressed in whole buffers, so the tests
// stay correct regardless of the compile-time buffer size.
std::string make_mem_config(size_t iz_max_buffers, size_t iz_initial_buffers)
{
    const size_t lz_bufsz = p8_test_get_buffer_size();
    return std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \"" + std::to_string(iz_max_buffers * lz_bufsz)
           + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE + "\": \"" + std::to_string(iz_initial_buffers * lz_bufsz)
           + "\"}";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cap the pool at iz_buffers (initial == max, so no partial growth) and initialize.
void init_capped_pool(size_t iz_buffers)
{
    const std::string  ls_cfg    = make_mem_config(iz_buffers, iz_buffers);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_cfg.c_str();
    ASSERT_TRUE(p8_initialize(&lo_config));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Spin (yielding) until ir_pred() holds or the timeout elapses. Returns the final
// predicate value, so callers assert on it instead of spinning forever.
template <typename t_pred> bool spin_until(std::chrono::milliseconds i_timeout, t_pred ir_pred)
{
    const auto lo_deadline = std::chrono::steady_clock::now() + i_timeout;
    while(!ir_pred())
    {
        if(std::chrono::steady_clock::now() >= lo_deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Run ir_body on a helper thread; return true if it finished within the timeout,
// false (and detach the helper) if it hung. A timeout only fires on a real bug
// (missing wakeup / deadlock), so failing loud beats wedging the whole suite. The
// helper keeps the promise alive via shared_ptr, so detaching is memory-safe.
template <typename t_body> bool completed_within(std::chrono::milliseconds i_timeout, t_body ir_body)
{
    auto              lp_done = std::make_shared<std::promise<void>>();
    std::future<void> lo_fut  = lp_done->get_future();
    std::thread       lo_worker(
        [lp_done, ir_body]() mutable
        {
            ir_body();
            lp_done->set_value();
        });

    if(lo_fut.wait_for(i_timeout) == std::future_status::ready)
    {
        lo_worker.join();
        return true;
    }
    lo_worker.detach();
    return false;
}
} // namespace

class c_p8_acquire_fifo_test : public ::testing::Test
{
protected:
    void TearDown() override
    {
        p8_release();
    }
};

// The next freed buffer must go to the oldest waiter: with N threads parked in a
// known arrival order, the wake order must equal the arrival order [0..N-1].
TEST_F(c_p8_acquire_fifo_test, wake_order_equals_arrival_order)
{
    static constexpr uint32_t lu_n = 8;
    ASSERT_NO_FATAL_FAILURE(init_capped_pool(1));

    // Drain the single buffer to zero-free on the controller, so every waiter parks
    // (acquire_no_lock returns null) instead of grabbing a free buffer.
    uint8_t *lp_hold = p8_test_acquire_buffer();
    ASSERT_NE(lp_hold, nullptr);
    ASSERT_EQ(p8_test_get_free_buffers_count(), 0u);

    struct s_state
    {
        std::mutex           mo_mx;
        std::vector<int>     mo_wake_order;
        std::atomic<int32_t> mi_gate { -1 }; // controller admits thread t once mi_gate >= t
        std::atomic<bool>    mb_enqueue_ok { true };
    };
    auto lp_st       = std::make_shared<s_state>();

    const bool lb_ok = completed_within(
        std::chrono::seconds(10),
        [lp_st, lp_hold]()
        {
            std::vector<std::thread> lo_threads; // lives on this (detachable) thread's stack
            lo_threads.reserve(lu_n);
            for(uint32_t lu_t = 0; lu_t < lu_n; ++lu_t)
            {
                lo_threads.emplace_back(
                    [lp_st, lu_t]()
                    {
                        while(lp_st->mi_gate.load(std::memory_order_acquire) < static_cast<int32_t>(lu_t))
                        {
                            std::this_thread::yield();
                        }
                        uint8_t *lp_buf = p8_test_acquire_buffer_wait();
                        {
                            std::lock_guard<std::mutex> lo_guard(lp_st->mo_mx);
                            lp_st->mo_wake_order.push_back(static_cast<int>(lu_t));
                        }
                        if(lp_buf)
                        {
                            p8_test_release_buffer(lp_buf); // hand off to the next waiter
                        }
                    });
            }

            // Admit threads one at a time; only advance once the previous one is
            // observably enqueued (arrivals is bumped atomically with the enqueue).
            for(uint32_t lu_t = 0; lu_t < lu_n; ++lu_t)
            {
                lp_st->mi_gate.store(static_cast<int32_t>(lu_t), std::memory_order_release);
                const uint64_t lu_target = static_cast<uint64_t>(lu_t) + 1;
                if(!spin_until(std::chrono::seconds(5),
                               [lu_target]() { return p8_test_get_wait_arrivals() >= lu_target; }))
                {
                    lp_st->mb_enqueue_ok.store(false, std::memory_order_relaxed);
                }
            }

            // Wake waiter 0; each waiter's own release then wakes the next in turn.
            p8_test_release_buffer(lp_hold);

            for(auto &lo_t : lo_threads)
            {
                lo_t.join();
            }
        });

    ASSERT_TRUE(lb_ok) << "FIFO scenario hung (missing wakeup / deadlock)";
    EXPECT_TRUE(lp_st->mb_enqueue_ok.load()) << "a waiter never enqueued in the intended order";

    std::vector<int> lo_expected(lu_n);
    for(uint32_t lu_i = 0; lu_i < lu_n; ++lu_i)
    {
        lo_expected[lu_i] = static_cast<int>(lu_i);
    }
    std::lock_guard<std::mutex> lo_guard(lp_st->mo_mx);
    EXPECT_EQ(lp_st->mo_wake_order, lo_expected) << "acquire_buffer(true) did not honor strict FIFO";
}

// Shutdown must release every parked waiter with a nullptr result so neither the
// waiters nor p8_release() hang.
TEST_F(c_p8_acquire_fifo_test, shutdown_wakes_blocked_waiters)
{
    static constexpr uint32_t lu_n = 8;
    ASSERT_NO_FATAL_FAILURE(init_capped_pool(1));

    uint8_t *lp_hold = p8_test_acquire_buffer(); // occupy the only buffer
    ASSERT_NE(lp_hold, nullptr);

    struct s_state
    {
        std::atomic<uint32_t>    mu_returned { 0 };
        std::vector<std::thread> mo_threads;
    };
    auto lp_st = std::make_shared<s_state>();
    lp_st->mo_threads.reserve(lu_n);
    for(uint32_t lu_t = 0; lu_t < lu_n; ++lu_t)
    {
        lp_st->mo_threads.emplace_back(
            [lp_st]()
            {
                uint8_t *lp_buf = p8_test_acquire_buffer_wait();
                if(lp_buf)
                {
                    p8_test_release_buffer(lp_buf);
                }
                lp_st->mu_returned.fetch_add(1, std::memory_order_relaxed);
            });
    }

    ASSERT_TRUE(spin_until(std::chrono::seconds(5), []() { return p8_test_get_waiter_count() >= lu_n; }))
        << "not all waiters parked";

    const bool lb_released = completed_within(std::chrono::seconds(5), []() { p8_release(); });
    EXPECT_TRUE(lb_released) << "p8_release() hung with blocked waiters (stop_waiters missing?)";

    const bool lb_joined = completed_within(std::chrono::seconds(5),
                                            [lp_st]()
                                            {
                                                for(auto &lo_t : lp_st->mo_threads)
                                                {
                                                    lo_t.join();
                                                }
                                            });
    ASSERT_TRUE(lb_joined) << "blocked waiters did not return after shutdown";
    EXPECT_EQ(lp_st->mu_returned.load(), lu_n);
    // lp_hold belonged to the now-destroyed instance; do not touch it.
}

// Heavy contention (N >> K) with threads recycling directly among themselves must
// make progress and never deadlock.
TEST_F(c_p8_acquire_fifo_test, heavy_contention_no_deadlock)
{
    static constexpr size_t   lz_k     = 2;
    static constexpr uint32_t lu_n     = 32;
    static constexpr uint32_t lu_iters = 5000;
    ASSERT_NO_FATAL_FAILURE(init_capped_pool(lz_k));

    const bool lb_ok = completed_within(std::chrono::seconds(30),
                                        []()
                                        {
                                            std::vector<std::thread> lo_threads;
                                            lo_threads.reserve(lu_n);
                                            for(uint32_t lu_t = 0; lu_t < lu_n; ++lu_t)
                                            {
                                                lo_threads.emplace_back(
                                                    []()
                                                    {
                                                        for(uint32_t lu_i = 0; lu_i < lu_iters; ++lu_i)
                                                        {
                                                            uint8_t *lp_buf = p8_test_acquire_buffer_wait();
                                                            if(!lp_buf)
                                                            {
                                                                return; // shutdown (not expected mid-test)
                                                            }
                                                            p8_test_release_buffer(lp_buf);
                                                        }
                                                    });
                                            }
                                            for(auto &lo_t : lo_threads)
                                            {
                                                lo_t.join();
                                            }
                                        });

    EXPECT_TRUE(lb_ok) << "heavy contention deadlocked";
}

// A single parked waiter must be served (with a real buffer, not the shutdown
// sentinel) when a buffer is recycled.
TEST_F(c_p8_acquire_fifo_test, blocked_waiter_served_on_recycle)
{
    ASSERT_NO_FATAL_FAILURE(init_capped_pool(1));

    uint8_t *lp_hold = p8_test_acquire_buffer();
    ASSERT_NE(lp_hold, nullptr);

    struct s_state
    {
        std::atomic<uint8_t *> mp_got { nullptr };
        std::atomic<bool>      mb_done { false };
        std::thread            mo_thread;
    };
    auto lp_st       = std::make_shared<s_state>();
    lp_st->mo_thread = std::thread(
        [lp_st]()
        {
            lp_st->mp_got.store(p8_test_acquire_buffer_wait(), std::memory_order_relaxed);
            lp_st->mb_done.store(true, std::memory_order_release);
        });

    ASSERT_TRUE(spin_until(std::chrono::seconds(5), []() { return p8_test_get_waiter_count() >= 1; }))
        << "waiter never parked";

    p8_test_release_buffer(lp_hold); // hand off to the waiter

    const bool lb_served
        = spin_until(std::chrono::seconds(5), [lp_st]() { return lp_st->mb_done.load(std::memory_order_acquire); });
    EXPECT_TRUE(lb_served) << "waiter not served after recycle";

    if(lb_served)
    {
        lp_st->mo_thread.join();
        uint8_t *lp_got = lp_st->mp_got.load(std::memory_order_relaxed);
        EXPECT_NE(lp_got, nullptr);
        if(lp_got)
        {
            p8_test_release_buffer(lp_got);
        }
    }
    else
    {
        lp_st->mo_thread.detach(); // avoid wedging the suite on a bug; state is shared_ptr-owned
    }
}

// A single batch recycle must hand a buffer to every fitting waiter
// (min(batch, waiters)) and send the remainder to the free list. Strict wake ORDER
// is proven by wake_order_equals_arrival_order; a batch signals its waiters
// near-simultaneously, so the order their threads subsequently run is a scheduling
// detail and is deliberately NOT asserted here (only the served count and balance).
TEST_F(c_p8_acquire_fifo_test, batch_handoff_serves_all_waiters)
{
    static constexpr uint32_t lu_waiters = 4; // W
    static constexpr size_t   lz_k       = 6; // pool capacity (> W, so a remainder exists)
    ASSERT_NO_FATAL_FAILURE(init_capped_pool(lz_k));

    // Drain all K buffers; keep them to release later as one batch.
    std::vector<uint8_t *> lo_held;
    lo_held.reserve(lz_k);
    for(size_t lz_i = 0; lz_i < lz_k; ++lz_i)
    {
        uint8_t *lp_buf = p8_test_acquire_buffer();
        ASSERT_NE(lp_buf, nullptr);
        lo_held.push_back(lp_buf);
    }
    ASSERT_EQ(p8_test_get_free_buffers_count(), 0u);

    struct s_state
    {
        std::atomic<uint32_t>    mu_served { 0 }; // woke with a real buffer
        std::atomic<uint32_t>    mu_null { 0 };   // woke with the shutdown sentinel
        std::vector<std::thread> mo_threads;
    };
    auto lp_st = std::make_shared<s_state>();
    lp_st->mo_threads.reserve(lu_waiters);
    for(uint32_t lu_t = 0; lu_t < lu_waiters; ++lu_t)
    {
        lp_st->mo_threads.emplace_back(
            [lp_st]()
            {
                uint8_t *lp_buf = p8_test_acquire_buffer_wait();
                if(lp_buf)
                {
                    lp_st->mu_served.fetch_add(1, std::memory_order_relaxed);
                    p8_test_release_buffer(lp_buf);
                }
                else
                {
                    lp_st->mu_null.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // Once all W waiters are parked, hand them all off in a single batch release.
    ASSERT_TRUE(spin_until(std::chrono::seconds(5), []() { return p8_test_get_waiter_count() >= lu_waiters; }))
        << "not all waiters parked";
    p8_test_release_buffers(lo_held.data(), lo_held.size());

    const bool lb_joined = completed_within(std::chrono::seconds(5),
                                            [lp_st]()
                                            {
                                                for(auto &lo_t : lp_st->mo_threads)
                                                {
                                                    lo_t.join();
                                                }
                                            });
    ASSERT_TRUE(lb_joined) << "batch hand-off did not wake all waiters";
    EXPECT_EQ(lp_st->mu_served.load(), lu_waiters) << "batch did not serve every waiter";
    EXPECT_EQ(lp_st->mu_null.load(), 0u);

    // Every buffer is back: the K-W remainder from the batch plus the W buffers the
    // waiters each released after waking.
    EXPECT_EQ(p8_test_get_free_buffers_count(), lz_k);
}

// End-to-end: a producer on a small pool must WAIT (not drop) under pressure and
// still make progress as the live worker recycles buffers. Blocking is the default
// behavior — there is no config knob.
TEST_F(c_p8_acquire_fifo_test, producer_blocks_not_drops_under_pressure)
{
    static constexpr uint32_t lu_msgs  = 10000;
    const size_t              lz_bufsz = p8_test_get_buffer_size();
    const std::string  ls_cfg    = std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \""
                                   + std::to_string(4 * lz_bufsz) + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE + "\": \""
                                   + std::to_string(4 * lz_bufsz) + "\"}";
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_cfg.c_str();
    ASSERT_TRUE(p8_initialize(&lo_config));

    // The live worker (capture OFF) drains this writer, flushes to the null sink and
    // recycles, so a blocked producer is handed buffers and keeps going.
    const bool lb_ok = completed_within(std::chrono::seconds(30),
                                        []()
                                        {
                                            std::thread lo_producer(
                                                []()
                                                {
                                                    for(uint32_t lu_i = 0; lu_i < lu_msgs; ++lu_i)
                                                    {
                                                        p8_log_sent(e_p8_trace0,
                                                                    nullptr,
                                                                    0,
                                                                    static_cast<uint32_t>(__LINE__),
                                                                    __FILE__,
                                                                    __FUNCTION__,
                                                                    0,
                                                                    nullptr,
                                                                    "pressure msg %u",
                                                                    lu_i);
                                                    }
                                                });
                                            lo_producer.join();
                                        });

    EXPECT_TRUE(lb_ok) << "producer stalled under pressure (worker not recycling?)";
    // Blocking means wait, not drop: no log should have been discarded.
    EXPECT_EQ(p8_test_get_dropped_stats().mu_logs, 0u);
}
