#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_log.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// helpers: walk the serialized P8_PACKET_SERVICE buffers into individual entries
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{

struct s_svc_entry
{
    uint8_t              mu_type = 0;
    std::vector<uint8_t> mo_bytes; // whole entry, including its s_p8_svc_hdr
};

static std::vector<s_svc_entry> collect_service_entries()
{
    std::vector<s_svc_entry> lo_entries;

    const auto lo_bufs = p8_test_get_service_buffers();
    for(const auto &lr_buf : lo_bufs)
    {
        // service buffers carry no data-buffer header: entries start at offset 0
        size_t lz_off = 0;
        while(lz_off + sizeof(s_p8_svc_hdr) <= lr_buf.size())
        {
            s_p8_svc_hdr lo_hdr = {};
            memcpy(&lo_hdr, lr_buf.data() + lz_off, sizeof(lo_hdr));

            if(lo_hdr.mu_size < sizeof(s_p8_svc_hdr) || lz_off + lo_hdr.mu_size > lr_buf.size())
            {
                break;
            }

            s_svc_entry lo_entry;
            lo_entry.mu_type = lo_hdr.mu_svc_type;
            lo_entry.mo_bytes.assign(lr_buf.data() + lz_off, lr_buf.data() + lz_off + lo_hdr.mu_size);
            lo_entries.push_back(std::move(lo_entry));

            lz_off += lo_hdr.mu_size;
        }
    }

    return lo_entries;
}

static std::string read_cstr(const std::vector<uint8_t> &ir_bytes, size_t iz_off)
{
    std::string lo_str;
    while(iz_off < ir_bytes.size() && ir_bytes[iz_off] != '\0')
    {
        lo_str.push_back(static_cast<char>(ir_bytes[iz_off]));
        ++iz_off;
    }
    return lo_str;
}

} // namespace

class c_p8_svc_serialize_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{}";
        ASSERT_TRUE(p8_initialize(&lo_config));

        // Freeze the background worker so it does not drain the serialized
        // service buffers out from under the snapshot taken by each test.
        // Same mechanism the data-buffer suites use for their captured store.
        p8_test_enable_buffer_capture();
    }

    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// attribute descriptor serialization
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_svc_serialize_test, attr_register_serializes_entry)
{
    p8_attr_id li_id = p8_attr_register("user_id", e_p8_attr_i64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_id));

    auto lo_entries = collect_service_entries();

    int li_found    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_ATTR)
        {
            continue;
        }

        s_p8_attr_svc lo_attr = {};
        ASSERT_GE(lr_entry.mo_bytes.size(), sizeof(s_p8_attr_svc));
        memcpy(&lo_attr, lr_entry.mo_bytes.data(), sizeof(lo_attr));

        if(lo_attr.mi_id != li_id)
        {
            continue;
        }

        ++li_found;

        EXPECT_EQ(lo_attr.ms_hdr.mu_size % 8u, 0u);
        EXPECT_EQ(lo_attr.ms_hdr.mu_size, lr_entry.mo_bytes.size());
        EXPECT_EQ(lo_attr.mu_type, P8_ATTR_TYPE_I64);
        EXPECT_EQ(read_cstr(lr_entry.mo_bytes, sizeof(s_p8_attr_svc)), "user_id");
    }

    EXPECT_EQ(li_found, 1);
}

TEST_F(c_p8_svc_serialize_test, attr_types_map_to_wire_codes)
{
    p8_attr_id li_str = p8_attr_register("a_str", e_p8_attr_str);
    p8_attr_id li_f64 = p8_attr_register("a_f64", e_p8_attr_f64);
    p8_attr_id li_i64 = p8_attr_register("a_i64", e_p8_attr_i64);
    p8_attr_id li_u64 = p8_attr_register("a_u64", e_p8_attr_u64);

    ASSERT_TRUE(P8_IS_ATTR_VALID(li_str));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_u64));

    auto lo_entries = collect_service_entries();

    auto find_type  = [&](p8_attr_id ii_id) -> int
    {
        for(const auto &lr_entry : lo_entries)
        {
            if(lr_entry.mu_type != P8_SVC_TYPE_ATTR)
            {
                continue;
            }
            s_p8_attr_svc lo_attr = {};
            memcpy(&lo_attr, lr_entry.mo_bytes.data(), sizeof(lo_attr));
            if(lo_attr.mi_id == ii_id)
            {
                return lo_attr.mu_type;
            }
        }
        return -1;
    };

    EXPECT_EQ(find_type(li_str), P8_ATTR_TYPE_STR);
    EXPECT_EQ(find_type(li_f64), P8_ATTR_TYPE_F64);
    EXPECT_EQ(find_type(li_i64), P8_ATTR_TYPE_I64);
    EXPECT_EQ(find_type(li_u64), P8_ATTR_TYPE_U64);
}

TEST_F(c_p8_svc_serialize_test, attr_reregister_does_not_duplicate_entry)
{
    p8_attr_id li_first  = p8_attr_register("dup", e_p8_attr_u64);
    p8_attr_id li_second = p8_attr_register("dup", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_first));
    EXPECT_EQ(li_first, li_second);

    auto lo_entries = collect_service_entries();

    int li_count    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_ATTR)
        {
            continue;
        }
        if(read_cstr(lr_entry.mo_bytes, sizeof(s_p8_attr_svc)) == "dup")
        {
            ++li_count;
        }
    }

    EXPECT_EQ(li_count, 1);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// log descriptor serialization
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_svc_serialize_test, log_resolve_serializes_entry)
{
    cp8_core *lp_core = cp8_core::get_global_core(0);
    ASSERT_NE(lp_core, nullptr);

    const uint64_t lu_hash = 0x123456789abcdef0ULL;
    s_p8_log_desc *lp_desc = lp_core->resolve_log_desc(lu_hash, "file.cpp", 42, "func", "%d %s");
    ASSERT_NE(lp_desc, nullptr);

    auto lo_entries = collect_service_entries();

    int li_found    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_LOG_DESC)
        {
            continue;
        }

        s_p8_log_item_svc lo_item = {};
        ASSERT_GE(lr_entry.mo_bytes.size(), sizeof(s_p8_log_item_svc));
        memcpy(&lo_item, lr_entry.mo_bytes.data(), sizeof(lo_item));

        if(lo_item.mu_hash != lu_hash)
        {
            continue;
        }

        ++li_found;

        EXPECT_EQ(lo_item.ms_hdr.mu_size % 8u, 0u);
        EXPECT_EQ(lo_item.ms_hdr.mu_size, lr_entry.mo_bytes.size());
        EXPECT_EQ(lo_item.mu_line, 42u);
        EXPECT_EQ(lo_item.mu_file_len, 8u);     // "file.cpp"
        EXPECT_EQ(lo_item.mu_function_len, 4u); // "func"
        EXPECT_EQ(lo_item.mu_format_len, 5u);   // "%d %s"
        EXPECT_EQ(lo_item.mu_args_count, 2u);   // %d, %s

        size_t      lz_off = sizeof(s_p8_log_item_svc);
        std::string lo_file(reinterpret_cast<const char *>(lr_entry.mo_bytes.data() + lz_off), lo_item.mu_file_len);
        lz_off += lo_item.mu_file_len;
        std::string lo_func(reinterpret_cast<const char *>(lr_entry.mo_bytes.data() + lz_off),
                            lo_item.mu_function_len);
        lz_off += lo_item.mu_function_len;
        std::string lo_fmt(reinterpret_cast<const char *>(lr_entry.mo_bytes.data() + lz_off), lo_item.mu_format_len);

        EXPECT_EQ(lo_file, "file.cpp");
        EXPECT_EQ(lo_func, "func");
        EXPECT_EQ(lo_fmt, "%d %s");
    }

    EXPECT_EQ(li_found, 1);
}

TEST_F(c_p8_svc_serialize_test, log_reresolve_does_not_duplicate_entry)
{
    cp8_core *lp_core = cp8_core::get_global_core(0);
    ASSERT_NE(lp_core, nullptr);

    const uint64_t lu_hash = 0xfeedfacecafebeefULL;
    lp_core->resolve_log_desc(lu_hash, "a.cpp", 7, "fn", "%d");
    lp_core->resolve_log_desc(lu_hash, "a.cpp", 7, "fn", "%d");

    auto lo_entries = collect_service_entries();

    int li_count    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_LOG_DESC)
        {
            continue;
        }
        s_p8_log_item_svc lo_item = {};
        memcpy(&lo_item, lr_entry.mo_bytes.data(), sizeof(lo_item));
        if(lo_item.mu_hash == lu_hash)
        {
            ++li_count;
        }
    }

    EXPECT_EQ(li_count, 1);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// accumulation: multiple descriptors land in the buffer, each 8-byte aligned
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_svc_serialize_test, multiple_entries_are_each_8_byte_aligned)
{
    cp8_core *lp_core = cp8_core::get_global_core(0);
    ASSERT_NE(lp_core, nullptr);

    p8_attr_register("attr_a", e_p8_attr_i64);
    p8_attr_register("attr_b", e_p8_attr_str);
    lp_core->resolve_log_desc(0x1111u, "x.cpp", 1, "fx", "%s");
    lp_core->resolve_log_desc(0x2222u, "y.cpp", 2, "fy", "%d %f");

    auto lo_entries = collect_service_entries();
    EXPECT_GE(lo_entries.size(), 4u);

    for(const auto &lr_entry : lo_entries)
    {
        EXPECT_EQ(lr_entry.mo_bytes.size() % 8u, 0u);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// module descriptor serialization
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_p8_svc_serialize_test, register_module_serializes_entry)
{
    p_p8_module lp_mod = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("net", e_p8_warning0, &lp_mod));
    ASSERT_NE(lp_mod, P8_MODULE_INVALID_ID);

    auto lo_entries = collect_service_entries();

    int li_found    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_MODULE)
        {
            continue;
        }

        s_p8_log_mod_svc lo_mod = {};
        ASSERT_GE(lr_entry.mo_bytes.size(), sizeof(s_p8_log_mod_svc));
        memcpy(&lo_mod, lr_entry.mo_bytes.data(), sizeof(lo_mod));

        if(read_cstr(lr_entry.mo_bytes, sizeof(s_p8_log_mod_svc)) != "net")
        {
            continue;
        }

        ++li_found;

        EXPECT_EQ(lo_mod.ms_hdr.mu_size % 8u, 0u);
        EXPECT_EQ(lo_mod.ms_hdr.mu_size, lr_entry.mo_bytes.size());
        EXPECT_EQ(lo_mod.mu_verb, static_cast<uint8_t>(e_p8_warning0));
    }

    EXPECT_EQ(li_found, 1);
}

TEST_F(c_p8_svc_serialize_test, register_duplicate_module_serializes_once)
{
    p_p8_module lp_a = P8_MODULE_INVALID_ID;
    p_p8_module lp_b = P8_MODULE_INVALID_ID;
    ASSERT_TRUE(p8_log_register_module("dup_mod", e_p8_info0, &lp_a));
    ASSERT_TRUE(p8_log_register_module("dup_mod", e_p8_error0, &lp_b));
    EXPECT_EQ(lp_a, lp_b);

    auto lo_entries = collect_service_entries();

    int li_count    = 0;
    for(const auto &lr_entry : lo_entries)
    {
        if(lr_entry.mu_type == P8_SVC_TYPE_MODULE
           && read_cstr(lr_entry.mo_bytes, sizeof(s_p8_log_mod_svc)) == "dup_mod")
        {
            ++li_count;
        }
    }

    EXPECT_EQ(li_count, 1);
}
