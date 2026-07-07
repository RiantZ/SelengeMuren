#include "p8_core.hpp"
#include "p8_config_keys.hpp"
#include "p8_hash.hpp"
#include "p8_log.hpp"
#include "p8_profiler.hpp"
#include "p8_sink_file.hpp"
#include "p8_sink_null.hpp"
#include "p8_tls_writer.hpp"

#include "kit/endian.hpp"
#include "kit/shared_mem.hpp"
#include "kit/system.hpp"
#include "kit/thread.hpp"
#include "kit/time.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <strings.h>
#include <chrono>
#include <new>
#include <string>
#include <thread>

// round a byte count up to the next 8-byte boundary
#define P8_ALIGN_UP_8(x) (((x) + 7u) & ~static_cast<size_t>(7u))

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// singleton state
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static cp8_core             *gp_instance   = nullptr;
static c_shared::h_shared    gp_shm_handle = nullptr;
static const tXCHAR         *gp_shm_name   = TM("p8");
static std::atomic<uint32_t> gu_instance_count { 0 };

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// helpers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static bool parse_size(const char *ip_str, size_t &oz_result)
{
    char   la_buf[128];
    size_t lz_dst = 0;

    for(size_t lz_i = 0; ip_str[lz_i] != '\0' && lz_dst < sizeof(la_buf) - 1; ++lz_i)
    {
        if(ip_str[lz_i] != ' ' && ip_str[lz_i] != '\t')
        {
            la_buf[lz_dst++] = ip_str[lz_i];
        }
    }
    la_buf[lz_dst] = '\0';

    if(strcasecmp(la_buf, "infinite") == 0)
    {
        oz_result = std::numeric_limits<size_t>::max();
        return true;
    }

    char              *lp_end = nullptr;
    unsigned long long lu_val = std::strtoull(la_buf, &lp_end, 10);

    if(lp_end == la_buf)
    {
        return false;
    }

    if(*lp_end == '\0')
    {
        oz_result = static_cast<size_t>(lu_val);
        return true;
    }

    if(strcasecmp(lp_end, "KB") == 0 || strcasecmp(lp_end, "Ki") == 0)
    {
        oz_result = static_cast<size_t>(lu_val * 1024);
        return true;
    }

    if(strcasecmp(lp_end, "MB") == 0 || strcasecmp(lp_end, "Mi") == 0)
    {
        oz_result = static_cast<size_t>(lu_val * 1024 * 1024);
        return true;
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Create a heap sink for the requested kind; the caller owns it and must delete
// it. Unimplemented kinds (net) and unrecognized values currently return nullptr;
// the caller falls back to a null sink.
static cp8_sink_iface *create_sink(const char *ip_value, const nlohmann::json &ir_config)
{
    if(!ip_value)
    {
        // No sink configured: default to the null sink.
        std::fprintf(stderr, "create_sink: warning! default null sinc is created\n");
        return new(std::nothrow) cp8_sink_null();
    }

    if(strcmp(ip_value, P8_CFG_VAL_SINK_FILE_BIN) == 0)
    {
        return new(std::nothrow) cp8_sink_file(ir_config);
    }

    if(strcmp(ip_value, P8_CFG_VAL_SINK_NETWORK_TCP) == 0)
    {
        std::fprintf(stderr, "create_sink: network sink not implemented yet, falling back to null sink\n");
        return nullptr;
    }

    if(strcmp(ip_value, P8_CFG_VAL_SINK_NETWORK_NULL) == 0)
    {
        return new(std::nothrow) cp8_sink_null();
    }

    std::fprintf(stderr, "create_sink: unrecognized sinc type %s\n", ip_value);
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cp8_core implementation
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_core::cp8_core(const struct s_p8_config *ip_config)
{
    gu_instance_count.fetch_add(1, std::memory_order_relaxed);

    if(!ip_config->mp_json_config)
    {
        std::fprintf(stderr, "cp8_core: mp_json_config is NULL\n");
        return;
    }

    nlohmann::json lo_json;

    try
    {
        lo_json = nlohmann::json::parse(ip_config->mp_json_config,
                                        nullptr, // callback
                                        true,    // allow_exceptions
                                        true);   // ignore_comments
    }
    catch(const nlohmann::json::parse_error &ir_err)
    {
        std::fprintf(stderr, "cp8_core: JSON parse error: %s\n", ir_err.what());
        return;
    }
    catch(const std::exception &ir_err)
    {
        std::fprintf(stderr, "cp8_core: unexpected error: %s\n", ir_err.what());
        return;
    }

    std::string ls_max_mem;
    std::string ls_init_mem;

    if(lo_json.contains(P8_CFG_KEY_MAX_MEMORY_SIZE))
    {
        ls_max_mem = lo_json[P8_CFG_KEY_MAX_MEMORY_SIZE].get<std::string>();
    }

    if(lo_json.contains(P8_CFG_KEY_INITIAL_MEMORY_SIZE))
    {
        ls_init_mem = lo_json[P8_CFG_KEY_INITIAL_MEMORY_SIZE].get<std::string>();
    }

    if(!init_buffer_pool(ls_max_mem.empty() ? nullptr : ls_max_mem.c_str(),
                         ls_init_mem.empty() ? nullptr : ls_init_mem.c_str()))
    {
        return;
    }

    init_header(ip_config);

    std::string ls_sink_value;

    if(lo_json.contains(P8_CFG_KEY_SINK))
    {
        ls_sink_value = lo_json[P8_CFG_KEY_SINK].get<std::string>();
    }

    mp_sink = create_sink(ls_sink_value.empty() ? nullptr : ls_sink_value.c_str(), lo_json);
    if(!mp_sink)
    {
        std::fprintf(stderr, "cp8_core: sink allocation failed\n");
        return;
    }

    if(!mp_sink->open())
    {
        std::fprintf(stderr, "cp8_core: sink open failed, falling back to null sink\n");
        delete mp_sink;
        mp_sink = new(std::nothrow) cp8_sink_null();
        if(!mp_sink || !mp_sink->open())
        {
            std::fprintf(stderr, "cp8_core: fallback null sink failed\n");
            delete mp_sink;
            mp_sink = nullptr;
            return;
        }
    }

    if(!mp_sink->write_hello(mo_hdr))
    {
        std::fprintf(stderr, "cp8_core: sink write_hello failed\n");
    }

    mb_initialized = true;

    start_worker();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_core::~cp8_core()
{
    stop_worker();

    // the final drain ran inside stop_worker(); release the sink afterwards
    if(mp_sink)
    {
        mp_sink->close();
        delete mp_sink;
        mp_sink = nullptr;
    }

    for(auto &lo_pair : mo_log_descs)
    {
        delete lo_pair.second;
    }
    mo_log_descs.clear();

    for(s_p8_attr_desc *lp_desc : mo_attr_descs)
    {
        std::free(lp_desc->mp_name);
        delete lp_desc;
    }
    mo_attr_descs.clear();
    mo_attr_name_map.clear();

    // drop references to any still-held service buffers; the pool destructor
    // frees the underlying memory below
    mo_svc_buffers.clear();

    delete mp_data_pool;
    mp_data_pool = nullptr;
    mp_memory_budget.reset();

    gu_instance_count.fetch_sub(1, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::addref()
{
    mu_ref_count.fetch_add(1, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::release()
{
    if(mu_ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        delete this;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_core::init_buffer_pool(const char *ip_max_memory_size, const char *ip_initial_memory_size)
{
    // N.B.: If you will modify this values, please update @doc/config_example.json:4-12
    size_t lz_initial_memory_size = 1024 * 1024;
    size_t lz_max_memory_size     = 16 * lz_initial_memory_size;

    if(ip_max_memory_size)
    {
        if(!parse_size(ip_max_memory_size, lz_max_memory_size))
        {
            std::fprintf(stderr, "cp8_core::init_buffer_pool: invalid max_memory_size value\n");
            return false;
        }
    }

    if(ip_initial_memory_size)
    {
        if(!parse_size(ip_initial_memory_size, lz_initial_memory_size))
        {
            std::fprintf(stderr, "cp8_core::init_buffer_pool: invalid initial_memory_size value\n");
            return false;
        }
    }

    if(lz_initial_memory_size > lz_max_memory_size)
    {
        lz_initial_memory_size = lz_max_memory_size;
    }

    try
    {
        mp_memory_budget = std::make_shared<cp8_memory_budget>(lz_max_memory_size);
    }
    catch(const std::bad_alloc &)
    {
        std::fprintf(stderr, "cp8_core::init_buffer_pool: budget allocation failed\n");
        return false;
    }

    mp_data_pool = new(std::nothrow) cp8_buffer_pool(mz_data_buffer_size, mp_memory_budget);
    if(!mp_data_pool)
    {
        std::fprintf(stderr, "cp8_core::init_buffer_pool: data pool allocation failed\n");
        mp_memory_budget.reset();
        return false;
    }

    mp_data_pool->init(lz_initial_memory_size / mz_data_buffer_size);

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_core::init_header(const struct s_p8_config *ip_config)
{
    std::memset(&mo_hdr, 0, sizeof(mo_hdr));

    mo_hdr.mu_packet_type      = P8_PACKET_MAIN;
    mo_hdr.mu_protocol_version = P8_PROTOCOL_VERSION;

    uint32_t lu_endian         = 0x1;
    mo_hdr.mu_is_big_endian    = (*reinterpret_cast<uint8_t *>(&lu_endian) == 0) ? 1 : 0;

    mo_hdr.mi_utc_sec_offset   = static_cast<int8_t>(kit::get_utc_offset_seconds() / 3600);
    mo_hdr.mu_size             = static_cast<uint32_t>(sizeof(struct s_p8_hdr));
    mo_hdr.mu_process_id       = kit::get_process_id();
    mo_hdr.mu_system_time      = kit::get_system_time();

    // TODO: use this callbacks by logs, traces, metrics
    if(ip_config->ml_timer_value && ip_config->ml_timer_frequency)
    {
        mo_hdr.mu_hires_tick       = ip_config->ml_timer_value(ip_config->mp_ctx_timer);
        uint64_t lu_freq           = ip_config->ml_timer_frequency(ip_config->mp_ctx_timer);
        mo_hdr.mu_hires_freq_numer = 1000000000ULL;
        mo_hdr.mu_hires_freq_denom = lu_freq;
    }
    else
    {
        mo_hdr.mu_hires_tick = kit::get_hires_ticks();
        kit::get_hires_ticks_freq(mo_hdr.mu_hires_freq_numer, mo_hdr.mu_hires_freq_denom);
    }

    kit::get_process_name(mo_hdr.mp_process_name, sizeof(mo_hdr.mp_process_name));
    kit::get_host_name(mo_hdr.mp_host_name, sizeof(mo_hdr.mp_host_name));

    if(mo_hdr.mu_is_big_endian)
    {
        mo_hdr.mu_size             = kit::bswap32(mo_hdr.mu_size);
        mo_hdr.mi_utc_sec_offset   = kit::bswap32(mo_hdr.mi_utc_sec_offset);
        mo_hdr.mu_process_id       = kit::bswap32(mo_hdr.mu_process_id);
        mo_hdr.mu_system_time      = kit::bswap64(mo_hdr.mu_system_time);
        mo_hdr.mu_hires_tick       = kit::bswap64(mo_hdr.mu_hires_tick);
        mo_hdr.mu_hires_freq_numer = kit::bswap64(mo_hdr.mu_hires_freq_numer);
        mo_hdr.mu_hires_freq_denom = kit::bswap64(mo_hdr.mu_hires_freq_denom);
    }

    mo_hdr.mu_hash = XXH3_64bits(&mo_hdr, offsetof(struct s_p8_hdr, mu_hash));

    if(mo_hdr.mu_is_big_endian)
    {
        mo_hdr.mu_hash = kit::bswap64(mo_hdr.mu_hash);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_core::get_initialized() const
{
    return mb_initialized;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::exceptional_flush()
{
    if(!mb_initialized)
    {
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::start_worker()
{
    const c_event::e_type la_types[] = { c_event::e_single_auto, c_event::e_multi };

    if(!mo_worker_event.init(2, la_types))
    {
        std::fprintf(stderr, "cp8_core::start_worker: event init failed\n");
        return;
    }

    mo_worker_thread  = std::thread(&cp8_core::worker_main, this);
    mb_worker_running = true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::stop_worker()
{
    if(!mb_worker_running)
    {
        return;
    }

    mo_worker_event.set(mu_event_stop);

    if(mo_worker_thread.joinable())
    {
        mo_worker_thread.join();
    }

    mb_worker_running = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::worker_main()
{
    kit::c_lst<uint8_t *>    lo_ready;
    kit::c_lst<s_p8_svc_buf> lo_bufs;

    // best-effort: raising priority typically needs elevated privileges, failure is non-fatal
    kit::set_thread_priority(kit::e_tp_time_critical);

    // Stats-poll cadence: the loop wakes irregularly (event or timeout), so gate
    // the drop-counter poll on a monotonic clock rather than an iteration count.
    // Precompute the interval in ticks once so the per-iteration test is just a
    // subtract-and-compare.
    uint64_t lu_stats_numer = 0;
    uint64_t lu_stats_denom = 0;
    kit::get_hires_ticks_freq(lu_stats_numer, lu_stats_denom);
    const uint64_t lu_stats_poll_ticks = lu_stats_numer ? static_cast<uint64_t>(P8_CORE_STATS_POLL_INTERVAL_MS)
                                                              * 1000000ULL * lu_stats_denom / lu_stats_numer
                                                        : 0;
    uint64_t       lu_stats_last_ticks = kit::get_hires_ticks();

    for(;;)
    {
        uint32_t lu_signal = mo_worker_event.wait(P8_CORE_THREAD_TIMEOUT_MS);

#ifdef P8_TESTING
        // While capture is active the synchronous test drain is the sole
        // consumer of both service and data buffers; the worker must not touch
        // either or it would race the test on the captured/serialized stores.
        const bool lb_skip = mb_capture_enabled.load(std::memory_order_relaxed);
#else
        constexpr bool lb_skip = false;
#endif

        if(!lb_skip)
        {
            // move buffers from list protected by mutex to local one
            mo_svc_mutex.lock();
            while(mo_svc_buffers.size())
            {
                lo_bufs.push_last(mo_svc_buffers.pull_first());
            }
            mo_svc_mutex.unlock();
            //--------------------------------------------------------

            if(lo_bufs.size() > 0)
            {
                mp_sink->write_service(lo_bufs);
            }

            // recycle the consumed service buffers back to the pool
            lo_bufs.clear([this](const s_p8_svc_buf &ir_pair) { release_buffer(ir_pair.mp_buf); },
                          kit::e_c_lst_pool_policy::e_keep);

            // move buffers from the ready queue (writer-shutdown path) to local one
            mo_ready_lock.lock();
            while(mo_ready_queue.size() > 0)
            {
                lo_ready.push_last(mo_ready_queue.pull_first());
            }
            mo_ready_lock.unlock();
            //--------------------------------------------------------

            // pull accumulated buffers from every live writer into the same batch
            drain_writers(lo_ready);

            // On the stats cadence, pull-and-reset every writer's drop counters
            // into the core accumulators.
            if(lu_stats_poll_ticks)
            {
                uint64_t lu_now_ticks = kit::get_hires_ticks();
                if((lu_now_ticks - lu_stats_last_ticks) >= lu_stats_poll_ticks)
                {
                    poll_dropped_stats();
                    lu_stats_last_ticks = lu_now_ticks;
                }
            }

            // Clear the pressure-wake debounce before draining. Every drain (wake,
            // submit, or timeout) pulls from all writers and relieves pressure, so
            // the clear is unconditional. It must happen strictly before the flush_ready
            // below: clearing after would let a pressure notify raced in during the
            // drain be suppressed and lost until the next timeout. seq_cst keeps the
            // store from sinking past the acquire-only mutex locks that follow.
            mb_wake_pending.store(false, std::memory_order_seq_cst);

            // write the batch to the sink and recycle it (single capture point)
            flush_ready(lo_ready);
        }
        else
        {
            // Clear the pressure-wake debounce before draining.
            mb_wake_pending.store(false, std::memory_order_seq_cst);
        }

        mp_sink->flush();

        if(lu_signal == mu_event_stop)
        {
            break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::notify()
{
    mo_worker_event.set(mu_event_wake);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::notify_pressure()
{
    // Fast path: a wake is already pending. Pure relaxed load, no write, so the
    // cache line stays Shared across all acquiring cores under sustained load.
    if(mb_wake_pending.load(std::memory_order_relaxed))
    {
        return;
    }

    // Claim the wake. Only the thread that flips false->true pays for set();
    // the worker clears the flag after wait(), re-arming the next pressure wake.
    if(!mb_wake_pending.exchange(true, std::memory_order_acquire))
    {
        notify();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::register_writer(cp8_tls_writer *ip_writer)
{
    std::lock_guard<std::mutex> lo_guard(mo_writers_lock);
    ip_writer->mp_prev_writer = nullptr;
    ip_writer->mp_next_writer = mp_writers_head;
    if(mp_writers_head)
    {
        mp_writers_head->mp_prev_writer = ip_writer;
    }
    mp_writers_head = ip_writer;
    ++mz_writers_count;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::unregister_writer(cp8_tls_writer *ip_writer)
{
    std::lock_guard<std::mutex> lo_guard(mo_writers_lock);
    if(ip_writer->mp_prev_writer)
    {
        ip_writer->mp_prev_writer->mp_next_writer = ip_writer->mp_next_writer;
    }
    else
    {
        mp_writers_head = ip_writer->mp_next_writer;
    }
    if(ip_writer->mp_next_writer)
    {
        ip_writer->mp_next_writer->mp_prev_writer = ip_writer->mp_prev_writer;
    }
    ip_writer->mp_next_writer = nullptr;
    ip_writer->mp_prev_writer = nullptr;
    --mz_writers_count;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_core::register_current_thread(const char *)
{
    if(!mb_initialized)
    {
        return false;
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::unregister_current_thread()
{
    if(!mb_initialized)
    {
        return;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
p8_attr_id cp8_core::attr_register(const char *ip_name, enum e_p8_attr_type ie_type)
{
    if(!mb_initialized)
    {
        return P8_ATTR_ERROR_NOT_INITIALIZED;
    }

    if(!ip_name || ip_name[0] == '\0')
    {
        return P8_ATTR_ERROR_INVALID_NAME;
    }

    std::lock_guard<std::mutex> lo_lock(mo_attr_mutex);

    auto lo_it = mo_attr_name_map.find(ip_name);
    if(lo_it != mo_attr_name_map.end())
    {
        p8_attr_id li_existing = lo_it->second;
        if(mo_attr_descs[static_cast<size_t>(li_existing)]->me_type != ie_type)
        {
            return P8_ATTR_ERROR_TYPE_MISMATCH;
        }
        return li_existing;
    }

    s_p8_attr_desc *lp_desc = new(std::nothrow) s_p8_attr_desc;
    if(!lp_desc)
    {
        return P8_ATTR_ERROR_ALLOC_FAILED;
    }

    lp_desc->mp_name = strdup(ip_name);
    if(!lp_desc->mp_name)
    {
        delete lp_desc;
        return P8_ATTR_ERROR_ALLOC_FAILED;
    }

    lp_desc->mi_id   = static_cast<p8_attr_id>(mo_attr_descs.size());
    lp_desc->me_type = ie_type;

    mo_attr_descs.push_back(lp_desc);
    mo_attr_name_map[ip_name] = lp_desc->mi_id;

    {
        std::lock_guard<std::mutex> lo_svc_lock(mo_svc_mutex);
        serialize_attr_desc(lp_desc);
    }

    return lp_desc->mi_id;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
p8_attr_id cp8_core::attr_get(const char *ip_name) const
{
    if(!mb_initialized)
    {
        return P8_ATTR_ERROR_NOT_INITIALIZED;
    }

    if(!ip_name || ip_name[0] == '\0')
    {
        return P8_ATTR_ERROR_NOT_FOUND;
    }

    std::lock_guard<std::mutex> lo_lock(mo_attr_mutex);

    auto lo_it = mo_attr_name_map.find(ip_name);
    if(lo_it != mo_attr_name_map.end())
    {
        return lo_it->second;
    }

    return P8_ATTR_ERROR_NOT_FOUND;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::sync_attr_cache(std::vector<const s_p8_attr_desc *> &io_cache)
{
    std::lock_guard<std::mutex> lo_lock(mo_attr_mutex);

    size_t lz_old = io_cache.size();
    size_t lz_new = mo_attr_descs.size();

    if(lz_old >= lz_new)
    {
        return;
    }

    io_cache.resize(lz_new);
    for(size_t lz_i = lz_old; lz_i < lz_new; ++lz_i)
    {
        io_cache[lz_i] = mo_attr_descs[lz_i];
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_core::acquire_buffer()
{
    if(!mb_initialized)
    {
        return nullptr;
    }

    mp_data_pool->lock();
    cp8_buffer_pool::s_stat ls_stat = {};
    uint8_t                *lp_buf  = mp_data_pool->acquire_no_lock();

    mp_data_pool->stat(ls_stat);
    mp_data_pool->unlock();

    if(!lp_buf)
    {
        return nullptr;
    }

    // Track pool pressure: when free memory drops below P8_CORE_FREE_MEM_PERCENT
    // of the budget max, wake the worker so it pulls from all writers. Free
    // memory here is the exact budget headroom (max - outstanding), so a pool
    // that has grown only partially is not treated as being under pressure.
    // Debounced: skip the wake if a previous pressure notify is still unhandled,
    // so a burst of acquiring threads does not storm the worker with redundant
    // set() calls (each a pthread lock + a counting-semaphore post).
    if(ls_stat.mz_max_size > 0 && (ls_stat.mz_free_size * 100ull / ls_stat.mz_max_size) < P8_CORE_FREE_MEM_PERCENT)
    {
        notify_pressure();
    }

    return lp_buf;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::release_buffer(uint8_t *ip_buffer)
{
    if(!ip_buffer || !mb_initialized)
    {
        return;
    }

    mp_data_pool->recycle(ip_buffer);
    mu_outstanding_buffers.fetch_sub(1, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::release_buffers(kit::c_lst<uint8_t *> &io_buffers)
{
    if(!mb_initialized)
    {
        return;
    }

    if(0 == io_buffers.size())
    {
        return;
    }

    io_buffers.clear([this](uint8_t *ip_buf) { release_buffer(ip_buf); }, kit::e_c_lst_pool_policy::e_keep);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::submit_buffer(uint8_t *ip_buffer)
{
    if(!ip_buffer || !mb_initialized)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lo_guard(mo_ready_lock);
        mo_ready_queue.push_last(ip_buffer);
    }

    notify();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::submit_chain(kit::c_lst<uint8_t *> &io_buffers)
{
    if(!mb_initialized)
    {
        return;
    }

    if(0 == io_buffers.size())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lo_guard(mo_ready_lock);

        io_buffers.clear([this](uint8_t *ip_buf) { mo_ready_queue.push_last(ip_buf); },
                         kit::e_c_lst_pool_policy::e_keep);
    }

    notify();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::drain_writers(kit::c_lst<uint8_t *> &io_data)
{
    // Counts every invocation (including the early-out below) plus its timing.
    // No-op unless the build was configured with a *-tracy preset.
    P8_PROF_ZONE();

    if(!mb_initialized)
    {
        return;
    }

    std::lock_guard<std::mutex> lo_guard(mo_writers_lock);

    for(cp8_tls_writer *lp_writer = mp_writers_head; lp_writer; lp_writer = lp_writer->mp_next_writer)
    {
        lp_writer->pull(io_data);
    }

    // Plot the drained buffer count on function exit. No-op without *-tracy.
    P8_PROF_PLOT("drain_writers io_data.size", static_cast<int64_t>(io_data.size()));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::poll_dropped_stats()
{
    s_p8_drop_stats lo_sum { 0, 0, 0 };

    {
        std::lock_guard<std::mutex> lo_guard(mo_writers_lock);
        for(cp8_tls_writer *lp_writer = mp_writers_head; lp_writer; lp_writer = lp_writer->mp_next_writer)
        {
            s_p8_drop_stats lo_writer  = lp_writer->pull_dropped();
            lo_sum.mu_logs            += lo_writer.mu_logs;
            lo_sum.mu_metrics         += lo_writer.mu_metrics;
            lo_sum.mu_traces          += lo_writer.mu_traces;
        }
    }

    accumulate_dropped(lo_sum);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::accumulate_dropped(const s_p8_drop_stats &ir_stats)
{
    mu_dropped_logs.fetch_add(ir_stats.mu_logs, std::memory_order_relaxed);
    mu_dropped_metrics.fetch_add(ir_stats.mu_metrics, std::memory_order_relaxed);
    mu_dropped_traces.fetch_add(ir_stats.mu_traces, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_p8_drop_stats cp8_core::get_dropped_stats() const
{
    s_p8_drop_stats lo_stats;
    lo_stats.mu_logs    = mu_dropped_logs.load(std::memory_order_relaxed);
    lo_stats.mu_metrics = mu_dropped_metrics.load(std::memory_order_relaxed);
    lo_stats.mu_traces  = mu_dropped_traces.load(std::memory_order_relaxed);
    return lo_stats;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::flush_ready(kit::c_lst<uint8_t *> &io_ready)
{
    // Counts every invocation (including the early-out below) plus its timing.
    // No-op unless the build was configured with a *-tracy preset.
    P8_PROF_ZONE();

    // Plot the incoming buffer count on function entry. No-op without *-tracy.
    P8_PROF_PLOT("flush_ready io_ready.size", static_cast<int64_t>(io_ready.size()));

    if(0 == io_ready.size())
    {
        return;
    }

#ifdef P8_TESTING
    if(mb_capture_enabled.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lo_lock(mo_capture_mutex);
        for(auto lo_it = io_ready.cbegin(); lo_it != io_ready.cend(); ++lo_it)
        {
            uint8_t *lp_buf = *lo_it;
            mo_captured_buffers.emplace_back(lp_buf, lp_buf + mz_data_buffer_size);
        }
    }
#endif

    mp_sink->write_data(io_ready);

    // recycle the consumed data buffers back to the pool
    io_ready.clear([this](uint8_t *ip_buf) { release_buffer(ip_buf); }, kit::e_c_lst_pool_policy::e_keep);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_core::get_buffer_size()
{
    return mz_data_buffer_size;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_p8_svc_buf *cp8_core::svc_acquire_new()
{
    uint8_t *lp_buf = acquire_buffer();
    if(!lp_buf)
    {
        // TODO: print error
        return nullptr;
    }

    // service buffers carry no data-buffer header — entries start at offset 0
    s_p8_svc_buf lo_pair;
    lo_pair.mp_buf  = lp_buf;
    lo_pair.mz_used = 0;
    mo_svc_buffers.push_last(lo_pair);

    return &mo_svc_buffers.back();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_core::svc_reserve(size_t iz_padded)
{
    const size_t lz_capacity = get_buffer_size();

    // an entry larger than a whole buffer can never fit — drop it
    // (cross-buffer fragmentation of service entries is out of scope)
    if(iz_padded > lz_capacity)
    {
        return nullptr;
    }

    s_p8_svc_buf *lp_cur = (mo_svc_buffers.size() == 0) ? svc_acquire_new() : &mo_svc_buffers.back();
    if(!lp_cur)
    {
        return nullptr;
    }

    if(lp_cur->mz_used + iz_padded > lz_capacity)
    {
        // current buffer is full — leave it in the list and start a fresh one
        lp_cur = svc_acquire_new();
        if(!lp_cur)
        {
            return nullptr;
        }
    }

    uint8_t *lp_dst = lp_cur->mp_buf + lp_cur->mz_used;
    memset(lp_dst, 0, iz_padded);
    lp_cur->mz_used += iz_padded;

    return lp_dst;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::serialize_attr_desc(const s_p8_attr_desc *ip_desc)
{
    if(!ip_desc)
    {
        return;
    }

    // truncate over-long names to the service-string boundary (length modulo)
    size_t lz_name   = ip_desc->mp_name ? strlen(ip_desc->mp_name) % P8_SVC_STR_MAX_LEN : 0;

    // fixed header + NUL-terminated name, padded so the whole entry is 8-aligned
    size_t lz_padded = P8_ALIGN_UP_8(sizeof(s_p8_attr_svc) + lz_name + 1);

    uint8_t *lp_dst  = svc_reserve(lz_padded);
    if(!lp_dst)
    {
        return;
    }

    s_p8_attr_svc *lp_entry         = reinterpret_cast<s_p8_attr_svc *>(lp_dst);
    lp_entry->ms_hdr.mu_packet_type = P8_PACKET_SERVICE;
    lp_entry->ms_hdr.mu_svc_type    = P8_SVC_TYPE_ATTR;
    lp_entry->ms_hdr.mu_size        = static_cast<uint16_t>(lz_padded);
    lp_entry->mi_id                 = ip_desc->mi_id;
    lp_entry->mu_type               = static_cast<uint8_t>(ip_desc->me_type);

    uint8_t *lp_name                = lp_dst + sizeof(s_p8_attr_svc);
    if(lz_name)
    {
        memcpy(lp_name, ip_desc->mp_name, lz_name);
    }
    lp_name[lz_name] = '\0';
    // trailing padding is already zeroed by svc_reserve
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_core::serialize_log_desc(const struct s_p8_log_desc *ip_desc)
{
    if(!ip_desc)
    {
        return;
    }

    // truncate over-long strings to the service-string boundary (length modulo)
    size_t lz_file = ip_desc->mp_file ? strlen(ip_desc->mp_file) % P8_SVC_STR_MAX_LEN : 0;
    size_t lz_func = ip_desc->mp_function ? strlen(ip_desc->mp_function) % P8_SVC_STR_MAX_LEN : 0;
    size_t lz_fmt  = ip_desc->mp_format ? strlen(ip_desc->mp_format) % P8_SVC_STR_MAX_LEN : 0;

    size_t lz_args = ip_desc->mz_args_count;
    if(lz_args > P8_LOG_MAX_ARGS)
    {
        lz_args = P8_LOG_MAX_ARGS;
    }

    // fixed header + [file][function][format] (byte counts, no NUL) + var-arg
    // descriptors, padded so the whole entry is 8-aligned
    size_t lz_padded
        = P8_ALIGN_UP_8(sizeof(s_p8_log_item_svc) + lz_file + lz_func + lz_fmt + lz_args * sizeof(s_p8_log_varg));

    uint8_t *lp_dst = svc_reserve(lz_padded);
    if(!lp_dst)
    {
        return;
    }

    s_p8_log_item_svc *lp_entry     = reinterpret_cast<s_p8_log_item_svc *>(lp_dst);
    lp_entry->ms_hdr.mu_packet_type = P8_PACKET_SERVICE;
    lp_entry->ms_hdr.mu_svc_type    = P8_SVC_TYPE_LOG_DESC;
    lp_entry->ms_hdr.mu_size        = static_cast<uint16_t>(lz_padded);
    lp_entry->mu_line               = ip_desc->mu_line;
    lp_entry->mu_hash               = ip_desc->mu_hash;
    lp_entry->mu_format_len         = static_cast<uint16_t>(lz_fmt);
    lp_entry->mu_file_len           = static_cast<uint16_t>(lz_file);
    lp_entry->mu_function_len       = static_cast<uint16_t>(lz_func);
    lp_entry->mu_args_count         = static_cast<uint8_t>(lz_args);

    uint8_t *lp_var                 = lp_dst + sizeof(s_p8_log_item_svc);
    if(lz_file)
    {
        memcpy(lp_var, ip_desc->mp_file, lz_file);
        lp_var += lz_file;
    }
    if(lz_func)
    {
        memcpy(lp_var, ip_desc->mp_function, lz_func);
        lp_var += lz_func;
    }
    if(lz_fmt)
    {
        memcpy(lp_var, ip_desc->mp_format, lz_fmt);
        lp_var += lz_fmt;
    }
    if(lz_args)
    {
        memcpy(lp_var, ip_desc->ma_args, lz_args * sizeof(s_p8_log_varg));
    }
    // trailing padding is already zeroed by svc_reserve
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct s_p8_log_desc *cp8_core::resolve_log_desc(uint64_t    iu_hash,
                                                 const char *ip_file,
                                                 uint32_t    iu_line,
                                                 const char *ip_function,
                                                 const char *ip_format)
{
    std::lock_guard<std::mutex> lo_lock(mo_log_desc_mutex);

    auto lo_it = mo_log_descs.find(iu_hash);
    if(lo_it != mo_log_descs.end())
    {
        return lo_it->second;
    }

    s_p8_log_desc *lp_desc = new(std::nothrow) s_p8_log_desc;
    if(!lp_desc)
    {
        return nullptr;
    }

    lp_desc->mu_hash            = iu_hash;
    lp_desc->mp_format          = ip_format;
    lp_desc->mp_file            = ip_file;
    lp_desc->mp_function        = ip_function;
    lp_desc->mu_line            = iu_line;
    lp_desc->mz_args_count      = cp8_log::parse_format_string(lp_desc->ma_args, P8_LOG_MAX_ARGS, ip_format);

    lp_desc->mz_fixed_args_size = 0;
    for(size_t lz_i = 0; lz_i < lp_desc->mz_args_count; lz_i++)
    {
        uint8_t lu_type = lp_desc->ma_args[lz_i].mu_type;
        if(lu_type != P8_LOG_ARG_TYPE_USTR8 && lu_type != P8_LOG_ARG_TYPE_USTR16 && lu_type != P8_LOG_ARG_TYPE_USTR32
           && lu_type != P8_LOG_ARG_TYPE_STRA)
        {
            lp_desc->mz_fixed_args_size += lp_desc->ma_args[lz_i].mu_size;
        }
    }

    mo_log_descs[iu_hash] = lp_desc;

    {
        std::lock_guard<std::mutex> lo_svc_lock(mo_svc_mutex);
        serialize_log_desc(lp_desc);
    }

    return lp_desc;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_core *cp8_core::get_global_core(uint32_t iu_timeoutms)
{
    if(gp_instance)
    {
        return gp_instance;
    }

    uint32_t lu_elapsed = 0;

    while(lu_elapsed < iu_timeoutms)
    {
        cp8_core       *lp_found = nullptr;
        c_shared::h_sem lp_sem   = nullptr;

        if(c_shared::lock(gp_shm_name, lp_sem, 10) == c_shared::e_ok)
        {
            c_shared::read(gp_shm_name, &lp_found, sizeof(lp_found));
            c_shared::unlock(lp_sem);
        }

        if(lp_found)
        {
            gp_instance = lp_found;
            return gp_instance;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lu_elapsed += 10;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// extern "C" API
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C"
{

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_initialize(const struct s_p8_config *ip_config)
    {
        cp8_core          *lp_new        = nullptr;
        cp8_core          *lp_existing   = nullptr;
        c_shared::h_shared lp_shm_handle = nullptr;
        c_shared::h_sem    lp_sem        = nullptr;
        bool               lb_error      = false;

        // 1. already initialized in this DSO
        if(gp_instance)
        {
            return true;
        }

        // 2. instance published by another DSO via shared memory
        if(c_shared::read(gp_shm_name, &lp_existing, sizeof(lp_existing)) && lp_existing)
        {
            gp_instance = lp_existing;
            return true;
        }

        // 3. validate config
        if(!ip_config)
        {
            std::fprintf(stderr, "p8_initialize: NULL config\n");
            lb_error = true;
            goto lbl_exit;
        }

        // 4. create core instance (constructor parses JSON, sets mb_initialized on failure)
        lp_new = new(std::nothrow) cp8_core(ip_config);
        if(!lp_new)
        {
            std::fprintf(stderr, "p8_initialize: allocation failed\n");
            lb_error = true;
            goto lbl_exit;
        }

        if(!lp_new->get_initialized())
        {
            lb_error = true;
            goto lbl_exit;
        }

        // 5. atomically publish pointer via shared memory (O_CREAT | O_EXCL)
        if(c_shared::create(&lp_shm_handle, gp_shm_name, reinterpret_cast<const uint8_t *>(&lp_new), sizeof(lp_new)))
        {
            gp_shm_handle = lp_shm_handle;
            gp_instance   = lp_new;
            lp_new        = nullptr;
        }
        else
        {
            // 6. another DSO won the race — read its pointer
            delete lp_new;
            lp_new      = nullptr;
            lp_existing = nullptr;

            if(c_shared::lock(gp_shm_name, lp_sem, 5000) == c_shared::e_ok)
            {
                c_shared::read(gp_shm_name, &lp_existing, sizeof(lp_existing));
                c_shared::unlock(lp_sem);
            }

            if(lp_existing)
            {
                gp_instance = lp_existing;
            }
            else
            {
                std::fprintf(stderr, "p8_initialize: failed to obtain instance from shared memory\n");
                lb_error = true;
                goto lbl_exit;
            }
        }

lbl_exit:
        if(lb_error && lp_new)
        {
            delete lp_new;
            lp_new = nullptr;
        }

        return !lb_error;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_get_initialized()
    {
        return gp_instance && gp_instance->get_initialized();
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_exceptional_flush()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool p8_register_current_thread(const char *)
    {
        return false;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_unregister_current_thread()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    p8_attr_id p8_attr_register(const char *ip_name, enum e_p8_attr_type ie_type)
    {
        if(!gp_instance)
        {
            return P8_ATTR_ERROR_NOT_INITIALIZED;
        }
        return gp_instance->attr_register(ip_name, ie_type);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    p8_attr_id p8_attr_get(const char *ip_name)
    {
        if(!gp_instance)
        {
            return P8_ATTR_ERROR_NOT_FOUND;
        }
        return gp_instance->attr_get(ip_name);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void p8_release()
    {
        if(!gp_instance)
        {
            return;
        }

        if(gp_shm_handle)
        {
            c_shared::close(gp_shm_handle);
            gp_shm_handle = nullptr;
        }
        c_shared::unlink(gp_shm_name);

        gp_instance->release();
        gp_instance = nullptr;
    }

} // extern "C"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// test helpers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef P8_TESTING

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_reset()
{
    p8_release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t p8_test_get_instance_count()
{
    return gu_instance_count.load(std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t p8_test_get_buffer_size()
{
    return gp_instance ? gp_instance->mz_data_buffer_size : 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t p8_test_get_free_buffers_count()
{
    if(!gp_instance || !gp_instance->mp_data_pool)
    {
        return 0;
    }
    return gp_instance->mp_data_pool->get_free_count();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *p8_test_acquire_buffer()
{
    return gp_instance ? gp_instance->acquire_buffer() : nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_release_buffer(uint8_t *ip_buffer)
{
    if(gp_instance)
    {
        gp_instance->release_buffer(ip_buffer);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_enable_buffer_capture()
{
    if(gp_instance)
    {
        std::lock_guard<std::mutex> lo_lock(gp_instance->mo_capture_mutex);
        gp_instance->mo_captured_buffers.clear();
        gp_instance->mb_capture_enabled = true;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_disable_buffer_capture()
{
    if(gp_instance)
    {
        std::lock_guard<std::mutex> lo_lock(gp_instance->mo_capture_mutex);
        gp_instance->mb_capture_enabled = false;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t p8_test_get_captured_count()
{
    if(!gp_instance)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lo_lock(gp_instance->mo_capture_mutex);
    return gp_instance->mo_captured_buffers.size();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const std::vector<std::vector<uint8_t>> &p8_test_get_captured_buffers()
{
    static const std::vector<std::vector<uint8_t>> lo_empty;
    return gp_instance ? gp_instance->mo_captured_buffers : lo_empty;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_clear_captured_buffers()
{
    if(gp_instance)
    {
        std::lock_guard<std::mutex> lo_lock(gp_instance->mo_capture_mutex);
        gp_instance->mo_captured_buffers.clear();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_core::get_writer_count()
{
    std::lock_guard<std::mutex> lo_guard(mo_writers_lock);
    return mz_writers_count;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_tls_writer *cp8_core::get_writers_head()
{
    return mp_writers_head;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t p8_test_get_writer_count()
{
    return gp_instance ? gp_instance->get_writer_count() : 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_tls_writer *p8_test_get_writers_head()
{
    return gp_instance ? gp_instance->get_writers_head() : nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
s_p8_drop_stats p8_test_get_dropped_stats()
{
    return gp_instance ? gp_instance->get_dropped_stats() : s_p8_drop_stats { 0, 0, 0 };
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<std::vector<uint8_t>> cp8_core::get_service_buffers()
{
    std::vector<std::vector<uint8_t>> lo_out;

    std::lock_guard<std::mutex> lo_guard(mo_svc_mutex);

    for(auto lo_it = mo_svc_buffers.cbegin(); lo_it != mo_svc_buffers.cend(); ++lo_it)
    {
        const s_p8_svc_buf &lr_pair = *lo_it;
        lo_out.emplace_back(lr_pair.mp_buf, lr_pair.mp_buf + lr_pair.mz_used);
    }

    return lo_out;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<std::vector<uint8_t>> p8_test_get_service_buffers()
{
    if(!gp_instance)
    {
        return {};
    }
    return gp_instance->get_service_buffers();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void p8_test_drain_writers()
{
    if(!gp_instance)
    {
        return;
    }

    kit::c_lst<uint8_t *> lo_ready;

    // include the writer-shutdown ready queue so the drain is exhaustive
    gp_instance->mo_ready_lock.lock();
    while(gp_instance->mo_ready_queue.size() > 0)
    {
        lo_ready.push_last(gp_instance->mo_ready_queue.pull_first());
    }
    gp_instance->mo_ready_lock.unlock();

    gp_instance->drain_writers(lo_ready);
    gp_instance->flush_ready(lo_ready);
}

#endif // P8_TESTING
