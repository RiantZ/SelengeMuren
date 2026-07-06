#pragma once

#include <atomic>
#include <cstddef>

/// Atomic shared memory budget used by buffer pools to coordinate
/// grow-on-demand without exceeding a global limit. The budget is
/// owned by `cp8_core` and referenced by every pool that participates
/// in the shared limit.
class cp8_memory_budget
{
public:
    explicit cp8_memory_budget(size_t iz_max_bytes);

    cp8_memory_budget(const cp8_memory_budget &)            = delete;
    cp8_memory_budget &operator=(const cp8_memory_budget &) = delete;
    cp8_memory_budget(cp8_memory_budget &&)                 = delete;
    cp8_memory_budget &operator=(cp8_memory_budget &&)      = delete;

    // Atomic reservation. Returns true if the budget had enough headroom.
    bool try_reserve(size_t iz_bytes);

    // Releases a previously-reserved amount back to the budget.
    void release(size_t iz_bytes);

    size_t get_used() const
    {
        return mu_used.load(std::memory_order_relaxed);
    }
    size_t get_max() const
    {
        return mz_max;
    }

private:
    std::atomic<size_t> mu_used { 0 };
    size_t              mz_max;
};
