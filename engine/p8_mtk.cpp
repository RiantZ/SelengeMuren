#include "p8_mtk.hpp"
#include "p8_protocol.h"

#include "kit/time.hpp"

#include <mutex>

static thread_local cp8_mtk go_tls_mtk;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_mtk::cp8_mtk()
    : cp8_tls_writer(&mo_lock)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
h_p8_mtk_id cp8_mtk::create(const struct s_p8_mtk_base *ip_base)
{
    return mp_core ? mp_core->register_mtk(ip_base) : -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_mtk::emit(h_p8_mtk_id ih_id, double id_value)
{
    if(!mp_core) [[unlikely]]
    {
        return false;
    }

    // bounds-check the id against the registry (lock-free). A never-created id is a
    // caller error, not a capacity drop, so it does not bump the drop counter.
    if(ih_id < 0 || static_cast<uint32_t>(ih_id) >= mp_core->get_mtk_count())
    {
        return false;
    }

    std::lock_guard<kit::c_spin_lock> lo_guard(mo_lock);

    // buffer availability — reuse the current buffer while it can still hold a full
    // item; otherwise park it (drained later by the worker via pull()). Every item
    // is a multiple of 8 bytes and so is the buffer header, so mz_buf_used stays
    // 8-aligned without any per-item padding.
    if(mp_buffer) [[likely]]
    {
        if((mz_buf_max - mz_buf_used) < sizeof(s_p8_mtk_item_dat)) [[unlikely]]
        {
            mo_fragments.push_last(mp_buffer);
            mp_buffer = nullptr;
        }
    }

    if(!mp_buffer) [[unlikely]]
    {
        mp_buffer = mp_core->acquire_buffer();
        if(!mp_buffer) [[unlikely]]
        {
            mu_dropped_metrics.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_packet_type    = P8_PACKET_METRICS;
        lp_buf_hdr->mu_flags          = 0;
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(sizeof(s_p8_data_buf_hdr));
        lp_buf_hdr->mu_thread_id      = mu_thread_id;
        lp_buf_hdr->mu_start_time     = kit::get_hires_ticks();
        lp_buf_hdr->mu_stop_time      = 0;

        mz_buf_used                   = sizeof(s_p8_data_buf_hdr);
    }

    // write the fixed-size sample item (guaranteed to fit after the check above)
    uint64_t lu_timestamp = kit::get_hires_ticks();
    {
        s_p8_mtk_item_dat *lp_hdr  = reinterpret_cast<s_p8_mtk_item_dat *>(mp_buffer + mz_buf_used);

        lp_hdr->mu_timestamp       = lu_timestamp;
        lp_hdr->md_value           = id_value;
        lp_hdr->mi_id              = ih_id;
        lp_hdr->mu_size            = static_cast<uint16_t>(sizeof(s_p8_mtk_item_dat));
        lp_hdr->mu_flags           = 0;
        lp_hdr->mu_attrs_count     = 0;

        mz_buf_used               += sizeof(s_p8_mtk_item_dat);
    }

    // update the data-buffer header
    {
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(mz_buf_used);
        lp_buf_hdr->mu_stop_time      = lu_timestamp;
    }

    // park the buffer for the worker once it can no longer hold another item
    if((mz_buf_max - mz_buf_used) < sizeof(s_p8_mtk_item_dat)) [[unlikely]]
    {
        mo_fragments.push_last(mp_buffer);
        mp_buffer   = nullptr;
        mz_buf_used = 0;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
h_p8_mtk_id cp8_mtk::create_query(const struct s_p8_mtk_base *ip_base,
                                  uint32_t                    iu_query_interval_ms,
                                  l_p8_mtk_query_cb           il_query,
                                  void                       *ip_user_context)
{
    (void)ip_base;
    (void)iu_query_interval_ms;
    (void)il_query;
    (void)ip_user_context;
    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
h_p8_mtk_group_id cp8_mtk::create_group(const struct s_p8_mtk_base *ip_base, bool ib_multi_thread)
{
    (void)ip_base;
    (void)ib_multi_thread;
    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
h_p8_mtk_group_id cp8_mtk::create_group_query(const struct s_p8_mtk_base *ip_base,
                                              uint32_t                    iu_query_interval_ms,
                                              l_p8_mtk_group_query_cb     il_query,
                                              void                       *ip_user_context)
{
    (void)ip_base;
    (void)iu_query_interval_ms;
    (void)il_query;
    (void)ip_user_context;
    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_mtk::begin_group_emit(h_p8_mtk_group_id ih_group_id)
{
    (void)ih_group_id;
    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_mtk::emit_group(const char *ip_name, double id_value)
{
    (void)ip_name;
    (void)id_value;
    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_mtk::end_group_emit(h_p8_mtk_group_id ih_group_id)
{
    (void)ih_group_id;
    return false;
}

extern "C"
{
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    h_p8_mtk_id p8_mtk_create(const struct s_p8_mtk_base *ip_base)
    {
        return go_tls_mtk.create(ip_base);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_mtk_emit(h_p8_mtk_id ih_id, double id_value)
    {
        return go_tls_mtk.emit(ih_id, id_value);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    h_p8_mtk_id p8_mtk_create_query(const struct s_p8_mtk_base *ip_base,
                                    uint32_t                    iu_query_interval_ms,
                                    l_p8_mtk_query_cb           il_query,
                                    void                       *ip_user_context)
    {
        return go_tls_mtk.create_query(ip_base, iu_query_interval_ms, il_query, ip_user_context);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    h_p8_mtk_group_id p8_mtk_create_group(const struct s_p8_mtk_base *ip_base, bool ib_multi_thread)
    {
        return go_tls_mtk.create_group(ip_base, ib_multi_thread);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    h_p8_mtk_group_id p8_mtk_create_group_query(const struct s_p8_mtk_base *ip_base,
                                                uint32_t                    iu_query_interval_ms,
                                                l_p8_mtk_group_query_cb     il_query,
                                                void                       *ip_user_context)
    {
        return go_tls_mtk.create_group_query(ip_base, iu_query_interval_ms, il_query, ip_user_context);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_mtk_group_emit_begin(h_p8_mtk_group_id ih_group_id)
    {
        return go_tls_mtk.begin_group_emit(ih_group_id);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_mtk_group_emit(const char *ip_name, double id_value)
    {
        return go_tls_mtk.emit_group(ip_name, id_value);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_mtk_group_emit_end(h_p8_mtk_group_id ih_group_id)
    {
        return go_tls_mtk.end_group_emit(ih_group_id);
    }

} // extern "C"
