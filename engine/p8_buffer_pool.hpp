#pragma once

#include "kit/list.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>

class cp8_memory_budget;

/// Buffer pool with a fixed buffer size and grow-on-demand semantics.
/// All buffers in a single pool are the same size; combine multiple pools
/// with different sizes (e.g. data 8 KB, mini 1 KB) when needed.
/// The pool holds a `shared_ptr` to the budget so the budget outlives every
/// pool that references it — destruction order between pools and the
/// owning core no longer matters.
class cp8_buffer_pool
{
public:
    struct s_stat
    {
        size_t mz_buffer_size;
        // Exact free space: budget headroom that has not been handed out yet,
        // i.e. max - outstanding. This counts memory that is not yet allocated
        // as free, so a pool that has grown only partially is not reported as
        // being under pressure.
        size_t mz_free_size;
        // The budget cap this pool draws from (mp_budget->get_max()), i.e. the
        // maximum memory the pool may ever occupy — not the currently allocated
        // amount.
        size_t mz_max_size;
    };

public:
    cp8_buffer_pool(size_t iz_buffer_size, std::shared_ptr<cp8_memory_budget> ip_budget);
    ~cp8_buffer_pool();

    cp8_buffer_pool(const cp8_buffer_pool &)            = delete;
    cp8_buffer_pool &operator=(const cp8_buffer_pool &) = delete;
    cp8_buffer_pool(cp8_buffer_pool &&)                 = delete;
    cp8_buffer_pool &operator=(cp8_buffer_pool &&)      = delete;

    // Pre-allocates up to iz_initial_count buffers (limited by the budget).
    // Returns the count actually pre-allocated. An init(0) is a valid no-op;
    // grow-on-demand will still kick in on the first acquire.
    size_t init(size_t iz_initial_count);

    void     lock();
    uint8_t *acquire_no_lock();
    void     stat(cp8_buffer_pool::s_stat &or_stat);
    void     unlock();

    // Returns a previously-acquired buffer to the free list.
    // Released memory is kept in the pool — it is not given back to the OS.
    void recycle(uint8_t *ip_buf);

    // Batch variant: returns every buffer in io_bufs to the free list under a
    // single lock acquisition, then empties io_bufs (keeping its node pool).
    // Equivalent to calling recycle() per element but pays the mutex cost once,
    // which matters on the worker's flush path where whole batches are recycled.
    // A recycled buffer is handed directly to the oldest waiter (if any) instead
    // of going to the free list, so acquire_wait() acquirers are served FIFO.
    void recycle(kit::c_lst<uint8_t *> &io_bufs);

    // Blocking, strict-FIFO acquire. If a buffer is available it behaves like
    // acquire_no_lock(); otherwise the caller parks in a FIFO waiter queue and is
    // handed the next recycled buffer directly (oldest waiter first).
    uint8_t *acquire_wait();

    // Wake every parked waiter with a nullptr result and refuse further parks.
    // Called once at shutdown, before the worker stops. Idempotent.
    void stop_waiters();

    size_t get_buffer_size() const;
    size_t get_free_count();

private:
    // One blocked acquirer. Lives on the waiting thread's stack and is linked into
    // the FIFO queue below under mo_mutex. Hand-off publishes mp_buf then releases
    // mo_sem; the woken thread reads only its own node, never the pool, so the pool
    // may be destroyed while a waiter is returning.
    struct s_waiter
    {
        s_waiter             *mp_next = nullptr; // intrusive FIFO link (guarded by mo_mutex)
        uint8_t              *mp_buf  = nullptr; // handed-off buffer, or nullptr sentinel on shutdown
        std::binary_semaphore mo_sem { 0 };      // per-waiter park; starts blocked
    };

    // Hand ip_buf to the oldest waiter if one is queued: returns true (the caller
    // must NOT push ip_buf to mo_free), or false when there is no waiter. The
    // caller must hold mo_mutex.
    bool try_handoff_no_lock(uint8_t *ip_buf);

    size_t                             mz_buffer_size;
    std::shared_ptr<cp8_memory_budget> mp_budget;
    kit::c_lst<uint8_t *>              mo_free;
    kit::c_lst<uint8_t *>              mo_all;
    std::mutex                         mo_mutex;

    // FIFO waiter queue for acquire_wait(). Every field is mutated only under
    // mo_mutex. mu_wait_count is loaded relaxed on the recycle path to skip the
    // queue when empty; it never guards the buffer hand-off (mo_sem carries that
    // happens-before). mb_stopping latches shutdown so no new waiter parks.
    s_waiter             *mp_wait_head = nullptr; // dequeue end (oldest waiter)
    s_waiter             *mp_wait_tail = nullptr; // enqueue end
    std::atomic<uint32_t> mu_wait_count { 0 };
    bool                  mb_stopping = false;

#ifdef P8_TESTING
    // Monotonic count of enqueue events, bumped under mo_mutex at the instant a
    // waiter joins the queue. Lets a test observe FIFO enqueue order without sleeps.
    std::atomic<uint64_t> mu_wait_arrivals { 0 };

public:
    size_t   get_wait_count() const;
    uint64_t get_wait_arrivals() const;
#endif
};
