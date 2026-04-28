cmake_minimum_required(VERSION 3.18)

# Build options (must be declared before project() so the CUDA language is
# only requested when GPUDirect support is actually needed)
option(ENABLE_GPU_DIRECT "Enable GPU Direct RDMA" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(ENABLE_GPU_DIRECT)
    project(rdma_kv_cache CXX CUDA)
    set(CMAKE_CUDA_STANDARD 17)
else()
    project(rdma_kv_cache CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find dependencies
find_package(pybind11 QUIET)
if(pybind11_FOUND)
    message(STATUS "pybind11 found - Python bindings will be built")
    set(BUILD_PYTHON_BINDINGS ON)
else()
    message(STATUS "pybind11 not found - Python bindings will be skipped")
    set(BUILD_PYTHON_BINDINGS OFF)
endif()

if(ENABLE_GPU_DIRECT)
    find_package(CUDA REQUIRED)
    message(STATUS "GPUDirect RDMA enabled")
else()
    message(STATUS "GPUDirect RDMA disabled — CPU-only RDMA path selected")
endif()

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
include_directories(${CMAKE_SOURCE_DIR}/cpp)
if(ENABLE_GPU_DIRECT)
    include_directories(${CUDA_INCLUDE_DIRS})
endif()

# Add subdirectories
add_subdirectory(cpp)

# Install configuration
install(FILES configs/rdma_config.yaml
              configs/cluster_config.yaml
              configs/model_config.yaml
        DESTINATION ${CMAKE_INSTALL_PREFIX}/configs)
