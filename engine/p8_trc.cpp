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
    uint64_t lu_parent_id = mo_open_spans.size() ? mo_open_spans.back() : P8_NO_PARENT_SPAN;
    uint64_t lu_span_id
        = write_begin(P8_TRC_ITEM_BEGIN, lu_parent_id, ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);

    if(P8_INVALID_SPAN != lu_span_id) [[likely]]
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
    if(P8_INVALID_SPAN == iu_span_id || !mp_core) [[unlikely]]
    {
        return;
    }

    // an async span is never on the stack
    if(!P8_IS_ASYNC_SPAN(iu_span_id))
    {
        pop_span(iu_span_id);
    }
    write_end(iu_span_id);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_trc::pop_span(uint64_t iu_span_id)
{
    size_t lz_depth = 1;
    auto   lo_it    = mo_open_spans.end();

    if(0 == mo_open_spans.size())
    {
        return;
    }

    if(mo_open_spans.back() == iu_span_id) [[likely]]
    {
        mo_open_spans.pull_last();
        return;
    }

    // spans opened inside this one missed their end and close with it; a span of another thread is not found
    // and leaves the stack as it is
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
uint64_t cp8_trc::write_begin(uint8_t                     iu_type,
                              uint64_t                    iu_parent_id,
                              const char                 *ip_name,
                              uint32_t                    iu_line,
                              const char                 *ip_file,
                              const char                 *ip_function,
                              size_t                      iz_attrs,
                              const struct s_p8_attr_val *ip_attrs)
{
    const char         *lp_file        = ip_file ? ip_file : "";
    uint64_t            lu_hash        = 0;
    uint64_t            lu_span_id     = 0;
    s_p8_trc_begin_dat *lp_hdr         = nullptr;
    uint8_t            *lp_dst         = nullptr;
    uint8_t            *lp_buf_end     = nullptr;
    size_t              lz_frag_commit = 0;
    s_p8_attrs_result   lo_attrs       = {};

    if(!mp_core) [[unlikely]]
    {
        return P8_INVALID_SPAN;
    }

    // the descriptor key is the file + line hash, as in logs
    lu_hash = P8_GET_LOG_HASH(lp_file, iu_line);
    if(mo_descs.find(lu_hash) == mo_descs.end()) [[unlikely]]
    {
        // the core may wait here for a service buffer, so no span goes out before its descriptor
        if(!mp_core->resolve_trc_desc(lu_hash, lp_file, iu_line, ip_function, ip_name))
        {
            mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);
            return P8_INVALID_SPAN;
        }
        mo_descs.insert(lu_hash);
    }

    if(mu_next_span_id == mu_end_span_id) [[unlikely]]
    {
        mu_next_span_id = mp_core->reserve_span_ids();
        mu_end_span_id  = mu_next_span_id + P8_CORE_SPAN_ID_BLOCK;
    }
    lu_span_id = mu_next_span_id++;
    if(P8_TRC_ITEM_BEGIN_ASYNC == iu_type)
    {
        lu_span_id |= P8_ASYNC_SPAN_BIT;
    }

    std::unique_lock<kit::c_spin_lock> lo_guard(mo_lock);

    if(!ensure_buffer(lo_guard, sizeof(s_p8_trc_begin_dat))) [[unlikely]]
    {
        mu_dropped_traces.fetch_add(1, std::memory_order_relaxed);
        return P8_INVALID_SPAN;
    }

    // a rollback of this record (lbl_discard) restores this list length
    lz_frag_commit         = mo_fragments.size();

    lp_hdr                 = reinterpret_cast<s_p8_trc_begin_dat *>(mp_buffer + mz_buf_used);
    lp_hdr->mu_timestamp   = get_ticks();
    lp_hdr->mu_span_id     = lu_span_id;
    lp_hdr->mu_size        = static_cast<uint16_t>(sizeof(s_p8_trc_begin_dat));
    lp_hdr->mu_type        = iu_type;
    lp_hdr->mu_attrs_count = 0;
    lp_hdr->mu_thread_id   = mu_thread_id;
    lp_hdr->mu_hash        = lu_hash;
    lp_hdr->mu_parent_id   = iu_parent_id;

    lp_dst                 = mp_buffer + mz_buf_used + sizeof(s_p8_trc_begin_dat);
    lp_buf_end             = mp_buffer + mz_buf_max;

    // attributes may continue in the next buffers, as in logs; a record that finds no buffer there is dropped
    if(ip_attrs && iz_attrs)
    {
        lo_attrs = serialize_attrs(lp_dst, lp_buf_end, sizeof(s_p8_trc_begin_dat), ip_attrs, iz_attrs);
        if(!lo_attrs.mb_done) [[unlikely]]
        {
            goto lbl_discard;
        }
    }

    // without gaps the write offset stays congruent to the record size, so padding the size to 8 also starts
    // the next record 8-aligned in its buffer, and the pad never crosses the buffer end
    {
        size_t lz_size          = sizeof(s_p8_trc_begin_dat) + lo_attrs.mz_bytes;
        size_t lz_pad           = ((lz_size + 7u) & ~static_cast<size_t>(7u)) - lz_size;
        lp_dst                 += lz_pad;
        lp_hdr->mu_attrs_count  = lo_attrs.mu_count;
        lp_hdr->mu_size         = static_cast<uint16_t>(lz_size + lz_pad);
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

    return P8_INVALID_SPAN;
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
    lp_rec->mu_timestamp      = get_ticks();
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
        // a failed begin; checked first, so a thread that never traced gets no writer for it
        if(P8_INVALID_SPAN != iu_span_id) [[likely]]
        {
            get_tls_trc().end(iu_span_id);
        }
    }

} // extern "C"
