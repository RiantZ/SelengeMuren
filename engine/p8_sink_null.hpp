#pragma once

#include "p8_sink.hpp"
#include <atomic>

// Drop-everything sink: accepts every buffer and discards it. Used as the
// default and as the fallback when a real sink cannot be constructed.
class cp8_sink_null final : public cp8_sink_iface
{
    std::atomic_int64_t mu_total = 0;

public:
    bool open() override;
    bool write_hello(const struct s_p8_hdr &ir_hdr) override;
    bool write_data(const kit::c_lst<uint8_t *> &ip_buffers) override;
    bool write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers) override;
    void flush() override;
    void close() override;
};
