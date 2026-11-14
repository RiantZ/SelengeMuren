#include "p8_buffer_pool.hpp"
#include "p8_memory_budget.hpp"

#include "kit/plasma.hpp"

#include <cstdio>
#include <new>
#include <thread>
#include <utility>

// free buffers a pool without a budget keeps in its ring; more of them wait in mo_spill
#define P8_POOL_UNBUDGETED_COUNT 1024

// the most ring cells: 8 MB of cells for a 256 MB budget; a larger pool keeps the extra free buffers in mo_spill
#define P8_POOL_MAX_RING_CELLS   65536

// pauses a thread spends waiting for another thread to finish a ring cell before it yields
#define P8_POOL_CELL_SPINS       64

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// two cells for every buffer the budget can pay for: a push lands on a cell popped long ago, so it rarely waits
static size_t get_ring_capacity(size_t iz_buffer_size, const cp8_memory_budget *ip_budget)
{
    size_t lz_count    = P8_POOL_UNBUDGETED_COUNT;
    size_t lz_capacity = 2;

    if(ip_budget && iz_buffer_size > 0)
    {
        lz_count = (ip_budget->get_max() + iz_buffer_size - 1) / iz_buffer_size;
    }

    while(lz_capacity < 2 * lz_count && lz_capacity < P8_POOL_MAX_RING_CELLS)
    {
        lz_capacity <<= 1;
    }
    return lz_capacity;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// another thread holds a ring cell for a moment: spin first, then give the core away
static void wait_for_cell(uint32_t iu_round)
{
    if(iu_round < P8_POOL_CELL_SPINS)
    {
        plasma_spin_pause();
    }
    else
    {
        std::this_thread::yield();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_buffer_pool::cp8_buffer_pool(size_t iz_buffer_size, std::shared_ptr<cp8_memory_budget> ip_budget)
    : mz_buffer_size(iz_buffer_size)
    , mp_budget(std::move(ip_budget))
    , mo_free(get_ring_capacity(iz_buffer_size, mp_budget.get()))
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_buffer_pool::~cp8_buffer_pool()
{
    // Wake and refuse any still-parked waiters even if the owner never called
    // stop_waiters() (robust to the mb_worker_running early-return in cp8_core::
    // stop_worker). Idempotent.
    stop_waiters();

    // Teardown barrier: wait until every woken waiter has fully left acquire_wait()
    // (unlocked mo_mutex and decremented mu_inflight) before destroying mo_mutex / the
    // CV shards. MUST spin WITHOUT holding mo_mutex — a returning waiter needs it to
    // leave wait(). The acquire load pairs with the waiter's release fetch_sub, so
    // inflight==0 also implies mo_mutex is quiescent here.
    while(mu_inflight.load(std::memory_order_acquire) != 0)
    {
        std::this_thread::yield();
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    size_t lz_reserved = mo_all.size() * mz_buffer_size;

    // the ring and mo_spill only point at buffers of mo_all
    mo_all.clear([](uint8_t *ip_buf) { delete[] ip_buf; });
    mo_spill.clear();

    if(mp_budget && lz_reserved > 0)
    {
        mp_budget->release(lz_reserved);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_buffer_pool::init(size_t iz_initial_count)
{
    if(0 == iz_initial_count)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    size_t lz_allocated = 0;

    for(size_t lz_i = 0; lz_i < iz_initial_count; ++lz_i)
    {
        uint8_t *lp_buf = grow_no_lock();
        if(!lp_buf)
        {
            break;
        }

        put_free_no_lock(lp_buf);
        ++lz_allocated;
    }

    return lz_allocated;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::lock()
{
    mo_mutex.lock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_buffer_pool::grow_no_lock()
{
    if(mp_budget && !mp_budget->try_reserve(mz_buffer_size))
    {
        return nullptr;
    }

    uint8_t *lp_buf = new(std::nothrow) uint8_t[mz_buffer_size];
    if(!lp_buf)
    {
        std::fprintf(stderr, "cp8_buffer_pool::acquire: allocation of %zu bytes failed\n", mz_buffer_size);
        if(mp_budget)
        {
            mp_budget->release(mz_buffer_size);
        }
        return nullptr;
    }

    mo_all.push_last(lp_buf);
    mu_all_count.fetch_add(1, std::memory_order_relaxed);
    return lp_buf;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_buffer_pool::push_free(uint8_t *ip_buf)
{
    for(uint32_t lu_round = 0; !mo_free.try_push(ip_buf); ++lu_round)
    {
        if(mo_free.get_count() >= mo_free.get_capacity())
        {
            return false;
        }
        wait_for_cell(lu_round);
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::put_free(uint8_t *ip_buf)
{
    if(!push_free(ip_buf))
    {
        std::lock_guard<std::mutex> lo_guard(mo_mutex);
        put_free_no_lock(ip_buf);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::put_free_no_lock(uint8_t *ip_buf)
{
    if(!push_free(ip_buf))
    {
        mo_spill.push_last(ip_buf);
        mu_spill_count.fetch_add(1, std::memory_order_relaxed);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_buffer_pool::pop_free(uint8_t *&op_buf)
{
    for(uint32_t lu_round = 0; !mo_free.try_pop(op_buf); ++lu_round)
    {
        if(0 == mo_free.get_count())
        {
            return false;
        }
        wait_for_cell(lu_round);
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_buffer_pool::pop_spill_no_lock(uint8_t *&op_buf)
{
    if(0 == mo_spill.size())
    {
        return false;
    }

    op_buf = mo_spill.pull_last();
    mu_spill_count.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_buffer_pool::acquire_no_lock()
{
    uint8_t *lp_buf = nullptr;

    // with a waiter queued every free buffer is its, as the list stayed empty while recycle() fed the waiters
    if(!mp_wait_head)
    {
        if(pop_free(lp_buf) || pop_spill_no_lock(lp_buf))
        {
            return lp_buf;
        }
    }

    return grow_no_lock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_buffer_pool::try_acquire()
{
    uint8_t *lp_buf = nullptr;

    // a thread on the slow path may be queued, and a newcomer must not take a buffer before it
    if(0 == mu_slow.load(std::memory_order_relaxed) && mo_free.try_pop(lp_buf))
    {
        return lp_buf;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);
    return acquire_no_lock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::stat(cp8_buffer_pool::s_stat &or_stat)
{
    const size_t lz_all         = mu_all_count.load(std::memory_order_relaxed);
    const size_t lz_free        = mo_free.get_count() + mu_spill_count.load(std::memory_order_relaxed);
    const size_t lz_outstanding = ((lz_all > lz_free) ? lz_all - lz_free : 0) * mz_buffer_size;

    or_stat.mz_buffer_size      = mz_buffer_size;
    or_stat.mz_max_size         = mp_budget ? mp_budget->get_max() : lz_all * mz_buffer_size;
    or_stat.mz_free_size        = (or_stat.mz_max_size > lz_outstanding) ? (or_stat.mz_max_size - lz_outstanding) : 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::unlock()
{
    mo_mutex.unlock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint8_t *cp8_buffer_pool::acquire_wait()
{
    s_waiter lo_self;
    uint8_t *lp_buf = nullptr;

    if(0 == mu_slow.load(std::memory_order_relaxed) && mo_free.try_pop(lp_buf))
    {
        return lp_buf;
    }

    std::unique_lock<std::mutex> lo_lock(mo_mutex);

    // announced before the ring is checked again: a recycle that misses the announcement put its buffer in before
    // this check, so the check finds it (the fences on both sides order the two)
    mu_slow.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    lp_buf = acquire_no_lock();
    if(lp_buf || mb_stopping)
    {
        mu_slow.fetch_sub(1, std::memory_order_relaxed);
        return lp_buf;
    }

    // Enqueue at the tail (FIFO) under the same lock that decides where a recycled
    // buffer goes, so there is no lost-wakeup window between "saw no buffer" and "parked".
    lo_self.mp_next = nullptr;
    lo_self.mp_cv   = &mo_wait_cvs[mu_cv_next % mz_wait_cv_count]; // round-robin shard
    ++mu_cv_next;
    if(mp_wait_tail)
    {
        mp_wait_tail->mp_next = &lo_self;
    }
    else
    {
        mp_wait_head = &lo_self;
    }
    mp_wait_tail = &lo_self;
    mu_wait_count.fetch_add(1, std::memory_order_relaxed);
    mu_inflight.fetch_add(1, std::memory_order_relaxed);

#ifdef P8_TESTING
    mu_wait_arrivals.fetch_add(1, std::memory_order_relaxed);
#endif

    // wait() atomically RELEASES lo_lock while parked and RE-ACQUIRES it on wake — the
    // mutex is NOT held during the block, so the recycler can take mo_mutex and hand
    // off. Holding lo_lock on ENTRY is mandatory: enqueue + wait under one continuous
    // lock closes the lost-wakeup window. Predicate reads only our own node.
    lo_self.mp_cv->wait(lo_lock, [&lo_self] { return lo_self.mb_done; });

    uint8_t *lp_result = lo_self.mp_buf;

    mu_slow.fetch_sub(1, std::memory_order_relaxed);
    lo_lock.unlock();

    // LAST pool access — MUST be after unlock(). Pairs (release) with the destructor's
    // acquire load so mu_inflight==0 proves this waiter unlocked mo_mutex and will
    // touch no pool memory again. Do NOT move this before unlock().
    mu_inflight.fetch_sub(1, std::memory_order_release);

    return lp_result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int cp8_buffer_pool::try_handoff_no_lock(uint8_t *ip_buf)
{
    if(mu_wait_count.load(std::memory_order_relaxed) == 0)
    {
        return -1;
    }

    // Dequeue the oldest waiter (FIFO) and mark it served; return its CV shard index so
    // the caller notify_all()s exactly that shard.
    s_waiter *lp_waiter = mp_wait_head;
    mp_wait_head        = lp_waiter->mp_next;
    if(!mp_wait_head)
    {
        mp_wait_tail = nullptr;
    }
    mu_wait_count.fetch_sub(1, std::memory_order_relaxed);

    lp_waiter->mp_buf  = ip_buf;
    lp_waiter->mb_done = true;

    return static_cast<int>(lp_waiter->mp_cv - mo_wait_cvs.data());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::serve_waiters_no_lock()
{
    uint8_t *lp_buf = nullptr;

    while(mp_wait_head && (pop_free(lp_buf) || pop_spill_no_lock(lp_buf)))
    {
        mo_wait_cvs[try_handoff_no_lock(lp_buf)].notify_all();
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::stop_waiters()
{
    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    mb_stopping  = true;

    bool lb_woke = false;
    while(mp_wait_head)
    {
        s_waiter *lp_waiter = mp_wait_head;
        mp_wait_head        = lp_waiter->mp_next; // read next BEFORE marking done
        lp_waiter->mp_buf   = nullptr;            // sentinel
        lp_waiter->mb_done  = true;
        lb_woke             = true;
    }
    mp_wait_tail = nullptr;
    mu_wait_count.store(0, std::memory_order_relaxed);

    // Shutdown wakes everyone regardless of shard.
    if(lb_woke)
    {
        for(std::condition_variable &lo_cv : mo_wait_cvs)
        {
            lo_cv.notify_all();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::recycle(uint8_t *ip_buf)
{
    if(!ip_buf)
    {
        return;
    }

    if(0 == mu_slow.load(std::memory_order_relaxed))
    {
        put_free(ip_buf);

        // a thread that announced itself after the check above either finds the buffer or is served here
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if(0 == mu_slow.load(std::memory_order_relaxed))
        {
            return;
        }

        std::lock_guard<std::mutex> lo_guard(mo_mutex);
        serve_waiters_no_lock();
        return;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    const int li_idx = try_handoff_no_lock(ip_buf);
    if(li_idx >= 0)
    {
        mo_wait_cvs[li_idx].notify_all();
    }
    else
    {
        put_free_no_lock(ip_buf);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::recycle(kit::c_lst<uint8_t *> &io_bufs)
{
    if(0 == io_bufs.size())
    {
        return;
    }

    if(0 == mu_slow.load(std::memory_order_relaxed))
    {
        io_bufs.clear(
            [this](uint8_t *ip_buf)
            {
                if(ip_buf)
                {
                    put_free(ip_buf);
                }
            },
            kit::e_c_lst_pool_policy::e_keep);

        // a thread that announced itself after the check above either finds the buffers or is served here
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if(0 == mu_slow.load(std::memory_order_relaxed))
        {
            return;
        }

        std::lock_guard<std::mutex> lo_guard(mo_mutex);
        serve_waiters_no_lock();
        return;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    io_bufs.clear(
        [this](uint8_t *ip_buf)
        {
            if(!ip_buf)
            {
                return;
            }
            const int li_idx = try_handoff_no_lock(ip_buf);
            if(li_idx >= 0)
            {
                mo_wait_cvs[li_idx].notify_all(); // wake the served waiter's shard
            }
            else
            {
                put_free_no_lock(ip_buf);
            }
        },
        kit::e_c_lst_pool_policy::e_keep);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_buffer_pool::get_buffer_size() const
{
    return mz_buffer_size;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_buffer_pool::get_free_count()
{
    return mo_free.get_count() + mu_spill_count.load(std::memory_order_relaxed);
}

#ifdef P8_TESTING
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t cp8_buffer_pool::get_wait_count() const
{
    return mu_wait_count.load(std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t cp8_buffer_pool::get_wait_arrivals() const
{
    return mu_wait_arrivals.load(std::memory_order_relaxed);
}
#endif
