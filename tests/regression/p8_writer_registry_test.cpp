#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_log.hpp"
#include "p8_tls_writer.hpp"

#include <gtest/gtest.h>

#include <latch>
#include <memory>
#include <thread>
#include <vector>

class c_writer_registry_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{}";
        ASSERT_TRUE(p8_initialize(&lo_config));
    }

    void TearDown() override
    {
        p8_release();
    }
};

TEST_F(c_writer_registry_test, single_writer_register_unregister)
{
    EXPECT_EQ(p8_test_get_writer_count(), 0u);

    {
        cp8_log lo_writer;
        EXPECT_EQ(p8_test_get_writer_count(), 1u);
    }

    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}

TEST_F(c_writer_registry_test, three_writers_per_thread)
{
    EXPECT_EQ(p8_test_get_writer_count(), 0u);

    {
        cp8_log lo_log;
        cp8_log lo_mtk;
        cp8_log lo_trc;
        EXPECT_EQ(p8_test_get_writer_count(), 3u);
    }

    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}

TEST_F(c_writer_registry_test, unregister_middle_writer)
{
    auto lp_w1 = std::make_unique<cp8_log>();
    auto lp_w2 = std::make_unique<cp8_log>();
    auto lp_w3 = std::make_unique<cp8_log>();
    EXPECT_EQ(p8_test_get_writer_count(), 3u);

    lp_w2.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 2u);

    lp_w1.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 1u);

    lp_w3.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}

TEST_F(c_writer_registry_test, unregister_head_writer)
{
    auto lp_w1              = std::make_unique<cp8_log>();
    auto lp_w2              = std::make_unique<cp8_log>();
    auto lp_w3              = std::make_unique<cp8_log>();

    cp8_tls_writer *lp_head = p8_test_get_writers_head();
    ASSERT_NE(lp_head, nullptr);

    lp_w3.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 2u);
    EXPECT_NE(p8_test_get_writers_head(), nullptr);

    lp_w2.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 1u);

    lp_w1.reset();
    EXPECT_EQ(p8_test_get_writer_count(), 0u);
    EXPECT_EQ(p8_test_get_writers_head(), nullptr);
}

TEST_F(c_writer_registry_test, concurrent_register_unregister)
{
    static constexpr uint32_t lu_thread_count = 16;

    std::latch               lo_start_latch(lu_thread_count);
    std::latch               lo_registered_latch(lu_thread_count);
    std::latch               lo_release_latch(1);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lu_thread_count);

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&]()
            {
                lo_start_latch.arrive_and_wait();
                cp8_log lo_writer;
                lo_registered_latch.count_down();
                lo_release_latch.wait();
            });
    }

    lo_registered_latch.wait();
    EXPECT_EQ(p8_test_get_writer_count(), lu_thread_count);

    lo_release_latch.count_down();
    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}

TEST_F(c_writer_registry_test, concurrent_register_unregister_churn)
{
    static constexpr uint32_t lu_thread_count = 16;
    static constexpr uint32_t lu_iterations   = 100;

    std::latch               lo_start_latch(lu_thread_count);
    std::vector<std::thread> lo_threads;
    lo_threads.reserve(lu_thread_count);

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&]()
            {
                lo_start_latch.arrive_and_wait();
                for(uint32_t lu_j = 0; lu_j < lu_iterations; ++lu_j)
                {
                    cp8_log lo_writer;
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}

TEST_F(c_writer_registry_test, no_core_writer_is_noop)
{
    p8_release();

    cp8_log lo_writer;
    EXPECT_EQ(p8_test_get_writer_count(), 0u);
}
