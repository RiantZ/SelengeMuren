#pragma once

#include "kit/list.hpp"

#include <atomic>
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
class cp8_buffer_pool
{
public:
    struct s_stat
    {
        size_t mz_buffer_size;
        size_t mz_free_size;
        // Bytes currently backed by real allocations (free + outstanding), i.e.
        // mo_all * mz_buffer_size. This is the *allocated* pool size, not the
        // budget cap: pressure must be judged against memory that actually
        // exists, so an infinite/default budget does not read as permanent
        // pressure.
        size_t mz_allocated_size;
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

    size_t get_buffer_size() const;
    size_t get_free_count();

private:
    size_t                             mz_buffer_size;
    size_t                             mz_acquired = 0;
    std::shared_ptr<cp8_memory_budget> mp_budget;
    kit::c_lst<uint8_t *>              mo_free;
    kit::c_lst<uint8_t *>              mo_all;
    std::mutex                         mo_mutex;
};
