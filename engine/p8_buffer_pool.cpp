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
void cp8_buffer_pool::recycle(uint8_t *ip_buf)
{
    if(!ip_buf)
    {
        return;
    }

    std::lock_guard<std::mutex> lo_guard(mo_mutex);

    mz_acquired -= mz_buffer_size;

    mo_free.push_last(ip_buf);
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
            mz_acquired -= mz_buffer_size;
            mo_free.push_last(ip_buf);
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
