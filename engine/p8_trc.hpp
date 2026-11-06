#pragma once

#include "p8_tls_writer.hpp"

#include "kit/list.hpp"
#include "kit/spin_lock.hpp"

#include <mutex>
#include <unordered_set>

// the largest record whose size padded to 8 still fits the 16-bit mu_size
#define P8_TRC_MAX_RECORD_SIZE (UINT16_MAX & ~7u)

class cp8_trc : public cp8_tls_writer
{
public:
    cp8_trc();

    uint64_t begin(const char                 *ip_name,
                   uint32_t                    iu_line,
                   const char                 *ip_file,
                   const char                 *ip_function,
                   size_t                      iz_attrs,
                   const struct s_p8_attr_val *ip_attrs);

    uint64_t begin_async(uint64_t                    iu_parent_id,
                         const char                 *ip_name,
                         uint32_t                    iu_line,
                         const char                 *ip_file,
                         const char                 *ip_function,
                         size_t                      iz_attrs,
                         const struct s_p8_attr_val *ip_attrs);

    void end(uint64_t iu_span_id);

private:
    uint64_t write_begin(uint8_t                     iu_type,
                         uint64_t                    iu_parent_id,
                         const char                 *ip_name,
                         uint32_t                    iu_line,
                         const char                 *ip_file,
                         const char                 *ip_function,
                         size_t                      iz_attrs,
                         const struct s_p8_attr_val *ip_attrs);

    void write_end(uint64_t iu_span_id);

    // a buffer with iz_needed free bytes; may unlock io_guard while it waits for one
    bool ensure_buffer(std::unique_lock<kit::c_spin_lock> &io_guard, size_t iz_needed);

    // bytes one attribute takes after iz_record bytes of its record, 0 when it is skipped; ou_len is the string
    // length to send, cut so the record stays within P8_TRC_MAX_RECORD_SIZE
    size_t get_attr_size(const struct s_p8_attr_val &ir_attr, size_t iz_record, bool &ob_str, uint16_t &ou_len);

    void pop_span(uint64_t iu_span_id);

    kit::c_spin_lock             mo_lock;
    std::unordered_set<uint64_t> mo_descs;            // call sites the core has already serialized
    kit::c_lst<uint64_t>         mo_open_spans;       // open spans of p8_trc_begin, innermost last
    uint64_t                     mu_next_span_id = 0; // next free ID of the reserved block
    uint64_t                     mu_end_span_id  = 0; // first ID after the reserved block
};
