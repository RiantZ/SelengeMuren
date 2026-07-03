#include "p8_sink_file.hpp"
#include "p8_config_keys.hpp"
#include "p8_protocol.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
/// @brief Unique per-run scratch directory under the OS temp location.
std::filesystem::path make_scratch_dir()
{
    const auto lo_root = std::filesystem::temp_directory_path()
                         / ("p8_sink_file_test_"
                            + std::to_string(static_cast<uint64_t>(::testing::UnitTest::GetInstance()->random_seed()))
                            + "_" + std::to_string(static_cast<uint64_t>(std::rand())));
    std::filesystem::create_directories(lo_root);
    return lo_root;
}

nlohmann::json make_config(const std::filesystem::path &ir_out_dir)
{
    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = ir_out_dir.string();
    return lo_json;
}

/// @brief The single file under ir_dir with extension ir_ext, or an empty path if none/multiple.
std::filesystem::path find_file_with_ext(const std::filesystem::path &ir_dir, const std::string &ir_ext)
{
    std::filesystem::path lo_found;
    size_t                lz_count = 0;
    for(const auto &lo_entry : std::filesystem::directory_iterator(ir_dir))
    {
        if(lo_entry.path().extension() == ir_ext)
        {
            lo_found = lo_entry.path();
            ++lz_count;
        }
    }
    return (lz_count == 1) ? lo_found : std::filesystem::path();
}

std::vector<uint8_t> read_file_bytes(const std::filesystem::path &ir_path)
{
    std::ifstream lo_in(ir_path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(lo_in)), std::istreambuf_iterator<char>());
}

/// @brief s_p8_data_buf_hdr + payload, with mu_size set to the total entry size.
std::vector<uint8_t> make_data_buffer(const std::vector<uint8_t> &ir_payload)
{
    std::vector<uint8_t> lo_buf(sizeof(s_p8_data_buf_hdr) + ir_payload.size());

    s_p8_data_buf_hdr lo_hdr = {};
    lo_hdr.mu_packet_type    = P8_PACKET_LOGS;
    lo_hdr.mu_size           = static_cast<uint16_t>(lo_buf.size());
    lo_hdr.mu_thread_id      = 42;

    std::memcpy(lo_buf.data(), &lo_hdr, sizeof(lo_hdr));
    if(!ir_payload.empty())
    {
        std::memcpy(lo_buf.data() + sizeof(lo_hdr), ir_payload.data(), ir_payload.size());
    }
    return lo_buf;
}

} // namespace

class c_p8_sink_file_test : public ::testing::Test
{
protected:
    std::filesystem::path mo_tmp_dir;

    void SetUp() override
    {
        mo_tmp_dir = make_scratch_dir();
    }

    void TearDown() override
    {
        std::error_code lo_ec;
        std::filesystem::remove_all(mo_tmp_dir, lo_ec);
    }
};

TEST_F(c_p8_sink_file_test, open_fails_when_file_bin_section_missing)
{
    nlohmann::json lo_json; // no "FileBin" key at all
    cp8_sink_file  lo_sink(lo_json);
    EXPECT_FALSE(lo_sink.open());
}

TEST_F(c_p8_sink_file_test, open_fails_when_out_dir_missing)
{
    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_FILE_BIN] = nlohmann::json::object();
    cp8_sink_file lo_sink(lo_json);
    EXPECT_FALSE(lo_sink.open());
}

TEST_F(c_p8_sink_file_test, open_fails_when_out_dir_empty)
{
    nlohmann::json lo_json;
    lo_json[P8_CFG_KEY_FILE_BIN][P8_CFG_KEY_FILE_OUT_DIR] = "";
    cp8_sink_file lo_sink(lo_json);
    EXPECT_FALSE(lo_sink.open());
}

TEST_F(c_p8_sink_file_test, open_creates_out_dir_and_two_files)
{
    const auto lo_out_dir = mo_tmp_dir / "nested" / "deeper";
    ASSERT_FALSE(std::filesystem::exists(lo_out_dir));

    cp8_sink_file lo_sink(make_config(lo_out_dir));
    ASSERT_TRUE(lo_sink.open());

    ASSERT_TRUE(std::filesystem::exists(lo_out_dir));
    EXPECT_FALSE(find_file_with_ext(lo_out_dir, ".p8svc").empty());
    EXPECT_FALSE(find_file_with_ext(lo_out_dir, ".p8dat").empty());

    lo_sink.close();
}

TEST_F(c_p8_sink_file_test, write_service_persists_exact_bytes)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    const std::vector<uint8_t> lo_payload = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t                    la_buf[8];
    std::memcpy(la_buf, lo_payload.data(), lo_payload.size());

    s_p8_svc_buf lo_entry;
    lo_entry.mp_buf  = la_buf;
    lo_entry.mz_used = lo_payload.size();

    kit::c_lst<s_p8_svc_buf> lo_bufs;
    lo_bufs.push_last(lo_entry);

    EXPECT_TRUE(lo_sink.write_service(lo_bufs));
    lo_sink.close();

    const auto lo_path = find_file_with_ext(mo_tmp_dir, ".p8svc");
    ASSERT_FALSE(lo_path.empty());
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), lo_payload.size());
    EXPECT_EQ(0, std::memcmp(lo_content.data(), lo_payload.data(), lo_payload.size()));

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), lo_payload.size());
    EXPECT_EQ(lo_sink.get_bytes_dropped(), 0u);
    EXPECT_EQ(lo_sink.get_write_error_count(), 0u);
#endif
}

TEST_F(c_p8_sink_file_test, write_data_persists_full_entry_including_header)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    const auto lo_entry_bytes = make_data_buffer({ 'h', 'e', 'l', 'l', 'o' });

    kit::c_lst<uint8_t *> lo_bufs;
    lo_bufs.push_last(const_cast<uint8_t *>(lo_entry_bytes.data()));

    EXPECT_TRUE(lo_sink.write_data(lo_bufs));
    lo_sink.close();

    const auto lo_path = find_file_with_ext(mo_tmp_dir, ".p8dat");
    ASSERT_FALSE(lo_path.empty());
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), lo_entry_bytes.size());
    EXPECT_EQ(0, std::memcmp(lo_content.data(), lo_entry_bytes.data(), lo_entry_bytes.size()));

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), lo_entry_bytes.size());
#endif
}

TEST_F(c_p8_sink_file_test, write_hello_persists_header_as_first_bytes_of_data_file)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    s_p8_hdr lo_hdr            = {};
    lo_hdr.mu_packet_type      = P8_PACKET_MAIN;
    lo_hdr.mu_protocol_version = P8_PROTOCOL_VERSION;
    lo_hdr.mu_process_id       = 4242;

    EXPECT_TRUE(lo_sink.write_hello(lo_hdr));

    const auto            lo_entry_bytes = make_data_buffer({ 'z' });
    kit::c_lst<uint8_t *> lo_bufs;
    lo_bufs.push_last(const_cast<uint8_t *>(lo_entry_bytes.data()));
    EXPECT_TRUE(lo_sink.write_data(lo_bufs));

    lo_sink.close();

    const auto lo_path = find_file_with_ext(mo_tmp_dir, ".p8dat");
    ASSERT_FALSE(lo_path.empty());
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), sizeof(lo_hdr) + lo_entry_bytes.size());
    EXPECT_EQ(0, std::memcmp(lo_content.data(), &lo_hdr, sizeof(lo_hdr)));
    EXPECT_EQ(0, std::memcmp(lo_content.data() + sizeof(lo_hdr), lo_entry_bytes.data(), lo_entry_bytes.size()));

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), sizeof(lo_hdr) + lo_entry_bytes.size());
    EXPECT_EQ(lo_sink.get_write_error_count(), 0u);
#endif
}

TEST_F(c_p8_sink_file_test, multiple_entries_in_one_write_data_call_preserve_order)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    const auto lo_e1 = make_data_buffer({ 'a', 'a' });
    const auto lo_e2 = make_data_buffer({ 'b', 'b', 'b' });
    const auto lo_e3 = make_data_buffer({ 'c' });

    kit::c_lst<uint8_t *> lo_bufs;
    lo_bufs.push_last(const_cast<uint8_t *>(lo_e1.data()));
    lo_bufs.push_last(const_cast<uint8_t *>(lo_e2.data()));
    lo_bufs.push_last(const_cast<uint8_t *>(lo_e3.data()));

    EXPECT_TRUE(lo_sink.write_data(lo_bufs));
    lo_sink.close();

    const auto lo_path    = find_file_with_ext(mo_tmp_dir, ".p8dat");
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), lo_e1.size() + lo_e2.size() + lo_e3.size());

    size_t lz_off = 0;
    EXPECT_EQ(0, std::memcmp(lo_content.data() + lz_off, lo_e1.data(), lo_e1.size()));
    lz_off += lo_e1.size();
    EXPECT_EQ(0, std::memcmp(lo_content.data() + lz_off, lo_e2.data(), lo_e2.size()));
    lz_off += lo_e2.size();
    EXPECT_EQ(0, std::memcmp(lo_content.data() + lz_off, lo_e3.data(), lo_e3.size()));

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), lo_e1.size() + lo_e2.size() + lo_e3.size());
    EXPECT_EQ(lo_sink.get_write_error_count(), 0u);
#endif
}

TEST_F(c_p8_sink_file_test, multiple_entries_in_one_write_service_call_preserve_order)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    uint8_t la_e1[3] = { 1, 2, 3 };
    uint8_t la_e2[2] = { 4, 5 };

    kit::c_lst<s_p8_svc_buf> lo_bufs;
    lo_bufs.push_last(s_p8_svc_buf { la_e1, sizeof(la_e1) });
    lo_bufs.push_last(s_p8_svc_buf { la_e2, sizeof(la_e2) });

    EXPECT_TRUE(lo_sink.write_service(lo_bufs));
    lo_sink.close();

    const auto lo_path    = find_file_with_ext(mo_tmp_dir, ".p8svc");
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), sizeof(la_e1) + sizeof(la_e2));
    EXPECT_EQ(0, std::memcmp(lo_content.data(), la_e1, sizeof(la_e1)));
    EXPECT_EQ(0, std::memcmp(lo_content.data() + sizeof(la_e1), la_e2, sizeof(la_e2)));
}

TEST_F(c_p8_sink_file_test, write_data_batch_larger_than_iov_max_is_fully_persisted)
{
    // exceeds the typical POSIX IOV_MAX (1024) by a comfortable margin, to
    // exercise the sink's internal chunking of the scatter/gather batch
    static constexpr size_t lz_entry_count = 2500;

    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    // reserve upfront so the outer vector never reallocates - the inner
    // vectors (and thus their data() pointers) stay put once inserted
    std::vector<std::vector<uint8_t>> lo_entries;
    lo_entries.reserve(lz_entry_count);

    size_t lz_total_size = 0;
    for(size_t lz_i = 0; lz_i < lz_entry_count; ++lz_i)
    {
        lo_entries.push_back(make_data_buffer({ static_cast<uint8_t>(lz_i % 256) }));
        lz_total_size += lo_entries.back().size();
    }

    kit::c_lst<uint8_t *> lo_bufs;
    for(auto &lo_entry : lo_entries)
    {
        lo_bufs.push_last(lo_entry.data());
    }

    EXPECT_TRUE(lo_sink.write_data(lo_bufs));
    lo_sink.close();

    const auto lo_path    = find_file_with_ext(mo_tmp_dir, ".p8dat");
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), lz_total_size);

    size_t lz_off = 0;
    for(const auto &lo_entry : lo_entries)
    {
        ASSERT_EQ(0, std::memcmp(lo_content.data() + lz_off, lo_entry.data(), lo_entry.size()));
        lz_off += lo_entry.size();
    }

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), lz_total_size);
    EXPECT_EQ(lo_sink.get_bytes_dropped(), 0u);
    EXPECT_EQ(lo_sink.get_write_error_count(), 0u);
#endif
}

TEST_F(c_p8_sink_file_test, multiple_write_data_calls_append)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    const auto lo_first  = make_data_buffer({ 'a', 'b', 'c' });
    const auto lo_second = make_data_buffer({ 'x', 'y' });

    {
        kit::c_lst<uint8_t *> lo_bufs;
        lo_bufs.push_last(const_cast<uint8_t *>(lo_first.data()));
        EXPECT_TRUE(lo_sink.write_data(lo_bufs));
    }
    lo_sink.flush();
    {
        kit::c_lst<uint8_t *> lo_bufs;
        lo_bufs.push_last(const_cast<uint8_t *>(lo_second.data()));
        EXPECT_TRUE(lo_sink.write_data(lo_bufs));
    }
    lo_sink.close();

    const auto lo_path    = find_file_with_ext(mo_tmp_dir, ".p8dat");
    const auto lo_content = read_file_bytes(lo_path);
    ASSERT_EQ(lo_content.size(), lo_first.size() + lo_second.size());
    EXPECT_EQ(0, std::memcmp(lo_content.data(), lo_first.data(), lo_first.size()));
    EXPECT_EQ(0, std::memcmp(lo_content.data() + lo_first.size(), lo_second.data(), lo_second.size()));
}

TEST_F(c_p8_sink_file_test, flush_before_open_does_not_crash)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    lo_sink.flush(); // not open yet - must be a safe no-op
}

TEST_F(c_p8_sink_file_test, close_is_idempotent)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());
    lo_sink.close();
    lo_sink.close(); // must not crash
}

TEST_F(c_p8_sink_file_test, write_before_open_counts_as_dropped)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    // deliberately not calling open()

    const auto            lo_entry = make_data_buffer({ '1', '2', '3' });
    kit::c_lst<uint8_t *> lo_bufs;
    lo_bufs.push_last(const_cast<uint8_t *>(lo_entry.data()));

    lo_sink.write_data(lo_bufs);

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), 0u);
    EXPECT_EQ(lo_sink.get_bytes_dropped(), lo_entry.size());
    EXPECT_EQ(lo_sink.get_write_error_count(), 1u);
#endif
}

TEST_F(c_p8_sink_file_test, write_after_close_counts_as_dropped)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());
    lo_sink.close();

    const auto            lo_entry = make_data_buffer({ '4', '5' });
    kit::c_lst<uint8_t *> lo_bufs;
    lo_bufs.push_last(const_cast<uint8_t *>(lo_entry.data()));

    lo_sink.write_data(lo_bufs);

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_dropped(), lo_entry.size());
    EXPECT_EQ(lo_sink.get_write_error_count(), 1u);
#endif
}

TEST_F(c_p8_sink_file_test, empty_buffer_lists_are_noop)
{
    cp8_sink_file lo_sink(make_config(mo_tmp_dir));
    ASSERT_TRUE(lo_sink.open());

    kit::c_lst<uint8_t *>    lo_data_bufs;
    kit::c_lst<s_p8_svc_buf> lo_svc_bufs;

    EXPECT_TRUE(lo_sink.write_data(lo_data_bufs));
    EXPECT_TRUE(lo_sink.write_service(lo_svc_bufs));

#ifdef P8_TESTING
    EXPECT_EQ(lo_sink.get_bytes_written(), 0u);
    EXPECT_EQ(lo_sink.get_bytes_dropped(), 0u);
    EXPECT_EQ(lo_sink.get_write_error_count(), 0u);
#endif
    lo_sink.close();
}
