#include "p8_client_api.h"
#include "p8_config_keys.hpp"
#include "p8_core.hpp"
#include "p8_hash.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <latch>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
// call sites built by hand, so tests know the file, function and line of each span
const char *gp_file = "engine/p8_trc_site.cpp";
const char *gp_func = "trc_site_func";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string make_mem_config(size_t iz_buffers)
{
    const size_t      lz_bytes = iz_buffers * p8_test_get_buffer_size();
    const std::string ls       = std::to_string(lz_bytes);
    return std::string("{\"") + P8_CFG_KEY_MAX_MEMORY_SIZE + "\": \"" + ls + "\",\"" + P8_CFG_KEY_INITIAL_MEMORY_SIZE
           + "\": \"" + ls + "\"}";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// a new thread gets a fresh trace writer, which hands its buffers to the core when the thread exits
void run_in_thread(const std::function<void()> &il_fn)
{
    std::thread lo_thread(il_fn);
    lo_thread.join();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t begin_at(const char *ip_name, uint32_t iu_line)
{
    return p8_trc_begin(ip_name, iu_line, gp_file, gp_func, 0, nullptr);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t begin_async_at(uint64_t iu_parent_id, const char *ip_name, uint32_t iu_line)
{
    return p8_trc_begin_async(iu_parent_id, ip_name, iu_line, gp_file, gp_func, 0, nullptr);
}

struct s_trc_rec
{
    uint32_t             mu_writer_id   = 0;
    uint8_t              mu_type        = 0;
    uint8_t              mu_attrs_count = 0;
    uint16_t             mu_size        = 0;
    uint32_t             mu_thread_id   = 0;
    uint64_t             mu_timestamp   = 0;
    uint64_t             mu_span_id     = 0;
    uint64_t             mu_hash        = 0;
    uint64_t             mu_parent_id   = 0;
    size_t               mz_buf_off     = 0; // record start inside the buffer it starts in
    std::vector<uint8_t> mo_tail;            // bytes after the fixed part: attributes and padding
};

struct s_trc_parsed
{
    std::vector<s_trc_rec> mo_recs;                // in write order within each writer
    std::vector<uint32_t>  mo_buf_seqs;            // mu_buf_seq of the trace buffers, writer by writer
    size_t                 mz_fragmented_bufs = 0; // trace buffers flagged P8_DATA_FLAG_FRAGMENT
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Drain the writers, join each writer's trace buffers in buf_seq order and split the stream into records
s_trc_parsed parse_captured_traces()
{
    s_trc_parsed                                                 lo_result;
    std::map<uint32_t, std::map<uint32_t, std::vector<uint8_t>>> lo_streams; // writer id -> buf_seq -> payload

    p8_test_drain_writers();

    for(const auto &lr_buf : p8_test_get_captured_buffers())
    {
        s_p8_data_buf_hdr lo_hdr = {};
        if(lr_buf.size() < sizeof(lo_hdr))
        {
            continue;
        }
        memcpy(&lo_hdr, lr_buf.data(), sizeof(lo_hdr));
        if(P8_PACKET_TRACES != lo_hdr.mu_packet_type)
        {
            continue;
        }
        if(lo_hdr.mu_flags & P8_DATA_FLAG_FRAGMENT)
        {
            ++lo_result.mz_fragmented_bufs;
        }
        const size_t lz_used = std::min<size_t>(lo_hdr.mu_size, lr_buf.size());
        lo_streams[lo_hdr.mu_writer_id][lo_hdr.mu_buf_seq].assign(lr_buf.begin() + sizeof(lo_hdr),
                                                                  lr_buf.begin() + static_cast<ptrdiff_t>(lz_used));
    }

    for(const auto &lr_writer : lo_streams)
    {
        std::vector<uint8_t> lo_stream;
        std::vector<size_t>  lo_starts; // stream offset of each buffer's payload
        for(const auto &lr_part : lr_writer.second)
        {
            lo_result.mo_buf_seqs.push_back(lr_part.first);
            lo_starts.push_back(lo_stream.size());
            lo_stream.insert(lo_stream.end(), lr_part.second.begin(), lr_part.second.end());
        }

        size_t lz_off = 0;
        while(lz_off < lo_stream.size())
        {
            // the record starts in the last payload that begins at or before it; 64-bit reads need it 8-aligned
            const auto   lo_part    = std::upper_bound(lo_starts.begin(), lo_starts.end(), lz_off) - 1;
            const size_t lz_buf_off = sizeof(s_p8_data_buf_hdr) + (lz_off - *lo_part);
            EXPECT_EQ(lz_buf_off % 8, 0u) << "trace record at stream offset " << lz_off << " is not 8-aligned";

            // timestamp, span ID, size, type and thread ID sit at the same offsets in every record
            s_p8_trc_end_dat lo_head = {};
            if(lz_off + sizeof(lo_head) > lo_stream.size())
            {
                ADD_FAILURE() << "truncated trace record at " << lz_off;
                break;
            }
            memcpy(&lo_head, lo_stream.data() + lz_off, sizeof(lo_head));
            if(lo_head.mu_size < sizeof(lo_head) || lz_off + lo_head.mu_size > lo_stream.size())
            {
                ADD_FAILURE() << "bad trace record size " << lo_head.mu_size << " at " << lz_off;
                break;
            }

            s_trc_rec lo_rec;
            lo_rec.mz_buf_off   = lz_buf_off;
            lo_rec.mu_writer_id = lr_writer.first;
            lo_rec.mu_type      = lo_head.mu_type;
            lo_rec.mu_size      = lo_head.mu_size;
            lo_rec.mu_thread_id = lo_head.mu_thread_id;
            lo_rec.mu_timestamp = lo_head.mu_timestamp;
            lo_rec.mu_span_id   = lo_head.mu_span_id;

            size_t lz_fixed     = sizeof(s_p8_trc_end_dat);
            if(P8_TRC_ITEM_BEGIN == lo_head.mu_type || P8_TRC_ITEM_BEGIN_ASYNC == lo_head.mu_type)
            {
                s_p8_trc_begin_dat lo_begin = {};
                if(lo_head.mu_size < sizeof(lo_begin))
                {
                    ADD_FAILURE() << "begin record shorter than its header at " << lz_off;
                    break;
                }
                memcpy(&lo_begin, lo_stream.data() + lz_off, sizeof(lo_begin));
                lo_rec.mu_attrs_count = lo_begin.mu_attrs_count;
                lo_rec.mu_hash        = lo_begin.mu_hash;
                lo_rec.mu_parent_id   = lo_begin.mu_parent_id;
                lz_fixed              = sizeof(lo_begin);
            }
            lo_rec.mo_tail.assign(lo_stream.begin() + static_cast<ptrdiff_t>(lz_off + lz_fixed),
                                  lo_stream.begin() + static_cast<ptrdiff_t>(lz_off + lo_head.mu_size));

            lo_result.mo_recs.push_back(std::move(lo_rec));
            lz_off += lo_head.mu_size;
        }
    }

    return lo_result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const s_trc_rec *find_begin(const std::vector<s_trc_rec> &ir_recs, uint64_t iu_span_id)
{
    for(const s_trc_rec &lr_rec : ir_recs)
    {
        if(lr_rec.mu_span_id == iu_span_id
           && (P8_TRC_ITEM_BEGIN == lr_rec.mu_type || P8_TRC_ITEM_BEGIN_ASYNC == lr_rec.mu_type))
        {
            return &lr_rec;
        }
    }
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const s_trc_rec *find_end(const std::vector<s_trc_rec> &ir_recs, uint64_t iu_span_id)
{
    for(const s_trc_rec &lr_rec : ir_recs)
    {
        if(lr_rec.mu_span_id == iu_span_id && P8_TRC_ITEM_END == lr_rec.mu_type)
        {
            return &lr_rec;
        }
    }
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t parent_of(const std::vector<s_trc_rec> &ir_recs, uint64_t iu_span_id)
{
    const s_trc_rec *lp_rec = find_begin(ir_recs, iu_span_id);
    EXPECT_NE(lp_rec, nullptr) << "no begin record for span " << iu_span_id;
    return lp_rec ? lp_rec->mu_parent_id : ~0ull;
}

struct s_svc_entry
{
    uint8_t              mu_type = 0;
    std::vector<uint8_t> mo_bytes; // whole entry, including its s_p8_svc_hdr
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<s_svc_entry> collect_service_entries()
{
    std::vector<s_svc_entry> lo_entries;

    for(const auto &lr_buf : p8_test_get_service_buffers())
    {
        size_t lz_off = 0;
        while(lz_off + sizeof(s_p8_svc_hdr) <= lr_buf.size())
        {
            s_p8_svc_hdr lo_hdr = {};
            memcpy(&lo_hdr, lr_buf.data() + lz_off, sizeof(lo_hdr));
            if(lo_hdr.mu_size < sizeof(s_p8_svc_hdr) || lz_off + lo_hdr.mu_size > lr_buf.size())
            {
                break;
            }

            s_svc_entry lo_entry;
            lo_entry.mu_type = lo_hdr.mu_svc_type;
            lo_entry.mo_bytes.assign(lr_buf.data() + lz_off, lr_buf.data() + lz_off + lo_hdr.mu_size);
            lo_entries.push_back(std::move(lo_entry));

            lz_off += lo_hdr.mu_size;
        }
    }

    return lo_entries;
}

struct s_str_entry
{
    size_t      mz_index = 0; // position in the service stream
    uint8_t     mu_type  = 0;
    uint64_t    mu_hash  = 0;
    std::string mo_str;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<s_str_entry> collect_strings(const std::vector<s_svc_entry> &ir_entries)
{
    std::vector<s_str_entry> lo_strs;

    for(size_t lz_i = 0; lz_i < ir_entries.size(); ++lz_i)
    {
        const s_svc_entry &lr_entry = ir_entries[lz_i];
        if(P8_SVC_TYPE_FILE_NAME != lr_entry.mu_type && P8_SVC_TYPE_FUNC_NAME != lr_entry.mu_type
           && P8_SVC_TYPE_SPAN_NAME != lr_entry.mu_type)
        {
            continue;
        }

        s_p8_str_svc lo_svc = {};
        EXPECT_GE(lr_entry.mo_bytes.size(), sizeof(lo_svc));
        if(lr_entry.mo_bytes.size() < sizeof(lo_svc))
        {
            continue;
        }
        memcpy(&lo_svc, lr_entry.mo_bytes.data(), sizeof(lo_svc));
        EXPECT_EQ(lr_entry.mo_bytes.size(), (sizeof(lo_svc) + lo_svc.mu_len + 7u) & ~static_cast<size_t>(7u));
        if(sizeof(lo_svc) + lo_svc.mu_len > lr_entry.mo_bytes.size())
        {
            continue;
        }

        s_str_entry lo_str;
        lo_str.mz_index = lz_i;
        lo_str.mu_type  = lr_entry.mu_type;
        lo_str.mu_hash  = lo_svc.mu_hash;
        lo_str.mo_str.assign(reinterpret_cast<const char *>(lr_entry.mo_bytes.data() + sizeof(lo_svc)), lo_svc.mu_len);
        lo_strs.push_back(std::move(lo_str));
    }

    return lo_strs;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<s_p8_trc_item_svc> collect_trc_descs(const std::vector<s_svc_entry> &ir_entries,
                                                 std::vector<size_t>            *op_indexes = nullptr)
{
    std::vector<s_p8_trc_item_svc> lo_descs;

    for(size_t lz_i = 0; lz_i < ir_entries.size(); ++lz_i)
    {
        if(P8_SVC_TYPE_TRACE != ir_entries[lz_i].mu_type)
        {
            continue;
        }
        s_p8_trc_item_svc lo_desc = {};
        EXPECT_EQ(ir_entries[lz_i].mo_bytes.size(), sizeof(lo_desc));
        if(ir_entries[lz_i].mo_bytes.size() < sizeof(lo_desc))
        {
            continue;
        }
        memcpy(&lo_desc, ir_entries[lz_i].mo_bytes.data(), sizeof(lo_desc));
        lo_descs.push_back(lo_desc);
        if(op_indexes)
        {
            op_indexes->push_back(lz_i);
        }
    }

    return lo_descs;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint64_t get_str_hash(const char *ip_str)
{
    return XXH3_64bits(ip_str, strlen(ip_str));
}
} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// test fixture
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class c_trc_content_test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // capture mode freezes the worker, so every record must fit the pool
        const std::string  ls_config = make_mem_config(/*buffers*/ 128);
        struct s_p8_config lo_config = {};
        lo_config.mp_json_config     = ls_config.c_str();
        ASSERT_TRUE(p8_initialize(&lo_config));
        p8_test_enable_buffer_capture();
    }

    void TearDown() override
    {
        p8_test_disable_buffer_capture();
        p8_test_clear_captured_buffers();
        p8_release();
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// records
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_trc_content_test, begin_and_end_write_two_records)
{
    uint64_t lu_id = 0;
    run_in_thread(
        [&]()
        {
            lu_id = begin_at("work", 10);
            p8_trc_end(lu_id);
        });
    ASSERT_NE(lu_id, 0u);

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2u);

    const s_trc_rec &lr_begin = lo_parsed.mo_recs[0];
    EXPECT_EQ(lr_begin.mu_type, P8_TRC_ITEM_BEGIN);
    EXPECT_EQ(lr_begin.mu_span_id, lu_id);
    EXPECT_EQ(lr_begin.mu_parent_id, 0u);
    EXPECT_EQ(lr_begin.mu_size, sizeof(s_p8_trc_begin_dat));
    EXPECT_EQ(lr_begin.mu_attrs_count, 0u);
    EXPECT_EQ(lr_begin.mu_hash, P8_GET_LOG_HASH(gp_file, 10));
    EXPECT_NE(lr_begin.mu_timestamp, 0u);

    const s_trc_rec &lr_end = lo_parsed.mo_recs[1];
    EXPECT_EQ(lr_end.mu_type, P8_TRC_ITEM_END);
    EXPECT_EQ(lr_end.mu_span_id, lu_id);
    EXPECT_EQ(lr_end.mu_size, sizeof(s_p8_trc_end_dat));
    EXPECT_EQ(lr_end.mu_thread_id, lr_begin.mu_thread_id);
    EXPECT_EQ(lr_end.mu_writer_id, lr_begin.mu_writer_id);
    EXPECT_GE(lr_end.mu_timestamp, lr_begin.mu_timestamp);
}

TEST_F(c_trc_content_test, nested_span_takes_innermost_open_span_as_parent)
{
    uint64_t lu_a = 0;
    uint64_t lu_b = 0;
    uint64_t lu_c = 0;
    run_in_thread(
        [&]()
        {
            lu_a = begin_at("a", 10);
            lu_b = begin_at("b", 11);
            p8_trc_end(lu_b);
            lu_c = begin_at("c", 12);
            p8_trc_end(lu_c);
            p8_trc_end(lu_a);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 6u);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_a), 0u);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_b), lu_a);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_c), lu_a);
}

TEST_F(c_trc_content_test, end_of_outer_span_closes_inner_spans_left_open)
{
    uint64_t lu_a = 0;
    uint64_t lu_d = 0;
    run_in_thread(
        [&]()
        {
            lu_a = begin_at("a", 10);
            begin_at("b", 11); // never ended
            p8_trc_end(lu_a);
            lu_d = begin_at("d", 12);
            p8_trc_end(lu_d);
        });

    auto lo_parsed = parse_captured_traces();
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_d), 0u);
}

TEST_F(c_trc_content_test, end_of_span_not_open_on_this_thread_keeps_stack)
{
    uint64_t lu_old = 0;
    uint64_t lu_new = 0;
    uint64_t lu_a   = 0;
    uint64_t lu_b   = 0;
    // async spans of other threads, one with an ID block taken before this thread's, one after
    run_in_thread([&]() { lu_old = begin_async_at(0, "old", 9); });
    run_in_thread(
        [&]()
        {
            lu_a = begin_at("a", 10);
            run_in_thread([&]() { lu_new = begin_async_at(0, "new", 9); });
            p8_trc_end(lu_old);
            p8_trc_end(lu_new);
            lu_b = begin_at("b", 11);
            p8_trc_end(lu_b);
            p8_trc_end(lu_a);
        });
    ASSERT_LT(lu_old, lu_a);
    ASSERT_GT(lu_new, lu_a);

    auto lo_parsed = parse_captured_traces();
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_b), lu_a);
    EXPECT_NE(find_end(lo_parsed.mo_recs, lu_old), nullptr);
    EXPECT_NE(find_end(lo_parsed.mo_recs, lu_new), nullptr);
}

TEST_F(c_trc_content_test, async_span_takes_given_parent_and_stays_off_stack)
{
    uint64_t lu_a    = 0;
    uint64_t lu_x    = 0;
    uint64_t lu_b    = 0;
    uint64_t lu_c    = 0;
    uint64_t lu_root = 0;
    run_in_thread(
        [&]()
        {
            lu_a = begin_at("a", 10);
            lu_x = begin_async_at(lu_a, "x", 11);
            lu_b = begin_at("b", 12);
            p8_trc_end(lu_b);
            p8_trc_end(lu_x);
            lu_c = begin_at("c", 13);
            p8_trc_end(lu_c);
            p8_trc_end(lu_a);
            lu_root = begin_async_at(0, "root", 14);
            p8_trc_end(lu_root);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 10u);

    const s_trc_rec *lp_x = find_begin(lo_parsed.mo_recs, lu_x);
    ASSERT_NE(lp_x, nullptr);
    EXPECT_EQ(lp_x->mu_type, P8_TRC_ITEM_BEGIN_ASYNC);
    EXPECT_EQ(lp_x->mu_parent_id, lu_a);
    EXPECT_EQ(lp_x->mu_size, sizeof(s_p8_trc_begin_dat));

    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_b), lu_a);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_c), lu_a);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_root), 0u);
}

TEST_F(c_trc_content_test, async_span_ends_on_another_thread)
{
    uint64_t lu_x = 0;
    run_in_thread([&]() { lu_x = begin_async_at(0, "x", 10); });
    ASSERT_NE(lu_x, 0u);
    run_in_thread([&]() { p8_trc_end(lu_x); });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2u);

    const s_trc_rec *lp_begin = find_begin(lo_parsed.mo_recs, lu_x);
    const s_trc_rec *lp_end   = find_end(lo_parsed.mo_recs, lu_x);
    ASSERT_NE(lp_begin, nullptr);
    ASSERT_NE(lp_end, nullptr);
    EXPECT_NE(lp_begin->mu_writer_id, lp_end->mu_writer_id);
}

TEST_F(c_trc_content_test, end_of_zero_span_writes_nothing)
{
    size_t lz_before = 0;
    size_t lz_after  = 0;
    run_in_thread(
        [&]()
        {
            lz_before = p8_test_get_writer_count();
            p8_trc_end(0);
            lz_after = p8_test_get_writer_count();
        });

    // a thread that only ends a failed span gets no writer
    EXPECT_EQ(lz_after, lz_before);
    auto lo_parsed = parse_captured_traces();
    EXPECT_TRUE(lo_parsed.mo_recs.empty());
}

TEST_F(c_trc_content_test, scope_guard_begins_and_ends_span)
{
    uint64_t lu_scope = 0;
    uint64_t lu_inner = 0;
    run_in_thread(
        [&]()
        {
            P8TRC_SCOPE(lo_span, "scope", 0, nullptr);
            lu_scope = lo_span.get_span_id();
            lu_inner = P8TRC_BEGIN("inner", 0, nullptr);
            P8TRC_END(lu_inner);
        });
    ASSERT_NE(lu_scope, 0u);

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 4u);
    EXPECT_EQ(lo_parsed.mo_recs[0].mu_span_id, lu_scope);
    EXPECT_EQ(parent_of(lo_parsed.mo_recs, lu_inner), lu_scope);
    EXPECT_EQ(lo_parsed.mo_recs[3].mu_type, P8_TRC_ITEM_END);
    EXPECT_EQ(lo_parsed.mo_recs[3].mu_span_id, lu_scope);
}

TEST_F(c_trc_content_test, trace_functions_share_one_writer_per_thread)
{
    size_t lz_before = 0;
    size_t lz_after  = 0;
    run_in_thread(
        [&]()
        {
            lz_before     = p8_test_get_writer_count();
            uint64_t lu_a = begin_at("a", 10);
            uint64_t lu_x = begin_async_at(lu_a, "x", 11);
            p8_trc_end(lu_x);
            p8_trc_end(lu_a);
            lz_after = p8_test_get_writer_count();
        });

    EXPECT_EQ(lz_after, lz_before + 1);
}

TEST_F(c_trc_content_test, span_ids_are_unique_across_threads)
{
    const size_t                       lz_threads = 8;
    const size_t                       lz_spans   = 1000;
    std::vector<std::vector<uint64_t>> lo_ids(lz_threads);
    std::vector<std::thread>           lo_threads;

    for(size_t lz_t = 0; lz_t < lz_threads; ++lz_t)
    {
        lo_threads.emplace_back(
            [&lo_ids, lz_t]()
            {
                for(size_t lz_i = 0; lz_i < lz_spans; ++lz_i)
                {
                    uint64_t lu_id = begin_at("span", 10);
                    lo_ids[lz_t].push_back(lu_id);
                    p8_trc_end(lu_id);
                }
            });
    }
    for(std::thread &lr_thread : lo_threads)
    {
        lr_thread.join();
    }

    std::unordered_set<uint64_t> lo_unique;
    for(const auto &lr_ids : lo_ids)
    {
        for(uint64_t lu_id : lr_ids)
        {
            EXPECT_NE(lu_id, 0u);
            lo_unique.insert(lu_id);
        }
    }
    EXPECT_EQ(lo_unique.size(), lz_threads * lz_spans);

    auto lo_parsed = parse_captured_traces();
    EXPECT_EQ(lo_parsed.mo_recs.size(), 2 * lz_threads * lz_spans);
}

TEST_F(c_trc_content_test, span_ids_stay_unique_past_one_id_block)
{
    // more spans than one block of IDs; the worker drains to the null sink, so the pool never runs out
    p8_test_disable_buffer_capture();

    const size_t                       lz_threads = 2;
    const size_t                       lz_spans   = P8_CORE_SPAN_ID_BLOCK + 1000;
    std::vector<std::vector<uint64_t>> lo_ids(lz_threads);
    std::vector<std::thread>           lo_threads;

    for(size_t lz_t = 0; lz_t < lz_threads; ++lz_t)
    {
        lo_threads.emplace_back(
            [&lo_ids, lz_t]()
            {
                lo_ids[lz_t].reserve(lz_spans);
                for(size_t lz_i = 0; lz_i < lz_spans; ++lz_i)
                {
                    uint64_t lu_id = begin_async_at(0, "span", 10);
                    lo_ids[lz_t].push_back(lu_id);
                    p8_trc_end(lu_id);
                }
            });
    }
    for(std::thread &lr_thread : lo_threads)
    {
        lr_thread.join();
    }

    std::unordered_set<uint64_t> lo_unique;
    for(const auto &lr_ids : lo_ids)
    {
        for(uint64_t lu_id : lr_ids)
        {
            ASSERT_NE(lu_id, 0u);
            lo_unique.insert(lu_id);
        }
    }
    EXPECT_EQ(lo_unique.size(), lz_threads * lz_spans);
}

TEST_F(c_trc_content_test, begin_with_attributes_serializes_them)
{
    const p8_attr_id li_count = p8_attr_register("trc_count", e_p8_attr_u64);
    const p8_attr_id li_name  = p8_attr_register("trc_name", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_count));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_name));

    uint64_t lu_id = 0;
    run_in_thread(
        [&]()
        {
            s_p8_attr_val lo_attrs[2] = {};
            lo_attrs[0].m_id          = li_count;
            lo_attrs[0].mu_u64        = 7;
            lo_attrs[1].m_id          = li_name;
            lo_attrs[1].mp_str        = "hello";
            lu_id                     = p8_trc_begin("with_attrs", 10, gp_file, gp_func, 2, lo_attrs);
            p8_trc_end(lu_id);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2u);

    const s_trc_rec &lr_begin = lo_parsed.mo_recs[0];
    EXPECT_EQ(lr_begin.mu_attrs_count, 2u);

    // [id][u64] + [id][u16 len][bytes], then padding to 8
    std::vector<uint8_t> lo_expected;
    const uint64_t       lu_count = 7;
    const uint16_t       lu_len   = 5;
    lo_expected.insert(lo_expected.end(),
                       reinterpret_cast<const uint8_t *>(&li_count),
                       reinterpret_cast<const uint8_t *>(&li_count) + sizeof(li_count));
    lo_expected.insert(lo_expected.end(),
                       reinterpret_cast<const uint8_t *>(&lu_count),
                       reinterpret_cast<const uint8_t *>(&lu_count) + sizeof(lu_count));
    lo_expected.insert(lo_expected.end(),
                       reinterpret_cast<const uint8_t *>(&li_name),
                       reinterpret_cast<const uint8_t *>(&li_name) + sizeof(li_name));
    lo_expected.insert(lo_expected.end(),
                       reinterpret_cast<const uint8_t *>(&lu_len),
                       reinterpret_cast<const uint8_t *>(&lu_len) + sizeof(lu_len));
    lo_expected.insert(lo_expected.end(), { 'h', 'e', 'l', 'l', 'o' });

    const size_t lz_size = (sizeof(s_p8_trc_begin_dat) + lo_expected.size() + 7u) & ~static_cast<size_t>(7u);
    EXPECT_EQ(lr_begin.mu_size, lz_size);
    ASSERT_GE(lr_begin.mo_tail.size(), lo_expected.size());
    EXPECT_TRUE(std::equal(lo_expected.begin(), lo_expected.end(), lr_begin.mo_tail.begin()));
}

TEST_F(c_trc_content_test, record_larger_than_a_buffer_continues_in_next_buffer)
{
    const p8_attr_id li_blob = p8_attr_register("trc_blob", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_blob));

    const std::string ls_blob(p8_test_get_buffer_size() + 1808, 'x');
    uint64_t          lu_id = 0;
    run_in_thread(
        [&]()
        {
            s_p8_attr_val lo_attr = {};
            lo_attr.m_id          = li_blob;
            lo_attr.mp_str        = ls_blob.c_str();
            lu_id                 = p8_trc_begin_async(0, "blob", 10, gp_file, gp_func, 1, &lo_attr);
            p8_trc_end(lu_id);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2u);
    EXPECT_EQ(lo_parsed.mz_fragmented_bufs, 1u);

    const size_t     lz_attr  = sizeof(p8_attr_id) + sizeof(uint16_t) + ls_blob.size();
    const size_t     lz_size  = (sizeof(s_p8_trc_begin_dat) + lz_attr + 7u) & ~static_cast<size_t>(7u);
    const s_trc_rec &lr_begin = lo_parsed.mo_recs[0];
    EXPECT_EQ(lr_begin.mu_attrs_count, 1u);
    EXPECT_EQ(lr_begin.mu_size, lz_size);
    ASSERT_GE(lr_begin.mo_tail.size(), lz_attr);
    const std::string ls_got(reinterpret_cast<const char *>(lr_begin.mo_tail.data()) + lz_attr - ls_blob.size(),
                             ls_blob.size());
    EXPECT_EQ(ls_got, ls_blob);

    // the first buffer is full, the rest of the record opens the second one
    const size_t lz_payload = p8_test_get_buffer_size() - sizeof(s_p8_data_buf_hdr);
    EXPECT_EQ(lo_parsed.mo_recs[1].mz_buf_off, sizeof(s_p8_data_buf_hdr) + lz_size - lz_payload);
}

TEST_F(c_trc_content_test, number_split_across_buffers_keeps_next_record_aligned)
{
    const p8_attr_id li_fill  = p8_attr_register("trc_fill", e_p8_attr_str);
    const p8_attr_id li_split = p8_attr_register("trc_split", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_fill));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_split));

    // one record larger than a buffer: the string stops 8 bytes before the buffer end, the attribute ID and half
    // of the number take them, the other half of the number goes to the next buffer
    const size_t      lz_payload = p8_test_get_buffer_size() - sizeof(s_p8_data_buf_hdr);
    const size_t      lz_fill    = lz_payload - sizeof(s_p8_trc_begin_dat) - sizeof(p8_attr_id) - sizeof(uint16_t) - 8;
    const std::string ls_fill(lz_fill, 'f');
    const uint64_t    lu_value = 0x1122334455667788ull;
    run_in_thread(
        [&]()
        {
            s_p8_attr_val lo_attrs[2] = {};
            lo_attrs[0].m_id          = li_fill;
            lo_attrs[0].mp_str        = ls_fill.c_str();
            lo_attrs[1].m_id          = li_split;
            lo_attrs[1].mu_u64        = lu_value;
            p8_trc_begin_async(0, "split", 10, gp_file, gp_func, 2, lo_attrs);
            p8_trc_begin_async(0, "after", 11, gp_file, gp_func, 0, nullptr);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2u);
    EXPECT_EQ(lo_parsed.mz_fragmented_bufs, 1u);

    const s_trc_rec &lr_split = lo_parsed.mo_recs[0];
    const size_t     lz_num   = sizeof(p8_attr_id) + sizeof(uint16_t) + lz_fill;
    EXPECT_EQ(lr_split.mu_attrs_count, 2u);
    EXPECT_EQ(lr_split.mu_size, lz_payload + 8u);
    ASSERT_GE(lr_split.mo_tail.size(), lz_num + sizeof(p8_attr_id) + sizeof(uint64_t));
    p8_attr_id li_got_id = 0;
    uint64_t   lu_got    = 0;
    memcpy(&li_got_id, lr_split.mo_tail.data() + lz_num, sizeof(li_got_id));
    memcpy(&lu_got, lr_split.mo_tail.data() + lz_num + sizeof(p8_attr_id), sizeof(lu_got));
    EXPECT_EQ(li_got_id, li_split);
    EXPECT_EQ(lu_got, lu_value);

    // 4 bytes of the number and 4 bytes of padding open the second buffer
    EXPECT_EQ(lo_parsed.mo_recs[1].mz_buf_off, sizeof(s_p8_data_buf_hdr) + 8u);
}

TEST_F(c_trc_content_test, record_that_fits_a_buffer_moves_whole_to_the_next_one)
{
    const p8_attr_id li_fill = p8_attr_register("trc_fill", e_p8_attr_str);
    const p8_attr_id li_num  = p8_attr_register("trc_num", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_fill));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_num));

    // the first record leaves 48 bytes, the second one needs 56
    const size_t      lz_payload = p8_test_get_buffer_size() - sizeof(s_p8_data_buf_hdr);
    const size_t      lz_fill = lz_payload - 48 - sizeof(s_p8_trc_begin_dat) - sizeof(p8_attr_id) - sizeof(uint16_t);
    const std::string ls_fill(lz_fill, 'f');
    run_in_thread(
        [&]()
        {
            s_p8_attr_val lo_fill = {};
            lo_fill.m_id          = li_fill;
            lo_fill.mp_str        = ls_fill.c_str();
            s_p8_attr_val lo_num  = {};
            lo_num.m_id           = li_num;
            lo_num.mu_u64         = 42;
            p8_trc_begin_async(0, "fill", 10, gp_file, gp_func, 1, &lo_fill);
            p8_trc_begin_async(0, "num", 11, gp_file, gp_func, 1, &lo_num);
            p8_trc_begin_async(0, "after", 12, gp_file, gp_func, 0, nullptr);
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 3u);
    EXPECT_EQ(lo_parsed.mz_fragmented_bufs, 0u);
    EXPECT_EQ(lo_parsed.mo_recs[0].mu_size, lz_payload - 48);
    EXPECT_EQ(lo_parsed.mo_recs[1].mu_size, 56u);
    EXPECT_EQ(lo_parsed.mo_recs[1].mz_buf_off, sizeof(s_p8_data_buf_hdr));
    EXPECT_EQ(lo_parsed.mo_recs[2].mz_buf_off, sizeof(s_p8_data_buf_hdr) + 56u);
    EXPECT_EQ(lo_parsed.mo_buf_seqs, (std::vector<uint32_t> { 0, 1 }));
}

TEST_F(c_trc_content_test, record_dropped_mid_write_leaves_no_trace)
{
    const p8_attr_id li_blob = p8_attr_register("trc_blob", e_p8_attr_str);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_blob));

    // the big record needs two buffers after the one it starts in; the next one fits a buffer but not the rest
    const std::string ls_big(2 * p8_test_get_buffer_size(), 'b');
    const std::string ls_next(p8_test_get_buffer_size() - 100, 'n');
    uint64_t          lu_big    = 1;
    uint64_t          lu_ids[3] = {};
    run_in_thread(
        [&]()
        {
            std::vector<uint8_t *> lo_held;
            s_p8_attr_val          lo_big = {};
            lo_big.m_id                   = li_blob;
            lo_big.mp_str                 = ls_big.c_str();
            s_p8_attr_val lo_next         = {};
            lo_next.m_id                  = li_blob;
            lo_next.mp_str                = ls_next.c_str();

            // one site for all records, so its descriptor is sent before the pool runs dry
            lu_ids[0]                     = p8_trc_begin_async(0, "warm", 10, gp_file, gp_func, 0, nullptr);

            // one free buffer is left: the big record gets one continuation and fails on the next one
            for(uint8_t *lp_buf = p8_test_acquire_buffer(); lp_buf; lp_buf = p8_test_acquire_buffer())
            {
                lo_held.push_back(lp_buf);
            }
            p8_test_release_buffer(lo_held.back());
            lo_held.pop_back();
            lu_big = p8_trc_begin_async(0, "big", 10, gp_file, gp_func, 1, &lo_big);
            p8_test_release_buffers(lo_held.data(), lo_held.size());

            lu_ids[1] = p8_trc_begin_async(0, "small", 10, gp_file, gp_func, 0, nullptr);
            lu_ids[2] = p8_trc_begin_async(0, "next", 10, gp_file, gp_func, 1, &lo_next);
        });

    EXPECT_EQ(lu_big, 0u);
    EXPECT_EQ(p8_test_get_dropped_stats().mu_traces, 1u);

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 3u);
    EXPECT_EQ(lo_parsed.mz_fragmented_bufs, 0u);
    for(size_t lz_i = 0; lz_i < 3; ++lz_i)
    {
        EXPECT_EQ(lo_parsed.mo_recs[lz_i].mu_span_id, lu_ids[lz_i]);
    }

    // the buffer taken for the dropped record never reached the server, so its number is used again
    EXPECT_EQ(lo_parsed.mo_buf_seqs, (std::vector<uint32_t> { 0, 1 }));
}

TEST_F(c_trc_content_test, oversized_attributes_are_cut_to_fit_the_record)
{
    const p8_attr_id li_str = p8_attr_register("trc_huge", e_p8_attr_str);
    const p8_attr_id li_num = p8_attr_register("trc_num", e_p8_attr_u64);
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_str));
    ASSERT_TRUE(P8_IS_ATTR_VALID(li_num));

    const std::string          ls_huge(70000, 'h');
    std::vector<s_p8_attr_val> lo_many(300);
    for(s_p8_attr_val &lr_attr : lo_many)
    {
        lr_attr.m_id   = li_num;
        lr_attr.mu_u64 = 1;
    }
    uint64_t lu_ids[3] = {};
    run_in_thread(
        [&]()
        {
            s_p8_attr_val lo_pair[2] = {};
            lo_pair[0].m_id          = li_num;
            lo_pair[0].mu_u64        = 42;
            lo_pair[1].m_id          = li_str;
            lo_pair[1].mp_str        = ls_huge.c_str();
            lu_ids[0]                = p8_trc_begin_async(0, "num_str", 10, gp_file, gp_func, 2, lo_pair);
            std::swap(lo_pair[0], lo_pair[1]);
            lu_ids[1] = p8_trc_begin_async(0, "str_num", 10, gp_file, gp_func, 2, lo_pair);
            lu_ids[2] = p8_trc_begin_async(0, "many", 10, gp_file, gp_func, lo_many.size(), lo_many.data());
        });

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 3u);

    // the largest record whose padded size fits the 16-bit mu_size
    const size_t lz_max   = UINT16_MAX & ~static_cast<size_t>(7u);
    const size_t lz_head  = sizeof(p8_attr_id) + sizeof(uint16_t);
    const size_t lz_num   = sizeof(p8_attr_id) + sizeof(uint64_t);
    uint16_t     lu_len   = 0;

    // the number comes first and stays, the string is cut to the room left
    const s_trc_rec *lp_0 = find_begin(lo_parsed.mo_recs, lu_ids[0]);
    ASSERT_NE(lp_0, nullptr);
    EXPECT_EQ(lp_0->mu_attrs_count, 2u);
    EXPECT_EQ(lp_0->mu_size, lz_max);
    ASSERT_GE(lp_0->mo_tail.size(), lz_num + lz_head);
    memcpy(&lu_len, lp_0->mo_tail.data() + lz_num + sizeof(p8_attr_id), sizeof(lu_len));
    EXPECT_EQ(lu_len, lz_max - sizeof(s_p8_trc_begin_dat) - lz_num - lz_head);

    // the string takes all the room, the number after it is skipped
    const s_trc_rec *lp_1 = find_begin(lo_parsed.mo_recs, lu_ids[1]);
    ASSERT_NE(lp_1, nullptr);
    EXPECT_EQ(lp_1->mu_attrs_count, 1u);
    EXPECT_EQ(lp_1->mu_size, lz_max);
    ASSERT_GE(lp_1->mo_tail.size(), lz_head);
    memcpy(&lu_len, lp_1->mo_tail.data() + sizeof(p8_attr_id), sizeof(lu_len));
    EXPECT_EQ(lu_len, lz_max - sizeof(s_p8_trc_begin_dat) - lz_head);

    // the 8-bit count keeps 255 of 300
    const s_trc_rec *lp_2 = find_begin(lo_parsed.mo_recs, lu_ids[2]);
    ASSERT_NE(lp_2, nullptr);
    EXPECT_EQ(lp_2->mu_attrs_count, 255u);
    EXPECT_EQ(lp_2->mu_size, (sizeof(s_p8_trc_begin_dat) + 255 * lz_num + 7u) & ~static_cast<size_t>(7u));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// service descriptors
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(c_trc_content_test, call_site_and_its_strings_are_sent_once)
{
    uint64_t lu_a = 0;
    uint64_t lu_b = 0;
    run_in_thread(
        [&]()
        {
            for(int li_i = 0; li_i < 3; ++li_i)
            {
                lu_a = begin_at("span_a", 100);
                p8_trc_end(lu_a);
            }
            lu_b = begin_at("span_b", 101);
            p8_trc_end(lu_b);
        });
    // another thread at the same site reuses the descriptor
    run_in_thread([&]() { p8_trc_end(begin_at("span_a", 100)); });

    const auto lo_entries = collect_service_entries();
    const auto lo_strs    = collect_strings(lo_entries);

    std::vector<size_t> lo_desc_idx;
    const auto          lo_descs = collect_trc_descs(lo_entries, &lo_desc_idx);
    ASSERT_EQ(lo_descs.size(), 2u);

    size_t lz_files = 0;
    size_t lz_funcs = 0;
    size_t lz_names = 0;
    for(const s_str_entry &lr_str : lo_strs)
    {
        EXPECT_EQ(lr_str.mu_hash, get_str_hash(lr_str.mo_str.c_str())) << lr_str.mo_str;
        lz_files += (P8_SVC_TYPE_FILE_NAME == lr_str.mu_type) ? 1 : 0;
        lz_funcs += (P8_SVC_TYPE_FUNC_NAME == lr_str.mu_type) ? 1 : 0;
        lz_names += (P8_SVC_TYPE_SPAN_NAME == lr_str.mu_type) ? 1 : 0;
    }
    EXPECT_EQ(lz_files, 1u);
    EXPECT_EQ(lz_funcs, 1u);
    EXPECT_EQ(lz_names, 2u);

    const char *lp_names[2] = { "span_a", "span_b" };
    for(size_t lz_i = 0; lz_i < lo_descs.size(); ++lz_i)
    {
        const s_p8_trc_item_svc &lr_desc = lo_descs[lz_i];
        EXPECT_EQ(lr_desc.ms_hdr.mu_packet_type, P8_PACKET_SERVICE);
        EXPECT_EQ(lr_desc.ms_hdr.mu_size, sizeof(s_p8_trc_item_svc));
        EXPECT_EQ(lr_desc.mu_line, 100 + lz_i);
        EXPECT_EQ(lr_desc.mu_hash, P8_GET_LOG_HASH(gp_file, static_cast<uint32_t>(100 + lz_i)));
        EXPECT_EQ(lr_desc.mu_file_hash, get_str_hash(gp_file));
        EXPECT_EQ(lr_desc.mu_func_hash, get_str_hash(gp_func));
        EXPECT_EQ(lr_desc.mu_name_hash, get_str_hash(lp_names[lz_i]));

        // every string a descriptor refers to comes before it, with the type of its field
        const std::pair<uint8_t, uint64_t> la_refs[3] = {
            { P8_SVC_TYPE_FILE_NAME, lr_desc.mu_file_hash },
            { P8_SVC_TYPE_FUNC_NAME, lr_desc.mu_func_hash },
            { P8_SVC_TYPE_SPAN_NAME, lr_desc.mu_name_hash }
        };
        for(const auto &lr_ref : la_refs)
        {
            auto lo_it = std::find_if(lo_strs.begin(),
                                      lo_strs.end(),
                                      [&lr_ref](const s_str_entry &ir_str)
                                      { return ir_str.mu_type == lr_ref.first && ir_str.mu_hash == lr_ref.second; });
            ASSERT_NE(lo_it, lo_strs.end()) << "no string of type " << static_cast<int>(lr_ref.first);
            EXPECT_LT(lo_it->mz_index, lo_desc_idx[lz_i]);
        }
    }

    auto lo_parsed = parse_captured_traces();
    EXPECT_EQ(find_begin(lo_parsed.mo_recs, lu_a)->mu_hash, lo_descs[0].mu_hash);
    EXPECT_EQ(find_begin(lo_parsed.mo_recs, lu_b)->mu_hash, lo_descs[1].mu_hash);
}

TEST_F(c_trc_content_test, null_strings_are_sent_as_empty)
{
    run_in_thread([]() { p8_trc_end(p8_trc_begin(nullptr, 10, nullptr, nullptr, 0, nullptr)); });

    const auto lo_entries = collect_service_entries();
    const auto lo_descs   = collect_trc_descs(lo_entries);
    ASSERT_EQ(lo_descs.size(), 1u);
    EXPECT_EQ(lo_descs[0].mu_hash, P8_GET_LOG_HASH("", 10));
    EXPECT_EQ(lo_descs[0].mu_file_hash, get_str_hash(""));
    EXPECT_EQ(lo_descs[0].mu_func_hash, get_str_hash(""));
    EXPECT_EQ(lo_descs[0].mu_name_hash, get_str_hash(""));
}

TEST_F(c_trc_content_test, long_name_is_cut_on_a_utf8_boundary)
{
    // 'a' and then two-byte characters, so byte P8_SVC_STR_MAX_LEN is the second byte of a character
    std::string ls_name("a");
    while(ls_name.size() < 2 * P8_SVC_STR_MAX_LEN)
    {
        ls_name += "\xC3\xA9";
    }
    run_in_thread([&]() { p8_trc_end(p8_trc_begin(ls_name.c_str(), 10, gp_file, gp_func, 0, nullptr)); });

    const auto lo_strs = collect_strings(collect_service_entries());
    auto       lo_it = std::find_if(lo_strs.begin(),
                                    lo_strs.end(),
                                    [](const s_str_entry &ir_str) { return P8_SVC_TYPE_SPAN_NAME == ir_str.mu_type; });
    ASSERT_NE(lo_it, lo_strs.end());
    EXPECT_EQ(lo_it->mo_str, ls_name.substr(0, P8_SVC_STR_MAX_LEN - 1));
    EXPECT_EQ(lo_it->mu_hash, XXH3_64bits(ls_name.data(), P8_SVC_STR_MAX_LEN - 1));
}

TEST_F(c_trc_content_test, first_use_of_a_site_by_many_threads_sends_one_descriptor)
{
    const size_t             lz_threads = 8;
    std::latch               lo_start(lz_threads);
    std::vector<std::thread> lo_threads;
    for(size_t lz_t = 0; lz_t < lz_threads; ++lz_t)
    {
        lo_threads.emplace_back(
            [&lo_start]()
            {
                lo_start.arrive_and_wait();
                p8_trc_end(begin_at("shared", 10));
            });
    }
    for(std::thread &lr_thread : lo_threads)
    {
        lr_thread.join();
    }

    const auto lo_descs = collect_trc_descs(collect_service_entries());
    ASSERT_EQ(lo_descs.size(), 1u);

    auto lo_parsed = parse_captured_traces();
    ASSERT_EQ(lo_parsed.mo_recs.size(), 2 * lz_threads);
    for(const s_trc_rec &lr_rec : lo_parsed.mo_recs)
    {
        if(P8_TRC_ITEM_BEGIN == lr_rec.mu_type)
        {
            EXPECT_EQ(lr_rec.mu_hash, lo_descs[0].mu_hash);
        }
    }
}

TEST_F(c_trc_content_test, span_never_goes_out_without_its_descriptor)
{
    // a site with a long name needs its name string and its descriptor in the service buffer
    const size_t lz_site
        = ((sizeof(s_p8_str_svc) + P8_SVC_STR_MAX_LEN + 7u) & ~static_cast<size_t>(7u)) + sizeof(s_p8_trc_item_svc);
    uint64_t lu_last = 1;
    run_in_thread(
        [&]()
        {
            std::vector<uint8_t *> lo_held;
            std::string            ls_name(P8_SVC_STR_MAX_LEN, 'n');

            // the first site takes a data buffer with room and a service buffer
            p8_trc_end(begin_at("first", 10));

            // sites with long names fill the service buffer until the next one does not fit
            for(uint32_t lu_line = 11;
                p8_test_get_service_buffers().back().size() + lz_site <= p8_test_get_buffer_size();
                ++lu_line)
            {
                ls_name.replace(0, 8, std::to_string(100000000 + lu_line).substr(1));
                p8_trc_end(p8_trc_begin(ls_name.c_str(), lu_line, gp_file, gp_func, 0, nullptr));
            }

            // in capture mode an acquire never waits, so with the pool empty the next site cannot be described
            for(uint8_t *lp_buf = p8_test_acquire_buffer(); lp_buf; lp_buf = p8_test_acquire_buffer())
            {
                lo_held.push_back(lp_buf);
            }
            ls_name.replace(0, 8, "last____");
            lu_last = p8_trc_begin(ls_name.c_str(), 99, gp_file, gp_func, 0, nullptr);
            p8_trc_end(lu_last);
            p8_test_release_buffers(lo_held.data(), lo_held.size());
        });

    EXPECT_EQ(lu_last, 0u);
    EXPECT_EQ(p8_test_get_dropped_stats().mu_traces, 1u);

    // every begin record that went out refers to a descriptor in the service stream
    std::unordered_set<uint64_t> lo_hashes;
    for(const s_p8_trc_item_svc &lr_desc : collect_trc_descs(collect_service_entries()))
    {
        lo_hashes.insert(lr_desc.mu_hash);
    }
    auto lo_parsed = parse_captured_traces();
    ASSERT_FALSE(lo_parsed.mo_recs.empty());
    for(const s_trc_rec &lr_rec : lo_parsed.mo_recs)
    {
        if(P8_TRC_ITEM_END != lr_rec.mu_type)
        {
            EXPECT_EQ(lo_hashes.count(lr_rec.mu_hash), 1u) << "span " << lr_rec.mu_span_id << " has no descriptor";
        }
    }
}

TEST_F(c_trc_content_test, begin_waits_for_a_buffer_when_the_pool_is_empty)
{
    // without capture an acquire waits, and the worker drains the service buffers to the null sink
    p8_test_disable_buffer_capture();

    std::vector<uint8_t *> lo_held;
    for(uint8_t *lp_buf = p8_test_acquire_buffer(); lp_buf; lp_buf = p8_test_acquire_buffer())
    {
        lo_held.push_back(lp_buf);
    }
    ASSERT_FALSE(lo_held.empty());

    std::atomic<bool> lb_done { false };
    uint64_t          lu_id = 0;
    std::thread       lo_thread(
        [&]()
        {
            lu_id = begin_at("late", 10);
            p8_trc_end(lu_id);
            lb_done = true;
        });

    // hand out one buffer each time the thread waits for one
    const auto lo_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!lb_done && !lo_held.empty() && std::chrono::steady_clock::now() < lo_deadline)
    {
        if(p8_test_get_waiter_count() > 0)
        {
            p8_test_release_buffer(lo_held.back());
            lo_held.pop_back();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    p8_test_release_buffers(lo_held.data(), lo_held.size());
    lo_held.clear();
    lo_thread.join();

    // the descriptor waited for a buffer: neither it nor the span was dropped
    EXPECT_NE(lu_id, 0u);
    EXPECT_GE(p8_test_get_wait_arrivals(), 1u);
    const s_p8_drop_stats lo_drops = p8_test_get_dropped_stats();
    EXPECT_EQ(lo_drops.mu_traces, 0u);
    EXPECT_EQ(lo_drops.mu_svc, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// no core
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TEST(c_trc_no_core_test, begin_without_core_returns_zero)
{
    uint64_t lu_id = 1;
    run_in_thread(
        [&]()
        {
            lu_id = begin_at("a", 10);
            p8_trc_end(lu_id);
        });
    EXPECT_EQ(lu_id, 0u);
}
