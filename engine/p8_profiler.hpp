#pragma once

// p8 profiling shim over the Tracy profiler.
//
// Every macro below forwards to the real Tracy API only when the build was
// configured with P8_ENABLE_TRACY (a *-tracy CMake preset, or
// -DP8_ENABLE_TRACY=ON). Otherwise the macros expand to nothing, Tracy is not
// fetched/compiled/linked, and instrumentation costs exactly zero at build time
// and run time. Include this header freely from any p8 translation unit and
// link the p8::profiler target.
//
// Naming note: these are preprocessor #define macros, so ALL_CAPS is the
// mandated casing (see doc/code_style.md).

#if defined(P8_ENABLE_TRACY)

    #include "tracy/Tracy.hpp"

    // Scoped CPU zone spanning the enclosing lexical scope. Named variant takes a
    // compile-time string literal shown in the Tracy timeline.
    #define P8_PROF_ZONE()                    ZoneScoped
    #define P8_PROF_ZONE_NAMED(ip_name)       ZoneScopedN(ip_name)

    // Frame boundary markers (unnamed / named continuous frame set).
    #define P8_PROF_FRAME_MARK()              FrameMark
    #define P8_PROF_FRAME_MARK_NAMED(ip_name) FrameMarkNamed(ip_name)

    // Label the calling OS thread in the Tracy timeline.
    #define P8_PROF_THREAD_NAME(ip_name)      tracy::SetThreadName(ip_name)

    // Numeric plot / free-form timeline message.
    #define P8_PROF_PLOT(ip_name, i_value)    TracyPlot(ip_name, i_value)
    #define P8_PROF_MESSAGE(ip_text, iz_size) TracyMessage(ip_text, iz_size)

    // Memory allocation tracking.
    #define P8_PROF_ALLOC(ip_ptr, iz_size)    TracyAlloc(ip_ptr, iz_size)
    #define P8_PROF_FREE(ip_ptr)              TracyFree(ip_ptr)

#else

    #define P8_PROF_ZONE()                    ((void)0)
    #define P8_PROF_ZONE_NAMED(ip_name)       ((void)0)
    #define P8_PROF_FRAME_MARK()              ((void)0)
    #define P8_PROF_FRAME_MARK_NAMED(ip_name) ((void)0)
    #define P8_PROF_THREAD_NAME(ip_name)      ((void)0)
    #define P8_PROF_PLOT(ip_name, i_value)    ((void)0)
    #define P8_PROF_MESSAGE(ip_text, iz_size) ((void)0)
    #define P8_PROF_ALLOC(ip_ptr, iz_size)    ((void)0)
    #define P8_PROF_FREE(ip_ptr)              ((void)0)

#endif
