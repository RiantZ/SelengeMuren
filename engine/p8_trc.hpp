#pragma once

#include "p8_core.hpp"

class cp8_trc
{
public:
    cp8_trc();
    ~cp8_trc();

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
    cp8_core *mp_core = nullptr;
};
