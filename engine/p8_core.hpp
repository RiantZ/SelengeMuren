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
#include <unordered_set>
#include <vector>

#define P8_CORE_ACQUIRE_TIMEOUT_MS     100
#define P8_CORE_THREAD_TIMEOUT_MS      50

// Cadence at which the worker pulls-and-resets the per-writer drop counters and
// folds them into the core loss accumulators. The loop wakes irregularly, so
// this is enforced against a monotonic clock, not an iteration count.
#define P8_CORE_STATS_POLL_INTERVAL_MS 250

// When free data-buffer memory drops below this percentage of the total
// *allocated* pool (buffers that actually exist, i.e. free + outstanding — not
// the budget cap), acquire_buffer wakes the worker so it can pull accumulated
// buffers from all writers. Measuring against allocated memory keeps an
// infinite/default budget from reading as permanent pressure.
#define P8_CORE_FREE_MEM_PERCENT       75

// The batch timestamp is taken this much early, so a writer's clock read that the CPU
// reorders around the writer lock still lands at or after it.
#define P8_CORE_BATCH_MARGIN_US        10

// While idle, the worker sends an empty batch packet this often, so the server sees the stream alive.
#define P8_CORE_IDLE_BATCH_INTERVAL_MS 5000

// Span IDs a trace writer takes from the core at once, so the shared counter is touched once per block
#define P8_CORE_SPAN_ID_BLOCK          65536

// iz_len moved back to the start of the UTF-8 sequence it falls in, so a cut string stays valid UTF-8;
// ip_str must hold more than iz_len bytes
inline size_t p8_cut_utf8(const char *ip_str, size_t iz_len)
{
    while(iz_len > 0 && 0x80 == (static_cast<uint8_t>(ip_str[iz_len]) & 0xC0))
    {
        --iz_len;
    }
    return iz_len;
}

class cp8_tls_writer;
struct s_p8_log_desc;
struct s_p8_log_mod;
struct s_p8_mtk_desc;

// Aggregated count of telemetry elements dropped before reaching the sink,
// split by kind. Returned by cp8_core::get_dropped_stats() and by each writer's
// pull_dropped(). mu_svc counts service/metadata descriptors dropped on buffer
// exhaustion (core-side only; writers never populate it).
struct s_p8_drop_stats
{
    uint64_t mu_logs;
    uint64_t mu_metrics;
    uint64_t mu_traces;
    uint64_t mu_svc;
};

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

    // loss statistics: running totals of dropped logs/metrics/traces. Both the
    // worker's periodic poll and each writer's destructor flush feed
    // accumulate_dropped; get_dropped_stats returns a lock-free snapshot.
    void            accumulate_dropped(const s_p8_drop_stats &ir_stats);
    s_p8_drop_stats get_dropped_stats() const;

    // thread
    bool register_current_thread(const char *ip_name);
    void unregister_current_thread();

    // attributes
    p8_attr_id attr_register(const char *ip_name, enum e_p8_attr_type ie_type);
    p8_attr_id attr_get(const char *ip_name) const;
    void       sync_attr_cache(std::vector<const s_p8_attr_desc *> &io_cache);

    // log modules
    bool            register_module(const char *ip_name, enum e_p8_level ie_verbosity, p_p8_module *op_module);
    p_p8_module     find_module(const char *ip_name);
    void            set_verbosity(p_p8_module ip_module, enum e_p8_level ie_verbosity);
    enum e_p8_level get_verbosity(p_p8_module ip_module);

    // metrics: register a push metric descriptor, serialize it once and return its
    // handle (>= 0). The handle is the descriptor's index in mo_mtk_descs, used by
    // cp8_mtk::emit to reference the metric. Returns negative on failure.
    h_p8_mtk_id register_mtk(const struct s_p8_mtk_base *ip_base);

    // lock-free count of registered metrics; cp8_mtk::emit uses it to bounds-check
    // an incoming id on the hot path without taking mo_mtk_desc_mutex.
    uint32_t get_mtk_count() const;

    // buffer pool
    static size_t get_buffer_size();
    // Acquire a data buffer. ib_wait == false (default) keeps the non-blocking
    // behavior: returns nullptr immediately when the pool is exhausted. ib_wait ==
    // true blocks the caller in a strict-FIFO queue until a buffer is recycled,
    // returning nullptr only on shutdown. The blocking form MUST NOT be called from
    // the worker thread, nor while holding a writer spinlock (see cp8_log/cp8_mtk).
    uint8_t *acquire_buffer(bool ib_wait = false);
    void     release_buffer(uint8_t *ip_buffer);
    void     release_buffers(kit::c_lst<uint8_t *> &io_buffers);

    // ready-queue: producers hand filled data buffers to the core for the
    // worker thread to consume. submit_buffer takes a single buffer,
    // submit_chain takes a fragment list (consumed in logical order).
    void submit_buffer(uint8_t *ip_buffer);
    void submit_chain(kit::c_lst<uint8_t *> &io_buffers);

    // log descriptors: a new one is serialized before it is returned, waiting for a buffer when the service buffer is
    // full, so call it without locks held; nullptr when no buffer comes (shutdown) or allocation fails
    s_p8_log_desc *resolve_log_desc(uint64_t    iu_hash,
                                    const char *ip_file,
                                    uint32_t    iu_line,
                                    const char *ip_function,
                                    const char *ip_format);

    // span descriptors: serialize a call site and its strings once per process. Waits for a free buffer when the
    // service buffer is full, so call it without locks held; false only when no buffer comes (shutdown)
    bool resolve_trc_desc(uint64_t    iu_hash,
                          const char *ip_file,
                          uint32_t    iu_line,
                          const char *ip_function,
                          const char *ip_name);

    // span IDs: the first of P8_CORE_SPAN_ID_BLOCK IDs that are unique in the process
    uint64_t reserve_span_ids();

    // clock of every stamp in the stream: the user's when p8_initialize got its callback and ratio, the system one
    // otherwise; a writer copies the callback once, so its stamps do not touch the core
    uint64_t             get_ticks() const;
    l_p8_get_timer_value get_timer_value() const;
    void                *get_timer_ctx() const;

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
    void init_clock(const struct s_p8_config *ip_config);
    bool init_header();

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

    // One cycle's batch: its timestamp first, then drain_writers and the ready queue; returns the timestamp.
    // Shared by the worker and p8_test_drain_writers so both keep the same order.
    uint64_t collect_batch(kit::c_lst<uint8_t *> &io_data);

    // Pull-and-reset every registered writer's drop counters and fold them into
    // the core accumulators. Runs on the worker's P8_CORE_STATS_POLL_INTERVAL_MS
    // cadence. Takes mo_writers_lock; the per-writer pull is lock-free.
    void poll_dropped_stats();

    // Send the batch packet and the batch to the sink and recycle the buffers; with no data, an empty batch goes
    // one worker cycle after data, then every idle interval. Data is captured only here under P8_TESTING.
    void flush_ready(kit::c_lst<uint8_t *> &io_ready, uint64_t iu_ts);

    // Encode attribute id+value pairs (same wire layout as
    // cp8_tls_writer::serialize_attrs) into or_blob, resolving each attribute's type
    // from the registry. Takes mo_attr_mutex, so it must be called WITHOUT
    // mo_svc_mutex held (lock order: attr -> svc). Returns the count encoded.
    uint8_t encode_attr_blob(std::vector<uint8_t> &or_blob, const struct s_p8_attr_val *ip_attrs, size_t iz_attrs);

    // service-data serialization (log + attr + module + metric descriptors). All
    // helpers below assume mo_svc_mutex is held by the caller; given that lock, they wait
    // for a new buffer with it unlocked instead of dropping the entry, and fail only on
    // shutdown or for an entry larger than a buffer.
    s_p8_svc_buf *svc_acquire_new(std::unique_lock<std::mutex> *iop_lock);
    uint8_t      *svc_reserve(size_t iz_padded, std::unique_lock<std::mutex> *iop_lock = nullptr);
    void          serialize_attr_desc(const s_p8_attr_desc *ip_desc);
    bool          serialize_log_desc(const struct s_p8_log_desc *ip_desc, std::unique_lock<std::mutex> &io_lock);
    void          serialize_log_mod(const s_p8_log_mod *ip_mod);
    void          serialize_mtk_desc(const struct s_p8_mtk_desc *ip_desc,
                                     const uint8_t              *ip_attr_blob,
                                     size_t                      iz_attr_bytes,
                                     uint8_t                     iu_attr_count);
    // s_p8_str_svc entry, written once per hash in io_sent
    bool serialize_str(uint8_t                       iu_svc_type,
                       const char                   *ip_str,
                       std::unordered_set<uint64_t> &io_sent,
                       uint64_t                     &ou_hash,
                       std::unique_lock<std::mutex> &io_lock);
    bool serialize_trc_desc(uint64_t                      iu_hash,
                            uint32_t                      iu_line,
                            uint64_t                      iu_file_hash,
                            uint64_t                      iu_func_hash,
                            uint64_t                      iu_name_hash,
                            std::unique_lock<std::mutex> &io_lock);

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

    // data buffers acquired from mp_data_pool but not yet recycled. Used by
    // acquire_buffer to detect pool pressure and wake the worker.
    std::atomic<size_t> mu_outstanding_buffers { 0 };

    // Running totals of dropped telemetry elements, fed by poll_dropped_stats()
    // on the worker cadence and by each writer's destructor flush. Monotonic;
    // read lock-free via get_dropped_stats(). Only logs are wired today.
    std::atomic<uint64_t> mu_dropped_logs { 0 };
    std::atomic<uint64_t> mu_dropped_metrics { 0 };
    std::atomic<uint64_t> mu_dropped_traces { 0 };
    std::atomic<uint64_t> mu_dropped_svc { 0 };

    // log descriptor registry (global, shared across all TLS cp8_log instances)
    std::map<uint64_t, s_p8_log_desc *> mo_log_descs;
    std::mutex                          mo_log_desc_mutex;

    // attribute registry (global, TLS consumers sync via sync_attr_cache)
    std::vector<s_p8_attr_desc *>               mo_attr_descs;
    std::unordered_map<std::string, p8_attr_id> mo_attr_name_map;
    mutable std::mutex                          mo_attr_mutex;

    // module registry (global, mutex-protected). The handle handed to callers is
    // a pointer to the owned s_p8_log_mod; me_default_verbosity backs the
    // null-module (whole-p8) case of set/get_verbosity.
    std::vector<s_p8_log_mod *>                     mo_log_mods;
    std::unordered_map<std::string, s_p8_log_mod *> mo_log_mod_map;
    std::mutex                                      mo_log_mod_mutex;
    std::atomic<enum e_p8_level>                    me_default_verbosity { e_p8_trace0 };

    // metric registry (global, mutex-protected). The handle handed to callers is the
    // descriptor's index in mo_mtk_descs; mu_mtk_count mirrors the size for a
    // lock-free bounds check on the emit hot path. mo_mtk_name_map deduplicates
    // registration by name so re-registering a metric returns its existing id.
    std::vector<s_p8_mtk_desc *>                 mo_mtk_descs;
    std::unordered_map<std::string, h_p8_mtk_id> mo_mtk_name_map;
    std::mutex                                   mo_mtk_desc_mutex;
    std::atomic<uint32_t>                        mu_mtk_count { 0 };

    // serialized service data (log + attr descriptors), drained by the worker
    // thread. The last element is the current in-progress buffer; the earlier
    // elements are full. Each entry inside a buffer is 8-byte aligned.
    kit::c_lst<s_p8_svc_buf> mo_svc_buffers { 8 };
    std::mutex               mo_svc_mutex;

    // hashes of the span call sites and strings already serialized; the mutex is held while their entries wait for a
    // service buffer, so none is sent twice (lock order: trc desc -> svc)
    std::unordered_set<uint64_t> mo_trc_descs;
    std::unordered_set<uint64_t> mo_file_names;
    std::unordered_set<uint64_t> mo_func_names;
    std::unordered_set<uint64_t> mo_span_names;
    std::mutex                   mo_trc_desc_mutex;

    // start of the next block of span IDs; IDs begin at 1 because 0 means "no span"
    std::atomic<uint64_t> mu_span_id_seq { 1 };

    // TLS writer registry: intrusive doubly-linked list of all live writers.
    // Lock ordering: mo_writers_lock -> writer->mp_lock (never reverse).
    cp8_tls_writer *mp_writers_head  = nullptr;
    size_t          mz_writers_count = 0;
    uint32_t        mu_writers_seq   = 0; // the last writer id given out, ids are never reused
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

    // clock of every stamp in the stream, chosen once by init_clock: a null ml_timer_value is the system clock;
    // nanoseconds = ticks * mu_clock_numer / mu_clock_denom
    l_p8_get_timer_value ml_timer_value { nullptr };
    void                *mp_timer_ctx { nullptr };
    uint64_t             mu_clock_numer { 0 };
    uint64_t             mu_clock_denom { 0 };

    // batch packets (s_p8_batch), used only by the thread that runs flush_ready
    uint64_t mu_batch_margin_ticks = 0;     // P8_CORE_BATCH_MARGIN_US (10usec) in clock ticks
    uint64_t mu_batch_tail_ticks   = 0;     // P8_CORE_THREAD_TIMEOUT_MS (50msec) in clock ticks
    uint64_t mu_batch_idle_ticks   = 0;     // P8_CORE_IDLE_BATCH_INTERVAL_MS (5sec) in clock ticks
    uint64_t mu_batch_last_ts      = 0;     // s_p8_batch::mu_timestamp of the last packet sent, 0 before the first one
    bool     mb_batch_with_data    = false; // the last batch packet sent had data buffers
    uint16_t mu_batch_index        = 0;     // s_p8_batch::mu_batch index of the next packet

#ifdef P8_TESTING
    friend size_t                                   p8_test_get_buffer_size();
    friend size_t                                   p8_test_get_free_buffers_count();
    friend size_t                                   p8_test_get_waiter_count();
    friend uint64_t                                 p8_test_get_wait_arrivals();
    friend void                                     p8_test_enable_buffer_capture();
    friend void                                     p8_test_disable_buffer_capture();
    friend size_t                                   p8_test_get_captured_count();
    friend const std::vector<std::vector<uint8_t>> &p8_test_get_captured_buffers();
    friend const std::vector<s_p8_batch>           &p8_test_get_captured_batches();
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
    std::vector<s_p8_batch>           mo_captured_batches;
#endif
};

#ifdef P8_TESTING
void                                     p8_test_reset();
uint32_t                                 p8_test_get_instance_count();
size_t                                   p8_test_get_buffer_size();
size_t                                   p8_test_get_free_buffers_count();
uint8_t                                 *p8_test_acquire_buffer();
uint8_t                                 *p8_test_acquire_buffer_wait();
size_t                                   p8_test_get_waiter_count();
uint64_t                                 p8_test_get_wait_arrivals();
void                                     p8_test_release_buffer(uint8_t *ip_buffer);
void                                     p8_test_release_buffers(uint8_t *const *ip_buffers, size_t iz_count);
void                                     p8_test_enable_buffer_capture();
void                                     p8_test_disable_buffer_capture();
size_t                                   p8_test_get_captured_count();
const std::vector<std::vector<uint8_t>> &p8_test_get_captured_buffers();
const std::vector<s_p8_batch>           &p8_test_get_captured_batches();
void                                     p8_test_clear_captured_buffers();
size_t                                   p8_test_get_writer_count();
cp8_tls_writer                          *p8_test_get_writers_head();
std::vector<std::vector<uint8_t>>        p8_test_get_service_buffers();
void                                     p8_test_drain_writers();
s_p8_drop_stats                          p8_test_get_dropped_stats();
#endif
