#pragma once

#include "kit/ts_helpers.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
// #include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef GTX64
    #define P8_SIZE_OF_ARG(t) ((uint8_t)(((sizeof(t)) + 7u) & ~7u))
#else
    #define P8_SIZE_OF_ARG(t) ((uint8_t)(((sizeof(t)) + 3u) & ~3u))
#endif

#define P8_PACKET_MAX_SIZE      (1 << 16) // 64KB max page

// WARNING: THIS IS THE FIRST BYTE IN EVERY PACKET
#define P8_PACKET_MAIN          0 // hello/handshake packet carrying s_p8_hdr (first packet on a connection)
#define P8_PACKET_LOGS          1 // log records buffer (s_p8_data_buf_hdr + s_p8_log_item_dat stream)
#define P8_PACKET_TRACES        2 // distributed-trace spans buffer
#define P8_PACKET_METRICS       3 // metric samples buffer
#define P8_PACKET_SERVICE       4 // service descriptors buffer (s_p8_svc_hdr entries: logs, attrs, threads, ...)

#define P8_PROTOCOL_VERSION     1

#define P8_PROCESS_NAME_MAX_LEN 256
#define P8_HOST_NAME_MAX_LEN    256

// Max length (in bytes) of a serialized service-descriptor string (file,
// function, format, attribute name). Longer strings are truncated to this
// boundary so a single descriptor never bloats a service buffer.
#define P8_SVC_STR_MAX_LEN      1024

// Service-data type codes (svc_entry.mu_type) carried inside a P8_PACKET_SERVICE
// buffer. Each entry describes one immutable, pre-serialized descriptor that the
// library transmits once so the receiver can resolve the compact IDs referenced
// by the LOGS/TRACES/METRICS data packets.
#define P8_SVC_TYPE_UNK         0x00     // unknown / unspecified service entry
#define P8_SVC_TYPE_LOG_DESC    0x01     // log descriptor: hash, line, file, function, format, var-arg types
#define P8_SVC_TYPE_ATTR        0x02     // attribute descriptor: id, value type, name
#define P8_SVC_TYPE_THREAD      0x03     // thread descriptor: OS thread id, thread name
#define P8_SVC_TYPE_TRACE       0x04     // trace descriptor: trace id, parent id, line, file, function, args format
#define P8_SVC_TYPE_MTK         0x05     // metric descriptor: id, flags, name, description, unit, min/max, on-state
#define P8_SVC_TYPE_MODULE      0x06     // module descriptor: handle, verbosity, name

#define P8_DATA_FLAG_FRAGMENT   (1 << 0) // buffer tail data continues in the next buffer for the same thread
#define P8_DATA_FLAG_RESERVED_1 (1 << 1)
#define P8_DATA_FLAG_RESERVED_2 (1 << 2)
#define P8_DATA_FLAG_RESERVED_3 (1 << 3)
#define P8_DATA_FLAG_RESERVED_4 (1 << 4)
#define P8_DATA_FLAG_RESERVED_5 (1 << 5)
#define P8_DATA_FLAG_RESERVED_6 (1 << 6)
#define P8_DATA_FLAG_RESERVED_7 (1 << 7)

// serialized argument types
#define P8_ATTR_TYPE_STR        0x00 // utf-8 zero terminated string
#define P8_ATTR_TYPE_F64        0x01 // 64-bit float
#define P8_ATTR_TYPE_I64        0x02 // 64-bit signed integer
#define P8_ATTR_TYPE_U64        0x03 // 64-bit unsigned integer

// Type codes for log var-args (s_p8_log_varg.mu_type), identifying how each
// serialized argument value should be interpreted/decoded.
#define P8_LOG_ARG_TYPE_UNK     0x00 // unknown / unspecified type
#define P8_LOG_ARG_TYPE_CHAR    0x01 // 8-bit character
#define P8_LOG_ARG_TYPE_INT8    0x01 // 8-bit signed integer
#define P8_LOG_ARG_TYPE_CHAR16  0x02 // 16-bit (UTF-16) character
#define P8_LOG_ARG_TYPE_INT16   0x03 // 16-bit signed integer
#define P8_LOG_ARG_TYPE_INT32   0x04 // 32-bit signed integer
#define P8_LOG_ARG_TYPE_INT64   0x05 // 64-bit signed integer
#define P8_LOG_ARG_TYPE_DOUBLE  0x06 // double-precision float
#define P8_LOG_ARG_TYPE_PVOID   0x07 // pointer (void*)
#define P8_LOG_ARG_TYPE_USTR16  0x08 // UTF-16 string
#define P8_LOG_ARG_TYPE_STRA    0x09 // ANSI/8-bit string
#define P8_LOG_ARG_TYPE_USTR8   0x0A // UTF-8 string
#define P8_LOG_ARG_TYPE_USTR32  0x0B // UTF-32 string
#define P8_LOG_ARG_TYPE_CHAR32  0x0C // 32-bit (UTF-32) character
#define P8_LOG_ARG_TYPE_INTMAX  0x0D // intmax_t (largest signed integer)
#define P8_LOG_ARG_TYPE_LDOUBLE 0x0E // long double

    // Base hello message from P8, the first packet to be sent
    // WARNING: the header is always in little-endian!
    struct s_p8_hdr
    {
        uint8_t  mu_packet_type;      // P8_PACKET_MAIN
        uint8_t  mu_protocol_version; // P8_PROTOCOL_VERSION
        uint8_t  mu_is_big_endian;    // 1 if host is big-endian, 0 - 0 otherwise
        uint8_t  mu_padding;          // padding
        int32_t  mi_utc_sec_offset;   // utc offset in seconds
        uint32_t mu_size;             // alligned on 8 bytes!
        uint32_t mu_process_id;       // host process ID
        uint64_t mu_system_time;      // system time, 100-nanosecond intervals since January 1, 1601 (UTC)
        uint64_t mu_hires_tick;       // high resolution monotonic time value
        uint64_t mu_hires_freq_numer; // high resolution monotonic time frequency numerator
        uint64_t mu_hires_freq_denom; // high resolution monotonic time frequency denominator
        char     mp_process_name[P8_PROCESS_NAME_MAX_LEN];
        char     mp_host_name[P8_HOST_NAME_MAX_LEN];
        uint64_t mu_hash; // header control hash
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // SERVICE
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Header of every service entry inside a P8_PACKET_SERVICE buffer (4 bytes).
    struct s_p8_svc_hdr
    {
        uint8_t  mu_packet_type; // P8_PACKET_SERVICE
        uint8_t  mu_svc_type;    // P8_SVC_TYPE_XXX
        uint16_t mu_size;        // size including header
    };

    // Serialized representation of s_p8_attr_desc
    // Fixed-size header; variable-length data follows in the buffer.
    // Name (string utf8) include a NUL terminator.
    struct s_p8_attr_svc
    {
        s_p8_svc_hdr ms_hdr;  // service item header, 4
        int32_t      mi_id;   // ID of the attribute
        uint8_t      mu_type; // attribute type: P8_ATTR_TYPE_XXX
        //* Serialized name
        //* padding to align total size on 8 bytes boundary
    };

    // Serialized representation of s_p8_log_mod
    // Fixed-size header; variable-length data follows in the buffer.
    // Name (string utf8) include a NUL terminator.
    struct s_p8_log_mod_svc
    {
        s_p8_svc_hdr ms_hdr;  // service item header, 4
        uint16_t     mu_id;   // ID of the module
        uint8_t      mu_verb; // verbosity == e_p8_level
        //* Serialized name
        //* padding to align total size on 8 bytes boundary
    };

    // Serialized representation of s_p8_log_desc (the immutable log descriptor).
    // Fixed-size header; variable-length data follows in the buffer.
    // String lengths are byte counts and do NOT include a NUL terminator.
    struct s_p8_log_item_svc
    {
        s_p8_svc_hdr ms_hdr;          // service item header, 4
        uint32_t     mu_line;         // source line number
        uint64_t     mu_hash;         // log descriptor hash (key into descriptor tree)
        uint16_t     mu_format_len;   // format string length in bytes
        uint16_t     mu_file_len;     // file path string length in bytes
        uint16_t     mu_function_len; // function name string length in bytes
        uint8_t      mu_args_count;   // number of args (<= P8_LOG_MAX_ARGS)
        //* Serialized strings: [file][function][format]
        //* Serialized args: s_p8_log_varg x mu_args_count
        //* padding to align total size on 8 bytes boundary
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // DATA: Logs, Metric, Traces items
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Header of EVERY data buffer: logs, traces, metrics
    struct s_p8_data_buf_hdr
    {
        uint8_t  mu_packet_type; // P8_PACKET_LOGS or P8_PACKET_TRACES or P8_PACKET_METRICS
        uint8_t  mu_flags;       // P8_DATA_FLAG_XXX
        uint16_t mu_size;        // size in bytes
        uint32_t mu_thread_id;   // thread ID which emits data
        uint64_t mu_start_time;  // first timestamp
        uint64_t mu_stop_time;   // last timestamp
    };

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // LOGS
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct s_p8_log_varg
    {
        uint8_t mu_type; // var arg type: P8_LOG_ARG_TYPE_XXXXXX
        uint8_t mu_size; // for fixed-size args: value size in bytes (1, 2, 4, 8, sizeof(void*),
                         // sizeof(intmax_t), sizeof(long double)); for string args
                         // (STRA/USTR8/USTR16/USTR32): has-width flag (0 or 1)
    };

    // Per-occurrence log record emitted into a P8_PACKET_LOGS buffer. References its
    // immutable descriptor (s_p8_log_item_svc) by mu_hash and carries the runtime data:
    // timestamp, thread/processor, level, serialized var-args and attributes.
    struct s_p8_log_item_dat
    {
        uint64_t mu_hash;      // log descriptor hash (key into descriptor tree)
        uint64_t mu_timestamp; // monotonic clock timestamp
        uint64_t mu_trace_id;  // distributed trace correlation ID

        // 64 bits
        uint32_t mu_thread_id; // OS thread ID
        uint8_t  mu_level;     // e_p8_level
        uint8_t  mu_processor; // CPU core ID
        uint16_t mu_args_size; // serialized variable arguments size in bytes

        // 32 bits
        uint16_t mu_size;   // total item size in bytes (header + args + attrs + padding)
        uint16_t mu_mod_id; // module ID

        // 16 bits
        uint8_t mu_attrs_count; // number of serialized attributes
        uint8_t mu_flags;       // todo
        //* Serialized Variable agruments [int32][int64][double][length in bytes (16b) + string data] ...
        //* Serialized attributes [p8_attr_id + data][...]
        //* padding to tail of the buffer to 8 bytes boundary, to be sure that following buffer will start from 8b
        // alignement
    };

#ifdef __cplusplus
}
#endif
