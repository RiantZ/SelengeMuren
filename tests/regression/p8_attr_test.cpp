#include "p8_client_api.h"
#include "p8_core.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <latch>
#include <thread>
#include <vector>

class c_p8_attr_test : public ::testing::Test
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

class c_p8_attr_no_init_test : public ::testing::Test
{
protected:
    void TearDown() override
    {
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// registration basics
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_attr_test, register_returns_valid_id)
{
    p8_attr_id li_id = p8_attr_register("user_id", e_p8_attr_i64);
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_id));
    EXPECT_EQ(li_id, 0);
}

TEST_F(c_p8_attr_test, register_multiple_distinct_ids)
{
    p8_attr_id li_a = p8_attr_register("attr_a", e_p8_attr_i64);
    p8_attr_id li_b = p8_attr_register("attr_b", e_p8_attr_f64);
    p8_attr_id li_c = p8_attr_register("attr_c", e_p8_attr_str);

    EXPECT_TRUE(P8_IS_ATTR_VALID(li_a));
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_b));
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_c));

    EXPECT_NE(li_a, li_b);
    EXPECT_NE(li_b, li_c);
    EXPECT_NE(li_a, li_c);
}

TEST_F(c_p8_attr_test, register_all_types)
{
    p8_attr_id li_str = p8_attr_register("a_str", e_p8_attr_str);
    p8_attr_id li_f64 = p8_attr_register("a_f64", e_p8_attr_f64);
    p8_attr_id li_i64 = p8_attr_register("a_i64", e_p8_attr_i64);
    p8_attr_id li_u64 = p8_attr_register("a_u64", e_p8_attr_u64);

    EXPECT_TRUE(P8_IS_ATTR_VALID(li_str));
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_f64));
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_i64));
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_u64));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// idempotent re-registration
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_attr_test, reregister_same_type_returns_same_id)
{
    p8_attr_id li_first  = p8_attr_register("user_id", e_p8_attr_i64);
    p8_attr_id li_second = p8_attr_register("user_id", e_p8_attr_i64);

    EXPECT_TRUE(P8_IS_ATTR_VALID(li_first));
    EXPECT_EQ(li_first, li_second);
}

TEST_F(c_p8_attr_test, reregister_different_type_returns_error)
{
    p8_attr_id li_first = p8_attr_register("user_id", e_p8_attr_i64);
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_first));

    p8_attr_id li_bad = p8_attr_register("user_id", e_p8_attr_str);
    EXPECT_EQ(li_bad, P8_ATTR_ERROR_TYPE_MISMATCH);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// lookup
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_attr_test, get_returns_registered_id)
{
    p8_attr_id li_reg = p8_attr_register("session", e_p8_attr_str);
    p8_attr_id li_got = p8_attr_get("session");
    EXPECT_EQ(li_reg, li_got);
}

TEST_F(c_p8_attr_test, get_unknown_returns_not_found)
{
    EXPECT_EQ(p8_attr_get("nonexistent"), P8_ATTR_ERROR_NOT_FOUND);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// error cases
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_attr_test, register_null_name)
{
    EXPECT_EQ(p8_attr_register(nullptr, e_p8_attr_i64), P8_ATTR_ERROR_INVALID_NAME);
}

TEST_F(c_p8_attr_test, register_empty_name)
{
    EXPECT_EQ(p8_attr_register("", e_p8_attr_i64), P8_ATTR_ERROR_INVALID_NAME);
}

TEST_F(c_p8_attr_test, get_null_name)
{
    EXPECT_EQ(p8_attr_get(nullptr), P8_ATTR_ERROR_NOT_FOUND);
}

TEST_F(c_p8_attr_test, get_empty_name)
{
    EXPECT_EQ(p8_attr_get(""), P8_ATTR_ERROR_NOT_FOUND);
}

TEST_F(c_p8_attr_no_init_test, register_without_init)
{
    EXPECT_EQ(p8_attr_register("x", e_p8_attr_i64), P8_ATTR_ERROR_NOT_INITIALIZED);
}

TEST_F(c_p8_attr_no_init_test, get_without_init)
{
    EXPECT_EQ(p8_attr_get("x"), P8_ATTR_ERROR_NOT_FOUND);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// thread safety
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_attr_test, concurrent_register_same_name)
{
    static constexpr uint32_t lu_thread_count = 16;

    std::latch               lo_start(lu_thread_count);
    std::vector<std::thread> lo_threads;
    std::vector<p8_attr_id>  lo_ids(lu_thread_count, -1);

    lo_threads.reserve(lu_thread_count);
    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&, lu_i]()
            {
                lo_start.arrive_and_wait();
                lo_ids[lu_i] = p8_attr_register("shared_attr", e_p8_attr_u64);
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    p8_attr_id li_expected = lo_ids[0];
    EXPECT_TRUE(P8_IS_ATTR_VALID(li_expected));

    for(uint32_t lu_i = 1; lu_i < lu_thread_count; ++lu_i)
    {
        EXPECT_EQ(lo_ids[lu_i], li_expected) << "thread " << lu_i << " got different ID";
    }
}

TEST_F(c_p8_attr_test, concurrent_register_distinct_names)
{
    static constexpr uint32_t lu_thread_count = 16;

    std::latch               lo_start(lu_thread_count);
    std::vector<std::thread> lo_threads;
    std::vector<p8_attr_id>  lo_ids(lu_thread_count, -1);

    lo_threads.reserve(lu_thread_count);
    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&, lu_i]()
            {
                lo_start.arrive_and_wait();
                std::string ls_name = "attr_" + std::to_string(lu_i);
                lo_ids[lu_i]        = p8_attr_register(ls_name.c_str(), e_p8_attr_i64);
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        EXPECT_TRUE(P8_IS_ATTR_VALID(lo_ids[lu_i])) << "thread " << lu_i << " failed";
    }

    // all IDs must be unique
    std::vector<p8_attr_id> lo_sorted(lo_ids.begin(), lo_ids.end());
    std::sort(lo_sorted.begin(), lo_sorted.end());
    auto lo_dup = std::adjacent_find(lo_sorted.begin(), lo_sorted.end());
    EXPECT_EQ(lo_dup, lo_sorted.end()) << "duplicate ID found";
}

TEST_F(c_p8_attr_test, concurrent_register_and_get)
{
    p8_attr_id li_pre = p8_attr_register("pre_registered", e_p8_attr_f64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_pre));

    static constexpr uint32_t lu_thread_count = 16;

    std::latch               lo_start(lu_thread_count);
    std::vector<std::thread> lo_threads;
    std::vector<p8_attr_id>  lo_get_results(lu_thread_count, -1);

    lo_threads.reserve(lu_thread_count);
    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&, lu_i]()
            {
                lo_start.arrive_and_wait();
                lo_get_results[lu_i] = p8_attr_get("pre_registered");
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        EXPECT_EQ(lo_get_results[lu_i], li_pre) << "thread " << lu_i << " got wrong ID";
    }
}
