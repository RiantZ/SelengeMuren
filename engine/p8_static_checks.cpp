#include "p8_client_api.h"
#include "p8_protocol.h"

#include <cstddef>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Packet-type discriminator contract.  Every structure that can appear as the first bytes of a packet starts with a
// 1-byte mu_packet_type (one of P8_PACKET_XXX), so the receiver can dispatch on a single leading byte regardless of
// which packet struct follows.  This macro freezes that invariant at compile time: mu_packet_type must live at
// offset 0 and be exactly one byte.  Any reorder, type change, or inserted field ahead of it breaks the build.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define P8_ASSERT_PACKET_TYPE_FIRST(T)                                                                                \
    static_assert(offsetof(struct T, mu_packet_type) == 0, #T ": mu_packet_type must be the first byte");             \
    static_assert(sizeof(((struct T *)0)->mu_packet_type) == 1, #T ": mu_packet_type must be exactly 1 byte")

P8_ASSERT_PACKET_TYPE_FIRST(s_p8_hdr);
P8_ASSERT_PACKET_TYPE_FIRST(s_p8_svc_hdr);
P8_ASSERT_PACKET_TYPE_FIRST(s_p8_data_buf_hdr);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Protocol structures must be 8-byte aligned so that uint64_t members can be accessed without unaligned loads when
// items are packed sequentially in shared-memory buffers.  A misaligned 64-bit read is either a fault (strict-align
// architectures) or a silent performance penalty (x86), so we enforce the invariant at compile time.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static_assert(sizeof(struct s_p8_hdr) % 8 == 0, "s_p8_hdr size must be a multiple of 8 bytes");
static_assert(sizeof(struct s_p8_data_buf_hdr) % 8 == 0, "s_p8_data_buf_hdr size must be a multiple of 8 bytes");
static_assert(sizeof(struct s_p8_log_item_dat) % 8 == 0, "s_p8_log_item_hdr size must be a multiple of 8 bytes");

static_assert(alignof(struct s_p8_hdr) >= 8, "s_p8_hdr must have at least 8-byte alignment");
static_assert(alignof(struct s_p8_data_buf_hdr) >= 8, "s_p8_data_buf_hdr must have at least 8-byte alignment");
static_assert(alignof(struct s_p8_log_item_dat) >= 8, "s_p8_log_item_hdr must have at least 8-byte alignment");

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Wire-format layout contract.  These structures are written to / read from shared memory and transmitted over the
// network between hosts that may build this header with different compilers (MSVC / GCC / Clang) and bitness (the
// alignment of uint64_t is 4 on the i386 System V ABI but 8 elsewhere).  The exact size and field offsets below are
// the binary contract: any field reorder, insert, remove or type change that alters the on-wire layout must break the
// build here, not silently corrupt data on the receiving end.  Endianness is handled separately (header is always
// little-endian); these asserts only freeze the byte layout.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// s_p8_hdr
static_assert(sizeof(struct s_p8_hdr) == 568, "s_p8_hdr wire size changed");
static_assert(offsetof(struct s_p8_hdr, mu_packet_type) == 0, "s_p8_hdr::mu_packet_type offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_protocol_version) == 1, "s_p8_hdr::mu_protocol_version offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_is_big_endian) == 2, "s_p8_hdr::mu_is_big_endian offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_padding) == 3, "s_p8_hdr::mu_padding offset changed");
static_assert(offsetof(struct s_p8_hdr, mi_utc_sec_offset) == 4, "s_p8_hdr::mi_utc_sec_offset offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_size) == 8, "s_p8_hdr::mu_size offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_process_id) == 12, "s_p8_hdr::mu_process_id offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_system_time) == 16, "s_p8_hdr::mu_system_time offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_hires_tick) == 24, "s_p8_hdr::mu_hires_tick offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_hires_freq_numer) == 32, "s_p8_hdr::mu_hires_freq_numer offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_hires_freq_denom) == 40, "s_p8_hdr::mu_hires_freq_denom offset changed");
static_assert(offsetof(struct s_p8_hdr, mp_process_name) == 48, "s_p8_hdr::mp_process_name offset changed");
static_assert(offsetof(struct s_p8_hdr, mp_host_name) == 304, "s_p8_hdr::mp_host_name offset changed");
static_assert(offsetof(struct s_p8_hdr, mu_hash) == 560, "s_p8_hdr::mu_hash offset changed");

// s_p8_svc_hdr
static_assert(sizeof(struct s_p8_svc_hdr) == 4, "s_p8_svc_hdr wire size changed");
static_assert(offsetof(struct s_p8_svc_hdr, mu_packet_type) == 0, "s_p8_svc_hdr::mu_packet_type offset changed");
static_assert(offsetof(struct s_p8_svc_hdr, mu_type) == 1, "s_p8_svc_hdr::mu_type offset changed");
static_assert(offsetof(struct s_p8_svc_hdr, mu_size) == 2, "s_p8_svc_hdr::mu_size offset changed");

// s_p8_data_buf_hdr
static_assert(sizeof(struct s_p8_data_buf_hdr) == 24, "s_p8_data_buf_hdr wire size changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_packet_type) == 0,
              "s_p8_data_buf_hdr::mu_packet_type offset changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_flags) == 1, "s_p8_data_buf_hdr::mu_flags offset changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_size) == 2, "s_p8_data_buf_hdr::mu_size offset changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_thread_id) == 4, "s_p8_data_buf_hdr::mu_thread_id offset changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_start_time) == 8,
              "s_p8_data_buf_hdr::mu_start_time offset changed");
static_assert(offsetof(struct s_p8_data_buf_hdr, mu_stop_time) == 16,
              "s_p8_data_buf_hdr::mu_stop_time offset changed");

// s_p8_log_varg
static_assert(sizeof(struct s_p8_log_varg) == 2, "s_p8_log_varg wire size changed");
static_assert(offsetof(struct s_p8_log_varg, mu_type) == 0, "s_p8_log_varg::mu_type offset changed");
static_assert(offsetof(struct s_p8_log_varg, mu_size) == 1, "s_p8_log_varg::mu_size offset changed");

// s_p8_log_item_svc
static_assert(sizeof(struct s_p8_log_item_svc) == 24, "s_p8_log_item_svc wire size changed");
static_assert(offsetof(struct s_p8_log_item_svc, ms_hdr) == 0, "s_p8_log_item_svc::ms_hdr offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_line) == 4, "s_p8_log_item_svc::mu_line offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_hash) == 8, "s_p8_log_item_svc::mu_hash offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_format_len) == 16,
              "s_p8_log_item_svc::mu_format_len offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_file_len) == 18, "s_p8_log_item_svc::mu_file_len offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_function_len) == 20,
              "s_p8_log_item_svc::mu_function_len offset changed");
static_assert(offsetof(struct s_p8_log_item_svc, mu_args_count) == 22,
              "s_p8_log_item_svc::mu_args_count offset changed");

// s_p8_log_item_dat
static_assert(sizeof(struct s_p8_log_item_dat) == 40, "s_p8_log_item_dat wire size changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_hash) == 0, "s_p8_log_item_dat::mu_hash offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_timestamp) == 8, "s_p8_log_item_dat::mu_timestamp offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_trace_id) == 16, "s_p8_log_item_dat::mu_trace_id offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_thread_id) == 24,
              "s_p8_log_item_dat::mu_thread_id offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_level) == 28, "s_p8_log_item_dat::mu_level offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_processor) == 29,
              "s_p8_log_item_dat::mu_processor offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_args_size) == 30,
              "s_p8_log_item_dat::mu_args_size offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_size) == 32, "s_p8_log_item_dat::mu_size offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_attrs_count) == 34,
              "s_p8_log_item_dat::mu_attrs_count offset changed");
static_assert(offsetof(struct s_p8_log_item_dat, mu_flags) == 35, "s_p8_log_item_dat::mu_flags offset changed");

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Attribute value type contract.  The public enum e_p8_attr_type (p8_client_api.h) is the source of truth the caller
// sees; the wire codes P8_ATTR_TYPE_XXX (p8_protocol.h) are what gets serialized into s_p8_attr_svc::mu_type.  The two
// must stay numerically identical so a value cast between them on either side of the wire keeps its meaning.  Any
// drift (reorder, insert, value change) must break the build here instead of silently mislabeling attribute values.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static_assert(e_p8_attr_str == P8_ATTR_TYPE_STR, "e_p8_attr_str must match P8_ATTR_TYPE_STR");
static_assert(e_p8_attr_f64 == P8_ATTR_TYPE_F64, "e_p8_attr_f64 must match P8_ATTR_TYPE_F64");
static_assert(e_p8_attr_i64 == P8_ATTR_TYPE_I64, "e_p8_attr_i64 must match P8_ATTR_TYPE_I64");
static_assert(e_p8_attr_u64 == P8_ATTR_TYPE_U64, "e_p8_attr_u64 must match P8_ATTR_TYPE_U64");
