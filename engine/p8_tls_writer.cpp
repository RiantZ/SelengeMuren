#include "p8_tls_writer.hpp"
#include "p8_protocol.h"

#include <cstring>
#include <functional>
#include <thread>

#include "kit/time.hpp"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_tls_writer::cp8_tls_writer(kit::c_spin_lock *ip_lock)
    : mp_core(cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS))
    , mu_thread_id(static_cast<uint32_t>(std::hash<std::thread::id> {}(std::this_thread::get_id())))
    , mp_lock(ip_lock)
{
    if(mp_core)
    {
        mp_core->addref();
        mz_buf_max = mp_core->get_buffer_size();
        mp_core->register_writer(this);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_tls_writer::~cp8_tls_writer()
{
    if(mp_core)
    {
        core_push();
        // Flush any residual drop counts before leaving the registry so a
        // short-lived thread's losses are never lost.
        mp_core->accumulate_dropped(pull_dropped());
        mp_core->unregister_writer(this);
        mp_core->release();
        mp_core = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_tls_writer::rotate_fragment_buffer()
{
    s_p8_data_buf_hdr *lp_buf_hdr      = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
    uint8_t            lu_packet_type  = lp_buf_hdr->mu_packet_type;

    lp_buf_hdr->mu_flags              |= P8_DATA_FLAG_FRAGMENT;
    lp_buf_hdr->mu_size                = static_cast<uint16_t>(mz_buf_used);

    mo_fragments.push_last(mp_buffer);

    // Non-blocking on purpose: this runs mid-record (a single item is being split
    // across buffers under the writer spinlock). Blocking here would hold the
    // spinlock across the wait (deadlocking the worker/recycler) and pin partial
    // fragments the worker cannot flush. On exhaustion the caller rolls the record
    // back via lz_frag_commit instead.
    mp_buffer = mp_core->acquire_buffer(false);
    if(!mp_buffer) [[unlikely]]
    {
        return false;
    }

    lp_buf_hdr                 = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
    lp_buf_hdr->mu_packet_type = lu_packet_type;
    lp_buf_hdr->mu_flags       = 0;
    lp_buf_hdr->mu_size        = static_cast<uint16_t>(sizeof(s_p8_data_buf_hdr));
    lp_buf_hdr->mu_thread_id   = mu_thread_id;
    lp_buf_hdr->mu_writer_id   = mu_writer_id;
    lp_buf_hdr->mu_buf_seq     = mu_buf_seq++;

    mz_buf_used                = sizeof(s_p8_data_buf_hdr);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_tls_writer::copy_fragmented(uint8_t   *&io_dst,
                                     uint8_t   *&io_buf_end,
                                     const void *ip_src,
                                     size_t      iz_len,
                                     size_t     &oz_written)
{
    const uint8_t *lp_src       = static_cast<const uint8_t *>(ip_src);
    size_t         lz_remaining = iz_len;

    while(lz_remaining > 0)
    {
        size_t lz_space = static_cast<size_t>(io_buf_end - io_dst);
        if(0 == lz_space) [[unlikely]]
        {
            mz_buf_used = static_cast<size_t>(io_dst - mp_buffer);
            // push current buffer to fragments list and pull new buffer for data
            if(!rotate_fragment_buffer())
            {
                return false;
            }
            io_dst     = mp_buffer + mz_buf_used;
            io_buf_end = mp_buffer + mz_buf_max;
            lz_space   = static_cast<size_t>(io_buf_end - io_dst);
        }
        size_t lz_copy = (lz_remaining < lz_space) ? lz_remaining : lz_space;
        memcpy(io_dst, lp_src, lz_copy);
        io_dst       += lz_copy;
        lp_src       += lz_copy;
        lz_remaining -= lz_copy;
        oz_written   += lz_copy;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_tls_writer::core_push()
{
    std::lock_guard<kit::c_spin_lock> lo_guard(*mp_lock);

    // Submit the whole logical record as a single bundle: fold the current tail
    // buffer into the fragment chain so the handoff takes the ready lock once.
    if(mo_fragments.size() > 0)
    {
        if(mp_buffer)
        {
            mo_fragments.push_last(mp_buffer);
            mp_buffer = nullptr;
        }
        mp_core->submit_chain(mo_fragments);
    }
    else if(mp_buffer)
    {
        mp_core->submit_buffer(mp_buffer);
        mp_buffer = nullptr;
    }

    // keep the invariant: no current buffer => no bytes used
    mz_buf_used = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_tls_writer::pull(kit::c_lst<uint8_t *> &io_data)
{
    std::lock_guard<kit::c_spin_lock> lo_guard(*mp_lock);

    // Move the finalized fragment/complete-record buffers first to preserve the
    // logical write order, then fold the in-progress tail buffer in behind them.
    while(mo_fragments.size() > 0)
    {
        io_data.push_last(mo_fragments.pull_first());
    }

    if(mp_buffer)
    {
        io_data.push_last(mp_buffer);
        mp_buffer   = nullptr;
        mz_buf_used = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_p8_drop_stats cp8_tls_writer::pull_dropped()
{
    s_p8_drop_stats lo_stats;
    lo_stats.mu_logs    = mu_dropped_logs.exchange(0, std::memory_order_relaxed);
    lo_stats.mu_metrics = mu_dropped_metrics.exchange(0, std::memory_order_relaxed);
    lo_stats.mu_traces  = mu_dropped_traces.exchange(0, std::memory_order_relaxed);
    lo_stats.mu_svc     = 0; // writers never emit service descriptors
    return lo_stats;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_p8_attrs_result cp8_tls_writer::serialize_attrs(uint8_t                   *&io_dst,
                                                  uint8_t                   *&io_buf_end,
                                                  size_t                      iz_record,
                                                  const struct s_p8_attr_val *ip_attrs,
                                                  size_t                      iz_attrs)
{
    s_p8_attrs_result lo_result = { true, 0, 0 };

    for(size_t lz_i = 0; ip_attrs && lz_i < iz_attrs && lo_result.mu_count < UINT8_MAX; ++lz_i)
    {
        const p8_attr_id li_id   = ip_attrs[lz_i].m_id;
        const size_t     lz_idx  = static_cast<size_t>(li_id);
        const size_t     lz_used = iz_record + lo_result.mz_bytes;
        const size_t     lz_room = (lz_used < P8_MAX_RECORD_SIZE) ? P8_MAX_RECORD_SIZE - lz_used : 0;
        const void      *lp_val  = &ip_attrs[lz_i].mu_u64;
        size_t           lz_len  = sizeof(uint64_t);
        size_t           lz_size = sizeof(p8_attr_id) + sizeof(uint64_t);
        uint16_t         lu_len  = 0;
        bool             lb_str  = false;

        if(li_id < 0)
        {
            continue;
        }

        if(lz_idx >= mo_attr_cache.size() || !mo_attr_cache[lz_idx])
        {
            mp_core->sync_attr_cache(mo_attr_cache);
            if(lz_idx >= mo_attr_cache.size() || !mo_attr_cache[lz_idx])
            {
                continue;
            }
        }

        if(e_p8_attr_str == mo_attr_cache[lz_idx]->me_type)
        {
            const char *lp_str = ip_attrs[lz_i].mp_str;
            if(lz_room < sizeof(p8_attr_id) + sizeof(uint16_t))
            {
                continue;
            }

            // a string longer than the room left is cut on a UTF-8 boundary
            lz_len = lp_str ? strlen(lp_str) : 0;
            if(lz_len > lz_room - sizeof(p8_attr_id) - sizeof(uint16_t))
            {
                lz_len = p8_cut_utf8(lp_str, lz_room - sizeof(p8_attr_id) - sizeof(uint16_t));
            }
            lu_len  = static_cast<uint16_t>(lz_len);
            lp_val  = lp_str;
            lz_size = sizeof(p8_attr_id) + sizeof(uint16_t) + lz_len;
            lb_str  = true;
        }
        else if(lz_room < lz_size)
        {
            continue;
        }

        if(lz_size <= static_cast<size_t>(io_buf_end - io_dst)) [[likely]]
        {
            // fixed sizes for the number, so the copies compile to plain stores
            memcpy(io_dst, &li_id, sizeof(p8_attr_id));
            if(lb_str)
            {
                memcpy(io_dst + sizeof(p8_attr_id), &lu_len, sizeof(lu_len));
                if(lz_len)
                {
                    memcpy(io_dst + sizeof(p8_attr_id) + sizeof(lu_len), lp_val, lz_len);
                }
            }
            else
            {
                memcpy(io_dst + sizeof(p8_attr_id), &ip_attrs[lz_i].mu_u64, sizeof(uint64_t));
            }
            io_dst += lz_size;
        }
        else
        {
            size_t lz_put = 0;
            if(!copy_fragmented(io_dst, io_buf_end, &li_id, sizeof(p8_attr_id), lz_put)
               || (lb_str && !copy_fragmented(io_dst, io_buf_end, &lu_len, sizeof(lu_len), lz_put))
               || !copy_fragmented(io_dst, io_buf_end, lp_val, lz_len, lz_put))
            {
                lo_result.mb_done = false;
                break;
            }
        }

        lo_result.mz_bytes += lz_size;
        ++lo_result.mu_count;
    }

    return lo_result;
}
