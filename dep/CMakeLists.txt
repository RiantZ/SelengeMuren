include(FetchContent)

cmake_policy(SET CMP0205 NEW)

# SYSTEM: consume kit as a third-party dependency so its headers are included
# with -isystem. That suppresses warnings originating inside kit headers (e.g.
# GCC's -Wclass-memaccess from list.hpp's memset on non-trivial types when a
# kit template is instantiated in a p8 translation unit) without weakening the
# project's -Werror on p8's own code.
# p8 only ever links the static kit archive (kit::kit). Skip building kit's
# shared library so we don't waste build time on an unused DLL/.so (and, on
# Windows, avoid its import lib colliding with the static kit.lib).
set(KIT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    kit
    GIT_REPOSITORY git@github.com:RiantZ/kit.git
    GIT_TAG        main
    SYSTEM
)
FetchContent_MakeAvailable(kit)

# JSON for Modern C++ - used by engine/p8_config.cpp to parse the p8 config file.
# Pinned to a stable release tag; bump deliberately, not to "master".
set(JSON_BuildTests       OFF CACHE INTERNAL "")
set(JSON_Install          OFF CACHE INTERNAL "")
set(JSON_MultipleHeaders  OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# xxHash -- header-only usage (Variant A). Only xxhash.h is needed; no compiled library.
# No CMakeLists.txt at repo root, so MakeAvailable just downloads without configuring.
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
)
FetchContent_MakeAvailable(xxhash)

add_library(xxhash_hdr INTERFACE)
add_library(xxhash::xxhash ALIAS xxhash_hdr)
target_include_directories(xxhash_hdr SYSTEM INTERFACE ${xxhash_SOURCE_DIR})

# Tracy profiler -- optional, controlled by P8_ENABLE_TRACY (OFF by default).
#
# p8_profiler is the single target every p8 module links against. It always
# exists so call sites and CMake wiring never change. When P8_ENABLE_TRACY is
# OFF the target is empty: Tracy is not fetched, not compiled and not linked,
# and the P8_PROF_* macros in p8_profiler.hpp compile to nothing. When ON, Tracy
# is fetched, TRACY_ENABLE is set so the client actually collects data, and the
# P8_ENABLE_TRACY compile definition is propagated to every consumer so the
# macros forward to the real Tracy API.
add_library(p8_profiler INTERFACE)
add_library(p8::profiler ALIAS p8_profiler)

if(P8_ENABLE_TRACY)
    set(TRACY_ENABLE    ON  CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.13.1
    )
    FetchContent_MakeAvailable(tracy)

    # Tracy's own sources are third-party; do not subject them to the project's
    # -Wall -Wextra -pedantic -Werror (or /W4 /WX) flags inherited via the
    # global CMAKE_CXX_FLAGS, or the build would fail on their warnings.
    if(TARGET TracyClient)
        if(MSVC)
            target_compile_options(TracyClient PRIVATE /w)
        else()
            target_compile_options(TracyClient PRIVATE -w)
        endif()
    endif()

    target_link_libraries(p8_profiler INTERFACE Tracy::TracyClient)
    target_compile_definitions(p8_profiler INTERFACE P8_ENABLE_TRACY)
    message(STATUS "p8: Tracy profiler ENABLED (v0.13.1)")
else()
    message(STATUS "p8: Tracy profiler disabled (default)")
endif()

if(NOT IS_SYMLINK "${CMAKE_CURRENT_SOURCE_DIR}/kit")
    file(CREATE_LINK
        ${kit_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/kit
        SYMBOLIC COPY_ON_ERROR
    )
endif()
if(NOT IS_SYMLINK "${PROJECT_SOURCE_DIR}/doc/code_style.md")
    file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/kit/doc/code_style.md
        ${PROJECT_SOURCE_DIR}/doc/code_style.md
        SYMBOLIC COPY_ON_ERROR
    )
endif()
if(NOT IS_SYMLINK "${PROJECT_SOURCE_DIR}/scripts/code_format.py")
    file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}/kit/scripts/code_format.py
        ${PROJECT_SOURCE_DIR}/scripts/code_format.py
        SYMBOLIC COPY_ON_ERROR
    )
endif()
