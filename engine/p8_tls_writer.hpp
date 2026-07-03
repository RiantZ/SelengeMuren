#pragma once

#include "p8_core.hpp"

#include <vector>
#include "kit/spin_lock.hpp"

struct s_p8_attrs_result
{
    uint8_t mu_count;
    size_t  mz_bytes;
};

class cp8_tls_writer
{
public:
    cp8_tls_writer(kit::c_spin_lock *ip_lock = nullptr);
    ~cp8_tls_writer();

    cp8_tls_writer(const cp8_tls_writer &)            = delete;
    cp8_tls_writer &operator=(const cp8_tls_writer &) = delete;
    cp8_tls_writer(cp8_tls_writer &&)                 = delete;
    cp8_tls_writer &operator=(cp8_tls_writer &&)      = delete;

protected:
    s_p8_attrs_result serialize_attrs(uint8_t                    *op_dst,
                                      const uint8_t              *ip_buf_end,
                                      const struct s_p8_attr_val *ip_attrs,
                                      size_t                      iz_attrs);

    bool rotate_fragment_buffer(uint64_t iu_timestamp);

    void core_push();

    // Hand every buffer currently held by this writer (the finalized entries in
    // mo_fragments plus the in-progress tail mp_buffer) over to io_data in
    // logical order, then reset the writer so the next record starts fresh. The
    // owning lock is taken internally, so this never races with send().
    void pull(kit::c_lst<uint8_t *> &io_data);

    static size_t serialize_utf8_string(uint8_t *op_dst, size_t iz_avail, const char *ip_str);

    bool copy_fragmented(uint8_t   *&io_dst,
                         uint8_t   *&io_buf_end,
                         const void *ip_src,
                         size_t      iz_len,
                         uint64_t    iu_timestamp,
                         size_t     &oz_written);

    cp8_core                           *mp_core = nullptr;
    std::vector<const s_p8_attr_desc *> mo_attr_cache;
    // Buffers that have already been filled while serializing the current
    // logical record but have not been handed back to the pool yet. Each entry
    // has been finalized with P8_DATA_FLAG_FRAGMENT set in its header and is
    // stored in logical (write) order.
    kit::c_lst<uint8_t *> mo_fragments { 16 };

    // Current tail for mo_fragments, but not yet in the list, writing to it is keep going.
    uint8_t *mp_buffer             = nullptr;

    // Write cursor inside mp_buffer expressed as a byte offset from its start
    // (i.e. the total number of bytes already occupied, including the leading
    // s_p8_data_buf_hdr and any items written by previous calls). The next
    // write begins at mp_buffer + mz_buf_used.
    size_t mz_buf_used             = 0;
    size_t mz_buf_max              = 0;

    // Stable identifier of the owning thread, derived once in the constructor
    // from std::hash<std::thread::id>
    uint32_t          mu_thread_id = 0;
    kit::c_spin_lock *mp_lock      = nullptr;

    cp8_tls_writer *mp_next_writer = nullptr;
    cp8_tls_writer *mp_prev_writer = nullptr;
    friend class cp8_core;
};

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define P8_ENSURE_SPACE(dst, buf_end, needed, timestamp)                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if((dst) + (needed) > (buf_end)) [[unlikely]]                                                                 \
        {                                                                                                             \
            mz_buf_used = static_cast<size_t>((dst) - mp_buffer);                                                     \
            if(!rotate_fragment_buffer((timestamp)))                                                                  \
            {                                                                                                         \
                goto lbl_discard;                                                                                     \
            }                                                                                                         \
            (dst)     = mp_buffer + mz_buf_used;                                                                      \
            (buf_end) = mp_buffer + mz_buf_max;                                                                       \
        }                                                                                                             \
    } while(0)
// NOLINTEND(cppcoreguidelines-macro-usage)
