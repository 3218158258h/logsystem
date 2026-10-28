cmake_minimum_required(VERSION 3.16)

project(ipc_log_system LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

include(ExternalProject)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)

set(FDBUS_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/fdbus-v5.4.0/fdbus-v5.4.0")
if(NOT EXISTS "${FDBUS_SOURCE_DIR}/cmake/CMakeLists.txt")
    message(FATAL_ERROR
        "FDBus 5.4.0 source was not found at ${FDBUS_SOURCE_DIR}. "
        "Keep the supplied FDBus source tree at that path.")
endif()

set(FDBUS_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/fdbus-build")
if(MSVC)
    set(FDBUS_LIBRARY "${FDBUS_BINARY_DIR}/fdbus.lib")
    set(FDBUS_NAME_SERVER "${FDBUS_BINARY_DIR}/name_server.exe")
    set(FDBUS_COMPAT_CXX_FLAGS "")
else()
    set(FDBUS_LIBRARY "${FDBUS_BINARY_DIR}/libfdbus.so")
    set(FDBUS_NAME_SERVER "${FDBUS_BINARY_DIR}/name_server")
    # FDBus 5.4's IFdbMsgBuilder.h uses uint8_t/int32_t without including
    # <cstdint>; keep the supplied third-party tree untouched.
    set(FDBUS_COMPAT_CXX_FLAGS "-include cstdint")
endif()

# FDBus derives its source root from CMAKE_SOURCE_DIR. Build its own project
# separately and compile only the transport library and name service.
ExternalProject_Add(fdbus_external
    SOURCE_DIR "${FDBUS_SOURCE_DIR}/cmake"
    BINARY_DIR "${FDBUS_BINARY_DIR}"
    CMAKE_ARGS
        "-DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/_deps/fdbus-install"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_CXX_FLAGS=${FDBUS_COMPAT_CXX_FLAGS}"
        "-Dfdbus_ENABLE_LOG=OFF"
        "-Dfdbus_BUILD_CLIB=OFF"
        "-Dfdbus_BUILD_JNI=OFF"
        "-Dfdbus_SEC_USING_SSL=OFF"
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build "<BINARY_DIR>" --target fdbus name_server
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${FDBUS_LIBRARY}" "${FDBUS_NAME_SERVER}"
)

add_library(fdbus_imported SHARED IMPORTED GLOBAL)
set_target_properties(fdbus_imported PROPERTIES
    IMPORTED_LOCATION "${FDBUS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FDBUS_SOURCE_DIR};${FDBUS_SOURCE_DIR}/public")

protobuf_generate_cpp(LOG_PROTOCOL_SOURCES LOG_PROTOCOL_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/log_protocol.proto")

# Core stays independent of FDBus; the transport library is the only layer that links both SDKs.
add_library(ipc_log_core
    src/log_record.cpp
    src/logger.cpp
    src/log_formatter.cpp
    src/console_sink.cpp
    src/rotating_file_sink.cpp
    src/reorder_buffer.cpp
    src/log_aggregator.cpp)
target_include_directories(ipc_log_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ipc_log_core PUBLIC Threads::Threads)

add_library(ipc_log_transport
    ${LOG_PROTOCOL_SOURCES}
    ${LOG_PROTOCOL_HEADERS}
    src/fdbus_transport.cpp
    src/log_server.cpp)
add_dependencies(ipc_log_transport fdbus_external)
target_include_directories(ipc_log_transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${FDBUS_SOURCE_DIR}"
    "${FDBUS_SOURCE_DIR}/public")
target_link_libraries(ipc_log_transport PUBLIC
    ipc_log_core fdbus_imported protobuf::libprotobuf Threads::Threads)
if(NOT MSVC)
    # Public FDBus 5.4 headers reference fixed-width types without including <cstdint>.
    target_compile_options(ipc_log_transport PRIVATE -include cstdint)
endif()

add_executable(local_log_demo apps/local_log_demo.cpp)
target_include_directories(local_log_demo PRIVATE apps)
target_link_libraries(local_log_demo PRIVATE ipc_log_core)

add_executable(log_server apps/log_server_main.cpp)
target_include_directories(log_server PRIVATE apps)
target_link_libraries(log_server PRIVATE ipc_log_transport)

add_executable(log_worker apps/log_worker_main.cpp)
target_include_directories(log_worker PRIVATE apps)
target_link_libraries(log_worker PRIVATE ipc_log_transport)

foreach(application local_log_demo log_server log_worker)
    set_target_properties(${application} PROPERTIES
        BUILD_RPATH "${FDBUS_BINARY_DIR}"
        INSTALL_RPATH "$ORIGIN/../_deps/fdbus-build")
endforeach()

add_custom_target(ipc_log_system ALL
    DEPENDS local_log_demo log_server log_worker)

message(STATUS "FDBus source: ${FDBUS_SOURCE_DIR}")
message(STATUS "FDBus build:  ${FDBUS_BINARY_DIR}")
message(STATUS "Protobuf:    ${Protobuf_VERSION}")
