#pragma once

#include "p8_sink.hpp"

// Drop-everything sink: accepts every buffer and discards it. Used as the
// default and as the fallback when a real sink cannot be constructed.
class cp8_sink_null final : public cp8_sink_iface
{
public:
    bool open() override;
    bool write_data(const kit::c_lst<uint8_t *> &ip_buffers) override;
    bool write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers) override;
    void flush() override;
    void close() override;
};
