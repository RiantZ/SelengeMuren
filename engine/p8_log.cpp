#include "p8_log.hpp"
#include "p8_hash.hpp"
#include "p8_protocol.h"

#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "kit/time.hpp"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
enum e_prefix_type
{
    e_prefix_i64 = 0,
    e_prefix_i32,
    e_prefix_ll,
    e_prefix_l,
    e_prefix_hh,
    e_prefix_h,
    e_prefix_i,
    e_prefix_w,
    e_prefix_j,
    e_prefix_upper_l,
    e_prefix_unknown
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct s_prefix_desc
{
    const char        *mp_prefix;
    uint32_t           mu_len;
    enum e_prefix_type me_type;
};

// longest-match-first order is critical
static const struct s_prefix_desc gs_prefixes[] = {
    { "I64", 3, e_prefix_i64     },
    { "I32", 3, e_prefix_i32     },
    { "ll",  2, e_prefix_ll      },
    { "hh",  2, e_prefix_hh      },
    { "h",   1, e_prefix_h       },
    { "l",   1, e_prefix_l       },
    { "L",   1, e_prefix_upper_l },
    { "I",   1, e_prefix_i       },
    { "z",   1, e_prefix_i       },
    { "t",   1, e_prefix_i       },
    { "w",   1, e_prefix_w       },
    { "j",   1, e_prefix_j       },
    { NULL,  0, e_prefix_unknown }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static const struct s_prefix_desc *find_prefix(const char *ip_fmt)
{
    const struct s_prefix_desc *lp_cur = &gs_prefixes[0];

    while(lp_cur->mu_len)
    {
        if(0 == strncmp(ip_fmt, lp_cur->mp_prefix, lp_cur->mu_len))
        {
            return lp_cur;
        }
        lp_cur++;
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_log::parse_format_string(struct s_p8_log_varg *op_args, size_t iz_args_max, const char *ip_format)
{
    size_t             lz_count     = 0;
    const char        *lp_iter      = ip_format;
    bool               lb_percent   = false;
    enum e_prefix_type le_prefix    = e_prefix_unknown;
    uint8_t            lu_has_width = 0;

    if(!ip_format || !op_args || 0 == iz_args_max)
    {
        return 0;
    }

    while(*lp_iter)
    {
        if(!lb_percent)
        {
            if('%' == *lp_iter)
            {
                lb_percent   = true;
                le_prefix    = e_prefix_unknown;
                lu_has_width = 0;
            }
        }
        else
        {
            switch(*lp_iter)
            {
            case '*':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }
                op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
                lz_count++;
                lu_has_width = 1;
                break;
            }
            case 'I':
            case 'l':
            case 'h':
            case 'w':
            case 'z':
            case 'j':
            case 't':
            case 'L':
            {
                const struct s_prefix_desc *lp_pfx = find_prefix(lp_iter);
                if(lp_pfx)
                {
                    le_prefix = lp_pfx->me_type;
                    if(1 < lp_pfx->mu_len)
                    {
                        lp_iter += (lp_pfx->mu_len - 1);
                    }
                }
                break;
            }
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
            case 'b':
            case 'B':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }

                if(e_prefix_unknown == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
                }
                else if(e_prefix_ll == le_prefix || e_prefix_i64 == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT64;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint64_t);
                }
                else if(e_prefix_h == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT16;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint16_t);
                }
                else if(e_prefix_hh == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT8;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint8_t);
                }
                else if(e_prefix_l == le_prefix)
                {
#if defined(G_OS_WINDOWS)
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
#elif defined(GTX64)
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT64;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint64_t);
#else
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
#endif
                }
                else if(e_prefix_i32 == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
                }
                else if(e_prefix_i == le_prefix)
                {
#ifdef GTX64
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT64;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint64_t);
#else
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
#endif
                }
                else if(e_prefix_j == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INTMAX;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uintmax_t);
                }
                else
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_INT32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(uint32_t);
                }

                lz_count++;
                lb_percent = false;
                break;
            }
            case 's':
            case 'S':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }

                if(e_prefix_h == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_STRA;
                    op_args[lz_count].mu_size = lu_has_width;
                }
                else if('S' == *lp_iter)
                {
#if defined(G_OS_WINDOWS)
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_STRA;
#else
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_USTR32;
#endif
                    op_args[lz_count].mu_size = lu_has_width;
                }
                else if(e_prefix_l == le_prefix)
                {
#if defined(G_OS_WINDOWS)
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_USTR16;
#else
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_USTR32;
#endif
                    op_args[lz_count].mu_size = lu_has_width;
                }
                else
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_USTR8;
                    op_args[lz_count].mu_size = lu_has_width;
                }

                lz_count++;
                lb_percent = false;
                break;
            }
            case 'p':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }
                op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_PVOID;
                op_args[lz_count].mu_size = P8_SIZE_OF_ARG(void *);
                lz_count++;
                lb_percent = false;
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }

                if(e_prefix_upper_l == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_LDOUBLE;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(long double);
                }
                else
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_DOUBLE;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(double);
                }

                lz_count++;
                lb_percent = false;
                break;
            }
            case 'c':
            case 'C':
            {
                if(lz_count >= iz_args_max)
                {
                    return lz_count;
                }

                if(e_prefix_h == le_prefix)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_CHAR;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(char);
                }
                else if(e_prefix_l == le_prefix)
                {
#if defined(G_OS_WINDOWS)
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_CHAR16;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(wchar_t);
#else
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_CHAR32;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(wchar_t);
#endif
                }
                else if('c' == *lp_iter)
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_CHAR;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(char);
                }
                else
                {
                    op_args[lz_count].mu_type = P8_LOG_ARG_TYPE_CHAR;
                    op_args[lz_count].mu_size = P8_SIZE_OF_ARG(char);
                }

                lz_count++;
                lb_percent = false;
                break;
            }
            case '%':
            {
                lb_percent = false;
                break;
            }
            } // switch

            if(lb_percent && *lp_iter >= '0' && *lp_iter <= '9')
            {
                lu_has_width = 1;
            }
        }

        lp_iter++;
    }

    return lz_count;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_log::cp8_log()
    : cp8_tls_writer(&mo_lock)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_log::send(enum e_p8_level             ie_level,
                   p_p8_module                 ip_module,
                   uint64_t                    iu_trace_id,
                   uint32_t                    iu_line,
                   const char                 *ip_file,
                   const char                 *ip_function,
                   size_t                      iz_attrs,
                   const struct s_p8_attr_val *ip_attrs,
                   const char                 *ip_format,
                   va_list                     io_args)
{
    s_p8_log_desc     *lp_desc          = nullptr;
    s_p8_log_item_dat *lp_hdr           = nullptr;
    uint64_t           lu_hash          = 0;
    uint8_t           *lp_dst           = nullptr;
    uint8_t           *lp_buf_end       = nullptr;
    size_t             lz_args_written  = 0;
    size_t             lz_attrs_written = 0;
    uint8_t            lu_attrs_count   = 0;
    s_p8_attrs_result  lo_attrs         = {};
    size_t             lz_frag_commit   = 0;
    uint16_t           lu_mod_id        = 0xFFFF;
    uint8_t            lu_flags         = 0;
    size_t             lz_opt           = 0;       // bytes of the optional fields
    uint8_t           *lp_attrs_head    = nullptr; // args size and attrs count, filled after the attributes
    enum e_p8_level    le_min           = e_p8_trace0;

    // verbosity gate: reject records below the module's threshold (or the whole-p8 default for a null module)
    if(ip_module)
    {
        le_min     = reinterpret_cast<const s_p8_log_mod *>(ip_module)->mb_vervosity.load(std::memory_order_relaxed);
        lu_mod_id  = reinterpret_cast<const s_p8_log_mod *>(ip_module)->mu_id;
        lu_flags  |= P8_LOG_FLAG_MODULE;
    }
    else if(mp_core)
    {
        le_min = mp_core->get_verbosity(nullptr);
    }

    if(ie_level < le_min)
    {
        return false;
    }

    if(!mp_core) [[unlikely]]
    {
        return false;
    }

    // before the writer lock: the core may wait for a service buffer, and the worker takes this lock before it
    // recycles one; mo_desc_map is touched by this thread only
    lu_hash    = P8_GET_LOG_HASH(ip_file, iu_line);

    auto lo_it = mo_desc_map.find(lu_hash);
    if(lo_it != mo_desc_map.end())
    {
        lp_desc = lo_it->second;
    }
    else
    {
        lp_desc = mp_core->resolve_log_desc(lu_hash, ip_file, iu_line, ip_function, ip_format);
        if(!lp_desc) [[unlikely]]
        {
            mu_dropped_logs.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        mo_desc_map[lu_hash] = lp_desc;
    }

    std::unique_lock<kit::c_spin_lock> lo_guard(mo_lock);

    // buffer availability check — reuse current buffer when possible. A full
    // buffer is parked in mo_fragments (not submitted); the worker drains it
    // later via pull().
    if(mp_buffer) [[likely]]
    {
        if((mz_buf_max - mz_buf_used) < (P8_LOG_MIN_BUFFER_SPACE + sizeof(s_p8_log_item_dat))) [[unlikely]]
        {
            mo_fragments.push_last(mp_buffer);
            mp_buffer = nullptr;
        }
    }

    if(!mp_buffer) [[unlikely]]
    {
        // Block until a buffer frees rather than dropping.
        lo_guard.unlock();
        // local variable needed for avoiding data corruption by Core since mutex is unlocked
        uint8_t *lp_new = mp_core->acquire_buffer(true);
        lo_guard.lock();

        if(!lp_new) [[unlikely]]
        {
            // acquire_buffer(true) returns nullptr only on shutdown
            mu_dropped_logs.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        mp_buffer                     = lp_new;
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_packet_type    = P8_PACKET_LOGS;
        lp_buf_hdr->mu_flags          = 0;
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(sizeof(s_p8_data_buf_hdr));
        lp_buf_hdr->mu_thread_id      = mu_thread_id;
        lp_buf_hdr->mu_writer_id      = mu_writer_id;
        lp_buf_hdr->mu_buf_seq        = mu_buf_seq++;

        mz_buf_used                   = sizeof(s_p8_data_buf_hdr);
    }

    // a rollback of this record (lbl_discard) restores this list length
    lz_frag_commit = mo_fragments.size();

    // write item header
    {
        uint8_t *lp_base = mp_buffer + mz_buf_used;
        lp_buf_end       = mp_buffer + mz_buf_max;

        if(P8_INVALID_SPAN != iu_trace_id)
        {
            lu_flags |= P8_LOG_FLAG_TRACE_ID;
        }
        if(ip_attrs && (0 != iz_attrs))
        {
            lu_flags |= P8_LOG_FLAG_ATTRS;
        }

        lp_hdr               = reinterpret_cast<struct s_p8_log_item_dat *>(lp_base);
        lp_hdr->mu_hash      = lp_desc->mu_hash;
        lp_hdr->mu_timestamp = get_ticks();
        lp_hdr->mu_thread_id = mu_thread_id;
        lp_hdr->mu_level     = static_cast<uint8_t>(ie_level);
        lp_hdr->mu_flags     = lu_flags;

        // the optional fields in flag order; no CPU core ID yet (P8_LOG_FLAG_PROCESSOR)
        lp_dst               = lp_base + sizeof(struct s_p8_log_item_dat);
        if(0 != (lu_flags & P8_LOG_FLAG_TRACE_ID))
        {
            memcpy(lp_dst, &iu_trace_id, sizeof(iu_trace_id));
            lp_dst += sizeof(iu_trace_id);
        }
        if(0 != (lu_flags & P8_LOG_FLAG_MODULE))
        {
            memcpy(lp_dst, &lu_mod_id, sizeof(lu_mod_id));
            lp_dst += sizeof(lu_mod_id);
        }
        if(0 != (lu_flags & P8_LOG_FLAG_ATTRS))
        {
            lp_attrs_head  = lp_dst;
            lp_dst        += sizeof(uint16_t) + sizeof(uint8_t);
        }
        lz_opt = static_cast<size_t>(lp_dst - lp_base) - sizeof(struct s_p8_log_item_dat);
    }

    // serialize variable arguments
    {
        const s_p8_log_varg *lp_arg     = lp_desc->ma_args;
        const s_p8_log_varg *lp_arg_end = lp_arg + lp_desc->mz_args_count;

        for(; lp_arg < lp_arg_end; ++lp_arg)
        {
            switch(lp_arg->mu_type)
            {
            case P8_LOG_ARG_TYPE_USTR8:
            case P8_LOG_ARG_TYPE_STRA:
            case P8_LOG_ARG_TYPE_USTR16:
            case P8_LOG_ARG_TYPE_USTR32:
            {
                const void *lp_str = nullptr;
                uint16_t    lu_len = 0;

                if(P8_LOG_ARG_TYPE_USTR16 == lp_arg->mu_type)
                {
                    const uint16_t *lp_u16  = va_arg(io_args, const uint16_t *);
                    lp_str                  = lp_u16;
                    const uint16_t *lp_zero = lp_u16;
                    if(lp_zero)
                    {
                        while(*lp_zero)
                        {
                            lp_zero++;
                        }
                        size_t lz_bytes = (lp_zero - lp_u16) * sizeof(uint16_t);
                        lu_len          = (lz_bytes > UINT16_MAX) ? UINT16_MAX : static_cast<uint16_t>(lz_bytes);
                    }
                }
                else if(P8_LOG_ARG_TYPE_USTR32 == lp_arg->mu_type)
                {
                    const wchar_t *lp_w = va_arg(io_args, const wchar_t *);
                    lp_str              = lp_w;
                    if(lp_w)
                    {
                        size_t lz_bytes = wcslen(lp_w) * sizeof(wchar_t);
                        lu_len          = (lz_bytes > UINT16_MAX) ? UINT16_MAX : static_cast<uint16_t>(lz_bytes);
                    }
                }
                else
                {
                    const char *lp_c = va_arg(io_args, const char *);
                    lp_str           = lp_c;
                    if(lp_c)
                    {
                        size_t lz_slen = strlen(lp_c);
                        lu_len         = (lz_slen > UINT16_MAX) ? UINT16_MAX : static_cast<uint16_t>(lz_slen);
                    }
                }

                P8_WRITE_PIECE(lp_dst, lp_buf_end, &lu_len, sizeof(lu_len), lz_args_written);

                if(!copy_fragmented(lp_dst, lp_buf_end, lp_str, lu_len, lz_args_written))
                {
                    goto lbl_discard;
                }
                break;
            }
            default:
            {
#if defined(GTX32)
                if(4 == lp_arg->mu_size)
                {
                    unsigned int lu_val = va_arg(io_args, unsigned int);
                    P8_WRITE_PIECE(lp_dst, lp_buf_end, &lu_val, sizeof(lu_val), lz_args_written);
                }
                else
#endif
                    if(sizeof(uint64_t) == lp_arg->mu_size)
                {
                    if(P8_LOG_ARG_TYPE_DOUBLE != lp_arg->mu_type) [[likely]]
                    {
                        uint64_t lu_val = va_arg(io_args, uint64_t);
                        P8_WRITE_PIECE(lp_dst, lp_buf_end, &lu_val, sizeof(lu_val), lz_args_written);
                    }
                    else
                    {
                        double ld_val = va_arg(io_args, double);
                        P8_WRITE_PIECE(lp_dst, lp_buf_end, &ld_val, sizeof(ld_val), lz_args_written);
                    }
                }
                else if(P8_LOG_ARG_TYPE_LDOUBLE == lp_arg->mu_type)
                {
                    long double ld_val = va_arg(io_args, long double);
                    P8_WRITE_PIECE(lp_dst, lp_buf_end, &ld_val, sizeof(ld_val), lz_args_written);
                }
                break;
            }
            } // switch
        }
    }

    // serialize attributes with fragmentation support
    if(ip_attrs && iz_attrs)
    {
        lo_attrs = serialize_attrs(lp_dst,
                                   lp_buf_end,
                                   sizeof(s_p8_log_item_dat) + lz_opt + lz_args_written,
                                   ip_attrs,
                                   iz_attrs);
        if(!lo_attrs.mb_done) [[unlikely]]
        {
            goto lbl_discard;
        }
        lz_attrs_written = lo_attrs.mz_bytes;
        lu_attrs_count   = lo_attrs.mu_count;
    }

    // Pad the record to 8 bytes so the next one starts 8-aligned: with no gaps the write offset is congruent to the
    // record size. Pad bytes stay as-is (consumers skip them via mu_size); a run of zeros would weaken encryption.
    {
        size_t lz_size   = sizeof(s_p8_log_item_dat) + lz_opt + lz_args_written + lz_attrs_written;
        size_t lz_pad    = ((lz_size + static_cast<size_t>(7u)) & ~static_cast<size_t>(7u)) - lz_size;
        lp_hdr->mu_size  = static_cast<uint16_t>(lz_size + lz_pad);
        lp_dst          += lz_pad;
        if(lp_attrs_head)
        {
            const uint16_t lu_args_size = static_cast<uint16_t>(lz_args_written);
            memcpy(lp_attrs_head, &lu_args_size, sizeof(lu_args_size));
            memcpy(lp_attrs_head + sizeof(lu_args_size), &lu_attrs_count, sizeof(lu_attrs_count));
        }
    }

    // update current buffer state
    mz_buf_used                                                      = static_cast<size_t>(lp_dst - mp_buffer);

    reinterpret_cast<struct s_p8_data_buf_hdr *>(mp_buffer)->mu_size = static_cast<uint16_t>(mz_buf_used);

    // Keep the buffers local: the worker drains them later via pull(). When the
    // current buffer can no longer fit another item (< P8_LOG_MIN_BUFFER_SPACE
    // left) park it in mo_fragments so the next record starts on a fresh buffer;
    // otherwise keep it for the next record.
    if((mz_buf_max - mz_buf_used) < P8_LOG_MIN_BUFFER_SPACE) [[unlikely]]
    {
        mo_fragments.push_last(mp_buffer);
        mp_buffer   = nullptr;
        mz_buf_used = 0;
    }

    return true;

lbl_discard:
    // rotate_fragment_buffer failed: give back the buffers taken only for this record, with their numbers, as the
    // worker never saw them (the writer lock is held for the whole record)
    while(mo_fragments.size() > lz_frag_commit + 1)
    {
        mp_core->release_buffer(mo_fragments.pull_last());
        --mu_buf_seq;
    }

    // The record began in this buffer at lp_hdr. The buffer only waited in mo_fragments: it was not released
    // and no one else has it, so lp_hdr still points into this writer's own memory. lp_hdr - mp_buffer is
    // where the record began; cut the buffer there and the earlier records stay.
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
        // undo the fragment flag and the size set by rotate_fragment_buffer; the stop time keeps the dropped
        // record's time until the next record overwrites it, nothing relies on it being exact
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_flags          = static_cast<uint8_t>(lp_buf_hdr->mu_flags & ~P8_DATA_FLAG_FRAGMENT);
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(mz_buf_used);
    }
    mu_dropped_logs.fetch_add(1, std::memory_order_relaxed);

    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// one writer per thread serves every log function
static cp8_log &get_tls_log()
{
    thread_local cp8_log lo_tls_log;
    return lo_tls_log;
}

extern "C"
{
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_log_set_verbosity(p_p8_module ip_module, enum e_p8_level ie_verbosity)
    {
        cp8_core *lp_core = cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS);
        if(lp_core)
        {
            lp_core->set_verbosity(ip_module, ie_verbosity);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    enum e_p8_level p8_log_get_verbosity(p_p8_module ip_module)
    {
        cp8_core *lp_core = cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS);
        if(lp_core)
        {
            return lp_core->get_verbosity(ip_module);
        }
        return e_p8_trace0;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_log_register_module(const char *ip_name, enum e_p8_level ie_verbosity, p_p8_module *op_module)
    {
        cp8_core *lp_core = cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS);
        if(!lp_core)
        {
            if(op_module)
            {
                *op_module = P8_MODULE_INVALID_ID;
            }
            return false;
        }
        return lp_core->register_module(ip_name, ie_verbosity, op_module);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    p_p8_module p8_log_find_module(const char *ip_name)
    {
        cp8_core *lp_core = cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS);
        if(!lp_core)
        {
            return P8_MODULE_INVALID_ID;
        }
        return lp_core->find_module(ip_name);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_log_sent(enum e_p8_level             ie_level,
                     p_p8_module                 ip_module,
                     uint64_t                    iu_trace_id,
                     uint32_t                    iu_line,
                     const char                 *ip_file,
                     const char                 *ip_function,
                     size_t                      iz_attrs,
                     const struct s_p8_attr_val *ip_attrs,
                     const char                 *ip_format,
                     ...)
    {
        va_list lo_args;
        va_start(lo_args, ip_format);
        bool lb_result = get_tls_log().send(ie_level,
                                            ip_module,
                                            iu_trace_id,
                                            iu_line,
                                            ip_file,
                                            ip_function,
                                            iz_attrs,
                                            ip_attrs,
                                            ip_format,
                                            lo_args);
        va_end(lo_args);
        return lb_result;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_log_sent_emb(enum e_p8_level             ie_level,
                         p_p8_module                 ip_module,
                         uint64_t                    iu_trace_id,
                         uint32_t                    iu_line,
                         const char                 *ip_file,
                         const char                 *ip_function,
                         size_t                      iz_attrs,
                         const struct s_p8_attr_val *ip_attrs,
                         const char                 *ip_format,
                         va_list                    *ip_va_list)
    {
        return get_tls_log().send(ie_level,
                                  ip_module,
                                  iu_trace_id,
                                  iu_line,
                                  ip_file,
                                  ip_function,
                                  iz_attrs,
                                  ip_attrs,
                                  ip_format,
                                  *ip_va_list);
    }

} // extern "C"
