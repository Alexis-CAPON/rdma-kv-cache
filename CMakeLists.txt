cmake_minimum_required(VERSION 3.18)

# Build options
option(ENABLE_GPU_DIRECT "Enable GPU Direct RDMA" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)

# Always start with CXX only - we'll enable CUDA later if needed
project(rdma_kv_cache CXX)

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
    # Try to enable CUDA language
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)

        # Find CUDA Toolkit
        find_package(CUDAToolkit REQUIRED)
        message(STATUS "GPUDirect RDMA enabled")
        message(STATUS "CUDA Toolkit version: ${CUDAToolkit_VERSION}")
        set(CUDA_AVAILABLE TRUE)
    else()
        message(WARNING "CUDA compiler not found. ENABLE_GPU_DIRECT=ON but nvcc is not available.")
        message(WARNING "Falling back to CPU-only RDMA mode.")
        message(WARNING "To fix: Install CUDA toolkit or set -DENABLE_GPU_DIRECT=OFF")
        set(ENABLE_GPU_DIRECT OFF)
        set(CUDA_AVAILABLE FALSE)
    endif()
else()
    message(STATUS "GPUDirect RDMA disabled — CPU-only RDMA path selected")
    set(CUDA_AVAILABLE FALSE)
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
