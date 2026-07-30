#pragma once

#include "p8_tls_writer.hpp"

#include "kit/spin_lock.hpp"

// Immutable metric descriptor. Owned by the core (cp8_core::mo_mtk_descs), created
// by register_mtk and serialized once into a P8_SVC_TYPE_MTK service entry. Indexed
// by mi_id (== position in the registry) for O(1) lookup on the emit path.
struct s_p8_mtk_desc
{
    h_p8_mtk_id mi_id;
    char       *mp_name;
    char       *mp_description;
    char       *mp_unit;
    uint8_t     mu_flags;
    double      md_min;
    double      md_max;
};

class cp8_mtk : public cp8_tls_writer
{

public:
    cp8_mtk();

    h_p8_mtk_id       create(const struct s_p8_mtk_base *ip_base);
    bool              emit(h_p8_mtk_id ih_id, double id_value);
    h_p8_mtk_id       create_query(const struct s_p8_mtk_base *ip_base,
                                   uint32_t                    iu_query_interval_ms,
                                   l_p8_mtk_query_cb           il_query,
                                   void                       *ip_user_context);
    h_p8_mtk_group_id create_group(const struct s_p8_mtk_base *ip_base, bool ib_multi_thread);
    h_p8_mtk_group_id create_group_query(const struct s_p8_mtk_base *ip_base,
                                         uint32_t                    iu_query_interval_ms,
                                         l_p8_mtk_group_query_cb     il_query,
                                         void                       *ip_user_context);
    bool              begin_group_emit(h_p8_mtk_group_id ih_group_id);
    bool              emit_group(const char *ip_name, double id_value);
    bool              end_group_emit(h_p8_mtk_group_id ih_group_id);

private:
    kit::c_spin_lock mo_lock;
};
