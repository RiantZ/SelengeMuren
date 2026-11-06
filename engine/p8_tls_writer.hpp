#pragma once

#include "p8_core.hpp"

#include <atomic>
#include <vector>
#include "kit/spin_lock.hpp"

// the largest record whose size padded to 8 still fits the 16-bit mu_size of a record
#define P8_MAX_RECORD_SIZE (UINT16_MAX & ~7u)

struct s_p8_attrs_result
{
    bool    mb_done; // false: a record larger than a buffer found no buffer to continue in
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
    // Attributes of a record in one pass, iz_record bytes into it: the record stays within P8_MAX_RECORD_SIZE (a
    // string is cut on a UTF-8 boundary, a value without room is skipped) and holds at most 255 attributes; one that
    // does not fit the buffer continues in the next buffers byte by byte
    s_p8_attrs_result serialize_attrs(uint8_t                   *&io_dst,
                                      uint8_t                   *&io_buf_end,
                                      size_t                      iz_record,
                                      const struct s_p8_attr_val *ip_attrs,
                                      size_t                      iz_attrs);

    bool rotate_fragment_buffer();

    void core_push();

    // Hand every buffer currently held by this writer (the finalized entries in
    // mo_fragments plus the in-progress tail mp_buffer) over to io_data in
    // logical order, then reset the writer so the next record starts fresh. The
    // owning lock is taken internally, so this never races with send().
    void pull(kit::c_lst<uint8_t *> &io_data);

    // Atomically read-and-reset the three drop counters and return them. Uses
    // atomic exchange (no spin lock), so it never contends with send() on the
    // hot path and can run while the writer is live.
    s_p8_drop_stats pull_dropped();

    bool
    copy_fragmented(uint8_t *&io_dst, uint8_t *&io_buf_end, const void *ip_src, size_t iz_len, size_t &oz_written);

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
    uint32_t          mu_writer_id = 0; // from cp8_core::register_writer, goes into every buffer header
    uint32_t          mu_buf_seq   = 0; // number of the next buffer this writer takes
    kit::c_spin_lock *mp_lock      = nullptr;

    // Per-thread dropped-element counters, incremented on the hot path at each
    // discard site (single relaxed atomic add). The core pulls-and-resets them
    // via pull_dropped() on its stats cadence and on destruction. Each subclass
    // bumps the counter for its own kind.
    std::atomic<uint64_t> mu_dropped_logs { 0 };
    std::atomic<uint64_t> mu_dropped_metrics { 0 };
    std::atomic<uint64_t> mu_dropped_traces { 0 };

    cp8_tls_writer *mp_next_writer = nullptr;
    cp8_tls_writer *mp_prev_writer = nullptr;
    friend class cp8_core;
};

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
// Write a piece of the record; one that does not fit continues in the next buffer byte by byte, so no buffer ends
// with a gap
#define P8_WRITE_PIECE(dst, buf_end, src, len, written)                                                               \
    do                                                                                                                \
    {                                                                                                                 \
        if((dst) + (len) <= (buf_end)) [[likely]]                                                                     \
        {                                                                                                             \
            memcpy((dst), (src), (len));                                                                              \
            (dst)     += (len);                                                                                       \
            (written) += (len);                                                                                       \
        }                                                                                                             \
        else if(!copy_fragmented((dst), (buf_end), (src), (len), (written)))                                          \
        {                                                                                                             \
            goto lbl_discard;                                                                                         \
        }                                                                                                             \
    } while(0)
// NOLINTEND(cppcoreguidelines-macro-usage)
