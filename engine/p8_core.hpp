#pragma once

#include "p8_buffer_pool.hpp"
#include "p8_client_api.h"
#include "p8_memory_budget.hpp"
#include "p8_protocol.h"
#include "p8_sink.hpp"

#include "kit/event.hpp"
#include "kit/list.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define P8_CORE_ACQUIRE_TIMEOUT_MS 100
#define P8_CORE_THREAD_TIMEOUT_MS  50

// When free data-buffer memory drops below this percentage of the total
// *allocated* pool (buffers that actually exist, i.e. free + outstanding — not
// the budget cap), acquire_buffer wakes the worker so it can pull accumulated
// buffers from all writers. Measuring against allocated memory keeps an
// infinite/default budget from reading as permanent pressure.
#define P8_CORE_FREE_MEM_PERCENT   25

class cp8_tls_writer;
struct s_p8_log_desc;

struct s_p8_attr_desc
{
    p8_attr_id          mi_id;
    enum e_p8_attr_type me_type;
    char               *mp_name;
};

class cp8_core
{
public:
    explicit cp8_core(const struct s_p8_config *ip_config);
    ~cp8_core();

    // lifetime
    void addref();
    void release();

    // core
    static cp8_core *get_global_core(uint32_t iu_timeoutms);
    bool             get_initialized() const;
    void             exceptional_flush();

    // worker thread: signal an external event so the loop runs an iteration immediately
    void notify();

    // TLS writer registry
    void register_writer(cp8_tls_writer *ip_writer);
    void unregister_writer(cp8_tls_writer *ip_writer);

    // thread
    bool register_current_thread(const char *ip_name);
    void unregister_current_thread();

    // attributes
    p8_attr_id attr_register(const char *ip_name, enum e_p8_attr_type ie_type);
    p8_attr_id attr_get(const char *ip_name) const;
    void       sync_attr_cache(std::vector<const s_p8_attr_desc *> &io_cache);

    // buffer pool
    static size_t get_buffer_size();
    uint8_t      *acquire_buffer();
    void          release_buffer(uint8_t *ip_buffer);
    void          release_buffers(kit::c_lst<uint8_t *> &io_buffers);

    // ready-queue: producers hand filled data buffers to the core for the
    // worker thread to consume. submit_buffer takes a single buffer,
    // submit_chain takes a fragment list (consumed in logical order).
    void submit_buffer(uint8_t *ip_buffer);
    void submit_chain(kit::c_lst<uint8_t *> &io_buffers);

    // log descriptors
    s_p8_log_desc *resolve_log_desc(uint64_t    iu_hash,
                                    const char *ip_file,
                                    uint32_t    iu_line,
                                    const char *ip_function,
                                    const char *ip_format);

    // non-copyable, non-movable
    cp8_core(const cp8_core &)            = delete;
    cp8_core &operator=(const cp8_core &) = delete;
    cp8_core(cp8_core &&)                 = delete;
    cp8_core &operator=(cp8_core &&)      = delete;

#ifdef P8_TESTING
    cp8_buffer_pool *get_data_pool()
    {
        return mp_data_pool;
    }
    cp8_memory_budget *get_memory_budget()
    {
        return mp_memory_budget.get();
    }
    size_t          get_writer_count();
    cp8_tls_writer *get_writers_head();

    // Snapshot of the serialized service buffers, each trimmed to its used size.
    std::vector<std::vector<uint8_t>> get_service_buffers();
#endif

private:
    bool init_buffer_pool(const char *ip_max_memory_size, const char *ip_initial_memory_size);
    bool init_header(const struct s_p8_config *ip_config);

    // worker thread
    void start_worker();
    void stop_worker();
    void worker_main();

    // Debounced wake for the memory-pressure path: only calls notify() if the
    // previous pressure wake has already been consumed by the worker.
    void notify_pressure();

    // Pull accumulated buffers from every registered writer into io_data, in
    // registry order. Takes mo_writers_lock and each writer's lock internally.
    void drain_writers(kit::c_lst<uint8_t *> &io_data);

    // Write a batch of ready data buffers to the sink and recycle them. Single
    // choke point for both the worker-pull path and the writer-shutdown ready
    // queue, and the only place data buffers are captured under P8_TESTING.
    void flush_ready(kit::c_lst<uint8_t *> &io_ready);

    // service-data serialization (log + attr descriptors). All helpers below
    // assume mo_svc_mutex is held by the caller.
    s_p8_svc_buf *svc_acquire_new();
    uint8_t      *svc_reserve(size_t iz_padded);
    void          serialize_attr_desc(const s_p8_attr_desc *ip_desc);
    void          serialize_log_desc(const struct s_p8_log_desc *ip_desc);

    bool                  mb_initialized = false;
    std::atomic<uint32_t> mu_ref_count { 1 };

    s_p8_hdr mo_hdr         = {};

    // Single consumer endpoint for all produced buffers. Owned by the core,
    // created in the constructor and destroyed in the destructor. Only the
    // worker thread calls into it.
    cp8_sink_iface *mp_sink = nullptr;

    // shared memory budget => memory limiter, to avoid memory allocation without control under the pressure
    std::shared_ptr<cp8_memory_budget> mp_memory_budget;

    // buffer pool geometry (kept on the core for backwards-compatible API)
    const static size_t mz_data_buffer_size = 8192;
    // data memory pool, uses & depends on mp_memory_budget
    cp8_buffer_pool *mp_data_pool           = nullptr;

    // log descriptor registry (global, shared across all TLS cp8_log instances)
    std::map<uint64_t, s_p8_log_desc *> mo_log_descs;
    std::mutex                          mo_log_desc_mutex;

    // attribute registry (global, TLS consumers sync via sync_attr_cache)
    std::vector<s_p8_attr_desc *>               mo_attr_descs;
    std::unordered_map<std::string, p8_attr_id> mo_attr_name_map;
    mutable std::mutex                          mo_attr_mutex;

    // serialized service data (log + attr descriptors), drained by the worker
    // thread. The last element is the current in-progress buffer; the earlier
    // elements are full. Each entry inside a buffer is 8-byte aligned.
    kit::c_lst<s_p8_svc_buf> mo_svc_buffers { 8 };
    std::mutex               mo_svc_mutex;

    // TLS writer registry: intrusive doubly-linked list of all live writers.
    // Lock ordering: mo_writers_lock -> writer->mp_lock (never reverse).
    cp8_tls_writer *mp_writers_head  = nullptr;
    size_t          mz_writers_count = 0;
    std::mutex      mo_writers_lock;

    // worker thread: wakes on an external event (mu_event_wake) or every
    // P8_CORE_THREAD_TIMEOUT_MS, stops on mu_event_stop.
    static constexpr uint32_t mu_event_wake = 0;
    static constexpr uint32_t mu_event_stop = 1;

    std::thread mo_worker_thread;
    c_event     mo_worker_event;
    bool        mb_worker_running = false;

    // Debounces the memory-pressure wake in acquire_buffer: once a pressure
    // wake is in flight, further pressure notifications are suppressed until
    // the worker consumes it. Set (false->true) by whichever acquiring thread
    // wins the claim, cleared by the worker after every wait(). Read on the hot
    // path (relaxed load), written only ~once per wake cycle, so cross-core
    // write traffic stays minimal. Own cache line to avoid false sharing.
    alignas(64) std::atomic<bool> mb_wake_pending { false };

    // ready-queue: filled data buffers submitted by producers, drained and
    // recycled by the worker thread. Protected by a mutex on the hot path.
    kit::c_lst<uint8_t *> mo_ready_queue { 4096 };
    std::mutex            mo_ready_lock;

#ifdef P8_TESTING
    friend size_t                                   p8_test_get_buffer_size();
    friend size_t                                   p8_test_get_free_buffers_count();
    friend void                                     p8_test_enable_buffer_capture();
    friend void                                     p8_test_disable_buffer_capture();
    friend size_t                                   p8_test_get_captured_count();
    friend const std::vector<std::vector<uint8_t>> &p8_test_get_captured_buffers();
    friend void                                     p8_test_clear_captured_buffers();
    friend size_t                                   p8_test_get_writer_count();
    friend cp8_tls_writer                          *p8_test_get_writers_head();
    friend void                                     p8_test_drain_writers();

    // When capture is enabled the worker leaves data buffers untouched so the
    // test-driven synchronous drain is the sole consumer (avoids racing on
    // mo_captured_buffers). Atomic so the worker can read it lock-free.
    std::atomic<bool>                 mb_capture_enabled { false };
    std::mutex                        mo_capture_mutex;
    std::vector<std::vector<uint8_t>> mo_captured_buffers;
#endif
};

#ifdef P8_TESTING
void                                     p8_test_reset();
uint32_t                                 p8_test_get_instance_count();
size_t                                   p8_test_get_buffer_size();
size_t                                   p8_test_get_free_buffers_count();
uint8_t                                 *p8_test_acquire_buffer();
void                                     p8_test_release_buffer(uint8_t *ip_buffer);
void                                     p8_test_enable_buffer_capture();
void                                     p8_test_disable_buffer_capture();
size_t                                   p8_test_get_captured_count();
const std::vector<std::vector<uint8_t>> &p8_test_get_captured_buffers();
void                                     p8_test_clear_captured_buffers();
size_t                                   p8_test_get_writer_count();
cp8_tls_writer                          *p8_test_get_writers_head();
std::vector<std::vector<uint8_t>>        p8_test_get_service_buffers();
void                                     p8_test_drain_writers();
#endif
