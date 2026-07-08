cmake_minimum_required(VERSION 4.0 FATAL_ERROR)

project(Protocol8)

if(NOT P8_BUILD_SCRIPT)
    message(FATAL_ERROR
        "Invoke CMake via the build script:\n"
        "  python scripts/cmake_build.py [--build] [--clean]"
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tracy profiler integration. OFF by default: when disabled nothing is fetched,
# compiled or linked, and the p8_profiler.hpp macros expand to no-ops, so there
# is zero build-time and run-time cost. Enable via a *-tracy CMake preset (see
# CMakePresets.json) or -DP8_ENABLE_TRACY=ON.
option(P8_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)

include(CheckCXXCompilerFlag)
include(CheckCCompilerFlag)

function(enable_c_compiler_flag_if_supported flag)
    string(FIND "${CMAKE_C_FLAGS}" "${flag}" flag_already_set)
    if(flag_already_set EQUAL -1)
        check_c_compiler_flag("${flag}" flag_supported)
        if(flag_supported)
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flag}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

function(enable_cpp_c_compiler_flag_if_supported flag)
    string(FIND "${CMAKE_CXX_FLAGS}" "${flag}" flag_already_set)
    if(flag_already_set EQUAL -1)
        check_cxx_compiler_flag("${flag}" flag_supported)
        if(flag_supported)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}" PARENT_SCOPE)
        endif()
    endif()

    string(FIND "${CMAKE_C_FLAGS}" "${flag}" flag_already_set)
    if(flag_already_set EQUAL -1)
        check_c_compiler_flag("${flag}" flag_supported)
        if(flag_supported)
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flag}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

set(ROOT_P8_PATH ${PROJECT_SOURCE_DIR} CACHE INTERNAL "")

if(MSVC)
    add_compile_definitions(_UNICODE UNICODE)

    # <windows.h> (pulled in via kit) defines min()/max() macros that clobber
    # std::min/std::max; NOMINMAX suppresses them.
    add_compile_definitions(NOMINMAX)

    if(NOT COMMAND set_ide_folder)
        macro(set_ide_folder _project_name)
            string(LENGTH ${ROOT_P8_PATH} ROOT_P8_PATH_LEN)
            string(LENGTH ${CMAKE_CURRENT_SOURCE_DIR} CURRENT_SOURCE_DIR_LEN)
            MATH(EXPR ROOT_P8_PATH_LEN "${ROOT_P8_PATH_LEN}+1")
            MATH(EXPR CURRENT_SOURCE_DIR_LEN "${CURRENT_SOURCE_DIR_LEN} - ${ROOT_P8_PATH_LEN}")

            string(SUBSTRING ${CMAKE_CURRENT_SOURCE_DIR} ${ROOT_P8_PATH_LEN} ${CURRENT_SOURCE_DIR_LEN} IDE_SUBFOLDER)
            set_property(TARGET ${_project_name} PROPERTY FOLDER ${IDE_SUBFOLDER})

            message(${_project_name})
        endmacro()
    endif()

    #enable_cpp_c_compiler_flag_if_supported("/Wall")
    enable_cpp_c_compiler_flag_if_supported("/W4")
    enable_cpp_c_compiler_flag_if_supported("/WX")
    # C4324: structure padded due to alignas() specifier. The padding is
    # intentional (cache-line alignment via alignas(64)), so silence it rather
    # than let /WX turn it into an error.
    enable_cpp_c_compiler_flag_if_supported("/wd4324")
else()
    enable_c_compiler_flag_if_supported("-std=gnu99")

    enable_cpp_c_compiler_flag_if_supported("-Wall")
    enable_cpp_c_compiler_flag_if_supported("-Wextra")
    enable_cpp_c_compiler_flag_if_supported("-pedantic")
    enable_cpp_c_compiler_flag_if_supported("-Werror")
    enable_cpp_c_compiler_flag_if_supported("-D_FILE_OFFSET_BITS=64")
    enable_cpp_c_compiler_flag_if_supported("-D_LARGEFILE64_SOURCE")
    enable_cpp_c_compiler_flag_if_supported("-fvisibility=hidden")
    enable_cpp_c_compiler_flag_if_supported("-Wno-variadic-macros")
    enable_cpp_c_compiler_flag_if_supported("-Wno-long-long")
    enable_cpp_c_compiler_flag_if_supported("-Wno-missing-field-initializers")
    enable_cpp_c_compiler_flag_if_supported("-Wno-unknown-pragmas")
    enable_cpp_c_compiler_flag_if_supported("-Wno-address-of-packed-member")

    macro(set_ide_folder _project_name)
    endmacro()
endif()

set(CMAKE_LINK_WARNING_AS_ERROR ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)


if(P8_EXTERNAL_INTEGRATION)
else()
    SET(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/_Build_)
endif()

enable_testing()

add_subdirectory(doc)
add_subdirectory(api)
add_subdirectory(dep)
add_subdirectory(engine)
add_subdirectory(tests/regression)
add_subdirectory(examples)
