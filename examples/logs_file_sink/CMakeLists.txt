cmake_minimum_required(VERSION 4.0)

project(P8_Example_LogsFileSink)

find_package(Threads REQUIRED)

add_executable(p8_example_multithread_file_sink
    multithread_file_sink.cpp
)

target_include_directories(p8_example_multithread_file_sink PRIVATE ${P8_API} ${ROOT_P8_PATH}/engine)

target_link_libraries(p8_example_multithread_file_sink
    p8
    kit::kit
    Threads::Threads
    p8::profiler
)

set_ide_folder(p8_example_multithread_file_sink)
