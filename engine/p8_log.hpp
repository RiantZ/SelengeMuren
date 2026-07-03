#pragma once

#include "p8_tls_writer.hpp"
#include "p8_protocol.h"

#include "kit/spin_lock.hpp"

#include <unordered_map>

#define P8_LOG_MAX_ARGS 32 // Max count of log variable arguments

struct s_p8_log_desc
{
    uint64_t      mu_hash;
    const char   *mp_format;
    const char   *mp_file;
    const char   *mp_function;
    uint32_t      mu_line;
    size_t        mz_args_count;
    size_t        mz_fixed_args_size;
    s_p8_log_varg ma_args[P8_LOG_MAX_ARGS];
};

class cp8_log : public cp8_tls_writer
{

public:
    cp8_log();
    void            set_verbosity(p_p8_module ip_module, enum e_p8_level ie_verbosity);
    enum e_p8_level get_verbosity(p_p8_module ip_module);

    bool        register_module(const char *ip_name, enum e_p8_level ie_verbosity, p_p8_module *op_module);
    p_p8_module find_module(const char *ip_name);

    bool send(enum e_p8_level             ie_level,
              p_p8_module                 ip_module,
              uint64_t                    iu_trace_id,
              uint32_t                    iu_line,
              const char                 *ip_file,
              const char                 *ip_function,
              size_t                      iz_attrs,
              const struct s_p8_attr_val *ip_attrs,
              const char                 *ip_format,
              va_list                     io_args);

    static size_t parse_format_string(struct s_p8_log_varg *op_args, size_t iz_args_max, const char *ip_format);

    // Number of logical records dropped because the buffer pool was exhausted
    // mid-serialization. Accessed only under mo_lock.
    uint64_t get_dropped_records() const
    {
        return mu_dropped_records;
    }

private:
    kit::c_spin_lock                    mo_lock;
    std::map<uint64_t, s_p8_log_desc *> mo_desc_map;
    uint64_t                            mu_dropped_records = 0;
};

#ifdef P8_TESTING
// Dropped-record count for the calling thread's TLS log writer. Must be called
// on the same thread that performed the sends.
uint64_t p8_test_get_tls_dropped_records();
#endif
