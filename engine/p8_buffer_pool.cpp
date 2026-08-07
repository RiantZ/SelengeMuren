#include "p8_buffer_pool.hpp"
#include "p8_memory_budget.hpp"

#include <cstdio>
#include <new>
#include <utility>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_buffer_pool::cp8_buffer_pool(size_t iz_buffer_size, std::shared_ptr<cp8_memory_budget> ip_budget)
    : mz_buffer_size(iz_buffer_size)
    , mp_budget(std::move(ip_budget))
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
cp8_buffer_pool::~cp8_buffer_pool()
{
    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    size_t lz_reserved = mo_all.size() * mz_buffer_size;

    mo_all.clear([](uint8_t *ip_buf) { delete[] ip_buf; });
    mo_free.clear();

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
        if(mp_budget && !mp_budget->try_reserve(mz_buffer_size))
        {
            break;
        }

        uint8_t *lp_buf = new(std::nothrow) uint8_t[mz_buffer_size];
        if(!lp_buf)
        {
            std::fprintf(stderr, "cp8_buffer_pool::init: allocation of %zu bytes failed\n", mz_buffer_size);
            if(mp_budget)
            {
                mp_budget->release(mz_buffer_size);
            }
            break;
        }

        mo_all.push_last(lp_buf);
        mo_free.push_last(lp_buf);
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
uint8_t *cp8_buffer_pool::acquire_no_lock()
{
    if(mo_free.size() == 0)
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
        mo_free.push_last(lp_buf);
    }

    return mo_free.pull_first();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::stat(cp8_buffer_pool::s_stat &or_stat)
{
    size_t lz_outstanding  = (mo_all.size() - mo_free.size()) * mz_buffer_size;

    or_stat.mz_buffer_size = mz_buffer_size;
    or_stat.mz_max_size    = mp_budget ? mp_budget->get_max() : mo_all.size() * mz_buffer_size;
    or_stat.mz_free_size   = (or_stat.mz_max_size > lz_outstanding) ? (or_stat.mz_max_size - lz_outstanding) : 0;
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

    mo_mutex.lock();

    // Fast path: a buffer is available (grow-on-demand still fires here).
    uint8_t *lp_buf = acquire_no_lock();
    if(lp_buf)
    {
        mo_mutex.unlock();
        return lp_buf;
    }

    // Shutting down: never park; callers treat nullptr as a drop.
    if(mb_stopping)
    {
        mo_mutex.unlock();
        return nullptr;
    }

    // Enqueue at the tail (FIFO) under the same lock that decides where a recycled
    // buffer goes, so there is no lost-wakeup window between "saw no buffer" and "parked".
    lo_self.mp_next = nullptr;
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

#ifdef P8_TESTING
    mu_wait_arrivals.fetch_add(1, std::memory_order_relaxed);
#endif

    mo_mutex.unlock();

    // Park until a hand-off (or shutdown) releases our semaphore. A release that
    // landed just above is remembered by the semaphore, so acquire() returns at once.
    lo_self.mo_sem.acquire();

    // Read only our own stack node — never `this` — so a concurrent pool teardown
    // during wake-up is safe.
    return lo_self.mp_buf;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_buffer_pool::try_handoff_no_lock(uint8_t *ip_buf)
{
    if(mu_wait_count.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }

    // Dequeue the oldest waiter (FIFO). Read mp_next and publish mp_buf BEFORE the
    // release: once signaled, the waiter may return and destroy its stack node.
    s_waiter *lp_waiter = mp_wait_head;
    mp_wait_head        = lp_waiter->mp_next;
    if(!mp_wait_head)
    {
        mp_wait_tail = nullptr;
    }
    mu_wait_count.fetch_sub(1, std::memory_order_relaxed);

    lp_waiter->mp_buf = ip_buf;
    lp_waiter->mo_sem.release();
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::stop_waiters()
{
    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    mb_stopping = true;

    while(mp_wait_head)
    {
        s_waiter *lp_waiter = mp_wait_head;
        mp_wait_head        = lp_waiter->mp_next; // read next BEFORE the release
        lp_waiter->mp_buf   = nullptr;            // sentinel BEFORE the release
        lp_waiter->mo_sem.release();
    }
    mp_wait_tail = nullptr;
    mu_wait_count.store(0, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::recycle(uint8_t *ip_buf)
{
    if(!ip_buf)
    {
        return;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    if(!try_handoff_no_lock(ip_buf))
    {
        mo_free.push_last(ip_buf);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void cp8_buffer_pool::recycle(kit::c_lst<uint8_t *> &io_bufs)
{
    if(0 == io_bufs.size())
    {
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

            if(!try_handoff_no_lock(ip_buf))
            {
                mo_free.push_last(ip_buf);
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
    std::lock_guard<std::mutex> lo_guard(mo_mutex);
    return mo_free.size();
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
