#include "p8_trc.hpp"
#include "p8_hash.hpp"
#include "p8_protocol.h"

#include "kit/time.hpp"

#include <cstring>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_trc::cp8_trc()
    : cp8_tls_writer(&mo_lock)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t cp8_trc::begin(const char                 *ip_name,
                        uint32_t                    iu_line,
                        const char                 *ip_file,
                        const char                 *ip_function,
                        size_t                      iz_attrs,
                        const struct s_p8_attr_val *ip_attrs)
{
    uint64_t lu_parent_id = mo_open_spans.size() ? mo_open_spans.back() : 0;
    uint64_t lu_span_id
        = write_begin(P8_TRC_ITEM_BEGIN, lu_parent_id, ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);

    if(lu_span_id) [[likely]]
    {
        mo_open_spans.push_last(lu_span_id);
    }

    return lu_span_id;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t cp8_trc::begin_async(uint64_t                    iu_parent_id,
                              const char                 *ip_name,
                              uint32_t                    iu_line,
                              const char                 *ip_file,
                              const char                 *ip_function,
                              size_t                      iz_attrs,
                              const struct s_p8_attr_val *ip_attrs)
{
    return write_begin(P8_TRC_ITEM_BEGIN_ASYNC,
                       iu_parent_id,
                       ip_name,
                       iu_line,
                       ip_file,
                       ip_function,
                       iz_attrs,
                       ip_attrs);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_trc::end(uint64_t iu_span_id)
{
    if(!iu_span_id || !mp_core) [[unlikely]]
    {
        return;
    }

    pop_span(iu_span_id);
    write_end(iu_span_id);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_trc::pop_span(uint64_t iu_span_id)
{
    size_t lz_depth = 1;
    auto   lo_it    = mo_open_spans.end();

    // IDs of one writer only grow, so they rise from the bottom of the stack to its top, and an ID above the
    // top one (an async span) is not on the stack
    if(0 == mo_open_spans.size() || iu_span_id > mo_open_spans.back())
    {
        return;
    }

    if(mo_open_spans.back() == iu_span_id) [[likely]]
    {
        mo_open_spans.pull_last();
        return;
    }

    // spans opened inside this one missed their end and close with it; the scan stops at the first older span,
    // so a span of another thread leaves the stack as it is
    --lo_it;
    while(lz_depth < mo_open_spans.size())
    {
        --lo_it;
        ++lz_depth;
        if(*lo_it == iu_span_id)
        {
            for(; lz_depth > 0; --lz_depth)
            {
                mo_open_spans.pull_last();
            }
            return;
        }
        if(*lo_it < iu_span_id)
        {
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_trc::ensure_buffer(std::unique_lock<kit::c_spin_lock> &io_guard, size_t iz_needed)
{
    // a buffer without room for the record is parked; the worker drains it later via pull()
    if(mp_buffer && (mz_buf_max - mz_buf_used) < iz_needed) [[unlikely]]
    {
        mo_fragments.push_last(mp_buffer);
        mp_buffer   = nullptr;
        mz_buf_used = 0;
    }

    if(!mp_buffer) [[unlikely]]
    {
        // wait without the lock, so the worker can pull and recycle buffers meanwhile
        io_guard.unlock();
        uint8_t *lp_new = mp_core->acquire_buffer(true);
        io_guard.lock();

        if(!lp_new) [[unlikely]]
        {
            // acquire_buffer(true) returns nullptr only on shutdown
            return false;
        }

        mp_buffer                     = lp_new;
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_packet_type    = P8_PACKET_TRACES;
        lp_buf_hdr->mu_flags          = 0;
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(sizeof(s_p8_data_buf_hdr));
        lp_buf_hdr->mu_thread_id      = mu_thread_id;
        lp_buf_hdr->mu_writer_id      = mu_writer_id;
        lp_buf_hdr->mu_buf_seq        = mu_buf_seq++;

        mz_buf_used                   = sizeof(s_p8_data_buf_hdr);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_trc::get_attr_size(const struct s_p8_attr_val &ir_attr, size_t iz_record, bool &ob_str, uint16_t &ou_len)
{
    const p8_attr_id li_id   = ir_attr.m_id;
    const size_t     lz_idx  = static_cast<size_t>(li_id);
    const size_t     lz_room = (iz_record < P8_TRC_MAX_RECORD_SIZE) ? P8_TRC_MAX_RECORD_SIZE - iz_record : 0;
    const size_t     lz_head = sizeof(p8_attr_id) + sizeof(uint16_t);
    size_t           lz_len  = 0;

    ob_str                   = false;
    ou_len                   = 0;

    if(li_id < 0)
    {
        return 0;
    }

    if(lz_idx >= mo_attr_cache.size() || !mo_attr_cache[lz_idx])
    {
        mp_core->sync_attr_cache(mo_attr_cache);
        if(lz_idx >= mo_attr_cache.size() || !mo_attr_cache[lz_idx])
        {
            return 0;
        }
    }

    if(e_p8_attr_str != mo_attr_cache[lz_idx]->me_type)
    {
        return (lz_room >= sizeof(p8_attr_id) + sizeof(uint64_t)) ? sizeof(p8_attr_id) + sizeof(uint64_t) : 0;
    }

    if(lz_room < lz_head)
    {
        return 0;
    }

    // a string longer than the room left is cut, on a UTF-8 boundary
    lz_len = ir_attr.mp_str ? strlen(ir_attr.mp_str) : 0;
    if(lz_len > lz_room - lz_head)
    {
        lz_len = p8_cut_utf8(ir_attr.mp_str, lz_room - lz_head);
    }

    ob_str = true;
    ou_len = static_cast<uint16_t>(lz_len);
    return lz_head + lz_len;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t cp8_trc::write_begin(uint8_t                     iu_type,
                              uint64_t                    iu_parent_id,
                              const char                 *ip_name,
                              uint32_t                    iu_line,
                              const char                 *ip_file,
                              const char                 *ip_function,
                              size_t                      iz_attrs,
                              const struct s_p8_attr_val *ip_attrs)
{
    const char         *lp_file          = ip_file ? ip_file : "";
    uint64_t            lu_hash          = 0;
    uint64_t            lu_span_id       = 0;
    s_p8_trc_begin_dat *lp_hdr           = nullptr;
    uint8_t            *lp_dst           = nullptr;
    uint8_t            *lp_buf_end       = nullptr;
    size_t              lz_size          = sizeof(s_p8_trc_begin_dat);
    size_t              lz_attr          = 0;
    size_t              lz_attrs_written = 0;
    size_t              lz_frag_commit   = 0;
    uint8_t             lu_attrs_count   = 0;
    uint16_t            lu_len           = 0;
    bool                lb_str           = false;

    if(!mp_core) [[unlikely]]
    {
        return 0;
    }

    // the descriptor key is the file + line hash, as in logs
    lu_hash = P8_GET_LOG_HASH(lp_file, iu_line);
    if(mo_descs.find(lu_hash) == mo_descs.end()) [[unlikely]]
    {
        // the core may wait here for a service buffer, so no span goes out before its descriptor
        if(!mp_core->resolve_trc_desc(lu_hash, lp_file, iu_line, ip_function, ip_name))
        {
            mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        mo_descs.insert(lu_hash);
    }

    // the size first: a record that fits one buffer is never split, so it cannot fail half written
    for(size_t lz_i = 0; ip_attrs && lz_i < iz_attrs && lu_attrs_count < UINT8_MAX; ++lz_i)
    {
        lz_attr = get_attr_size(ip_attrs[lz_i], lz_size, lb_str, lu_len);
        if(lz_attr)
        {
            lz_size += lz_attr;
            ++lu_attrs_count;
        }
    }
    lz_size        = (lz_size + 7u) & ~static_cast<size_t>(7u);
    lu_attrs_count = 0;

    if(mu_next_span_id == mu_end_span_id) [[unlikely]]
    {
        mu_next_span_id = mp_core->reserve_span_ids();
        mu_end_span_id  = mu_next_span_id + P8_CORE_SPAN_ID_BLOCK;
    }
    lu_span_id = mu_next_span_id++;

    std::unique_lock<kit::c_spin_lock> lo_guard(mo_lock);

    // a bigger record starts in the current buffer and continues in the next ones
    if(!ensure_buffer(lo_guard,
                      (lz_size <= mz_buf_max - sizeof(s_p8_data_buf_hdr)) ? lz_size : sizeof(s_p8_trc_begin_dat)))
        [[unlikely]]
    {
        mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // a rollback of this record (lbl_discard) restores this list length
    lz_frag_commit         = mo_fragments.size();

    lp_hdr                 = reinterpret_cast<s_p8_trc_begin_dat *>(mp_buffer + mz_buf_used);
    lp_hdr->mu_timestamp   = kit::get_hires_ticks();
    lp_hdr->mu_span_id     = lu_span_id;
    lp_hdr->mu_size        = static_cast<uint16_t>(sizeof(s_p8_trc_begin_dat));
    lp_hdr->mu_type        = iu_type;
    lp_hdr->mu_attrs_count = 0;
    lp_hdr->mu_thread_id   = mu_thread_id;
    lp_hdr->mu_hash        = lu_hash;
    lp_hdr->mu_parent_id   = iu_parent_id;

    lp_dst                 = mp_buffer + mz_buf_used + sizeof(s_p8_trc_begin_dat);
    lp_buf_end             = mp_buffer + mz_buf_max;

    for(size_t lz_i = 0; ip_attrs && lz_i < iz_attrs && lu_attrs_count < UINT8_MAX; ++lz_i)
    {
        lz_attr = get_attr_size(ip_attrs[lz_i], sizeof(s_p8_trc_begin_dat) + lz_attrs_written, lb_str, lu_len);
        if(!lz_attr)
        {
            continue;
        }

        if(lz_attr <= static_cast<size_t>(lp_buf_end - lp_dst)) [[likely]]
        {
            memcpy(lp_dst, &ip_attrs[lz_i].m_id, sizeof(p8_attr_id));
            if(lb_str)
            {
                memcpy(lp_dst + sizeof(p8_attr_id), &lu_len, sizeof(lu_len));
                if(lu_len)
                {
                    memcpy(lp_dst + sizeof(p8_attr_id) + sizeof(lu_len), ip_attrs[lz_i].mp_str, lu_len);
                }
            }
            else
            {
                memcpy(lp_dst + sizeof(p8_attr_id), &ip_attrs[lz_i].mu_u64, sizeof(uint64_t));
            }
            lp_dst           += lz_attr;
            lz_attrs_written += lz_attr;
        }
        // the attribute continues in the next buffer byte by byte, so no buffer ends with a gap
        else if(!copy_fragmented(lp_dst, lp_buf_end, &ip_attrs[lz_i].m_id, sizeof(p8_attr_id), lz_attrs_written)
                || (lb_str && !copy_fragmented(lp_dst, lp_buf_end, &lu_len, sizeof(lu_len), lz_attrs_written))
                || !copy_fragmented(lp_dst,
                                    lp_buf_end,
                                    lb_str ? static_cast<const void *>(ip_attrs[lz_i].mp_str) : &ip_attrs[lz_i].mu_u64,
                                    lb_str ? lu_len : sizeof(uint64_t),
                                    lz_attrs_written))
        {
            goto lbl_discard;
        }

        ++lu_attrs_count;
    }

    // without gaps the write offset stays congruent to the record size, so padding the size to 8 also starts
    // the next record 8-aligned in its buffer, and the pad never crosses the buffer end
    {
        size_t lz_used          = sizeof(s_p8_trc_begin_dat) + lz_attrs_written;
        size_t lz_pad           = ((lz_used + 7u) & ~static_cast<size_t>(7u)) - lz_used;
        lp_dst                 += lz_pad;
        lp_hdr->mu_attrs_count  = lu_attrs_count;
        lp_hdr->mu_size         = static_cast<uint16_t>(lz_used + lz_pad);
    }

    mz_buf_used                                               = static_cast<size_t>(lp_dst - mp_buffer);
    reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer)->mu_size = static_cast<uint16_t>(mz_buf_used);

    return lu_span_id;

lbl_discard:
    // rotate_fragment_buffer failed: give back the buffers taken only for this record, with their numbers, as the
    // server never saw them
    while(mo_fragments.size() > lz_frag_commit + 1)
    {
        mp_core->release_buffer(mo_fragments.pull_last());
        --mu_buf_seq;
    }

    // the record began in this buffer at lp_hdr; cut the buffer there and the earlier records stay
    mp_buffer   = mo_fragments.pull_last();
    mz_buf_used = static_cast<size_t>(reinterpret_cast<uint8_t *>(lp_hdr) - mp_buffer);
    if(sizeof(s_p8_data_buf_hdr) == mz_buf_used) [[unlikely]]
    {
        // taken for this record alone
        mp_core->release_buffer(mp_buffer);
        mp_buffer   = nullptr;
        mz_buf_used = 0;
        --mu_buf_seq;
    }
    else
    {
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_flags          = static_cast<uint8_t>(lp_buf_hdr->mu_flags & ~P8_DATA_FLAG_FRAGMENT);
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(mz_buf_used);
    }
    mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_trc::write_end(uint64_t iu_span_id)
{
    std::unique_lock<kit::c_spin_lock> lo_guard(mo_lock);

    if(!ensure_buffer(lo_guard, sizeof(s_p8_trc_end_dat))) [[unlikely]]
    {
        mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    s_p8_trc_end_dat *lp_rec  = reinterpret_cast<s_p8_trc_end_dat *>(mp_buffer + mz_buf_used);
    lp_rec->mu_timestamp      = kit::get_hires_ticks();
    lp_rec->mu_span_id        = iu_span_id;
    lp_rec->mu_size           = static_cast<uint16_t>(sizeof(s_p8_trc_end_dat));
    lp_rec->mu_type           = P8_TRC_ITEM_END;
    lp_rec->mu_padding        = 0;
    lp_rec->mu_thread_id      = mu_thread_id;

    mz_buf_used              += sizeof(s_p8_trc_end_dat);
    reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer)->mu_size = static_cast<uint16_t>(mz_buf_used);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// one writer per thread serves every trace function, so begin and end share its stack of open spans
static cp8_trc &get_tls_trc()
{
    thread_local cp8_trc lo_tls_trc;
    return lo_tls_trc;
}

extern "C"
{
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    uint64_t p8_trc_begin(const char                 *ip_name,
                          uint32_t                    iu_line,
                          const char                 *ip_file,
                          const char                 *ip_function,
                          size_t                      iz_attrs,
                          const struct s_p8_attr_val *ip_attrs)
    {
        return get_tls_trc().begin(ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    uint64_t p8_trc_begin_async(uint64_t                    iu_parent_id,
                                const char                 *ip_name,
                                uint32_t                    iu_line,
                                const char                 *ip_file,
                                const char                 *ip_function,
                                size_t                      iz_attrs,
                                const struct s_p8_attr_val *ip_attrs)
    {
        return get_tls_trc().begin_async(iu_parent_id, ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_trc_end(uint64_t iu_span_id)
    {
        // 0 is a failed begin; checked first, so a thread that never traced gets no writer for it
        if(iu_span_id) [[likely]]
        {
            get_tls_trc().end(iu_span_id);
        }
    }

} // extern "C"
