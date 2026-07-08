#include "p8_client_api.h"

#include <gtest/gtest.h>

// Registry-level tests for the log-module API (register/find + verbosity
// get/set). These exercise only the global module registry in cp8_core and do
// not emit any records, so a bare "{}" config is enough to bring the core up.
class c_log_module_test : public ::testing::Test
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

TEST_F(c_log_module_test, register_new_returns_handle)
{
    p_p8_module lp_mod = P8_MODULE_INVALID_ID;
    EXPECT_TRUE(p8_log_register_module("mod_a", e_p8_info0, &lp_mod));
    EXPECT_NE(lp_mod, P8_MODULE_INVALID_ID);
}

TEST_F(c_log_module_test, register_stores_requested_verbosity)
{
    p_p8_module lp_mod = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("mod_v", e_p8_warning0, &lp_mod));
    EXPECT_EQ(p8_log_get_verbosity(lp_mod), e_p8_warning0);
}

TEST_F(c_log_module_test, register_duplicate_returns_same_handle)
{
    p_p8_module lp_first  = P8_MODULE_INVALID_ID;
    p_p8_module lp_second = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("dup", e_p8_info0, &lp_first));
    ASSERT_TRUE(p8_log_register_module("dup", e_p8_info0, &lp_second));
    EXPECT_EQ(lp_first, lp_second);
}

TEST_F(c_log_module_test, register_duplicate_updates_verbosity)
{
    p_p8_module lp_first  = P8_MODULE_INVALID_ID;
    p_p8_module lp_second = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("dup_v", e_p8_info0, &lp_first));
    ASSERT_TRUE(p8_log_register_module("dup_v", e_p8_error0, &lp_second));
    EXPECT_EQ(lp_first, lp_second);
    EXPECT_EQ(p8_log_get_verbosity(lp_first), e_p8_error0);
}

TEST_F(c_log_module_test, find_existing_returns_registered_handle)
{
    p_p8_module lp_reg = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("findme", e_p8_debug0, &lp_reg));
    EXPECT_EQ(p8_log_find_module("findme"), lp_reg);
}

TEST_F(c_log_module_test, find_missing_returns_invalid)
{
    EXPECT_EQ(p8_log_find_module("does_not_exist"), P8_MODULE_INVALID_ID);
}

TEST_F(c_log_module_test, set_get_verbosity_on_module)
{
    p_p8_module lp_mod = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("setget", e_p8_trace0, &lp_mod));
    p8_log_set_verbosity(lp_mod, e_p8_warning0);
    EXPECT_EQ(p8_log_get_verbosity(lp_mod), e_p8_warning0);
}

TEST_F(c_log_module_test, set_get_default_verbosity_null_module)
{
    p8_log_set_verbosity(nullptr, e_p8_error0);
    EXPECT_EQ(p8_log_get_verbosity(nullptr), e_p8_error0);
}

TEST_F(c_log_module_test, register_null_name_fails_and_clears_out)
{
    int         li_dummy = 0;
    p_p8_module lp_mod   = &li_dummy;
    EXPECT_FALSE(p8_log_register_module(nullptr, e_p8_info0, &lp_mod));
    EXPECT_EQ(lp_mod, P8_MODULE_INVALID_ID);
}

TEST_F(c_log_module_test, register_empty_name_fails_and_clears_out)
{
    int         li_dummy = 0;
    p_p8_module lp_mod   = &li_dummy;
    EXPECT_FALSE(p8_log_register_module("", e_p8_info0, &lp_mod));
    EXPECT_EQ(lp_mod, P8_MODULE_INVALID_ID);
}

TEST_F(c_log_module_test, register_null_out_fails)
{
    EXPECT_FALSE(p8_log_register_module("noout", e_p8_info0, nullptr));
}
