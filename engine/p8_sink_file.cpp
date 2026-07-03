#include "p8_sink_file.hpp"
#include "p8_config_keys.hpp"
#include "p8_protocol.h"

#include "kit/system.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <system_error>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_sink_file::cp8_sink_file(const nlohmann::json &ir_config)
{
    if(!ir_config.contains(P8_CFG_KEY_FILE_BIN))
    {
        std::fprintf(stderr, "cp8_sink_file: missing \"%s\" config section\n", P8_CFG_KEY_FILE_BIN);
        return;
    }

    const auto &lo_file_bin = ir_config[P8_CFG_KEY_FILE_BIN];

    if(!lo_file_bin.contains(P8_CFG_KEY_FILE_OUT_DIR))
    {
        std::fprintf(stderr, "cp8_sink_file: missing \"%s\" key\n", P8_CFG_KEY_FILE_OUT_DIR);
        return;
    }

    const std::string ls_out_dir = lo_file_bin[P8_CFG_KEY_FILE_OUT_DIR].get<std::string>();
    if(ls_out_dir.empty())
    {
        std::fprintf(stderr, "cp8_sink_file: \"%s\" is empty\n", P8_CFG_KEY_FILE_OUT_DIR);
        return;
    }

    mo_out_dir    = ls_out_dir;
    mb_configured = true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_sink_file::~cp8_sink_file()
{
    std::free(mp_iov);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::open()
{
    if(!mb_configured)
    {
        return false;
    }

    std::error_code lo_ec;
    std::filesystem::create_directories(mo_out_dir, lo_ec);
    if(lo_ec)
    {
        std::fprintf(stderr,
                     "cp8_sink_file::open: failed to create output directory %s: %s\n",
                     mo_out_dir.string().c_str(),
                     lo_ec.message().c_str());
        return false;
    }

    // session-unique base name: UTC timestamp + process id, guards against
    // filename collisions between processes sharing one OutDir
    const std::time_t lz_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm           lo_tm  = {};
#if defined(_WIN32)
    gmtime_s(&lo_tm, &lz_now);
#else
    gmtime_r(&lz_now, &lo_tm);
#endif

    char la_stamp[32];
    std::strftime(la_stamp, sizeof(la_stamp), "%Y%m%d_%H%M%S", &lo_tm);

    const std::string ls_base = std::string("p8_") + la_stamp + "_" + std::to_string(kit::get_process_id());

    const std::filesystem::path lo_svc_path  = mo_out_dir / (ls_base + ".p8svc");
    const std::filesystem::path lo_data_path = mo_out_dir / (ls_base + ".p8dat");

    if(!mo_svc_file.open(lo_svc_path, kit::e_fom_create_new, kit::e_ff_write))
    {
        std::fprintf(stderr,
                     "cp8_sink_file::open: failed to create %s (err=%d)\n",
                     lo_svc_path.string().c_str(),
                     mo_svc_file.get_last_error());
        return false;
    }

    if(!mo_data_file.open(lo_data_path, kit::e_fom_create_new, kit::e_ff_write))
    {
        std::fprintf(stderr,
                     "cp8_sink_file::open: failed to create %s (err=%d)\n",
                     lo_data_path.string().c_str(),
                     mo_data_file.get_last_error());
        mo_svc_file.close(false);
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::write_hello(const struct s_p8_hdr &ir_hdr)
{
    kit::s_iovec lo_iov { const_cast<struct s_p8_hdr *>(&ir_hdr), sizeof(ir_hdr) };
    return write_entries(mo_data_file, &lo_iov, 1, sizeof(ir_hdr));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::grow_iov(size_t iz_needed)
{
    if(iz_needed <= mz_iov_cap)
    {
        return true;
    }

    kit::s_iovec *lp_grown = static_cast<kit::s_iovec *>(std::realloc(mp_iov, iz_needed * sizeof(kit::s_iovec)));
    if(!lp_grown)
    {
        return false;
    }

    mp_iov     = lp_grown;
    mz_iov_cap = iz_needed;
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::drop_batch(size_t iz_total_size, const char *ip_reason)
{
    if(iz_total_size == 0)
    {
        return true;
    }

    mu_bytes_dropped += iz_total_size;
    if(mu_write_errors.fetch_add(1) == 0)
    {
        std::fprintf(stderr,
                     "cp8_sink_file: %s, dropping %llu bytes\n",
                     ip_reason,
                     static_cast<unsigned long long>(iz_total_size));
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Sends the [ip_iov, ip_iov + iz_count) scatter/gather batch to ir_file, in
// chunks of at most mz_max_iov entries (kit::c_file::write_v rejects a
// larger count outright on POSIX). iz_total_size is the sum of every
// mz_size in the batch, passed in by the caller since it already computed
// it while filling the array.
bool cp8_sink_file::write_entries(kit::c_file        &ir_file,
                                  const kit::s_iovec *ip_iov,
                                  size_t              iz_count,
                                  size_t              iz_total_size)
{
    if(iz_total_size == 0)
    {
        return true;
    }

    if(!ir_file.is_open())
    {
        return drop_batch(iz_total_size, "write attempted while sink is not open");
    }

    bool                lb_ok        = true;
    const kit::s_iovec *lp_chunk     = ip_iov;
    size_t              lz_remaining = iz_count;

    while(lz_remaining > 0)
    {
        const size_t lz_chunk_count       = std::min(mz_max_iov, lz_remaining);

        size_t              lz_chunk_size = 0;
        const kit::s_iovec *lp_entry      = lp_chunk;
        for(size_t lz_i = 0; lz_i < lz_chunk_count; ++lz_i, ++lp_entry)
        {
            lz_chunk_size += lp_entry->mz_size;
        }

        const size_t lz_written  = ir_file.write_v(lp_chunk, lz_chunk_count, false);
        mu_bytes_written        += lz_written;

        if(lz_written != lz_chunk_size)
        {
            const size_t lz_missing  = lz_chunk_size - lz_written;
            mu_bytes_dropped        += lz_missing;
            if(mu_write_errors.fetch_add(1) == 0)
            {
                std::fprintf(stderr,
                             "cp8_sink_file: short scatter/gather write (%llu/%llu bytes), dropping %llu bytes\n",
                             static_cast<unsigned long long>(lz_written),
                             static_cast<unsigned long long>(lz_chunk_size),
                             static_cast<unsigned long long>(lz_missing));
            }
            lb_ok = false;
        }

        lp_chunk     += lz_chunk_count;
        lz_remaining -= lz_chunk_count;
    }

    return lb_ok;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::write_data(const kit::c_lst<uint8_t *> &ip_buffers)
{
    const size_t lz_count = ip_buffers.size();
    if(lz_count == 0)
    {
        return true;
    }

    if(!grow_iov(lz_count))
    {
        size_t lz_total = 0;
        for(const auto &lp_buf : ip_buffers)
        {
            lz_total += reinterpret_cast<const s_p8_data_buf_hdr *>(lp_buf)->mu_size;
        }
        return drop_batch(lz_total, "failed to grow scatter/gather buffer");
    }

    kit::s_iovec *lp_iov   = mp_iov;
    size_t        lz_total = 0;

    for(const auto &lp_buf : ip_buffers)
    {
        const s_p8_data_buf_hdr *lp_hdr  = reinterpret_cast<const s_p8_data_buf_hdr *>(lp_buf);
        lp_iov->mp_data                  = lp_buf;
        lp_iov->mz_size                  = lp_hdr->mu_size;
        lz_total                        += lp_hdr->mu_size;
        ++lp_iov;
    }

    return write_entries(mo_data_file, mp_iov, lz_count, lz_total);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_sink_file::write_service(const kit::c_lst<s_p8_svc_buf> &ip_buffers)
{
    const size_t lz_count = ip_buffers.size();
    if(lz_count == 0)
    {
        return true;
    }

    if(!grow_iov(lz_count))
    {
        size_t lz_total = 0;
        for(const auto &lo_entry : ip_buffers)
        {
            lz_total += lo_entry.mz_used;
        }
        return drop_batch(lz_total, "failed to grow scatter/gather buffer");
    }

    kit::s_iovec *lp_iov   = mp_iov;
    size_t        lz_total = 0;

    for(const auto &lo_entry : ip_buffers)
    {
        lp_iov->mp_data  = lo_entry.mp_buf;
        lp_iov->mz_size  = lo_entry.mz_used;
        lz_total        += lo_entry.mz_used;
        ++lp_iov;
    }

    return write_entries(mo_svc_file, mp_iov, lz_count, lz_total);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_sink_file::flush()
{
    if(mo_svc_file.is_open())
    {
        mo_svc_file.sync();
    }

    if(mo_data_file.is_open())
    {
        mo_data_file.sync();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_sink_file::close()
{
    mo_svc_file.close(true);
    mo_data_file.close(true);

    std::fprintf(stderr,
                 "cp8_sink_file: %llu bytes written, %llu bytes dropped, %llu write errors\n",
                 static_cast<unsigned long long>(mu_bytes_written.load()),
                 static_cast<unsigned long long>(mu_bytes_dropped.load()),
                 static_cast<unsigned long long>(mu_write_errors.load()));
}
