#include "p8_client_api.h"
#include "p8_core.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>
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

std::vector<s_svc_entry> collect_service_entries()
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

// create a metric on a dedicated thread (fresh TLS writer per test); the metric
// descriptor is serialized synchronously into the core service buffer.
h_p8_mtk_id create_mtk_in_thread(const s_p8_mtk_base *ip_base)
{
    h_p8_mtk_id li_id = -1;
    std::thread lo_thread([&]() { li_id = p8_mtk_create(ip_base); });
    lo_thread.join();
    return li_id;
}

std::string read_str(const std::vector<uint8_t> &ir_bytes, size_t iz_off, size_t iz_len)
{
    if(iz_off + iz_len > ir_bytes.size())
    {
        return std::string();
    }
    return std::string(reinterpret_cast<const char *>(ir_bytes.data() + iz_off), iz_len);
}

// locate the single P8_SVC_TYPE_MTK entry that carries the given id
bool find_mtk_entry(const std::vector<s_svc_entry> &ir_entries, h_p8_mtk_id ii_id, s_svc_entry &or_entry)
{
    for(const auto &lr_entry : ir_entries)
    {
        if(lr_entry.mu_type != P8_SVC_TYPE_MTK)
        {
            continue;
        }
        s_p8_mtk_svc lo_mtk = {};
        if(lr_entry.mo_bytes.size() < sizeof(s_p8_mtk_svc))
        {
            continue;
        }
        memcpy(&lo_mtk, lr_entry.mo_bytes.data(), sizeof(lo_mtk));
        if(lo_mtk.mi_id == ii_id)
        {
            or_entry = lr_entry;
            return true;
        }
    }
    return false;
}

} // namespace

class c_mtk_svc_serialize_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = "{}";
        ASSERT_TRUE(p8_initialize(&lo_config));

        // Freeze the worker so it does not drain the serialized service buffers
        // out from under the snapshot taken by each test.
        p8_test_enable_buffer_capture();
    }

    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// descriptor fields
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_svc_serialize_test, create_serializes_entry)
{
    s_p8_mtk_base lo_base  = {};
    lo_base.mp_name        = "cpu.load";
    lo_base.mp_description = "cpu load average";
    lo_base.mp_unit        = "percent";
    lo_base.mb_on          = true;
    lo_base.md_min         = 0.0;
    lo_base.md_max         = 100.0;

    h_p8_mtk_id li_id      = create_mtk_in_thread(&lo_base);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));

    s_svc_entry lo_entry;
    ASSERT_TRUE(find_mtk_entry(collect_service_entries(), li_id, lo_entry));

    s_p8_mtk_svc lo_mtk = {};
    memcpy(&lo_mtk, lo_entry.mo_bytes.data(), sizeof(lo_mtk));

    EXPECT_EQ(lo_mtk.ms_hdr.mu_packet_type, P8_PACKET_SERVICE);
    EXPECT_EQ(lo_mtk.ms_hdr.mu_svc_type, P8_SVC_TYPE_MTK);
    EXPECT_EQ(lo_mtk.ms_hdr.mu_size % 8u, 0u);
    EXPECT_EQ(lo_mtk.ms_hdr.mu_size, lo_entry.mo_bytes.size());
    EXPECT_DOUBLE_EQ(lo_mtk.md_min, 0.0);
    EXPECT_DOUBLE_EQ(lo_mtk.md_max, 100.0);
    EXPECT_EQ(lo_mtk.mu_flags & P8_MTK_FLAG_ON, static_cast<uint8_t>(P8_MTK_FLAG_ON));
    EXPECT_EQ(lo_mtk.mu_attrs_count, 0u);
    EXPECT_EQ(lo_mtk.mu_name_len, 8u);  // "cpu.load"
    EXPECT_EQ(lo_mtk.mu_desc_len, 16u); // "cpu load average"
    EXPECT_EQ(lo_mtk.mu_unit_len, 7u);  // "percent"

    size_t lz_off = sizeof(s_p8_mtk_svc);
    EXPECT_EQ(read_str(lo_entry.mo_bytes, lz_off, lo_mtk.mu_name_len), "cpu.load");
    lz_off += lo_mtk.mu_name_len;
    EXPECT_EQ(read_str(lo_entry.mo_bytes, lz_off, lo_mtk.mu_desc_len), "cpu load average");
    lz_off += lo_mtk.mu_desc_len;
    EXPECT_EQ(read_str(lo_entry.mo_bytes, lz_off, lo_mtk.mu_unit_len), "percent");
}

TEST_F(c_mtk_svc_serialize_test, off_state_clears_flag_and_null_optionals)
{
    s_p8_mtk_base lo_base = {};
    lo_base.mp_name       = "off_metric";
    lo_base.mb_on         = false;
    // description and unit left null

    h_p8_mtk_id li_id     = create_mtk_in_thread(&lo_base);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));

    s_svc_entry lo_entry;
    ASSERT_TRUE(find_mtk_entry(collect_service_entries(), li_id, lo_entry));

    s_p8_mtk_svc lo_mtk = {};
    memcpy(&lo_mtk, lo_entry.mo_bytes.data(), sizeof(lo_mtk));

    EXPECT_EQ(lo_mtk.mu_flags & P8_MTK_FLAG_ON, 0u);
    EXPECT_EQ(lo_mtk.mu_desc_len, 0u);
    EXPECT_EQ(lo_mtk.mu_unit_len, 0u);
    EXPECT_EQ(read_str(lo_entry.mo_bytes, sizeof(s_p8_mtk_svc), lo_mtk.mu_name_len), "off_metric");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// embedded attributes
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_svc_serialize_test, attributes_embedded_in_descriptor)
{
    p8_attr_id li_host = p8_attr_register("host", e_p8_attr_str);
    p8_attr_id li_pid  = p8_attr_register("pid", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_host));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_pid));

    s_p8_attr_val lo_attrs[2] = {};
    lo_attrs[0].m_id          = li_host;
    lo_attrs[0].mp_str        = "server1";
    lo_attrs[1].m_id          = li_pid;
    lo_attrs[1].mu_u64        = 4242;

    s_p8_mtk_base lo_base     = {};
    lo_base.mp_name           = "req.count";
    lo_base.mz_attrs          = 2;
    lo_base.mp_attrs          = lo_attrs;

    h_p8_mtk_id li_id         = create_mtk_in_thread(&lo_base);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));

    s_svc_entry lo_entry;
    ASSERT_TRUE(find_mtk_entry(collect_service_entries(), li_id, lo_entry));

    s_p8_mtk_svc lo_mtk = {};
    memcpy(&lo_mtk, lo_entry.mo_bytes.data(), sizeof(lo_mtk));
    EXPECT_EQ(lo_mtk.mu_attrs_count, 2u);

    // tail layout: [name][description][unit] then attribute id+value pairs
    size_t lz_off  = sizeof(s_p8_mtk_svc) + lo_mtk.mu_name_len + lo_mtk.mu_desc_len + lo_mtk.mu_unit_len;

    // attr 0: string -> [id:int32][u16 len + utf8]
    int32_t li_id0 = 0;
    memcpy(&li_id0, lo_entry.mo_bytes.data() + lz_off, sizeof(int32_t));
    lz_off           += sizeof(int32_t);
    uint16_t lu_len0  = 0;
    memcpy(&lu_len0, lo_entry.mo_bytes.data() + lz_off, sizeof(uint16_t));
    lz_off              += sizeof(uint16_t);
    std::string lo_val0  = read_str(lo_entry.mo_bytes, lz_off, lu_len0);
    lz_off              += lu_len0;
    EXPECT_EQ(li_id0, li_host);
    EXPECT_EQ(lo_val0, "server1");

    // attr 1: numeric -> [id:int32][u64]
    int32_t li_id1 = 0;
    memcpy(&li_id1, lo_entry.mo_bytes.data() + lz_off, sizeof(int32_t));
    lz_off           += sizeof(int32_t);
    uint64_t lu_val1  = 0;
    memcpy(&lu_val1, lo_entry.mo_bytes.data() + lz_off, sizeof(uint64_t));
    EXPECT_EQ(li_id1, li_pid);
    EXPECT_EQ(lu_val1, 4242u);
}

TEST_F(c_mtk_svc_serialize_test, unregistered_attribute_is_skipped)
{
    s_p8_attr_val lo_attrs[1] = {};
    lo_attrs[0].m_id          = 9999; // never registered
    lo_attrs[0].mu_u64        = 1;

    s_p8_mtk_base lo_base     = {};
    lo_base.mp_name           = "skip_attr";
    lo_base.mz_attrs          = 1;
    lo_base.mp_attrs          = lo_attrs;

    h_p8_mtk_id li_id         = create_mtk_in_thread(&lo_base);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_id));

    s_svc_entry lo_entry;
    ASSERT_TRUE(find_mtk_entry(collect_service_entries(), li_id, lo_entry));

    s_p8_mtk_svc lo_mtk = {};
    memcpy(&lo_mtk, lo_entry.mo_bytes.data(), sizeof(lo_mtk));
    EXPECT_EQ(lo_mtk.mu_attrs_count, 0u); // invalid attr skipped
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// id assignment
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_mtk_svc_serialize_test, multiple_metrics_have_sequential_ids)
{
    s_p8_mtk_base lo_a = {};
    lo_a.mp_name       = "m_a";
    s_p8_mtk_base lo_b = {};
    lo_b.mp_name       = "m_b";

    h_p8_mtk_id li_a   = create_mtk_in_thread(&lo_a);
    h_p8_mtk_id li_b   = create_mtk_in_thread(&lo_b);
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_a));
    ASSERT_TRUE(P8_IS_METRIC_VALID(li_b));
    EXPECT_EQ(li_b, li_a + 1);
}

TEST_F(c_mtk_svc_serialize_test, invalid_base_returns_negative)
{
    // null base and empty name must fail without serializing an entry
    EXPECT_FALSE(P8_IS_METRIC_VALID(create_mtk_in_thread(nullptr)));

    s_p8_mtk_base lo_empty = {};
    lo_empty.mp_name       = "";
    EXPECT_FALSE(P8_IS_METRIC_VALID(create_mtk_in_thread(&lo_empty)));

    for(const auto &lr_entry : collect_service_entries())
    {
        EXPECT_NE(lr_entry.mu_type, P8_SVC_TYPE_MTK);
    }
}
