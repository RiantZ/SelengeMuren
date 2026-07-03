cmake_minimum_required(VERSION 3.20...4.0)

project(P8_Example_LogsFileSink)

find_package(Threads REQUIRED)

add_executable(p8_example_multithread_file_sink
    multithread_file_sink.cpp
)

target_include_directories(p8_example_multithread_file_sink PRIVATE ${P8_API})

target_link_libraries(p8_example_multithread_file_sink
    p8
    kit::kit
    Threads::Threads
)

set_ide_folder(p8_example_multithread_file_sink)
