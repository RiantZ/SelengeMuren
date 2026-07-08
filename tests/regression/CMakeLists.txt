cmake_minimum_required(VERSION 4.0)

project(P8_RegressionTests)

include(FetchContent)

#see: https://github.com/google/googletest/releases/
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

# For Windows: Prevent overriding the parent project's compiler/linker on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(${PROJECT_NAME}
    main.cpp
    p8_attr_test.cpp
    p8_buffer_pool_test.cpp
    p8_config_test.cpp
    p8_core_test.cpp
    p8_memory_budget_test.cpp
    p8_xxh_test.cpp
    p8_log_format_test.cpp
    p8_log_fragment_test.cpp
    p8_log_content_test.cpp
    p8_log_module_test.cpp
    p8_log_perf_test.cpp
    p8_writer_registry_test.cpp
    p8_svc_serialize_test.cpp
    p8_sink_file_test.cpp
    p8_pull_test.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${P8_API} ${CMAKE_SOURCE_DIR}/engine)

target_compile_definitions(p8 PRIVATE P8_TESTING)
target_compile_definitions(${PROJECT_NAME} PRIVATE P8_TESTING)

target_link_libraries(${PROJECT_NAME} gtest_main p8 kit::kit xxhash::xxhash nlohmann_json::nlohmann_json)

add_test(NAME P8_RegressionTests COMMAND P8_RegressionTests)

set_ide_folder(${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME} DESTINATION bin)