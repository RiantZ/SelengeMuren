#pragma once

#include "p8_tls_writer.hpp"

#include "kit/list.hpp"
#include "kit/spin_lock.hpp"

#include <mutex>
#include <unordered_set>

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

    void pop_span(uint64_t iu_span_id);

    kit::c_spin_lock             mo_lock;
    std::unordered_set<uint64_t> mo_descs;            // call sites the core has already serialized
    kit::c_lst<uint64_t>         mo_open_spans;       // open spans of p8_trc_begin, innermost last
    uint64_t                     mu_next_span_id = 0; // next free ID of the reserved block
    uint64_t                     mu_end_span_id  = 0; // first ID after the reserved block
};
