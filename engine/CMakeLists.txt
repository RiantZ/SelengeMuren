add_library(p8 STATIC
            "${P8_API}/p8_client_api.h"
            "${P8_API}/p8_config.h"
            p8_buffer_pool.hpp
            p8_buffer_pool.cpp
            p8_memory_budget.hpp
            p8_memory_budget.cpp
            p8_core.hpp
            p8_core.cpp
            p8_static_checks.cpp
            p8_tls_writer.hpp
            p8_tls_writer.cpp
            p8_sink.hpp
            p8_sink_null.hpp
            p8_sink_null.cpp
            p8_log.hpp
            p8_log.cpp
            p8_mtk.hpp
            p8_mtk.cpp
            p8_trc.hpp
            p8_trc.cpp
            p8_hash.hpp
            p8_config.cpp
            )

target_include_directories(p8 PRIVATE "${P8_API}")
set_property(TARGET p8 PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(p8 PRIVATE kit::kit nlohmann_json::nlohmann_json xxhash::xxhash)

set_ide_folder(p8)
