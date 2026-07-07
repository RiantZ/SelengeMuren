#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_config_keys.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace
{
// Build a memory config whose sizes are expressed in whole buffers, so the
// tests stay correct regardless of the compile-time buffer size.
std::string make_mem_config(size_t iz_max_buffers, size_t iz_initial_buffers)
{
    const size_t lz_bufsz = p8_test_get_buffer_size();
    return std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \"" + std::to_string(iz_max_buffers * lz_bufsz)
           + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE + "\": \"" + std::to_string(iz_initial_buffers * lz_bufsz)
           + "\"}";
}
} // namespace

class c_p8_core_test : public ::testing::Test
{
protected:
    void TearDown() override
    {
        p8_release();
    }
};

TEST_F(c_p8_core_test, single_thread_initialize)
{
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{}";

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_TRUE(p8_get_initialized());
    EXPECT_EQ(p8_test_get_instance_count(), 1u);
}

TEST_F(c_p8_core_test, double_initialize_same_thread)
{
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{}";

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_TRUE(p8_get_initialized());
    EXPECT_EQ(p8_test_get_instance_count(), 1u);
}

TEST_F(c_p8_core_test, concurrent_initialize)
{
    static constexpr uint32_t lu_thread_count = 16;

    struct s_p8_config lo_config              = {};
    lo_config.mp_json_config                  = "{}";

    std::latch               lo_start_latch(lu_thread_count);
    std::vector<std::thread> lo_threads;
    std::vector<uint8_t>     lo_results(lu_thread_count, 0);

    lo_threads.reserve(lu_thread_count);
    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        lo_threads.emplace_back(
            [&, lu_i]()
            {
                lo_start_latch.arrive_and_wait();
                lo_results[lu_i] = p8_initialize(&lo_config);
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_TRUE(p8_get_initialized());
    EXPECT_EQ(p8_test_get_instance_count(), 1u);

    for(uint32_t lu_i = 0; lu_i < lu_thread_count; ++lu_i)
    {
        EXPECT_TRUE(lo_results[lu_i]) << "thread " << lu_i << " failed";
    }
}

TEST_F(c_p8_core_test, null_config_fails)
{
    EXPECT_FALSE(p8_initialize(nullptr));
    EXPECT_FALSE(p8_get_initialized());
}

TEST_F(c_p8_core_test, invalid_json_fails)
{
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{not valid json}";

    EXPECT_FALSE(p8_initialize(&lo_config));
    EXPECT_FALSE(p8_get_initialized());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// buffer pool tests
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_core_test, buffer_pool_preallocated)
{
    const std::string  ls_config = make_mem_config(/*max*/ 16, /*initial*/ 8);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_GT(p8_test_get_buffer_size(), 0u);
    EXPECT_EQ(p8_test_get_free_buffers_count(), 8u);
}

TEST_F(c_p8_core_test, buffer_pool_initial_clamped_to_max)
{
    // initial (256 buffers) far exceeds max (2 buffers) → clamped to max
    const std::string  ls_config = make_mem_config(/*max*/ 2, /*initial*/ 256);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_EQ(p8_test_get_free_buffers_count(), 2u);
}

TEST_F(c_p8_core_test, buffer_acquire_release)
{
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{"
                                   "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"128KB\","
                                   "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"64KB\""
                                   "}";

    EXPECT_TRUE(p8_initialize(&lo_config));

    size_t   lz_free_before = p8_test_get_free_buffers_count();
    uint8_t *lp_buf         = p8_test_acquire_buffer();
    ASSERT_NE(lp_buf, nullptr);
    EXPECT_EQ(p8_test_get_free_buffers_count(), lz_free_before - 1);

    p8_test_release_buffer(lp_buf);
    EXPECT_EQ(p8_test_get_free_buffers_count(), lz_free_before);
}

TEST_F(c_p8_core_test, buffer_acquire_on_demand)
{
    // 2 pre-allocated, may grow on demand up to 3 (max)
    const std::string  ls_config = make_mem_config(/*max*/ 3, /*initial*/ 2);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_EQ(p8_test_get_free_buffers_count(), 2u);

    uint8_t *lp_buf1 = p8_test_acquire_buffer();
    uint8_t *lp_buf2 = p8_test_acquire_buffer();
    ASSERT_NE(lp_buf1, nullptr);
    ASSERT_NE(lp_buf2, nullptr);
    EXPECT_EQ(p8_test_get_free_buffers_count(), 0u);

    uint8_t *lp_buf3 = p8_test_acquire_buffer();
    ASSERT_NE(lp_buf3, nullptr);

    uint8_t *lp_buf4 = p8_test_acquire_buffer();
    EXPECT_EQ(lp_buf4, nullptr);

    p8_test_release_buffer(lp_buf1);
    p8_test_release_buffer(lp_buf2);
    p8_test_release_buffer(lp_buf3);
}

TEST_F(c_p8_core_test, buffer_acquire_on_demand_within_limit)
{
    // 8 pre-allocated, generous max so on-demand growth stays within the limit
    const std::string  ls_config = make_mem_config(/*max*/ 128, /*initial*/ 8);
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    EXPECT_TRUE(p8_initialize(&lo_config));

    size_t lz_pre_count = p8_test_get_free_buffers_count();
    EXPECT_EQ(lz_pre_count, 8u);

    // acquire all pre-allocated
    std::vector<uint8_t *> lo_bufs;
    lo_bufs.reserve(lz_pre_count);
    for(size_t lz_i = 0; lz_i < lz_pre_count; ++lz_i)
    {
        uint8_t *lp_buf = p8_test_acquire_buffer();
        ASSERT_NE(lp_buf, nullptr);
        lo_bufs.push_back(lp_buf);
    }
    EXPECT_EQ(p8_test_get_free_buffers_count(), 0u);

    // next acquire triggers on-demand allocation
    uint8_t *lp_on_demand = p8_test_acquire_buffer();
    ASSERT_NE(lp_on_demand, nullptr);

    // release all
    p8_test_release_buffer(lp_on_demand);
    for(uint8_t *lp_buf : lo_bufs)
    {
        p8_test_release_buffer(lp_buf);
    }
}

TEST_F(c_p8_core_test, buffer_concurrent_acquire_release)
{
    static constexpr uint32_t lu_thread_count = 16;
    static constexpr uint32_t lu_iterations   = 100;

    struct s_p8_config lo_config              = {};
    lo_config.mp_json_config                  = "{"
                                                "\"" P8_CFG_KEY_MAX_MEMORY_SIZE "\": \"1MB\","
                                                "\"" P8_CFG_KEY_INITIAL_MEMORY_SIZE "\": \"1MB\""
                                                "}";

    EXPECT_TRUE(p8_initialize(&lo_config));
    size_t lz_initial_free = p8_test_get_free_buffers_count();

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
                    uint8_t *lp_buf = p8_test_acquire_buffer();
                    if(lp_buf)
                    {
                        p8_test_release_buffer(lp_buf);
                    }
                }
            });
    }

    for(auto &lo_t : lo_threads)
    {
        lo_t.join();
    }

    EXPECT_EQ(p8_test_get_free_buffers_count(), lz_initial_free);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// get_global_core tests
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_core_test, get_global_core_after_init)
{
    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = "{}";

    EXPECT_TRUE(p8_initialize(&lo_config));

    cp8_core *lp_core = cp8_core::get_global_core(0);
    EXPECT_NE(lp_core, nullptr);
    EXPECT_TRUE(lp_core->get_initialized());
}

TEST_F(c_p8_core_test, get_global_core_timeout_no_init)
{
    auto lo_start     = std::chrono::steady_clock::now();

    cp8_core *lp_core = cp8_core::get_global_core(50);

    auto lo_elapsed
        = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lo_start);

    EXPECT_EQ(lp_core, nullptr);
    EXPECT_GE(lo_elapsed.count(), 50);
    EXPECT_LE(lo_elapsed.count(), 200);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// file.bin sink tests
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
std::filesystem::path make_scratch_dir(const char *ip_stem)
{
    const auto      lo_root = std::filesystem::temp_directory_path()
                              / (std::string(ip_stem) + "_" + std::to_string(static_cast<uint64_t>(std::rand())));
    std::error_code lo_ec;
    std::filesystem::remove_all(lo_root, lo_ec);
    return lo_root;
}
} // namespace

TEST_F(c_p8_core_test, file_bin_sink_writes_service_data_end_to_end)
{
    const auto lo_out_dir = make_scratch_dir("p8_core_filebin_ok");

    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_SINK]                              = P8_CFG_VAL_SINK_FILE_BIN;
    lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = lo_out_dir.string();
    const std::string ls_config                           = lo_json.dump();

    struct s_p8_config lo_config                          = {};
    lo_config.mp_json_config                              = ls_config.c_str();

    ASSERT_TRUE(p8_initialize(&lo_config));
    EXPECT_TRUE(P8_IS_ATTR_VALID(p8_attr_register("test_attr", e_p8_attr_str)));

    p8_release();

    ASSERT_TRUE(std::filesystem::exists(lo_out_dir));

    bool lb_found_nonempty_svc = false;
    for(const auto &lo_entry : std::filesystem::recursive_directory_iterator(lo_out_dir))
    {
        if(lo_entry.path().extension() == ".p8svc" && std::filesystem::file_size(lo_entry.path()) > 0)
        {
            lb_found_nonempty_svc = true;
        }
    }
    EXPECT_TRUE(lb_found_nonempty_svc);

    std::error_code lo_ec;
    std::filesystem::remove_all(lo_out_dir, lo_ec);
}

TEST_F(c_p8_core_test, file_bin_sink_open_failure_falls_back_to_null_sink)
{
    // a regular file in place of the OutDir directory makes create_directories fail
    const auto lo_blocker = make_scratch_dir("p8_core_filebin_blocked");
    {
        std::ofstream lo_out(lo_blocker);
        lo_out << "not a directory";
    }
    const auto lo_out_dir = lo_blocker / "subdir";

    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_SINK]                              = P8_CFG_VAL_SINK_FILE_BIN;
    lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = lo_out_dir.string();
    const std::string ls_config                           = lo_json.dump();

    struct s_p8_config lo_config                          = {};
    lo_config.mp_json_config                              = ls_config.c_str();

    EXPECT_TRUE(p8_initialize(&lo_config));
    EXPECT_TRUE(p8_get_initialized());

    std::error_code lo_ec;
    std::filesystem::remove_all(lo_blocker, lo_ec);
}
