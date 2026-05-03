# Python bindings for RDMA engine

cmake_minimum_required(VERSION 3.18)

# Find pybind11
find_package(pybind11 REQUIRED)

# ============================================================================
# RDMA Bindings Module
# ============================================================================

pybind11_add_module(rdma_bindings
    rdma_bindings.cpp
)

target_link_libraries(rdma_bindings
    PRIVATE
        rdma_engine
        common
        ${IBVERBS_LIB}
        ${RDMACM_LIB}
)

if(ENABLE_GPU_DIRECT)
    target_link_libraries(rdma_bindings PRIVATE CUDA::cudart)
    target_include_directories(rdma_bindings PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
endif()

target_include_directories(rdma_bindings PRIVATE
    ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# Node Accessor Module
# ============================================================================

pybind11_add_module(node_accessor
    node_accessor.cpp
)

target_link_libraries(node_accessor
    PRIVATE
        nodes
        rdma_engine
        common
)

target_include_directories(node_accessor PRIVATE
    ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# Installation
# ============================================================================

install(TARGETS rdma_bindings node_accessor
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/python
)
