cmake_minimum_required(VERSION 3.16)

project(ipc_log_console_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

#示例是独立 CMake 项目：源码不进入日志系统的 apps/，构建时只把日志系统作为依赖引入。
set(IPC_LOG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "IPC log system source root")
if(NOT EXISTS "${IPC_LOG_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "IPC log system was not found at ${IPC_LOG_ROOT}. Configure with -DIPC_LOG_ROOT=/path/to/HJM-fast.")
endif()

add_subdirectory("${IPC_LOG_ROOT}" "${CMAKE_BINARY_DIR}/ipc_log_project")

add_executable(fdbus_console_demo demo_client.cpp)
target_link_libraries(fdbus_console_demo PRIVATE ipc_log_transport)
#运行时从构建目录直接找到 FDBus 共享库，不需要额外设置 LD_LIBRARY_PATH。
set_target_properties(fdbus_console_demo PROPERTIES
    BUILD_RPATH "$<TARGET_FILE_DIR:fdbus_imported>")
