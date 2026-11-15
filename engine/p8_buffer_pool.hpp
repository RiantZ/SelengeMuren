#pragma once

#include "p8_free_ring.hpp"

#include "kit/list.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

class cp8_memory_budget;

/// Buffer pool with a fixed buffer size and grow-on-demand semantics.
/// All buffers in a single pool are the same size; combine multiple pools
/// with different sizes (e.g. data 8 KB, mini 1 KB) when needed.
/// The pool holds a `shared_ptr` to the budget so the budget outlives every
/// pool that references it — destruction order between pools and the
/// owning core no longer matters.
/// Free buffers wait in a lock-free ring; the mutex guards growth and the FIFO queue of waiters.
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

    // acquire_no_lock() without the caller's lock: the ring is read without the mutex while no thread waits for a
    // buffer, the mutex is taken to grow the pool or to queue behind a waiter
    uint8_t *try_acquire();

    // Returns a previously-acquired buffer to the free list.
    // Released memory is kept in the pool — it is not given back to the OS.
    void recycle(uint8_t *ip_buf);

    // Batch variant: returns every buffer in io_bufs, then empties io_bufs (keeping its node pool).
    // While no thread waits for a buffer they go to the ring without the mutex; otherwise, under one lock,
    // a recycled buffer is handed directly to the oldest waiter (if any) instead
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
    // One blocked acquirer. Lives on the waiting thread's stack and is linked into the
    // FIFO queue below under mo_mutex. A hand-off publishes mp_buf, sets mb_done, and
    // notify_all()s the waiter's shard (mp_cv); the woken thread reads only its own node.
    // The pool outlives every parked waiter via the mu_inflight teardown barrier.
    struct s_waiter
    {
        s_waiter                *mp_next = nullptr; // intrusive FIFO link (guarded by mo_mutex)
        uint8_t                 *mp_buf  = nullptr; // handed-off buffer, or nullptr sentinel on shutdown
        bool                     mb_done = false;   // served flag = wait predicate (guarded by mo_mutex)
        std::condition_variable *mp_cv   = nullptr; // shard this waiter parks on (points into mo_wait_cvs)
    };

    // Hand ip_buf to the oldest queued waiter and mark it served. Returns the index of
    // that waiter's CV shard (caller notify_all()s it), or -1 when no waiter is queued
    // (caller pushes ip_buf to mo_free). Holds mo_mutex.
    int try_handoff_no_lock(uint8_t *ip_buf);

    // a new buffer when the budget allows it; holds mo_mutex
    uint8_t *grow_no_lock();

    // puts ip_buf into the ring, waiting while a pop finishes the cell; false only when every cell holds a buffer
    bool push_free(uint8_t *ip_buf);

    // puts ip_buf into the ring, or into mo_spill when the ring is full; put_free_no_lock holds mo_mutex
    void put_free(uint8_t *ip_buf);
    void put_free_no_lock(uint8_t *ip_buf);

    // pops a buffer, waiting while a push finishes the head cell: empty only when no buffer is on its way, as a pop
    // under the mutex would see it
    bool pop_free(uint8_t *&op_buf);

    // a buffer from mo_spill; holds mo_mutex
    bool pop_spill_no_lock(uint8_t *&op_buf);

    // hands the free buffers to the queued waiters, oldest first; holds mo_mutex
    void serve_waiters_no_lock();

    size_t                             mz_buffer_size;
    std::shared_ptr<cp8_memory_budget> mp_budget;
    cp8_free_ring                      mo_free;  // free buffers: writers pop, the worker pushes
    kit::c_lst<uint8_t *>              mo_spill; // free buffers beyond the ring cells, only in a pool without a budget
    kit::c_lst<uint8_t *>              mo_all;   // every buffer, for the destructor
    std::atomic<size_t>                mu_spill_count { 0 };
    std::atomic<size_t>                mu_all_count { 0 };
    std::mutex                         mo_mutex;

    // threads in acquire_wait() that found no buffer and may park; while it is 0, recycle() and try_acquire() use
    // the ring without the mutex
    alignas(128) std::atomic<uint32_t> mu_slow { 0 };

    // FIFO waiter queue for acquire_wait(). Every field is mutated only under mo_mutex.
    // mu_wait_count is loaded relaxed on the recycle path to skip the queue when empty.
    // mb_stopping latches shutdown so no new waiter parks.
    s_waiter             *mp_wait_head = nullptr; // dequeue end (oldest waiter)
    s_waiter             *mp_wait_tail = nullptr; // enqueue end
    std::atomic<uint32_t> mu_wait_count { 0 };
    bool                  mb_stopping                                      = false;

    // Sharded parking. Waiters are round-robin-assigned across a fixed set of CVs
    // (mu_cv_next), so a hand-off's notify_all() wakes only the ~1/count parked waiters
    // sharing that shard instead of all of them. All shards share the one mo_mutex (a
    // supported pattern). Fixed size: std::condition_variable is non-movable and we want
    // zero allocation on the park path (back-pressure correlates with low memory).
    static constexpr size_t                               mz_wait_cv_count = 64;
    std::array<std::condition_variable, mz_wait_cv_count> mo_wait_cvs;
    uint32_t                                              mu_cv_next = 0; // round-robin cursor (guarded by mo_mutex)

    // Waiters enqueued but not yet fully returned. Distinct from mu_wait_count (dropped
    // by the signaller at dequeue): mu_inflight is dropped by the waiter AFTER it
    // unlocks mo_mutex, so ~cp8_buffer_pool can wait on it before destroying mo_mutex /
    // the CV shards.
    std::atomic<uint32_t> mu_inflight { 0 };

#ifdef P8_TESTING
    // Monotonic count of enqueue events, bumped under mo_mutex at the instant a
    // waiter joins the queue. Lets a test observe FIFO enqueue order without sleeps.
    std::atomic<uint64_t> mu_wait_arrivals { 0 };

public:
    size_t   get_wait_count() const;
    uint64_t get_wait_arrivals() const;
#endif
};
