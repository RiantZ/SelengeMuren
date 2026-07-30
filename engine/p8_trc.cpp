#include "p8_trc.hpp"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_trc::cp8_trc()
    : mp_core(cp8_core::get_global_core(P8_CORE_ACQUIRE_TIMEOUT_MS))
{
    if(mp_core)
    {
        mp_core->addref();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_trc::~cp8_trc()
{
    if(mp_core)
    {
        mp_core->release();
        mp_core = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t cp8_trc::begin(uint64_t                    iu_parent_trace_id,
                        uint32_t                    iu_line,
                        const char                 *ip_file,
                        const char                 *ip_function,
                        size_t                      iz_attrs,
                        const struct s_p8_attr_val *ip_attrs,
                        const char                 *ip_function_args,
                        va_list                     io_args)
{
    (void)iu_parent_trace_id;
    (void)iu_line;
    (void)ip_file;
    (void)ip_function;
    (void)iz_attrs;
    (void)ip_attrs;
    (void)ip_function_args;
    (void)io_args;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_trc::end(uint64_t iu_trace_id)
{
    (void)iu_trace_id;
    return false;
}

extern "C"
{
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    uint64_t p8_trc_begin(uint64_t                    iu_parent_trace_id,
                          uint32_t                    iu_line,
                          const char                 *ip_file,
                          const char                 *ip_function,
                          size_t                      iz_attrs,
                          const struct s_p8_attr_val *ip_attrs,
                          const char                 *ip_function_args,
                          ...)
    {
        thread_local cp8_trc lo_tls_trc;
        va_list              lo_args;
        va_start(lo_args, ip_function_args);
        uint64_t lu_result = lo_tls_trc.begin(iu_parent_trace_id,
                                              iu_line,
                                              ip_file,
                                              ip_function,
                                              iz_attrs,
                                              ip_attrs,
                                              ip_function_args,
                                              lo_args);
        va_end(lo_args);
        return lu_result;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_trc_end(uint64_t iu_trace_id)
    {
        thread_local cp8_trc lo_tls_trc;
        return lo_tls_trc.end(iu_trace_id);
    }

} // extern "C"
