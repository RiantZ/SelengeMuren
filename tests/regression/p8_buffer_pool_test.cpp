#include "p8_buffer_pool.hpp"
#include "p8_memory_budget.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename t_pred> static bool spin_until(std::chrono::milliseconds i_timeout, t_pred i_pred)
{
    auto lo_deadline = std::chrono::steady_clock::now() + i_timeout;
    while(std::chrono::steady_clock::now() < lo_deadline)
    {
        if(i_pred())
        {
            return true;
        }
        std::this_thread::yield();
    }
    return i_pred();
}
} // namespace

class c_p8_buffer_pool_test : public ::testing::Test
{
};

TEST_F(c_p8_buffer_pool_test, buffer_size_reported)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    EXPECT_EQ(lo_pool.get_buffer_size(), 1024u);
    EXPECT_EQ(lo_pool.get_free_count(), 0u);
}

TEST_F(c_p8_buffer_pool_test, init_zero_is_noop)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    EXPECT_EQ(lo_pool.init(0), 0u);
    EXPECT_EQ(lo_pool.get_free_count(), 0u);
    EXPECT_EQ(lp_budget->get_used(), 0u);
}

TEST_F(c_p8_buffer_pool_test, init_pre_allocates_requested_count)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    EXPECT_EQ(lo_pool.init(4), 4u);
    EXPECT_EQ(lo_pool.get_free_count(), 4u);
    EXPECT_EQ(lp_budget->get_used(), 4u * 1024u);
}

TEST_F(c_p8_buffer_pool_test, init_clamped_by_budget)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(3 * 1024);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    EXPECT_EQ(lo_pool.init(10), 3u);
    EXPECT_EQ(lo_pool.get_free_count(), 3u);
    EXPECT_EQ(lp_budget->get_used(), 3u * 1024u);
}

TEST_F(c_p8_buffer_pool_test, acquire_returns_preallocated_buffer)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.init(2);

    lo_pool.lock();
    uint8_t *lp_buf = lo_pool.acquire_no_lock();
    lo_pool.unlock();
    ASSERT_NE(lp_buf, nullptr);
    EXPECT_EQ(lo_pool.get_free_count(), 1u);

    lo_pool.recycle(lp_buf);
    EXPECT_EQ(lo_pool.get_free_count(), 2u);
}

TEST_F(c_p8_buffer_pool_test, acquire_grows_on_demand)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.init(1);

    lo_pool.lock();
    uint8_t *lp_buf1 = lo_pool.acquire_no_lock();
    uint8_t *lp_buf2 = lo_pool.acquire_no_lock();
    lo_pool.unlock();
    ASSERT_NE(lp_buf1, nullptr);
    ASSERT_NE(lp_buf2, nullptr);
    EXPECT_EQ(lo_pool.get_free_count(), 0u);
    EXPECT_EQ(lp_budget->get_used(), 2u * 1024u);

    lo_pool.recycle(lp_buf1);
    lo_pool.recycle(lp_buf2);
}

TEST_F(c_p8_buffer_pool_test, acquire_returns_null_when_budget_exhausted)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(2 * 1024);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.lock();
    uint8_t *lp_buf1 = lo_pool.acquire_no_lock();
    uint8_t *lp_buf2 = lo_pool.acquire_no_lock();
    lo_pool.unlock();
    ASSERT_NE(lp_buf1, nullptr);
    ASSERT_NE(lp_buf2, nullptr);

    lo_pool.lock();
    uint8_t *lp_buf3 = lo_pool.acquire_no_lock();
    lo_pool.unlock();
    EXPECT_EQ(lp_buf3, nullptr);

    lo_pool.recycle(lp_buf1);

    lo_pool.lock();
    uint8_t *lp_buf4 = lo_pool.acquire_no_lock();
    lo_pool.unlock();
    EXPECT_NE(lp_buf4, nullptr);

    lo_pool.recycle(lp_buf2);
    lo_pool.recycle(lp_buf4);
}

TEST_F(c_p8_buffer_pool_test, stat_reports_budget_max_and_free_headroom)
{
    // stat() reports the budget cap as mz_max_size and the exact free headroom
    // (max - outstanding) as mz_free_size. Nothing is outstanding yet, so the
    // whole budget reads as free even though only part of it is pre-allocated.
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8u * 1024u);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.init(4);

    cp8_buffer_pool::s_stat lo_stat = {};
    lo_pool.lock();
    lo_pool.stat(lo_stat);
    lo_pool.unlock();

    EXPECT_EQ(lo_stat.mz_buffer_size, 1024u);
    EXPECT_EQ(lo_stat.mz_max_size, 8u * 1024u);
    // nothing outstanding => the full budget is free, not just the 4 allocated
    // buffers.
    EXPECT_EQ(lo_stat.mz_free_size, 8u * 1024u);

    // 100% free => not under pressure.
    EXPECT_EQ(lo_stat.mz_free_size * 100ull / lo_stat.mz_max_size, 100u);
}

TEST_F(c_p8_buffer_pool_test, stat_tracks_outstanding_after_acquire)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8u * 1024u);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.init(4);

    lo_pool.lock();
    uint8_t *lp_buf1                = lo_pool.acquire_no_lock();
    uint8_t *lp_buf2                = lo_pool.acquire_no_lock();
    uint8_t *lp_buf3                = lo_pool.acquire_no_lock();

    cp8_buffer_pool::s_stat lo_stat = {};
    lo_pool.stat(lo_stat);
    lo_pool.unlock();

    ASSERT_NE(lp_buf1, nullptr);
    ASSERT_NE(lp_buf2, nullptr);
    ASSERT_NE(lp_buf3, nullptr);

    // 3 buffers outstanding: free = max - outstanding, max stays at the budget.
    EXPECT_EQ(lo_stat.mz_max_size, 8u * 1024u);
    EXPECT_EQ(lo_stat.mz_free_size, (8u - 3u) * 1024u);

    lo_pool.recycle(lp_buf1);
    lo_pool.recycle(lp_buf2);
    lo_pool.recycle(lp_buf3);
}

TEST_F(c_p8_buffer_pool_test, stat_free_shrinks_with_on_demand_growth)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8u * 1024u);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.init(1);

    lo_pool.lock();
    uint8_t *lp_buf1                = lo_pool.acquire_no_lock();
    uint8_t *lp_buf2                = lo_pool.acquire_no_lock();

    cp8_buffer_pool::s_stat lo_stat = {};
    lo_pool.stat(lo_stat);
    lo_pool.unlock();

    ASSERT_NE(lp_buf1, nullptr);
    ASSERT_NE(lp_buf2, nullptr);

    // pool grew on demand from 1 to 2 buffers, both outstanding: free drops by
    // 2 buffers off the budget cap.
    EXPECT_EQ(lo_stat.mz_max_size, 8u * 1024u);
    EXPECT_EQ(lo_stat.mz_free_size, (8u - 2u) * 1024u);

    lo_pool.recycle(lp_buf1);
    lo_pool.recycle(lp_buf2);
}

TEST_F(c_p8_buffer_pool_test, recycle_null_is_safe)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(8192);
    cp8_buffer_pool lo_pool(1024, lp_budget);

    lo_pool.recycle(nullptr);

    EXPECT_EQ(lo_pool.get_free_count(), 0u);
}

TEST_F(c_p8_buffer_pool_test, two_pools_share_budget)
{
    auto            lp_budget = std::make_shared<cp8_memory_budget>(4 * 1024);
    cp8_buffer_pool lo_data_pool(2 * 1024, lp_budget);
    cp8_buffer_pool lo_mini_pool(1024, lp_budget);

    lo_data_pool.lock();
    uint8_t *lp_data = lo_data_pool.acquire_no_lock();
    lo_data_pool.unlock();
    ASSERT_NE(lp_data, nullptr);
    EXPECT_EQ(lp_budget->get_used(), 2u * 1024u);

    lo_mini_pool.lock();
    uint8_t *lp_mini1 = lo_mini_pool.acquire_no_lock();
    uint8_t *lp_mini2 = lo_mini_pool.acquire_no_lock();
    lo_mini_pool.unlock();
    ASSERT_NE(lp_mini1, nullptr);
    ASSERT_NE(lp_mini2, nullptr);
    EXPECT_EQ(lp_budget->get_used(), 4u * 1024u);

    lo_mini_pool.lock();
    uint8_t *lp_mini3 = lo_mini_pool.acquire_no_lock();
    lo_mini_pool.unlock();
    EXPECT_EQ(lp_mini3, nullptr);

    lo_data_pool.lock();
    uint8_t *lp_data_extra = lo_data_pool.acquire_no_lock();
    lo_data_pool.unlock();
    EXPECT_EQ(lp_data_extra, nullptr);

    lo_data_pool.recycle(lp_data);
    lo_mini_pool.recycle(lp_mini1);
    lo_mini_pool.recycle(lp_mini2);
}

TEST_F(c_p8_buffer_pool_test, destructor_releases_budget)
{
    auto lp_budget = std::make_shared<cp8_memory_budget>(4 * 1024);

    {
        cp8_buffer_pool lo_pool(1024, lp_budget);
        EXPECT_EQ(lo_pool.init(3), 3u);
        EXPECT_EQ(lp_budget->get_used(), 3u * 1024u);
    }

    EXPECT_EQ(lp_budget->get_used(), 0u);

    cp8_buffer_pool lo_pool2(1024, lp_budget);
    EXPECT_EQ(lo_pool2.init(4), 4u);
    EXPECT_EQ(lp_budget->get_used(), 4u * 1024u);
}

TEST_F(c_p8_buffer_pool_test, budget_outlives_pool_via_shared_ownership)
{
    // Reverse destruction order: the pool dies first while the budget is
    // still kept alive by the test's shared_ptr. The pool's destructor calls
    // mp_budget->release() on its leftover reservation, and the budget must
    // remain valid for that call. With shared_ptr the order is enforced by
    // refcount — no manual sequencing in the owner is needed.
    auto lp_budget = std::make_shared<cp8_memory_budget>(4 * 1024);

    {
        cp8_buffer_pool lo_pool(1024, lp_budget);
        EXPECT_EQ(lo_pool.init(2), 2u);
        EXPECT_EQ(lp_budget->get_used(), 2u * 1024u);
    }

    EXPECT_EQ(lp_budget->get_used(), 0u);
    EXPECT_EQ(lp_budget.use_count(), 1);
}

TEST_F(c_p8_buffer_pool_test, pool_dropping_shared_ptr_keeps_budget_alive)
{
    // Two pools share the budget. Drop the test's own shared_ptr first; the
    // budget must stay alive as long as either pool holds a reference. This
    // is the property that lifts the destruction-order constraint in
    // cp8_core: the budget reference held by each pool is a real owner.
    std::weak_ptr<cp8_memory_budget> lp_weak;
    {
        auto lp_budget = std::make_shared<cp8_memory_budget>(4 * 1024);
        lp_weak        = lp_budget;
        cp8_buffer_pool lo_pool_a(1024, lp_budget);
        cp8_buffer_pool lo_pool_b(1024, lp_budget);

        // drop the test's reference; only pools hold the budget now
        lp_budget.reset();
        EXPECT_FALSE(lp_weak.expired()) << "pools failed to keep the budget alive";

        lo_pool_a.lock();
        uint8_t *lp_buf_a = lo_pool_a.acquire_no_lock();
        lo_pool_a.unlock();
        ASSERT_NE(lp_buf_a, nullptr);

        lo_pool_b.lock();
        uint8_t *lp_buf_b = lo_pool_b.acquire_no_lock();
        lo_pool_b.unlock();
        ASSERT_NE(lp_buf_b, nullptr);
    }
    EXPECT_TRUE(lp_weak.expired()) << "budget leaked past all pool lifetimes";
}

TEST_F(c_p8_buffer_pool_test, pool_without_budget_grows_unbounded)
{
    cp8_buffer_pool lo_pool(64, nullptr);

    std::vector<uint8_t *> lo_bufs;
    lo_bufs.reserve(8);

    for(size_t lz_i = 0; lz_i < 8; ++lz_i)
    {
        lo_pool.lock();
        uint8_t *lp_buf = lo_pool.acquire_no_lock();
        lo_pool.unlock();
        ASSERT_NE(lp_buf, nullptr);
        lo_bufs.push_back(lp_buf);
    }

    for(uint8_t *lp_buf : lo_bufs)
    {
        lo_pool.recycle(lp_buf);
    }
}

TEST_F(c_p8_buffer_pool_test, concurrent_acquire_release)
{
    static constexpr size_t lz_thread_count = 16;
    static constexpr size_t lz_iterations   = 200;
    static constexpr size_t lz_pool_size    = lz_thread_count;

    auto                     lp_budget      = std::make_shared<cp8_memory_budget>(lz_pool_size * 1024);
    cp8_buffer_pool          lo_pool(1024, lp_budget);
    std::latch               lo_start_latch(lz_thread_count);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_threads.emplace_back(
            [&]()
            {
                lo_start_latch.arrive_and_wait();
                for(size_t lz_j = 0; lz_j < lz_iterations; ++lz_j)
                {
                    lo_pool.lock();
                    uint8_t *lp_buf = lo_pool.acquire_no_lock();
                    lo_pool.unlock();
                    if(lp_buf)
                    {
                        lo_pool.recycle(lp_buf);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }
}

TEST_F(c_p8_buffer_pool_test, concurrent_two_pools_share_budget_never_exceeds_limit)
{
    // Two pools of different geometry compete for the same budget under heavy
    // contention. The invariant we enforce: at no point does the sum of live
    // buffers across both pools (each * its own buffer_size) exceed the budget.
    // If try_reserve is racy, two threads could each grow their pool and the
    // total reservation would overshoot the cap.
    static constexpr size_t lz_thread_count     = 16;
    static constexpr size_t lz_iterations       = 5000;
    static constexpr size_t lz_data_buffer_size = 8192;
    static constexpr size_t lz_mini_buffer_size = 1024;
    static constexpr size_t lz_capacity         = lz_data_buffer_size * 4 + lz_mini_buffer_size * 4;

    auto            lp_budget                   = std::make_shared<cp8_memory_budget>(lz_capacity);
    cp8_buffer_pool lo_data_pool(lz_data_buffer_size, lp_budget);
    cp8_buffer_pool lo_mini_pool(lz_mini_buffer_size, lp_budget);

    std::atomic<bool>        lb_invariant_broken { false };
    std::atomic<size_t>      lu_max_used { 0 };
    std::latch               lo_start_latch(lz_thread_count);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_threads.emplace_back(
            [&, lz_i]()
            {
                lo_start_latch.arrive_and_wait();
                for(size_t lz_j = 0; lz_j < lz_iterations; ++lz_j)
                {
                    cp8_buffer_pool *lp_pool = ((lz_i + lz_j) & 1) ? &lo_data_pool : &lo_mini_pool;

                    lp_pool->lock();
                    uint8_t *lp_buf = lp_pool->acquire_no_lock();
                    lp_pool->unlock();

                    size_t lz_used = lp_budget->get_used();
                    if(lz_used > lz_capacity)
                    {
                        lb_invariant_broken.store(true, std::memory_order_relaxed);
                    }
                    size_t lz_prev_max = lu_max_used.load(std::memory_order_relaxed);
                    while(lz_used > lz_prev_max && !lu_max_used.compare_exchange_weak(lz_prev_max, lz_used))
                    {
                    }

                    if(lp_buf)
                    {
                        lp_pool->recycle(lp_buf);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_FALSE(lb_invariant_broken.load()) << "shared budget over-reserved under contention";
    EXPECT_LE(lu_max_used.load(), lz_capacity);
}

TEST_F(c_p8_buffer_pool_test, concurrent_acquire_unique_pointers)
{
    static constexpr size_t lz_thread_count = 8;
    static constexpr size_t lz_per_thread   = 50;

    auto                     lp_budget = std::make_shared<cp8_memory_budget>(lz_thread_count * lz_per_thread * 1024);
    cp8_buffer_pool          lo_pool(1024, lp_budget);
    std::latch               lo_start_latch(lz_thread_count);
    std::vector<std::thread> lo_threads;
    std::vector<std::vector<uint8_t *>> lo_collected(lz_thread_count);

    lo_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_threads.emplace_back(
            [&, lz_i]()
            {
                lo_collected[lz_i].reserve(lz_per_thread);
                lo_start_latch.arrive_and_wait();
                for(size_t lz_j = 0; lz_j < lz_per_thread; ++lz_j)
                {
                    lo_pool.lock();
                    uint8_t *lp_buf = lo_pool.acquire_no_lock();
                    lo_pool.unlock();
                    ASSERT_NE(lp_buf, nullptr);
                    lo_collected[lz_i].push_back(lp_buf);
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    std::unordered_set<uint8_t *> lo_unique;
    for(auto &lo_vec : lo_collected)
    {
        for(uint8_t *lp_buf : lo_vec)
        {
            EXPECT_TRUE(lo_unique.insert(lp_buf).second) << "duplicate buffer pointer";
            lo_pool.recycle(lp_buf);
        }
    }

    EXPECT_EQ(lo_unique.size(), lz_thread_count * lz_per_thread);
}

TEST_F(c_p8_buffer_pool_test, destructor_wakes_parked_waiter)
{
    // Destroying the pool while a producer is parked in acquire_wait() must wake it
    // (nullptr) and drain the teardown barrier without hanging or a UAF. On the pre-fix
    // code ~cp8_buffer_pool never wakes the waiter, so lb_returned stays false and the
    // second spin_until below times out.
    auto lp_budget = std::make_shared<cp8_memory_budget>(1024);
    auto lp_pool   = std::make_unique<cp8_buffer_pool>(1024, lp_budget);
    ASSERT_EQ(lp_pool->init(1), 1u);

    // exhaust the single buffer so the next acquire must park
    lp_pool->lock();
    ASSERT_NE(lp_pool->acquire_no_lock(), nullptr);
    lp_pool->unlock();

    std::atomic<bool>      lb_returned { false };
    std::atomic<uint8_t *> lp_got { reinterpret_cast<uint8_t *>(0x1) };

    cp8_buffer_pool *lp_raw = lp_pool.get();
    std::thread      lo_waiter(
        [&lb_returned, &lp_got, lp_raw]
        {
            uint8_t *lp_r = lp_raw->acquire_wait();
            lp_got.store(lp_r);
            lb_returned.store(true, std::memory_order_release);
        });

    ASSERT_TRUE(spin_until(std::chrono::seconds(2), [&] { return lp_raw->get_wait_count() == 1; }));

    lp_pool.reset(); // fixed: stop_waiters() + inflight barrier; buggy: waiter never woken -> timeout

    bool lb_ok = spin_until(std::chrono::seconds(2), [&] { return lb_returned.load(std::memory_order_acquire); });
    if(lb_ok)
    {
        lo_waiter.join(); // barrier guaranteed the waiter returned before reset() finished
    }
    else
    {
        lo_waiter.detach(); // buggy path hangs; don't wedge the suite
    }
    ASSERT_TRUE(lb_ok);
    EXPECT_EQ(lp_got.load(), nullptr);
}
