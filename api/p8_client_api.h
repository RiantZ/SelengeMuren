#pragma once

#include "kit/ts_helpers.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief callback function type to get high-resolution system timer frequency
    /// @param ip_ctx [in] calling software context
    /// @return frequency in hertz
    typedef uint64_t (*l_p8_get_timer_frequency)(void *ip_ctx);

    /// @brief callback function type to get high-resolution system timer value
    /// @param ip_ctx [in] calling software context
    /// @return timer value
    /// WARNING: it is not recommended to take ANY LOCK inside function, performance penalties will be significant!
    typedef uint64_t (*l_p8_get_timer_value)(void *ip_ctx);

    /// @brief P8 configuration structure
    struct s_p8_config
    {
        const char *mp_json_config; //! Json config data (content), example: "examples/config.json"

        //! Fields provides the way to reimplement internal mechanism of timestamping, by default it is reccommended to
        //! provide nullptr as arguments. But if you want to syncronize different streams and clocks - it might be
        //! used.
        void *mp_ctx_timer;                      //! Context for timer functions, nullptr accepted
        l_p8_get_timer_frequency
                             ml_timer_frequency; //! callback to get system high-resolution timer frequency, or nullptr
        l_p8_get_timer_value ml_timer_value;     //! callback to get system high-resolution timer value or nullptr
    };

    /// @brief P8 initialization function, should be called once at CPU startup
    /// @param ip_config [in] configuration
    /// @return true - P8 has been initialized, false - failure
    bool p8_initialize(const struct s_p8_config *ip_config);

    /// @brief Check whether P8 has been successfully initialized
    /// @return true - P8 is initialized, false - not initialized
    bool p8_get_initialized();

    /// @brief Release the global P8 core instance.
    ///
    /// Decrements the internal reference count on the core. The core is not
    /// destroyed immediately if per-thread objects (log, trace, metrics) still
    /// hold references — it stays alive until the last thread-local destructor
    /// releases its reference, at which point the core is deleted
    /// automatically. Shared memory is cleaned up here so a subsequent
    /// p8_initialize() can start fresh.
    ///
    /// Safe to call when P8 is not initialized (no-op).
    /// After this call p8_get_initialized() returns false.
    void p8_release();

    /// @brief Function allows to flush (deliver) not  delivered/saved  P8  buffers  for  all opened clients and
    /// related channels owned by process in CASE OF your app/proc. crash. This function do not call system  memory
    /// allocation functions  only  writes to file/socket. Classical scenario: your application has been crushed, you
    /// catch the moment of crush and call this function once. Has to be used if integrator implements own crash
    /// handling mechanism. N.B.: DO NOT USE OTHER P8 FUNCTION AFTER CALLING OF THIS FUNCTION
    void p8_exceptional_flush();

    /// @brief function used to specify name for current thread, allows to have nice log/trace message formatting on
    /// Baical server. Call the function from the newly created thread & call p8_trc_unregister_current_thread() right
    /// before thread destroying
    /// @param ip_name [in] thread name
    /// @return true - successful registration, false - failure
    bool p8_register_current_thread(const char *ip_name);

    /// @brief function used to unregister thread name (called on thread exit)
    void p8_unregister_current_thread();

#ifdef __cplusplus
} // extern "C"

/// @brief RAII guard that registers the current thread name on construction and unregisters it on
/// destruction. Designed for zero overhead beyond the two underlying C calls — no virtual
/// dispatch, no heap allocation, no exceptions.
class cp8_thread final
{
public:
    explicit cp8_thread(const char *ip_name) noexcept
    {
        p8_register_current_thread(ip_name);
    }

    ~cp8_thread() noexcept
    {
        p8_unregister_current_thread();
    }

    cp8_thread(const cp8_thread &)            = delete;
    cp8_thread &operator=(const cp8_thread &) = delete;
    cp8_thread(cp8_thread &&)                 = delete;
    cp8_thread &operator=(cp8_thread &&)      = delete;
};

extern "C"
{
#endif // __cplusplus

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //                                         P8::Attributes //
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    /// @brief Supported attribute value types.
    enum e_p8_attr_type
    {
        e_p8_attr_str = 0, ///< Null-terminated UTF-8 string (const char *)
        e_p8_attr_f64,     ///< IEEE 754 double-precision float
        e_p8_attr_i64,     ///< Signed 64-bit integer
        e_p8_attr_u64,     ///< Unsigned 64-bit integer
    };

    /// @brief Attribute ID returned by p8_attr_register.
    ///
    /// Non-negative values (>= 0) are valid attribute handles.
    /// Negative values indicate an error (registration failure or not found).
    typedef int32_t p8_attr_id;

/// @brief Sentinel value for an invalid / unregistered attribute.
#define P8_IS_ATTR_VALID(x)           (((p8_attr_id)(x)) >= 0)

#define P8_ATTR_ERROR_NOT_INITIALIZED -1
#define P8_ATTR_ERROR_INVALID_NAME    -2
#define P8_ATTR_ERROR_TYPE_MISMATCH   -3
#define P8_ATTR_ERROR_ALLOC_FAILED    -4
#define P8_ATTR_ERROR_NOT_FOUND       -5

    /// @brief Register a named attribute with a fixed value type.
    ///
    /// The name must be unique, non-empty, null-terminated UTF-8.
    /// Once registered, the attribute keeps its type for the process lifetime.
    /// Re-registering the same name + type returns the existing ID.
    /// Re-registering with a different type returns a negative error code.
    ///
    /// @param ip_name [in] attribute name, e.g. "user_id". Must not be nullptr.
    /// @param ie_type [in] value type that will accompany every record
    /// @return non-negative ID on success, negative error code on failure
    p8_attr_id p8_attr_register(const char *ip_name, enum e_p8_attr_type ie_type);

    /// @brief Look up a previously registered attribute by name.
    ///
    /// @param ip_name [in] attribute name, e.g. "user_id". Must not be nullptr.
    /// @return non-negative attribute ID if found, P8_ATTR_ERROR_NOT_FOUND if not registered
    p8_attr_id p8_attr_get(const char *ip_name);

    /// @brief A single attribute value bound to a registered attribute ID.
    ///
    /// Construct instances with p8_av_str / p8_av_f64 / p8_av_i64 / p8_av_u64.
    /// The active union member must match the type declared at registration;
    /// a mismatch is detected at runtime and the record is dropped.
    struct s_p8_attr_val
    {
        p8_attr_id m_id; ///< ID obtained from p8_attr_register.
        union
        {
            const char *mp_str; ///< Valid when registered as e_p8_attr_str.
            double      md_f64; ///< Valid when registered as e_p8_attr_f64.
            int64_t     mi_i64; ///< Valid when registered as e_p8_attr_i64.
            uint64_t    mu_u64; ///< Valid when registered as e_p8_attr_u64.
        };
    };

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //                                         P8::Logging structures & functions //
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    /// @brief Enum describes different verbosity & logs levels
    enum e_p8_level
    {
        e_p8_trace0 = 0, //! minimum verbosity level, maximum information to be transmitted
        e_p8_trace1,     //!
        e_p8_trace2,     //!
        e_p8_trace3,     //!
        e_p8_debug0,     //! debug messages, info, up to critical
        e_p8_debug1,     //!
        e_p8_debug2,     //!
        e_p8_debug3,     //!
        e_p8_info0,      //! info messages, warning, up to critical
        e_p8_info1,      //!
        e_p8_info2,      //!
        e_p8_info3,      //!
        e_p8_warning0,   //! warning messages, error, up to critical
        e_p8_warning1,   //!
        e_p8_warning2,   //!
        e_p8_warning3,   //!
        e_p8_error0,     //! only error & critical messages
        e_p8_error1,     //!
        e_p8_error2,     //!
        e_p8_error3,     //!
        e_p8_critical0,  //! only critical messages
        e_p8_critical1,  //!
        e_p8_critical2,  //!
        e_p8_critical3,  //!

        e_p8_levels_count
    };

    /// @brief P8 Log module type, used to create different modules for logging. Each module has it's own name &
    /// verbosity
    typedef void *p_p8_module;

/// @brief P8 invalid module ID value
#ifdef __cplusplus
    #define P8_MODULE_INVALID_ID nullptr
#else
    #define P8_MODULE_INVALID_ID NULL
#endif

    // Trace & ID to have parent,
    // Logs + trace ID + attributes + more levels

#define P8TRC0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_trace0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define P8DBG0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_debug0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define P8INF0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_info0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define P8WRN0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_warning0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define P8ERR0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_error0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)
#define P8CRT0(ip_module, ip_format_str, ...)                                                                         \
    p8_log_sent(e_p8_critical0, ip_module, (uint32_t)__LINE__, __FILE__, __FUNCTION__, ##__VA_ARGS__)

    /// @brief set module log verbosity, logs with less priority will be rejected. You may set verbosity online from
    /// Baical server. See documentation for details.
    /// @param ip_module [in] module, if nullptr - default verbosity will be set for entire p8
    /// @param ie_verbosity [in] verbosity level
    void p8_log_set_verbosity(p_p8_module ip_module, enum e_p8_level ie_verbosity);

    /// @brief get current log verbosity level for a module
    /// @param ip_module [in] module handle, if nullptr - returns default verbosity for entire p8
    /// @return current verbosity level
    enum e_p8_level p8_log_get_verbosity(p_p8_module ip_module);

    /// @brief function is used to register log module. Modules are used to group log messages by modules, use the same
    ///       verbosity level per module and to have nice formatting on Baical side - each log will be marked with
    ///       module name. If module with such name is already registered - existing handle will be returned
    /// @param ip_name [in] module name
    /// @param ie_verbosity [in] module verbosity
    /// @param op_module [out] module handle
    /// @return true - successful registration, false - failure
    bool p8_log_register_module(const char *ip_name, enum e_p8_level ie_verbosity, p_p8_module *op_module);

    /// @brief function is used to find log module by name. Modules are used to group log messages by modules, use the
    /// same verbosity level per module and to have nice formatting on Baical side - each log will be marked with
    /// module
    ///       name. If module with such name is already registered - existing handle will be returned
    /// @param ip_name [in] module name
    /// @return module handle
    p_p8_module p8_log_find_module(const char *ip_name);

    /// @brief Function for internal usage, please use macros P8TRC, P8DBG, etc. instead.
    /// @param ie_level [in] log level
    /// @param ip_module [in] module
    /// @param iu_trace_id [in] distributed trace identifier that correlates this log record; pass 0 when
    ///                         the log is not associated with any trace
    /// @param iu_line [in] source code line index
    /// @param ip_file [in] source code file name
    /// @param ip_function [in] source code function name
    /// @param iz_attrs [in] number of elements in ip_attrs array; pass 0 when no attributes are attached
    /// @param ip_attrs [in] pointer to array of s_p8_attr_val of length iz_attrs; each element must carry
    ///                      an ID obtained from p8_attr_register and a value matching the registered type
    /// @param ip_format [in] format string, for format specification please refer to documentation, all standard
    /// format options are supported, and in addition few custom
    /// @param ... [in] variable arguments list
    /// @return true - success, false - failure
    // log("My log %d, %s", 10, "my")
    bool p8_log_sent(enum e_p8_level             ie_level,
                     p_p8_module                 ip_module,
                     uint64_t                    iu_trace_id,
                     uint32_t                    iu_line,
                     const char                 *ip_file,
                     const char                 *ip_function,
                     size_t                      iz_attrs,
                     const struct s_p8_attr_val *ip_attrs,
                     const char                 *ip_format,
                     ...);

    /// @brief Function for embedding into another logger
    /// @param ie_level [in] log level
    /// @param ip_module [in] module
    /// @param iu_trace_id [in] distributed trace identifier that correlates this log record; pass 0 when
    ///                         the log is not associated with any trace
    /// @param iu_line [in] source code line index
    /// @param ip_file [in] source code file name
    /// @param ip_function [in] source code function name
    /// @param iz_attrs [in] number of elements in ip_attrs array; pass 0 when no attributes are attached
    /// @param ip_attrs [in] pointer to array of s_p8_attr_val of length iz_attrs; each element must carry
    ///                      an ID obtained from p8_attr_register and a value matching the registered type
    /// @param ip_format [in] format string, for format specification please refer to documentation, all
    /// standard format options are supported, and in addition few custom
    /// @param ip_va_list [in] variable arguments list
    /// @return true - success, false - failure
    bool p8_log_sent_emb(enum e_p8_level             ie_level,
                         p_p8_module                 ip_module,
                         uint64_t                    iu_trace_id,
                         uint32_t                    iu_line,
                         const char                 *ip_file,
                         const char                 *ip_function,
                         size_t                      iz_attrs,
                         const struct s_p8_attr_val *ip_attrs,
                         const char                 *ip_format,
                         va_list                    *ip_va_list);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//                                         P8::Trace structures & functions                                      //
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// TODO: add condition MACRO to switch off traces

/// @brief Start a trace
#define P8TRC_BEGIN(iu_parent_id, iz_attrs, ip_attrs, ip_fmt, ...)                                                    \
    p8_trc_begin((iu_parent_id),                                                                                      \
                 (uint32_t)__LINE__,                                                                                  \
                 __FILE__,                                                                                            \
                 __FUNCTION__,                                                                                        \
                 (iz_attrs),                                                                                          \
                 (ip_attrs),                                                                                          \
                 (ip_fmt),                                                                                            \
                 ##__VA_ARGS__)

/// @brief End a trace by its trace ID.
#define P8TRC_END(iu_trace_id) p8_trc_end(iu_trace_id)

    /// @brief Start a trace span. For internal usage — prefer P8TRC_BEGIN / P8TRC_SCOPE macros.
    /// @param iu_parent_trace_id [in] parent span ID for nested traces; pass 0 for a root span
    /// @param iu_line            [in] source code line index
    /// @param ip_file            [in] source code file name
    /// @param ip_function        [in] source code function name
    /// @param iz_attrs           [in] number of elements in ip_attrs; pass 0 when no attributes
    /// @param ip_attrs           [in] array of s_p8_attr_val of length iz_attrs; may be nullptr when iz_attrs == 0
    /// @param ip_function_args   [in] printf-style format string describing call arguments; may be nullptr
    /// @param ...                [in] arguments for ip_function_args format string
    /// @return assigned trace ID (non-zero on success, 0 on failure)
    uint64_t p8_trc_begin(uint64_t                    iu_parent_trace_id,
                          uint32_t                    iu_line,
                          const char                 *ip_file,
                          const char                 *ip_function,
                          size_t                      iz_attrs,
                          const struct s_p8_attr_val *ip_attrs,
                          const char                 *ip_function_args,
                          ...);

    /// @brief Start a trace span, designed to be embedded into existing function with variable arguments
    /// @param iu_parent_trace_id [in] parent span ID for nested traces; pass 0 for a root span
    /// @param iu_line            [in] source code line index
    /// @param ip_file            [in] source code file name
    /// @param ip_function        [in] source code function name
    /// @param iz_attrs           [in] number of elements in ip_attrs; pass 0 when no attributes
    /// @param ip_attrs           [in] array of s_p8_attr_val of length iz_attrs; may be nullptr when iz_attrs == 0
    /// @param ip_function_args   [in] printf-style format string describing call arguments; may be nullptr
    /// @param ip_va_list         [in] variable arguments list
    /// @return assigned trace ID (non-zero on success, 0 on failure)
    uint64_t p8_trc_begin_emb(uint64_t                    iu_parent_trace_id,
                              uint32_t                    iu_line,
                              const char                 *ip_file,
                              const char                 *ip_function,
                              size_t                      iz_attrs,
                              const struct s_p8_attr_val *ip_attrs,
                              const char                 *ip_function_args,
                              va_list                    *ip_va_list);

    /// @brief End a trace span. For internal usage — prefer P8TRC_SCOPE macro (RAII) or P8TRC_END.
    /// @param iu_trace_id [in] trace ID returned by p8_trc_begin; passing 0 is a no-op
    /// @return true - success, false - failure
    bool p8_trc_end(uint64_t iu_trace_id);

#ifdef __cplusplus
} // extern "C"

    /// @brief Declare a named cp8_trace_guard with structured attributes.
    #define P8TRC_SCOPE(var_name, iu_parent_id, iz_attrs, ip_attrs, ip_fmt, ...)                                      \
        cp8_trace var_name((iu_parent_id),                                                                            \
                           (uint32_t)__LINE__,                                                                        \
                           __FILE__,                                                                                  \
                           __FUNCTION__,                                                                              \
                           (iz_attrs),                                                                                \
                           (ip_attrs),                                                                                \
                           (ip_fmt),                                                                                  \
                           ##__VA_ARGS__)

    /// @brief Declare a named cp8_trace_guard with structured attributes.
    #define P8TRC(var_name, iu_parent_id, iz_attrs, ip_attrs)                                                         \
        cp8_trace var_name((iu_parent_id), (uint32_t)__LINE__, __FILE__, __FUNCTION__, (iz_attrs), (ip_attrs), )

/// @brief RAII guard that calls p8_trc_begin on construction and p8_trc_end on destruction.
/// Stores the assigned trace ID so it can be forwarded to p8_log_sent for span correlation.
/// Designed for zero overhead beyond the two underlying C calls — no virtual dispatch,
/// no heap allocation, no exceptions.
class cp8_trace final
{
public:
    explicit cp8_trace(uint64_t                    iu_parent_trace_id,
                       uint32_t                    iu_line,
                       const char                 *ip_file,
                       const char                 *ip_function,
                       size_t                      iz_attrs         = 0,
                       const struct s_p8_attr_val *ip_attrs         = nullptr,
                       const char                 *ip_function_args = nullptr) noexcept
        : mu_trace_id(
              p8_trc_begin(iu_parent_trace_id, iu_line, ip_file, ip_function, iz_attrs, ip_attrs, ip_function_args))
    {
    }

    ~cp8_trace() noexcept
    {
        p8_trc_end(mu_trace_id);
    }

    /// Returns the trace ID assigned by p8_trc_begin; pass it to p8_log_sent / P8INF0 etc.
    uint64_t get_trace_id() const noexcept
    {
        return mu_trace_id;
    }

    cp8_trace(const cp8_trace &)            = delete;
    cp8_trace &operator=(const cp8_trace &) = delete;
    cp8_trace(cp8_trace &&)                 = delete;
    cp8_trace &operator=(cp8_trace &&)      = delete;

private:
    uint64_t mu_trace_id;
};

extern "C"
{
#endif // __cplusplus

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //                                         P8::Metrics structures & functions //
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    /// @brief P8 Metric ID type
    typedef int32_t h_p8_mtk_id;

    /// @brief P8 Metric group ID type
    typedef int16_t h_p8_mtk_group_id;

/// @brief Check for metric ID validity
#define P8_IS_METRIC_VALID(id)       ((id) >= 0)

/// @brief Error codes returned (as a negative h_p8_mtk_id) by the p8_mtk_* create functions.
#define P8_MTK_ERROR_NOT_INITIALIZED -1 ///< p8 is not initialized
#define P8_MTK_ERROR_INVALID_NAME    -2 ///< metric name is null or empty
#define P8_MTK_ERROR_ALLOC_FAILED    -3 ///< descriptor allocation failed
#define P8_MTK_ERROR_CONFLICT        -4 ///< a metric with this name already exists with a different definition

    /// @brief Callback type for query-based metric: P8 invokes this periodically to obtain the current metric value
    /// @param ip_user_context [in] opaque user-defined context passed during metric creation
    /// @param ii_id           [in] metric ID being queried
    /// @return current metric value
    typedef double (*l_p8_mtk_query_cb)(void *ip_user_context, h_p8_mtk_id ii_id);

    /// @brief Callback type for group query, grouped metrics share a common time base and are emitted together
    ///        via p8_mtk_group_emit_begin / p8_mtk_group_emit / p8_mtk_group_emit_end sequence
    /// @param ip_user_context [in] opaque user-defined context passed during group creation
    /// @param ii_group_id     [in] group ID being queried
    typedef void (*l_p8_mtk_group_query_cb)(void *ip_user_context, h_p8_mtk_group_id ii_group_id);

    /// @brief Base descriptor used to create any metric (single or group, push or query)
    struct s_p8_mtk_base
    {
        const char                 *mp_name;        ///< metric display name (must not be nullptr)
        const char                 *mp_description; ///< human-readable description (may be nullptr)
        const char                 *mp_unit;        ///< measurement unit label, e.g. "ms", "bytes" (may be nullptr)
        bool                        mb_on;    ///< initial enabled state: true - metric is active, false - disabled
        double                      md_min;   ///< expected minimum value (used for visualization hints)
        double                      md_max;   ///< expected maximum value (used for visualization hints)
        size_t                      mz_attrs; ///< number of elements in ip_attrs array; 0 when no attributes
        const struct s_p8_attr_val *mp_attrs; ///< array of attributes of length iz_attrs; nullptr when iz_attrs == 0
    };

    /// @brief Create a push-based metric. Caller is responsible for emitting values via p8_mtk_emit.
    ///        Registration is idempotent by name: if a metric with the same name and an identical
    ///        definition (description, unit, min, max, on-state) already exists, its id is returned;
    ///        a same-name metric with a different definition fails with P8_MTK_ERROR_CONFLICT.
    /// @param ip_base [in] metric descriptor
    /// @return positive metric ID on success, negative P8_MTK_ERROR_* code on failure
    h_p8_mtk_id p8_mtk_create(const struct s_p8_mtk_base *ip_base);

    /// @brief Emit (push) a sample value for a previously created metric
    /// @param ih_id    [in] metric ID returned by p8_mtk_create
    /// @param id_value [in] sample value
    /// @return true - success, false - failure
    bool p8_mtk_emit(h_p8_mtk_id ih_id, double id_value);

    /// @brief Create a query-based (pull) metric. P8 will periodically invoke il_query callback at the specified
    ///        interval to obtain the current value.
    /// @param ip_base               [in] metric descriptor
    /// @param iu_query_interval_ms  [in] query period in milliseconds
    /// @param il_query              [in] callback invoked by P8 to obtain the metric value
    /// @param ip_user_context       [in] opaque pointer forwarded to il_query on each invocation
    /// @return positive metric ID on success, negative value on failure
    h_p8_mtk_id p8_mtk_create_query(const struct s_p8_mtk_base *ip_base,
                                    uint32_t                    iu_query_interval_ms,
                                    l_p8_mtk_query_cb           il_query,
                                    void                       *ip_user_context);

    /// @brief Create a push-based metric group. Grouped metrics share a common time base and are emitted together
    ///        via p8_mtk_group_emit_begin / p8_mtk_group_emit / p8_mtk_group_emit_end sequence.
    /// @param ip_base          [in] group descriptor (mp_name becomes the group name)
    /// @param ib_multi_thread  [in] true if emit calls for p8_mtk_group_emit_begin / p8_mtk_group_emit /
    /// p8_mtk_group_emit_end may come from different threads (enables internal locking)
    /// @return positive group ID on success, negative value on failure
    h_p8_mtk_group_id p8_mtk_create_group(const struct s_p8_mtk_base *ip_base, bool ib_multi_thread);

    /// @brief Create a query-based (pull) metric group. P8 will periodically invoke il_query callback at the
    ///        specified interval. Inside the callback, use p8_mtk_group_emit_begin / p8_mtk_group_emit /
    ///        p8_mtk_group_emit_end to submit values for each metric in the group.
    /// @param ip_base              [in] group descriptor (mp_name becomes the group name)
    /// @param iu_query_interval_ms [in] query period in milliseconds
    /// @param il_query             [in] callback invoked by P8 to collect group metric values
    /// @param ip_user_context      [in] opaque pointer forwarded to il_query on each invocation
    /// @return positive group ID on success, negative value on failure
    h_p8_mtk_group_id p8_mtk_create_group_query(const struct s_p8_mtk_base *ip_base,
                                                uint32_t                    iu_query_interval_ms,
                                                l_p8_mtk_group_query_cb     il_query,
                                                void                       *ip_user_context);

    /// @brief Begin an atomic emission batch for a metric group. Must be followed by one or more p8_mtk_group_emit
    ///        calls and terminated by p8_mtk_group_emit_end. All values within a batch share the same timestamp.
    /// @param ih_group_id [in] group ID returned by p8_mtk_create_group or p8_mtk_create_group_query
    /// @return true - success, false - failure
    bool p8_mtk_group_emit_begin(h_p8_mtk_group_id ih_group_id);

    /// @brief Emit a single metric value inside an active group batch (between begin/end calls)
    /// @param ip_name  [in] metric name
    /// @param id_value [in] sample value
    /// @return true - success, false - failure
    bool p8_mtk_group_emit(const char *ip_name, double id_value);

    /// @brief Finalize and flush the current group emission batch started by p8_mtk_group_emit_begin
    /// @param ih_group_id [in] group ID matching the preceding p8_mtk_group_emit_begin call
    /// @return true - success, false - failure
    bool p8_mtk_group_emit_end(h_p8_mtk_group_id ih_group_id);

#ifdef __cplusplus
} // extern "C"
#endif