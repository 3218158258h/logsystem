cmake_minimum_required(VERSION 3.16)

project(ipc_log_core_stage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

find_package(Threads REQUIRED)

#本阶段只构建进程内日志核心和本地演示，不引入 Protobuf/FDBus。
add_library(ipc_log_core
    src/log_record.cpp
    src/logger.cpp
    src/log_formatter.cpp
    src/console_sink.cpp
    src/rotating_file_sink.cpp
    src/reorder_buffer.cpp
    src/log_aggregator.cpp)
target_include_directories(ipc_log_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ipc_log_core PUBLIC Threads::Threads)

add_executable(local_log_demo apps/local_log_demo.cpp)
target_include_directories(local_log_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/apps")
target_link_libraries(local_log_demo PRIVATE ipc_log_core)
