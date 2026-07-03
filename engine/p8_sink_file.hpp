#pragma once

#include "p8_sink.hpp"

#include "kit/file.hpp"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>

struct s_p8_hdr;

// Binary file sink: writes two append-only binary files under the
// configured "FileBin"."OutDir" directory - one for service descriptors
// (*.p8svc, fed by write_service) and one for the hello header plus
// log / trace / metric data (*.p8dat: write_hello's s_p8_hdr first, then
// every write_data batch). Only "OutDir" is honored for now; rotation keys
// documented in examples/config.json ("Roll", "MaxFiles", "MaxSizeMB") are
// reserved for a future change.
class cp8_sink_file final : public cp8_sink_iface
{
public:
    explicit cp8_sink_file(const nlohmann::json &ir_config);
    ~cp8_sink_file() override;

    bool open() override;
    bool write_hello(const struct s_p8_hdr &ir_hdr) override;
    bool write_data(const kit::c_lst<uint8_t *> &ip_buffers) override;
    bool write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers) override;
    void flush() override;
    void close() override;

#ifdef P8_TESTING
    uint64_t get_bytes_written() const
    {
        return mu_bytes_written.load();
    }
    uint64_t get_bytes_dropped() const
    {
        return mu_bytes_dropped.load();
    }
    uint64_t get_write_error_count() const
    {
        return mu_write_errors.load();
    }
#endif

private:
    // Sends up to mz_max_iov entries per underlying write_v() call (POSIX
    // IOV_MAX guard - kit::c_file::write_v rejects a larger count outright).
    bool write_entries(kit::c_file &ir_file, const kit::s_iovec *ip_iov, size_t iz_count, size_t iz_total_size);

    // Grows mp_iov (realloc) so it can hold at least iz_needed entries.
    // Never shrinks. Returns false on allocation failure.
    bool grow_iov(size_t iz_needed);

    // Accounts iz_total_size bytes as dropped (with ip_reason logged once) and
    // returns false. Used whenever a whole batch cannot be sent.
    bool drop_batch(size_t iz_total_size, const char *ip_reason);

    static constexpr size_t mz_max_iov  = 1024;

    bool                  mb_configured = false;
    std::filesystem::path mo_out_dir;

    kit::c_file mo_svc_file;
    kit::c_file mo_data_file;

    // scatter/gather scratch buffer: allocated on first use and grown
    // (realloc, never shrunk) as larger batches arrive, so steady-state
    // operation performs no further allocation. Reused across
    // write_data/write_service since the core only ever calls this sink
    // from a single worker thread, never concurrently.
    kit::s_iovec *mp_iov     = nullptr;
    size_t        mz_iov_cap = 0;

    std::atomic<uint64_t> mu_bytes_written { 0 };
    std::atomic<uint64_t> mu_bytes_dropped { 0 };
    std::atomic<uint64_t> mu_write_errors { 0 };
};
