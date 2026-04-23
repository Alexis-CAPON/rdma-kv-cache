# C++ Components Build Configuration

cmake_minimum_required(VERSION 3.18)

# ============================================================================
# External Dependencies
# ============================================================================

# Find nlohmann/json for IPC communication
find_package(nlohmann_json 3.2.0)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, attempting to use FetchContent")
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(json)
endif()

# ============================================================================
# Common Libraries
# ============================================================================

# Common utilities
add_library(common STATIC
    common/config.cpp
    common/connection.cpp
    common/messages.cpp
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# Network Libraries
# ============================================================================

# Find CURL
find_package(CURL REQUIRED)

add_library(network STATIC
    network/tcp_server.cpp
    network/http_client.cpp
)

target_link_libraries(network
    PUBLIC
        common
        CURL::libcurl
)

target_include_directories(network PUBLIC
    ${CURL_INCLUDE_DIRS}
)

# ============================================================================
# RDMA Libraries
# ============================================================================

add_library(rdma_engine STATIC
    rdma/rdma_engine.cpp
    rdma/device_probe.cpp
    rdma/gpudirect_rdma.cpp
)

target_link_libraries(rdma_engine
    PUBLIC
        common
        ${IBVERBS_LIB}
        ${RDMACM_LIB}
        ${CUDA_LIBRARIES}
)

target_include_directories(rdma_engine PUBLIC
    ${CUDA_INCLUDE_DIRS}
)

# ============================================================================
# vLLM Integration Library
# ============================================================================

add_library(vllm_integration STATIC
    nodes/vllm_client.cpp
    nodes/kv_rdma_manager.cpp
)

target_link_libraries(vllm_integration
    PUBLIC
        common
        rdma_engine
        nlohmann_json::nlohmann_json
)

target_include_directories(vllm_integration PUBLIC
    ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# Node Components
# ============================================================================

add_library(nodes STATIC
    nodes/node.cpp
    nodes/worker.cpp
    nodes/epoll_worker.cpp
    nodes/worker_pool.cpp
    nodes/request_tracker.cpp
)

target_link_libraries(nodes
    PUBLIC
        common
        network
        rdma_engine
        vllm_integration
)

# ============================================================================
# Applications
# ============================================================================

# Orchestrator
add_executable(orchestrator
    apps/orchestrator/orchestrator_main.cpp
    apps/orchestrator/orchestrator_engine.cpp
    apps/orchestrator/node_registry.cpp
    apps/orchestrator/request_tracker.cpp
    apps/orchestrator/request_router.cpp
    apps/orchestrator/rdma_exchange_tracker.cpp
)

target_link_libraries(orchestrator
    PRIVATE
        nodes
        common
        network
)

# Prefill Node
add_executable(prefill_node
    apps/prefill_main.cpp
)

target_link_libraries(prefill_node
    PRIVATE
        nodes
        vllm_integration
)

# Decode Node
add_executable(decode_node
    apps/decode_main.cpp
)

target_link_libraries(decode_node
    PRIVATE
        nodes
        vllm_integration
)

# Benchmark Client
add_executable(benchmark_client
    apps/benchmark_client.cpp
)

target_link_libraries(benchmark_client
    PRIVATE
        common
)

# ============================================================================
# Installation
# ============================================================================

install(TARGETS orchestrator prefill_node decode_node benchmark_client
    RUNTIME DESTINATION bin
)

install(TARGETS vllm_integration rdma_engine nodes network common
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# ============================================================================
# Python Bindings (Optional)
# ============================================================================

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bindings/CMakeLists.txt)
    add_subdirectory(bindings)
endif()
