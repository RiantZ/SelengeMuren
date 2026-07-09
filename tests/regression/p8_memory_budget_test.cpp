#include "p8_memory_budget.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

class c_p8_memory_budget_test : public ::testing::Test
{
};

TEST_F(c_p8_memory_budget_test, initial_state)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_EQ(lo_budget.get_max(), 1024u);
    EXPECT_EQ(lo_budget.get_used(), 0u);
}

TEST_F(c_p8_memory_budget_test, reserve_within_limit_succeeds)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(256));
    EXPECT_EQ(lo_budget.get_used(), 256u);

    EXPECT_TRUE(lo_budget.try_reserve(512));
    EXPECT_EQ(lo_budget.get_used(), 768u);
}

TEST_F(c_p8_memory_budget_test, reserve_exact_capacity_succeeds)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(1024));
    EXPECT_EQ(lo_budget.get_used(), 1024u);
}

TEST_F(c_p8_memory_budget_test, reserve_beyond_limit_fails)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(900));
    EXPECT_FALSE(lo_budget.try_reserve(200));
    EXPECT_EQ(lo_budget.get_used(), 900u);
}

TEST_F(c_p8_memory_budget_test, reserve_zero_always_succeeds)
{
    cp8_memory_budget lo_budget(0);
    EXPECT_TRUE(lo_budget.try_reserve(0));
    EXPECT_EQ(lo_budget.get_used(), 0u);
}

TEST_F(c_p8_memory_budget_test, reserve_from_empty_budget_fails)
{
    cp8_memory_budget lo_budget(0);

    EXPECT_FALSE(lo_budget.try_reserve(1));
    EXPECT_EQ(lo_budget.get_used(), 0u);
}

TEST_F(c_p8_memory_budget_test, release_returns_capacity)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(800));
    EXPECT_FALSE(lo_budget.try_reserve(300));

    lo_budget.release(500);
    EXPECT_EQ(lo_budget.get_used(), 300u);

    EXPECT_TRUE(lo_budget.try_reserve(500));
    EXPECT_EQ(lo_budget.get_used(), 800u);
}

TEST_F(c_p8_memory_budget_test, reserve_failure_does_not_modify_used)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(700));
    EXPECT_FALSE(lo_budget.try_reserve(400));
    EXPECT_EQ(lo_budget.get_used(), 700u);
}

TEST_F(c_p8_memory_budget_test, concurrent_reserve_release_stays_within_limit)
{
    // Stress test: many threads compete for a small budget. Each successful
    // reserve increments a shared counter; releases decrement it. If
    // try_reserve had a race (e.g. plain load+store instead of CAS-loop), the
    // counter would briefly exceed the slot capacity and the assertion below
    // would fire. The counter is updated *under the reservation*, so the
    // observation is anchored to the moment we believe we own a slot.
    static constexpr size_t lz_thread_count = 32;
    static constexpr size_t lz_iterations   = 20000;
    static constexpr size_t lz_chunk_size   = 64;
    static constexpr size_t lz_max_slots    = 8;
    static constexpr size_t lz_capacity     = lz_chunk_size * lz_max_slots;

    cp8_memory_budget        lo_budget(lz_capacity);
    std::atomic<size_t>      lu_concurrent_holders { 0 };
    std::atomic<size_t>      lu_max_holders { 0 };
    std::atomic<size_t>      lu_total_success { 0 };
    std::atomic<size_t>      lu_total_failure { 0 };
    std::atomic<bool>        lb_invariant_broken { false };
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
                    if(lo_budget.try_reserve(lz_chunk_size))
                    {
                        size_t lz_holders = lu_concurrent_holders.fetch_add(1, std::memory_order_acq_rel) + 1;

                        // hard invariant: live holders × chunk must never exceed capacity
                        if(lz_holders > lz_max_slots)
                        {
                            lb_invariant_broken.store(true, std::memory_order_relaxed);
                        }

                        // track high-water-mark for diagnostic output
                        size_t lz_prev_max = lu_max_holders.load(std::memory_order_relaxed);
                        while(lz_holders > lz_prev_max
                              && !lu_max_holders.compare_exchange_weak(lz_prev_max, lz_holders))
                        {
                        }

                        // cross-check: get_used() reports a consistent value while we hold the slot
                        size_t lz_used = lo_budget.get_used();
                        if(lz_used > lz_capacity)
                        {
                            lb_invariant_broken.store(true, std::memory_order_relaxed);
                        }

                        lu_concurrent_holders.fetch_sub(1, std::memory_order_acq_rel);
                        lo_budget.release(lz_chunk_size);
                        lu_total_success.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        lu_total_failure.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_FALSE(lb_invariant_broken.load()) << "budget over-reserved under contention";
    EXPECT_EQ(lo_budget.get_used(), 0u) << "leak after symmetric reserve/release";
    EXPECT_EQ(lu_concurrent_holders.load(), 0u);
    EXPECT_LE(lu_max_holders.load(), lz_max_slots);

    // sanity: the stress loop must have exercised the success path
    size_t lz_success = lu_total_success.load();
    size_t lz_failure = lu_total_failure.load();
    EXPECT_EQ(lz_success + lz_failure, lz_thread_count * lz_iterations);
    EXPECT_GT(lz_success, 0u) << "no successful reservations — test degenerate";

    // Deterministic rejection burst: every thread attempts a single reserve at
    // once and holds it until all threads have attempted, so the budget stays
    // saturated for the whole burst. With more threads than slots, exactly
    // lz_max_slots reservations succeed and the rest MUST be rejected — this
    // validates the try_reserve failure path without relying on scheduling
    // timing (the loop above cannot guarantee overlap on a fast machine).
    std::atomic<size_t>      lu_burst_success { 0 };
    std::atomic<size_t>      lu_burst_failure { 0 };
    std::latch               lo_burst_latch(lz_thread_count);
    std::latch               lo_attempt_done(lz_thread_count);
    std::vector<std::thread> lo_burst_threads;
    lo_burst_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_burst_threads.emplace_back(
            [&]()
            {
                lo_burst_latch.arrive_and_wait();
                bool lb_reserved = lo_budget.try_reserve(lz_chunk_size);
                if(lb_reserved)
                {
                    lu_burst_success.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    lu_burst_failure.fetch_add(1, std::memory_order_relaxed);
                }

                // hold the slot until every thread has attempted, keeping the
                // budget saturated so rejections are guaranteed
                lo_attempt_done.arrive_and_wait();
                if(lb_reserved)
                {
                    lo_budget.release(lz_chunk_size);
                }
            });
    }

    for(auto &lo_t : lo_burst_threads)
    {
        lo_t.join();
    }

    EXPECT_EQ(lu_burst_success.load(), lz_max_slots) << "budget admitted the wrong number of concurrent reservations";
    EXPECT_EQ(lu_burst_failure.load(), lz_thread_count - lz_max_slots)
        << "try_reserve failed to reject over-capacity requests";
    EXPECT_EQ(lo_budget.get_used(), 0u) << "leak after burst reserve/release";
}

TEST_F(c_p8_memory_budget_test, concurrent_reserve_release_exact_accounting)
{
    // Pure accounting test: every successful try_reserve in any thread must
    // be matched by exactly one release in the same thread, with random sizes.
    // get_used() at the end must be zero — this catches the classic
    // "increment by N but rollback by M" kind of bug if it ever creeps in.
    static constexpr size_t lz_thread_count = 16;
    static constexpr size_t lz_iterations   = 50000;
    static constexpr size_t lz_capacity     = 1024 * 1024;

    cp8_memory_budget        lo_budget(lz_capacity);
    std::atomic<size_t>      lu_success { 0 };
    std::latch               lo_start_latch(lz_thread_count);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_threads.emplace_back(
            [&, lz_i]()
            {
                // deterministic per-thread pseudo-random sequence (no RNG dep)
                size_t lz_seed = (lz_i + 1) * 1103515245u;
                lo_start_latch.arrive_and_wait();
                for(size_t lz_j = 0; lz_j < lz_iterations; ++lz_j)
                {
                    lz_seed         = lz_seed * 1103515245u + 12345u;
                    size_t lz_bytes = 1 + (lz_seed % 256);

                    if(lo_budget.try_reserve(lz_bytes))
                    {
                        lu_success.fetch_add(1, std::memory_order_relaxed);
                        lo_budget.release(lz_bytes);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_EQ(lo_budget.get_used(), 0u) << "asymmetric reserve/release accounting";
    EXPECT_GT(lu_success.load(), 0u);
}

TEST_F(c_p8_memory_budget_test, concurrent_failed_reserve_does_not_consume_budget)
{
    // Pre-fill the budget to near-full, then hammer it with try_reserve calls
    // that should all fail. If a failed try_reserve mutates mu_used (even
    // briefly visible to another thread), this catches it.
    static constexpr size_t lz_thread_count = 16;
    static constexpr size_t lz_iterations   = 10000;
    static constexpr size_t lz_capacity     = 1024;
    static constexpr size_t lz_pre_fill     = 1000;

    cp8_memory_budget lo_budget(lz_capacity);
    ASSERT_TRUE(lo_budget.try_reserve(lz_pre_fill));

    std::latch               lo_start_latch(lz_thread_count);
    std::vector<std::thread> lo_threads;
    std::atomic<bool>        lb_unexpected_success { false };
    lo_threads.reserve(lz_thread_count);

    for(size_t lz_i = 0; lz_i < lz_thread_count; ++lz_i)
    {
        lo_threads.emplace_back(
            [&]()
            {
                lo_start_latch.arrive_and_wait();
                for(size_t lz_j = 0; lz_j < lz_iterations; ++lz_j)
                {
                    // remaining budget is 24 bytes; every 100-byte reserve must fail
                    if(lo_budget.try_reserve(100))
                    {
                        lb_unexpected_success.store(true, std::memory_order_relaxed);
                        lo_budget.release(100);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_FALSE(lb_unexpected_success.load());
    EXPECT_EQ(lo_budget.get_used(), lz_pre_fill);
    lo_budget.release(lz_pre_fill);
    EXPECT_EQ(lo_budget.get_used(), 0u);
}

TEST_F(c_p8_memory_budget_test, partial_reserve_then_release_full_cycle)
{
    cp8_memory_budget lo_budget(1024);

    EXPECT_TRUE(lo_budget.try_reserve(300));
    EXPECT_TRUE(lo_budget.try_reserve(500));
    EXPECT_EQ(lo_budget.get_used(), 800u);

    lo_budget.release(300);
    lo_budget.release(500);
    EXPECT_EQ(lo_budget.get_used(), 0u);

    EXPECT_TRUE(lo_budget.try_reserve(1024));
}
