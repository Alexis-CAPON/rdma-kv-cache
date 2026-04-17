cmake_minimum_required(VERSION 3.18)
project(rdma_kv_cache CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(ENABLE_GPU_DIRECT "Enable GPU Direct RDMA" ON)

# Find dependencies
find_package(pybind11 REQUIRED)
find_package(CUDA REQUIRED)

# RDMA libraries
find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(RDMACM_LIB rdmacm REQUIRED)

if(NOT IBVERBS_LIB)
    message(FATAL_ERROR "libibverbs not found. Install with: sudo apt-get install libibverbs-dev")
endif()

if(NOT RDMACM_LIB)
    message(FATAL_ERROR "librdmacm not found. Install with: sudo apt-get install librdmacm-dev")
endif()

message(STATUS "Found libibverbs: ${IBVERBS_LIB}")
message(STATUS "Found librdmacm: ${RDMACM_LIB}")

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/cpp/include
    ${CUDA_INCLUDE_DIRS}
)

# Add subdirectories
add_subdirectory(cpp)

# Install configuration
install(FILES configs/rdma_config.yaml
              configs/cluster_config.yaml
              configs/model_config.yaml
        DESTINATION ${CMAKE_INSTALL_PREFIX}/configs)
