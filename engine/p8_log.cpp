#include "p8_log.hpp"
#include "p8_hash.hpp"
#include "p8_protocol.h"

#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "kit/time.hpp"

#define P8_LOG_MIN_BUFFER_SPACE 32 // Minimal remaining space in data buffer

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

static thread_local cp8_log go_tls_log;

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
    size_t             lz_frag_commit   = 0;
    uint16_t           lu_mod_id        = 0xFFFF;
    enum e_p8_level    le_min           = e_p8_trace0;

    // verbosity gate: reject records below the module's threshold (or the whole-p8 default for a null module)
    if(ip_module)
    {
        le_min    = reinterpret_cast<const s_p8_log_mod *>(ip_module)->mb_vervosity.load(std::memory_order_relaxed);
        lu_mod_id = reinterpret_cast<const s_p8_log_mod *>(ip_module)->mu_id;
    }
    else if(mp_core)
    {
        le_min = mp_core->get_verbosity(nullptr);
    }

    if(ie_level < le_min)
    {
        return false;
    }

    std::lock_guard<kit::c_spin_lock> lo_guard(mo_lock);

    if(!mp_core) [[unlikely]]
    {
        return false;
    }

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
        mp_buffer = mp_core->acquire_buffer();
        if(!mp_buffer) [[unlikely]]
        {
            mu_dropped_logs.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_packet_type    = P8_PACKET_LOGS;
        lp_buf_hdr->mu_flags          = 0;
        lp_buf_hdr->mu_size           = static_cast<uint16_t>(sizeof(s_p8_data_buf_hdr));
        lp_buf_hdr->mu_thread_id      = mu_thread_id;
        lp_buf_hdr->mu_start_time     = kit::get_hires_ticks();
        lp_buf_hdr->mu_stop_time      = 0;

        mz_buf_used                   = sizeof(s_p8_data_buf_hdr);
    }

    // compute hash from file + line
    lu_hash    = P8_GET_LOG_HASH(ip_file, iu_line);

    auto lo_it = mo_desc_map.find(lu_hash);
    if(lo_it != mo_desc_map.end())
    {
        lp_desc = lo_it->second;
    }
    else
    {
        lp_desc = mp_core->resolve_log_desc(lu_hash, ip_file, iu_line, ip_function, ip_format);
        if(!lp_desc)
        {
            return false;
        }
        mo_desc_map[lu_hash] = lp_desc;
    }

    // Mark the fragment-list boundary for this record. Any buffer parked past
    // this point belongs to the record being serialized now, so a mid-record
    // failure can roll back exactly this record without touching earlier ones.
    lz_frag_commit = mo_fragments.size();

    // write item header
    {
        uint8_t *lp_base       = mp_buffer + mz_buf_used;
        lp_buf_end             = mp_buffer + mz_buf_max;

        lp_hdr                 = reinterpret_cast<struct s_p8_log_item_dat *>(lp_base);
        lp_hdr->mu_hash        = lp_desc->mu_hash;
        lp_hdr->mu_timestamp   = kit::get_hires_ticks();
        lp_hdr->mu_trace_id    = iu_trace_id;
        lp_hdr->mu_thread_id   = mu_thread_id;
        lp_hdr->mu_level       = static_cast<uint8_t>(ie_level);
        lp_hdr->mu_processor   = 0; // TODO: get current processor CORE
        lp_hdr->mu_attrs_count = 0;
        lp_hdr->mu_mod_id      = lu_mod_id;
        lp_hdr->mu_flags       = 0;

        lp_dst                 = lp_base + sizeof(struct s_p8_log_item_dat);
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

                P8_ENSURE_SPACE(lp_dst, lp_buf_end, sizeof(lu_len), lp_hdr->mu_timestamp);
                memcpy(lp_dst, &lu_len, sizeof(lu_len));
                lp_dst          += sizeof(lu_len);
                lz_args_written += sizeof(lu_len);

                if(!copy_fragmented(lp_dst, lp_buf_end, lp_str, lu_len, lp_hdr->mu_timestamp, lz_args_written))
                {
                    goto lbl_discard;
                }
                break;
            }
            default:
            {
                P8_ENSURE_SPACE(lp_dst, lp_buf_end, lp_arg->mu_size, lp_hdr->mu_timestamp);

#if defined(GTX32)
                if(4 == lp_arg->mu_size)
                {
                    unsigned int lu_val = va_arg(io_args, unsigned int);
                    memcpy(lp_dst, &lu_val, sizeof(lu_val));
                }
                else
#endif
                    if(sizeof(uint64_t) == lp_arg->mu_size)
                {
                    if(P8_LOG_ARG_TYPE_DOUBLE != lp_arg->mu_type) [[likely]]
                    {
                        uint64_t lu_val = va_arg(io_args, uint64_t);
                        memcpy(lp_dst, &lu_val, sizeof(lu_val));
                    }
                    else
                    {
                        double ld_val = va_arg(io_args, double);
                        memcpy(lp_dst, &ld_val, sizeof(ld_val));
                    }
                }
                else if(P8_LOG_ARG_TYPE_LDOUBLE == lp_arg->mu_type)
                {
                    long double ld_val = va_arg(io_args, long double);
                    memcpy(lp_dst, &ld_val, sizeof(ld_val));
                }

                lp_dst          += lp_arg->mu_size;
                lz_args_written += lp_arg->mu_size;
                break;
            }
            } // switch
        }
    }

    lp_hdr->mu_args_size = static_cast<uint16_t>(lz_args_written);

    // serialize attributes inline with fragmentation support
    for(size_t lz_i = 0; ip_attrs && lz_i < iz_attrs; ++lz_i)
    {
        p8_attr_id li_id  = ip_attrs[lz_i].m_id;
        size_t     lz_idx = static_cast<size_t>(li_id);

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

        const s_p8_attr_desc *lp_attr_desc = mo_attr_cache[lz_idx];

        if(lp_attr_desc->me_type == e_p8_attr_type::e_p8_attr_str)
        {
            P8_ENSURE_SPACE(lp_dst, lp_buf_end, sizeof(p8_attr_id), lp_hdr->mu_timestamp);
            memcpy(lp_dst, &li_id, sizeof(p8_attr_id));
            lp_dst             += sizeof(p8_attr_id);
            lz_attrs_written   += sizeof(p8_attr_id);

            const char *lp_str  = ip_attrs[lz_i].mp_str;
            uint16_t    lu_len  = 0;
            if(lp_str)
            {
                size_t lz_slen = strlen(lp_str);
                lu_len         = (lz_slen > UINT16_MAX) ? UINT16_MAX : static_cast<uint16_t>(lz_slen);
            }

            P8_ENSURE_SPACE(lp_dst, lp_buf_end, sizeof(lu_len), lp_hdr->mu_timestamp);
            memcpy(lp_dst, &lu_len, sizeof(lu_len));
            lp_dst           += sizeof(lu_len);
            lz_attrs_written += sizeof(lu_len);

            if(!copy_fragmented(lp_dst, lp_buf_end, lp_str, lu_len, lp_hdr->mu_timestamp, lz_attrs_written))
            {
                goto lbl_discard;
            }
        }
        else
        {
            size_t lz_needed = sizeof(p8_attr_id) + sizeof(uint64_t);
            P8_ENSURE_SPACE(lp_dst, lp_buf_end, lz_needed, lp_hdr->mu_timestamp);
            memcpy(lp_dst, &li_id, sizeof(p8_attr_id));
            lp_dst += sizeof(p8_attr_id);
            memcpy(lp_dst, &ip_attrs[lz_i].mu_u64, sizeof(uint64_t));
            lp_dst           += sizeof(uint64_t);
            lz_attrs_written += lz_needed;
        }

        lu_attrs_count++;
    }

    // Align the next item header to 8 bytes for direct 64-bit field reads. Pad
    // the buffer write offset, not the logical size; the buffer is a multiple of
    // 8, so the pad always fits without crossing the buffer end. The pad bytes are
    // left as-is (consumers skip them via mu_size) to avoid emitting a predictable
    // run of zeros that would weaken downstream encryption.
    {
        size_t lz_size          = sizeof(s_p8_log_item_dat) + lz_args_written + lz_attrs_written;
        size_t lz_pad           = ((lz_size + static_cast<size_t>(7u)) & ~static_cast<size_t>(7u)) - lz_size;
        lp_hdr->mu_attrs_count  = lu_attrs_count;
        lp_hdr->mu_size         = static_cast<uint16_t>(lz_size + lz_pad);
        lp_dst                 += lz_pad;
    }

    // update current buffer state
    mz_buf_used = static_cast<size_t>(lp_dst - mp_buffer);

    {
        struct s_p8_data_buf_hdr *lp_buf_hdr = reinterpret_cast<struct s_p8_data_buf_hdr *>(mp_buffer);
        lp_buf_hdr->mu_size                  = static_cast<uint16_t>(mz_buf_used);
        lp_buf_hdr->mu_stop_time             = lp_hdr->mu_timestamp;
    }

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
    // rotate_fragment_buffer failed — finalize partial item and roll back only
    // the buffers this record added, leaving earlier complete records intact.
    lp_hdr->mu_args_size   = static_cast<uint16_t>(lz_args_written);
    lp_hdr->mu_attrs_count = lu_attrs_count;
    lp_hdr->mu_size        = static_cast<uint32_t>(sizeof(s_p8_log_item_dat) + lz_args_written + lz_attrs_written);

    while(mo_fragments.size() > lz_frag_commit)
    {
        mp_core->release_buffer(mo_fragments.pull_last());
    }
    mp_buffer   = nullptr;
    mz_buf_used = 0;
    mu_dropped_logs.fetch_add(1, std::memory_order_relaxed);

    return false;
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
        bool lb_result = go_tls_log.send(ie_level,
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
        return go_tls_log.send(ie_level,
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
