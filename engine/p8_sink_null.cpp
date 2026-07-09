#include "p8_sink_null.hpp"
#include "p8_protocol.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_null::open()
{
    mu_total = 0;
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_null::write_hello(const struct s_p8_hdr &ir_hdr)
{
    mu_total += sizeof(ir_hdr);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_null::write_data(const kit::c_lst<uint8_t *> &ip_buffers)
{
    for(const auto &it : ip_buffers)
    {
        const s_p8_data_buf_hdr *lp_hdr  = reinterpret_cast<const s_p8_data_buf_hdr *>(it);
        mu_total                        += lp_hdr->mu_size;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_null::write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers)
{
    for(const auto &it : ip_buffers)
    {
        mu_total += it.mz_used;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_sink_null::flush()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_sink_null::close()
{
    const int64_t lu_total = mu_total.load();
    const char   *lp_unit  = "B";
    double        ld_value = (double)lu_total;

    if(lu_total >= (1ll << 30))
    {
        ld_value = (double)lu_total / (double)(1ll << 30);
        lp_unit  = "GB";
    }
    else if(lu_total >= (1ll << 20))
    {
        ld_value = (double)lu_total / (double)(1ll << 20);
        lp_unit  = "MB";
    }
    else if(lu_total >= (1ll << 10))
    {
        ld_value = (double)lu_total / (double)(1ll << 10);
        lp_unit  = "KB";
    }

    printf("cp8_sink_null: %.2f %s (%lld) processed\n", ld_value, lp_unit, (long long)lu_total);
}
