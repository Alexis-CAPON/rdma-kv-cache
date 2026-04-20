# Python bindings for RDMA engine

cmake_minimum_required(VERSION 3.18)

# Find pybind11
find_package(pybind11 REQUIRED)

# Create Python module
pybind11_add_module(rdma_bindings
    rdma_bindings.cpp
)

# Link to RDMA engine and dependencies
target_link_libraries(rdma_bindings
    PRIVATE
        rdma_engine
        common
        ${IBVERBS_LIB}
        ${RDMACM_LIB}
        ${CUDA_LIBRARIES}
)

target_include_directories(rdma_bindings PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CUDA_INCLUDE_DIRS}
)

# Install Python module
install(TARGETS rdma_bindings
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/python
)
