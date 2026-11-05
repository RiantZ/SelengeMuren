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
uint64_t cp8_trc::begin(const char                 *ip_name,
                        uint32_t                    iu_line,
                        const char                 *ip_file,
                        const char                 *ip_function,
                        size_t                      iz_attrs,
                        const struct s_p8_attr_val *ip_attrs)
{
    (void)ip_name;
    (void)iu_line;
    (void)ip_file;
    (void)ip_function;
    (void)iz_attrs;
    (void)ip_attrs;
    return 0;
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
    (void)iu_parent_id;
    (void)ip_name;
    (void)iu_line;
    (void)ip_file;
    (void)ip_function;
    (void)iz_attrs;
    (void)ip_attrs;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_trc::end(uint64_t iu_span_id)
{
    (void)iu_span_id;
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
        thread_local cp8_trc lo_tls_trc;
        return lo_tls_trc.begin(ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);
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
        thread_local cp8_trc lo_tls_trc;
        return lo_tls_trc.begin_async(iu_parent_id, ip_name, iu_line, ip_file, ip_function, iz_attrs, ip_attrs);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_trc_end(uint64_t iu_span_id)
    {
        thread_local cp8_trc lo_tls_trc;
        lo_tls_trc.end(iu_span_id);
    }

} // extern "C"
