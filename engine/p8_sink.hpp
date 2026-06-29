#pragma once

#include "kit/list.hpp"

#include <cstddef>
#include <cstdint>

// Service buffers carry no header, only individual sub-elements, so the used
// byte count travels alongside the pointer. Shared with cp8_core.
struct s_p8_svc_buf
{
    uint8_t *mp_buf  = nullptr;
    size_t   mz_used = 0;
};

// Consumer endpoint for every serialized buffer the library produces. The core
// owns exactly one sink for its lifetime and feeds it from the single worker
// thread in protocol order:
//   1. write_hello   - the P8_PACKET_MAIN header, once, before anything else
//   2. write_service - descriptor batches, before the data referencing them
//   3. write_data    - log / trace / metric data batches
// All calls happen on that one worker thread, so implementations need not be
// thread-safe.
class cp8_sink_iface
{
public:
    // Sink kinds parsed from the "sink" configuration key.
    enum e_kind
    {
        e_null = 0, // drop everything
        e_file,     // binary file (not implemented yet -> falls back to null)
        e_net,      // tcp network (not implemented yet -> falls back to null)
    };

    virtual ~cp8_sink_iface()                                              = default;

    // Acquire the underlying medium (open file / connect socket). Returns false
    // on failure, in which case the core falls back to a drop sink.
    virtual bool open()                                                    = 0;

    // Consume a batch of data buffers (logs / traces / metrics). Each buffer
    // begins with s_p8_data_buf_hdr (mu_packet_type + mu_size). Read-only: the
    // core recycles the buffers after the call returns. Returns true on success.
    virtual bool write_data(const kit::c_lst<uint8_t *> &ip_buffers)       = 0;

    // Consume a batch of service buffers (descriptors). Each entry's payload
    // length is in mz_used; the payload type is P8_PACKET_SERVICE. Read-only:
    // the core recycles the buffers after the call returns. Returns true on
    // success.
    virtual bool write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers) = 0;

    // Flush any internal batching buffer to the medium. Called at the end of
    // each drain iteration and on shutdown.
    virtual void flush()                                                   = 0;

    // Release the medium. Called once after the final drain during shutdown.
    virtual void close()                                                   = 0;

    cp8_sink_iface(const cp8_sink_iface &)                                 = delete;
    cp8_sink_iface &operator=(const cp8_sink_iface &)                      = delete;
    cp8_sink_iface(cp8_sink_iface &&)                                      = delete;
    cp8_sink_iface &operator=(cp8_sink_iface &&)                           = delete;

protected:
    cp8_sink_iface() = default;
};
