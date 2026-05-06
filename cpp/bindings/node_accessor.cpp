// cpp/bindings/node_accessor.cpp
// Python bindings for node accessor functions

#include <pybind11/pybind11.h>
namespace py = pybind11;

#include "cpp/bindings/node_accessor.h"
#include "cpp/nodes/node.h"
#include "cpp/rdma/rdma_engine.h"

PYBIND11_MODULE(node_accessor, m)
{
    m.doc() = "Node accessor for accessing RDMA engine from vLLM Python connector";

    m.def("set_current_node", &set_current_node,
          py::arg("node"),
          "Set the current node instance");

    m.def("get_node_rdma_engine_ptr", &get_node_rdma_engine,
          py::return_value_policy::reference,
          "Get pointer to the node's RDMA engine");
}