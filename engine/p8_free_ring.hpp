#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Bounded MPMC ring of buffer pointers: D. Vyukov's algorithm as in kit::c_mpsc_queue, with the positions and every
// cell in their own 128 bytes, the Apple M cache line and the line pair Intel's L2 prefetcher fetches (measured on M4)
class cp8_free_ring
{
    struct alignas(128) s_cell
    {
        std::atomic<size_t> mu_seq;
        uint8_t            *mp_buf;
    };

    alignas(128) s_cell *mp_cells; // this and the next two are read-only after the constructor
    size_t mz_capacity;
    size_t mz_mask;
    alignas(128) std::atomic<size_t> mu_enq { 0 };
    alignas(128) std::atomic<size_t> mu_deq { 0 };

public:
    // iz_capacity is a power of two
    explicit cp8_free_ring(size_t iz_capacity)
        : mp_cells(new s_cell[iz_capacity])
        , mz_capacity(iz_capacity)
        , mz_mask(iz_capacity - 1)
    {
        for(size_t lz_i = 0; lz_i < iz_capacity; ++lz_i)
        {
            mp_cells[lz_i].mu_seq.store(lz_i, std::memory_order_relaxed);
        }
    }

    ~cp8_free_ring()
    {
        delete[] mp_cells;
    }

    cp8_free_ring(const cp8_free_ring &)            = delete;
    cp8_free_ring &operator=(const cp8_free_ring &) = delete;
    cp8_free_ring(cp8_free_ring &&)                 = delete;
    cp8_free_ring &operator=(cp8_free_ring &&)      = delete;

    // false when the ring is full, and also while the pop that frees the tail cell is still writing it
    bool try_push(uint8_t *ip_buf)
    {
        s_cell *lp_cell = nullptr;
        size_t  lz_pos  = mu_enq.load(std::memory_order_relaxed);

        for(;;)
        {
            lp_cell                = &mp_cells[lz_pos & mz_mask];
            const size_t   lz_seq  = lp_cell->mu_seq.load(std::memory_order_acquire);
            const intptr_t li_diff = static_cast<intptr_t>(lz_seq) - static_cast<intptr_t>(lz_pos);

            if(0 == li_diff)
            {
                if(mu_enq.compare_exchange_weak(lz_pos, lz_pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if(li_diff < 0)
            {
                return false;
            }
            else
            {
                lz_pos = mu_enq.load(std::memory_order_relaxed);
            }
        }

        lp_cell->mp_buf = ip_buf;
        lp_cell->mu_seq.store(lz_pos + 1, std::memory_order_release);
        return true;
    }

    // false when the ring is empty, and also while the push that fills the head cell is still writing it
    bool try_pop(uint8_t *&op_buf)
    {
        s_cell *lp_cell = nullptr;
        size_t  lz_pos  = mu_deq.load(std::memory_order_relaxed);

        for(;;)
        {
            lp_cell                = &mp_cells[lz_pos & mz_mask];
            const size_t   lz_seq  = lp_cell->mu_seq.load(std::memory_order_acquire);
            const intptr_t li_diff = static_cast<intptr_t>(lz_seq) - static_cast<intptr_t>(lz_pos + 1);

            if(0 == li_diff)
            {
                if(mu_deq.compare_exchange_weak(lz_pos, lz_pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if(li_diff < 0)
            {
                return false;
            }
            else
            {
                lz_pos = mu_deq.load(std::memory_order_relaxed);
            }
        }

        op_buf = lp_cell->mp_buf;
        lp_cell->mu_seq.store(lz_pos + mz_mask + 1, std::memory_order_release);
        return true;
    }

    // buffers pushed and not popped yet, pushes and pops in progress included; the head is read before the tail, so
    // a ring that holds a buffer never reads as empty
    size_t get_count() const
    {
        const size_t lz_deq = mu_deq.load(std::memory_order_acquire);
        const size_t lz_enq = mu_enq.load(std::memory_order_acquire);
        return (lz_enq > lz_deq) ? lz_enq - lz_deq : 0;
    }

    size_t get_capacity() const
    {
        return mz_capacity;
    }
};
